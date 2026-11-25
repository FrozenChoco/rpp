#include "rpp/host.hpp"

namespace rpp {

namespace {
uint32_t next_timeout(uint32_t t, uint32_t cap) { return t > cap / 2 ? cap : t * 2; }
}

uint32_t HostConfig::worst_case_duration_ms() const {
    uint32_t total = 0, t = initial_timeout_ms;
    uint8_t n = max_attempts ? max_attempts : 1;
    for (uint8_t i = 0; i < n; ++i) {
        total += t;
        t = next_timeout(t, max_timeout_ms);
    }
    return total;
}

Host::Host(ITransport& port, const HostConfig& cfg)
    : port_(port), cfg_(cfg), next_seq_(cfg.initial_seq) {
    if (cfg_.max_attempts == 0) cfg_.max_attempts = 1;
    if (cfg_.max_timeout_ms < cfg_.initial_timeout_ms) cfg_.max_timeout_ms = cfg_.initial_timeout_ms;
}

bool Host::begin(Command cmd, TimeMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Pending) return false;
    cur_cmd_ = cmd;
    cur_seq_ = next_seq_++;
    result_ = TxResult{};
    timeout_ = cfg_.initial_timeout_ms;
    started_ = now;
    state_ = State::Pending;
    ++stats_.transactions;
    transmit(now);
    return true;
}

void Host::transmit(TimeMs now) {
    Frame f;
    f.type = FrameType::Request;
    f.cmd = cur_cmd_;
    f.seq = cur_seq_;  // retransmissions reuse the seq: that is what makes them safe
    port_.send(f, now);
    ++result_.attempts;
    deadline_ = now + timeout_;
}

void Host::finish(Outcome o, TimeMs now) {
    result_.outcome = o;
    result_.elapsed_ms = now - started_;
    state_ = State::Done;
}

bool Host::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::Pending;
}

bool Host::done() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::Done;
}

TxResult Host::result() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

HostStats Host::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void Host::poll(TimeMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 1. Responses first, so one arriving exactly at the deadline still counts.
    Frame f;
    while (port_.receive(f, now)) {
        bool match = state_ == State::Pending && f.type == FrameType::Response &&
                     f.seq == cur_seq_ && f.cmd == cur_cmd_;
        if (!match) { ++stats_.ignored_frames; continue; }  // stale, duplicate, or idle
        result_.status = f.status;
        result_.value = f.value;
        finish(f.status == Status::Ok ? Outcome::Success : Outcome::DeviceError, now);
    }
    // 2. Timer.
    if (state_ == State::Pending && time_reached(now, deadline_)) {
        if (result_.attempts < cfg_.max_attempts) {
            timeout_ = next_timeout(timeout_, cfg_.max_timeout_ms);
            ++stats_.retransmissions;
            transmit(now);
        } else {
            finish(is_idempotent(cur_cmd_) ? Outcome::Timeout : Outcome::Uncertain, now);
        }
    }
}

}  // namespace rpp
