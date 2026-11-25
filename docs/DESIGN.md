# Design notes

## Architecture

```
 Host (transaction manager) ──send/recv──▶ Port ─▶ Channel ─▶ Port ──▶ Peripheral
                            ◀──────────── Channel (to_host)  ◀────────
```

* **Host** — owns one transaction at a time. `begin()` sends; `poll(now)` consumes
  responses and runs the retry timer. Non-blocking and clock-agnostic: the caller
  supplies `now` in milliseconds.
* **Peripheral** — `on_request()` is pure protocol logic; `service()` just pumps
  it from a transport port.
* **SimTransport** — two `Channel`s (one per direction) behind the `ITransport`
  interface that Host and Peripheral depend on, so a real link could replace it.
  Faults per frame: drop, duplicate (up to 4 copies), delay (⇒ reordering).
  Faults are either *scripted* (a FIFO of `FaultAction`s, used for deterministic
  tests) or *random* (seeded xorshift32, used for the soak test).
* The simulated clock is driven by the test/demo loop (1 tick = 1 ms), so every
  run is deterministic and takes no wall-clock time.

## Protocol

Request: `{type, cmd, seq}`. Response: `{type, cmd, seq, status, value}`.

* The host assigns a 16-bit `seq` per **transaction**; **retransmissions reuse
  the same `seq`**. This is what lets the peripheral recognise a retry.
* Peripheral state is one `last_seq` and the cached `last_rsp` (O(1) memory):
  * `seq == last_seq` → duplicate/retry: **replay the cached response, do not execute**.
  * `seq` newer (modular compare) → execute, cache, respond.
  * `seq` older → stale delayed packet: ignore (the host has moved on).
* Host accepts a response only if `seq` and `cmd` match the pending transaction.
  Duplicates, late responses from earlier transactions, and responses after
  completion are counted in `ignored_frames` and dropped.
* Sequence arithmetic is wrap-safe (`int16_t` difference), tested across 0xFFFF→0.

## Retry / timeout policy

`HostConfig`: `initial_timeout_ms=100`, doubling per retry up to
`max_timeout_ms=800`, `max_attempts=4` (1 send + 3 retries). Worst case is the
sum of the timeouts (1500 ms with defaults), exposed as
`worst_case_duration_ms()` and asserted by the tests. A response that arrives
during *any* attempt's wait completes the transaction, including a slow response
to an earlier transmission. Responses are processed before the timer check, so a
response arriving exactly at the deadline wins.

## Uncertain outcomes

When retries are exhausted the host cannot tell "request never arrived" from
"executed but every response was lost". For `ReadCounter` that doesn't matter
(→ `Timeout`, safe to repeat). For `Increment` it does, so the result is
`Uncertain`. The protocol never guesses. Resolution is the application's job:
issue a `ReadCounter` (the tests show this) or apply a domain-specific rule.
A response arriving after the transaction was abandoned is ignored; it does not
rewrite the reported outcome.

## Key decisions

* **At-most-once via seq + cached response**, not via making the command
  idempotent — it works for any command, and costs one cached frame.
* **No heap, no exceptions, no STL in the library**; fixed-size queues
  (32 frames in flight, 16 scripted faults). Overflow is treated as loss and counted.
* **Transport interface (`ITransport`)** instead of the host calling the simulator
  directly, so the protocol logic is transport-agnostic.
* **Wrap-safe time and sequence comparisons** from the start rather than as a patch.
* **Tests assert invariants, not just examples**: the soak (300 seeds × 40
  transactions, 25 % loss, 20 % duplication, 0–120 ms delay) checks
  `counter ≤ increments attempted`, `confirmed ≤ counter ≤ confirmed + uncertain`,
  every `Success` value equals the device's counter, bounded duration, and that
  `Uncertain` is only ever reported for increments. I also mutation-checked the
  suite by temporarily breaking dedupe, stale-rejection and host seq matching;
  each was caught.

## Assumptions

* One outstanding transaction; the host does not start a new one until the
  previous finished (successfully or not).
* No frame corruption (loss/delay/duplication/reordering only), per the brief.
* Delays are bounded well below half the seq space (32768 transactions); a frame
  delayed longer than that could be mistaken for a newer one.
* Host and peripheral share a ms clock *rate*, not a clock *value*.

## Known limitations

* **No byte-level framing or CRC.** Frames are passed as structs. Corruption
  would need a checksum and decode path.
* **Peripheral remembers only the last transaction.** Sufficient for one
  outstanding transaction. Pipelining would need a window of cached responses.
* **Peripheral reset / host reset is out of scope.** After a peripheral reset
  `has_last_` is cleared and a replayed old request could execute again; a
  session/epoch id in the frame would fix that.
* **Fixed backoff, no jitter and no RTT estimation** (single link, one transaction).
* **An `Increment` followed by `Uncertain` is not auto-resolved**; callers decide.
* Counter is `uint32_t` and wraps silently.
* Simulation is single-threaded; thread-safety is not provided.
