// Simulated unreliable duplex link: loss, duplication, delay (hence reordering).
// Fixed-size storage, no heap, deterministic (seeded PRNG + scripted faults).
#pragma once
#include <cstddef>
#include "rpp/protocol.hpp"

namespace rpp {

// Fault applied to ONE sent frame.
struct FaultAction {
    bool drop = false;
    uint8_t copies = 1;       // total deliveries (>1 => duplicates); capped at 4
    uint32_t delay_ms = 0;    // extra delay of first copy
    uint32_t dup_gap_ms = 0;  // each further copy is this much later than the previous
};

// Probabilistic faults, used when no scripted fault is queued.
struct RandomFaults {
    uint32_t seed = 1;
    uint16_t loss_permille = 0;
    uint16_t dup_permille = 0;
    uint32_t min_delay_ms = 0;
    uint32_t max_delay_ms = 0;
};

struct ChannelStats {
    uint32_t sent = 0, dropped = 0, duplicated = 0, delivered = 0, overflowed = 0;
};

// One direction of the link.
class Channel {
public:
    static constexpr size_t kCapacity = 32;       // frames in flight
    static constexpr size_t kScriptCapacity = 16; // queued scripted faults

    void send(const Frame& f, TimeMs now);
    bool receive(Frame& out, TimeMs now);

    // Scripted faults are consumed one per sent frame, FIFO, before random faults.
    bool push_fault(const FaultAction& a);
    void set_random(const RandomFaults& r);
    void set_base_latency(uint32_t ms) { base_latency_ = ms; }
    const ChannelStats& stats() const { return stats_; }

private:
    struct Entry {
        Frame frame;
        TimeMs deliver_at = 0;
        uint32_t order = 0;
        bool used = false;
    };
    FaultAction next_action();
    uint32_t rand32();
    uint32_t rand_range(uint32_t lo, uint32_t hi);
    void enqueue(const Frame& f, TimeMs at);

    Entry q_[kCapacity];
    FaultAction script_[kScriptCapacity];
    size_t script_head_ = 0, script_count_ = 0;
    RandomFaults rnd_;
    uint32_t rng_ = 1;
    uint32_t order_ = 0;
    uint32_t base_latency_ = 1;
    ChannelStats stats_;
};

// Endpoint view: sends on one channel, receives from the other.
class Port : public ITransport {
public:
    Port(Channel& tx, Channel& rx) : tx_(tx), rx_(rx) {}
    void send(const Frame& f, TimeMs now) override { tx_.send(f, now); }
    bool receive(Frame& out, TimeMs now) override { return rx_.receive(out, now); }
private:
    Channel& tx_;
    Channel& rx_;
};

class SimTransport {
public:
    SimTransport() : host_(to_peripheral_, to_host_), peripheral_(to_host_, to_peripheral_) {}
    SimTransport(const SimTransport&) = delete;
    SimTransport& operator=(const SimTransport&) = delete;

    ITransport& host_port() { return host_; }
    ITransport& peripheral_port() { return peripheral_; }
    Channel& to_peripheral() { return to_peripheral_; }  // requests
    Channel& to_host() { return to_host_; }              // responses

private:
    Channel to_peripheral_, to_host_;
    Port host_, peripheral_;
};

}  // namespace rpp
