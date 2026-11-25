# Resilient Peripheral Communication Protocol

A small, heap-free C++17 implementation of a reliable host ⇄ peripheral command
protocol on top of a transport that can **lose, delay, reorder and duplicate**
messages.

The peripheral holds a counter. The host can `Increment` it and `ReadCounter` it.
An increment is never executed twice because of a retransmission, and when the
host cannot confirm whether an increment happened it says so (`Outcome::Uncertain`).

## Build & run

Requirements: a C++17 compiler (g++/clang++) and `make`. No other dependencies.

```sh
make test        # build + run the automated tests (17 scenarios, ~48k checks)
make demo        # short narrated run of the main fault scenarios
make sanitize    # run the tests under AddressSanitizer + UBSan
make clean
```

`CXX=clang++ make test` selects another compiler. Warnings are errors
(`-Wall -Wextra -Wpedantic -Werror`).

## Layout

| Path | Role |
|---|---|
| `include/rpp/protocol.hpp` | `Frame`, enums, wrap-safe `seq_newer` / `time_reached`, `ITransport` |
| `include/rpp/host.hpp`, `src/host.cpp` | Host transaction manager (retries, timeouts, outcomes) |
| `include/rpp/peripheral.hpp`, `src/peripheral.cpp` | Peripheral command processor (at-most-once execution) |
| `include/rpp/sim_transport.hpp`, `src/sim_transport.cpp` | Fault-injecting simulated link |
| `tests/test_main.cpp` | Scripted scenarios + randomized soak |
| `src/demo.cpp` | Demo program |
| `docs/DESIGN.md` | Architecture, decisions, assumptions, limitations |

## Usage sketch

```cpp
SimTransport net;
Peripheral dev;
Host host(net.host_port(), HostConfig{});

host.begin(Command::Increment, now_ms);       // non-blocking
for (;;) {                                    // your loop / tick
    dev.service(net.peripheral_port(), now_ms);
    host.poll(now_ms);
    if (host.done()) break;
}
switch (host.result().outcome) { /* Success, DeviceError, Timeout, Uncertain */ }
```

## Outcomes

| Outcome | Meaning | What the caller should do |
|---|---|---|
| `Success` | Response received, status OK | Use `value` |
| `DeviceError` | Response received, device refused the command | Handle error |
| `Timeout` | Retries exhausted for an **idempotent** command (read) | Safe to retry |
| `Uncertain` | Retries exhausted for a **non-idempotent** command (increment) | Do **not** blindly retry; issue a `ReadCounter` to learn the state |

See `docs/DESIGN.md` for details.
# rpp
