// Peripheral-side command processor with at-most-once execution.
#pragma once
#include "rpp/protocol.hpp"
#include <mutex>

namespace rpp {

struct PeripheralStats {
    uint32_t executed = 0;           // commands actually executed
    uint32_t duplicates_served = 0;  // retransmits answered from the cached response
    uint32_t stale_ignored = 0;      // requests older than the last executed one
};

class Peripheral {
public:
    // Core logic (transport-free, unit-testable). Returns true if `rsp` must be sent.
    bool on_request(const Frame& req, Frame& rsp);

    // Drain all requests deliverable at `now` and send the responses.
    void service(ITransport& port, TimeMs now);

    uint32_t counter() const;
    PeripheralStats stats() const;

private:
    Frame execute(const Frame& req);

    uint32_t counter_ = 0;
    bool has_last_ = false;
    Seq last_seq_ = 0;
    Frame last_rsp_;  // response to last_seq_, replayed for duplicates
    PeripheralStats stats_;
    mutable std::mutex mutex_;
};

}  // namespace rpp
