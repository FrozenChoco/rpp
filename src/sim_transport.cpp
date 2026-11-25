#include "rpp/sim_transport.hpp"

namespace rpp {

void Channel::set_base_latency(uint32_t ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    base_latency_ = ms;
}

ChannelStats Channel::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

uint32_t Channel::rand32() {  // xorshift32
    uint32_t x = rng_;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_ = x;
}

uint32_t Channel::rand_range(uint32_t lo, uint32_t hi) {
    if (hi <= lo) return lo;
    return lo + rand32() % (hi - lo + 1);
}

void Channel::set_random(const RandomFaults& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    rnd_ = r;
    rng_ = r.seed ? r.seed : 1;
}

bool Channel::push_fault(const FaultAction& a) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (script_count_ == kScriptCapacity) return false;
    script_[(script_head_ + script_count_) % kScriptCapacity] = a;
    ++script_count_;
    return true;
}

FaultAction Channel::next_action() {
    if (script_count_ > 0) {
        FaultAction a = script_[script_head_];
        script_head_ = (script_head_ + 1) % kScriptCapacity;
        --script_count_;
        return a;
    }
    FaultAction a;
    if (rnd_.loss_permille && rand32() % 1000 < rnd_.loss_permille) {
        a.drop = true;
        return a;
    }
    if (rnd_.dup_permille && rand32() % 1000 < rnd_.dup_permille) a.copies = 2;
    if (rnd_.corrupt_permille && rand32() % 1000 < rnd_.corrupt_permille) a.corrupt = true;
    if (rnd_.max_delay_ms) {
        a.delay_ms = rand_range(rnd_.min_delay_ms, rnd_.max_delay_ms);
        a.dup_gap_ms = rand_range(0, rnd_.max_delay_ms);
    }
    return a;
}

void Channel::enqueue(const Frame& f, TimeMs at) {
    for (Entry& e : q_) {
        if (!e.used) {
            e.frame = f; e.deliver_at = at; e.order = order_++; e.used = true;
            return;
        }
    }
    ++stats_.overflowed;  // link congested: behaves like loss
}

void Channel::send(const Frame& f, TimeMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.sent;
    FaultAction a = next_action();
    if (a.drop) { ++stats_.dropped; return; }
    uint8_t copies = a.copies < 1 ? 1 : (a.copies > 4 ? 4 : a.copies);
    stats_.duplicated += copies - 1u;
    for (uint8_t i = 0; i < copies; ++i) {
        Frame delivered = f;
        if (i == 0 && a.corrupt && delivered.type == FrameType::Response && delivered.fec_present) {
            delivered.fec_a.value ^= 1u;
            ++stats_.corrupted;
        }
        enqueue(delivered, now + base_latency_ + a.delay_ms + i * a.dup_gap_ms);
    }
}

bool Channel::receive(Frame& out, TimeMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* best = nullptr;
    for (Entry& e : q_) {
        if (!e.used || !time_reached(now, e.deliver_at)) continue;
        if (!best) { best = &e; continue; }
        uint32_t age_e = now - e.deliver_at, age_b = now - best->deliver_at;
        if (age_e > age_b || (age_e == age_b && e.order < best->order)) best = &e;
    }
    if (!best) return false;
    out = best->frame;
    best->used = false;
    ++stats_.delivered;
    return true;
}

}  // namespace rpp
