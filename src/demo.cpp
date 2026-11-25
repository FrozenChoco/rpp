// Small walk-through of the interesting scenarios, with a transaction summary each.
#include <cstdio>
#include "rpp/host.hpp"
#include "rpp/peripheral.hpp"
#include "rpp/sim_transport.hpp"

using namespace rpp;

static const char* name(Outcome o) {
    switch (o) {
        case Outcome::Success: return "SUCCESS";
        case Outcome::DeviceError: return "DEVICE_ERROR";
        case Outcome::Timeout: return "TIMEOUT (no side effect)";
        case Outcome::Uncertain: return "UNCERTAIN (may or may not have executed)";
        default: return "NONE";
    }
}

struct Demo {
    SimTransport net;
    Peripheral dev;
    Host host{net.host_port(), HostConfig{}};
    TimeMs now = 0;

    void run(const char* title, Command c) {
        host.begin(c, now);
        while (!host.done()) { ++now; dev.service(net.peripheral_port(), now); host.poll(now); }
        const TxResult& r = host.result();
        std::printf("%-34s -> %-42s attempts=%u elapsed=%ums value=%u | device counter=%u\n",
                    title, name(r.outcome), r.attempts, r.elapsed_ms, r.value, dev.counter());
    }
};

int main() {
    Demo d;
    FaultAction drop; drop.drop = true;

    d.run("clean increment", Command::Increment);

    d.net.to_peripheral().push_fault(drop);
    d.run("request lost once", Command::Increment);

    d.net.to_host().push_fault(drop);
    d.run("response lost once (no re-exec)", Command::Increment);

    for (int i = 0; i < 4; ++i) d.net.to_host().push_fault(drop);
    d.run("all responses lost", Command::Increment);

    d.run("read resolves the uncertainty", Command::ReadCounter);
    return 0;
}
