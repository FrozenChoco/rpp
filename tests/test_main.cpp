// Dependency-free test runner. Every scenario runs on a deterministic simulated
// clock (1 tick = 1 ms), so results are reproducible.
#include <cstdio>
#include "rpp/host.hpp"
#include "rpp/peripheral.hpp"
#include "rpp/sim_transport.hpp"

using namespace rpp;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

// Wires host, link and peripheral together and drives the clock.
struct Sim {
    SimTransport net;
    Peripheral dev;
    Host host;
    TimeMs now;

    explicit Sim(HostConfig cfg = HostConfig{}, TimeMs start = 1000)
        : host(net.host_port(), cfg), now(start) {}

    void step() {
        ++now;
        dev.service(net.peripheral_port(), now);
        host.poll(now);
    }
    void advance(uint32_t ms) { while (ms--) step(); }
    TxResult run(Command c) {
        host.begin(c, now);
        while (!host.done()) step();
        return host.result();
    }
};

static FaultAction drop() { FaultAction a; a.drop = true; return a; }
static FaultAction delay(uint32_t ms) { FaultAction a; a.delay_ms = ms; return a; }
static FaultAction dup(uint8_t copies, uint32_t gap) {
    FaultAction a; a.copies = copies; a.dup_gap_ms = gap; return a;
}

// ---------------------------------------------------------------------------
static void test_success_clean_link() {
    Sim s;
    TxResult r = s.run(Command::Increment);
    CHECK(r.outcome == Outcome::Success);
    CHECK(r.value == 1 && r.attempts == 1);
    r = s.run(Command::Increment);
    CHECK(r.outcome == Outcome::Success && r.value == 2);
    r = s.run(Command::ReadCounter);
    CHECK(r.outcome == Outcome::Success && r.value == 2);
    CHECK(s.dev.counter() == 2 && s.dev.stats().executed == 3);
}

static void test_lost_request_is_retransmitted() {
    Sim s;
    s.net.to_peripheral().push_fault(drop());
    TxResult r = s.run(Command::Increment);
    CHECK(r.outcome == Outcome::Success);
    CHECK(r.attempts == 2);
    CHECK(r.value == 1 && s.dev.counter() == 1);
}

static void test_lost_response_does_not_double_execute() {
    Sim s;
    s.net.to_host().push_fault(drop());  // device executes, host never hears
    TxResult r = s.run(Command::Increment);
    CHECK(r.outcome == Outcome::Success);
    CHECK(r.attempts == 2);
    CHECK(r.value == 1);                         // replayed original result
    CHECK(s.dev.counter() == 1);                 // executed exactly once
    CHECK(s.dev.stats().duplicates_served == 1);
}

static void test_duplicated_request_executes_once() {
    Sim s;
    s.net.to_peripheral().push_fault(dup(3, 5));
    TxResult r = s.run(Command::Increment);
    CHECK(r.outcome == Outcome::Success && r.value == 1);
    s.advance(50);  // let the extra copies and their responses drain
    CHECK(s.dev.counter() == 1);
    CHECK(s.dev.stats().executed == 1);
    CHECK(s.dev.stats().duplicates_served == 2);
    CHECK(s.host.stats().ignored_frames >= 2);
}

static void test_duplicated_response_does_not_confuse_next_transaction() {
    Sim s;
    s.net.to_host().push_fault(dup(2, 50));  // tx1 response arrives twice, 50ms apart
    s.net.to_host().push_fault(delay(60));   // tx2 response is slow: dup of tx1 lands meanwhile
    TxResult r1 = s.run(Command::Increment);
    TxResult r2 = s.run(Command::Increment);
    CHECK(r1.outcome == Outcome::Success && r1.value == 1);
    CHECK(r2.outcome == Outcome::Success && r2.value == 2);
    s.advance(100);
    CHECK(s.dev.counter() == 2);
    CHECK(s.host.stats().ignored_frames >= 1);
}

static void test_delayed_response_after_retransmit() {
    Sim s;
    s.net.to_host().push_fault(delay(150));  // > first timeout (100ms)
    TxResult r = s.run(Command::Increment);
    CHECK(r.outcome == Outcome::Success);
    CHECK(r.attempts == 2 && r.value == 1);
    s.advance(200);  // original delayed response finally arrives
    CHECK(s.host.result().outcome == Outcome::Success);  // result untouched
    CHECK(s.host.stats().ignored_frames >= 1);
    CHECK(s.dev.counter() == 1);
}

static void test_late_original_request_is_ignored_as_stale() {
    Sim s;
    s.net.to_peripheral().push_fault(delay(400));  // tx1 request #1 crawls
    TxResult r1 = s.run(Command::Increment);       // completes via retransmit
    TxResult r2 = s.run(Command::Increment);
    CHECK(r1.outcome == Outcome::Success && r2.outcome == Outcome::Success);
    CHECK(s.dev.counter() == 2);
    s.advance(500);  // old seq=1 request arrives now
    CHECK(s.dev.counter() == 2);                 // not executed
    CHECK(s.dev.stats().stale_ignored == 1);
}

static void test_all_requests_lost_increment_is_uncertain() {
    HostConfig cfg;  // 100/200/400/800 => 1500ms worst case, 4 attempts
    Sim s(cfg);
    for (int i = 0; i < 4; ++i) s.net.to_peripheral().push_fault(drop());
    TxResult r = s.run(Command::Increment);
    CHECK(r.outcome == Outcome::Uncertain);
    CHECK(r.attempts == 4);
    CHECK(r.elapsed_ms == cfg.worst_case_duration_ms());
    CHECK(r.elapsed_ms == 1500);
    CHECK(s.dev.counter() == 0);  // (host can't know this, we can)
}

static void test_all_responses_lost_increment_is_uncertain_but_executed_once() {
    Sim s;
    for (int i = 0; i < 4; ++i) s.net.to_host().push_fault(drop());
    TxResult r = s.run(Command::Increment);
    CHECK(r.outcome == Outcome::Uncertain && r.attempts == 4);
    CHECK(s.dev.counter() == 1);  // executed, but host cannot confirm
    CHECK(s.dev.stats().executed == 1);
}

static void test_read_exhaustion_reports_timeout_not_uncertain() {
    Sim s;
    for (int i = 0; i < 4; ++i) s.net.to_peripheral().push_fault(drop());
    TxResult r = s.run(Command::ReadCounter);
    CHECK(r.outcome == Outcome::Timeout);
}

static void test_response_after_giving_up_is_ignored_and_read_resolves_uncertainty() {
    Sim s;
    for (int i = 0; i < 3; ++i) s.net.to_peripheral().push_fault(drop());
    s.net.to_host().push_fault(delay(2000));  // 4th attempt executes; response very late
    TxResult r = s.run(Command::Increment);
    CHECK(r.outcome == Outcome::Uncertain);
    s.advance(2500);  // late response arrives after the transaction closed
    CHECK(s.host.result().outcome == Outcome::Uncertain);  // not rewritten
    CHECK(s.dev.counter() == 1);
    // Application-level resolution: READ to learn the truth.
    TxResult rd = s.run(Command::ReadCounter);
    CHECK(rd.outcome == Outcome::Success && rd.value == 1);
}

static void test_busy_rejects_second_begin() {
    Sim s;
    CHECK(s.host.begin(Command::Increment, s.now));
    CHECK(!s.host.begin(Command::Increment, s.now));
    while (!s.host.done()) s.step();
    CHECK(s.host.begin(Command::ReadCounter, s.now));  // fine once finished
}

static void test_sequence_number_wraparound() {
    HostConfig cfg;
    cfg.initial_seq = 0xFFFE;
    Sim s(cfg);
    s.net.to_host().push_fault(drop());  // include a retransmit across the wrap
    for (int i = 0; i < 5; ++i) CHECK(s.run(Command::Increment).outcome == Outcome::Success);
    CHECK(s.dev.counter() == 5);
}

static void test_clock_wraparound() {
    Sim s(HostConfig{}, 0xFFFFFFF0u);
    s.net.to_peripheral().push_fault(drop());
    TxResult r = s.run(Command::Increment);  // deadline computation crosses 2^32
    CHECK(r.outcome == Outcome::Success && r.attempts == 2 && s.dev.counter() == 1);
}

static void test_peripheral_unit_semantics() {
    Peripheral p;
    Frame rsp, req;
    req.type = FrameType::Request; req.cmd = Command::Increment;

    req.seq = 0xFFFF;
    CHECK(p.on_request(req, rsp) && rsp.value == 1);
    CHECK(p.on_request(req, rsp) && rsp.value == 1 && p.counter() == 1);  // duplicate
    req.seq = 0;                                                          // wrap: newer
    CHECK(p.on_request(req, rsp) && rsp.value == 2);
    req.seq = 0xFFFF;                                                     // old: ignored
    CHECK(!p.on_request(req, rsp) && p.counter() == 2);
    req.seq = 3; req.cmd = static_cast<Command>(99);                      // bad command
    CHECK(p.on_request(req, rsp) && rsp.status == Status::UnknownCommand);
    CHECK(p.on_request(req, rsp) && p.stats().executed == 3);             // error replay too
    Frame notreq; notreq.type = FrameType::Response;
    CHECK(!p.on_request(notreq, rsp));
}

static void test_device_error_is_reported() {
    Sim s;
    // Drive the host with a command the device does not know.
    s.host.begin(static_cast<Command>(99), s.now);
    while (!s.host.done()) s.step();
    CHECK(s.host.result().outcome == Outcome::DeviceError);
    CHECK(s.host.result().status == Status::UnknownCommand);
}

static void test_random_fault_soak() {
    uint32_t total_success = 0, total_uncertain = 0, total_timeout = 0, total_retx = 0;
    for (uint32_t seed = 1; seed <= 300; ++seed) {
        HostConfig cfg;
        cfg.initial_timeout_ms = 50;
        cfg.max_timeout_ms = 400;
        cfg.max_attempts = 4;
        Sim s(cfg);
        RandomFaults f;
        f.seed = seed * 2654435761u;
        f.loss_permille = 250;
        f.dup_permille = 200;
        f.min_delay_ms = 0;
        f.max_delay_ms = 120;  // can exceed timeouts => delayed/stale/reordered traffic
        s.net.to_peripheral().set_random(f);
        f.seed ^= 0xA5A5A5A5u;
        s.net.to_host().set_random(f);

        uint32_t rng = seed, success_inc = 0, uncertain_inc = 0, begun_inc = 0;
        for (int i = 0; i < 40; ++i) {
            rng = rng * 1664525u + 1013904223u;
            Command c = (rng >> 24) & 1 ? Command::Increment : Command::ReadCounter;
            if (c == Command::Increment) ++begun_inc;
            TxResult r = s.run(c);

            CHECK(r.elapsed_ms <= cfg.worst_case_duration_ms());  // bounded time
            CHECK(r.attempts >= 1 && r.attempts <= cfg.max_attempts);
            CHECK(s.dev.counter() <= begun_inc);                  // never over-executed
            if (r.outcome == Outcome::Success) {
                CHECK(r.value == s.dev.counter());                // response is truthful
                if (c == Command::Increment) ++success_inc;
                ++total_success;
            } else if (r.outcome == Outcome::Uncertain) {
                CHECK(c == Command::Increment);
                ++uncertain_inc; ++total_uncertain;
            } else {
                CHECK(r.outcome == Outcome::Timeout && c == Command::ReadCounter);
                ++total_timeout;
            }
            s.advance((rng >> 8) % 40);  // let stragglers arrive between transactions
        }
        s.advance(1000);
        CHECK(s.dev.counter() >= success_inc);                     // every confirmed INC happened
        CHECK(s.dev.counter() <= success_inc + uncertain_inc);     // only uncertain ones are unknown
        total_retx += s.host.stats().retransmissions;
    }
    // The soak must actually exercise the interesting paths.
    CHECK(total_success > 0 && total_uncertain > 0 && total_timeout > 0 && total_retx > 0);
    std::printf("    soak: success=%u uncertain=%u read-timeout=%u retransmissions=%u\n",
                total_success, total_uncertain, total_timeout, total_retx);
}

// ---------------------------------------------------------------------------
struct TestCase { const char* name; void (*fn)(); };
#define T(fn) {#fn, fn}

int main() {
    const TestCase tests[] = {
        T(test_success_clean_link),
        T(test_lost_request_is_retransmitted),
        T(test_lost_response_does_not_double_execute),
        T(test_duplicated_request_executes_once),
        T(test_duplicated_response_does_not_confuse_next_transaction),
        T(test_delayed_response_after_retransmit),
        T(test_late_original_request_is_ignored_as_stale),
        T(test_all_requests_lost_increment_is_uncertain),
        T(test_all_responses_lost_increment_is_uncertain_but_executed_once),
        T(test_read_exhaustion_reports_timeout_not_uncertain),
        T(test_response_after_giving_up_is_ignored_and_read_resolves_uncertainty),
        T(test_busy_rejects_second_begin),
        T(test_sequence_number_wraparound),
        T(test_clock_wraparound),
        T(test_peripheral_unit_semantics),
        T(test_device_error_is_reported),
        T(test_random_fault_soak),
    };
    int failed_tests = 0;
    for (const TestCase& t : tests) {
        int before = g_failures;
        t.fn();
        bool ok = g_failures == before;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", t.name);
        if (!ok) ++failed_tests;
    }
    std::printf("\n%zu tests, %d checks, %d failed\n",
                sizeof(tests) / sizeof(tests[0]), g_checks, failed_tests);
    return failed_tests ? 1 : 0;
}
