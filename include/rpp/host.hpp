// Host-side transaction manager: one outstanding transaction, bounded retries.
// Non-blocking: call begin(), then poll() periodically until done().
#pragma once
#include "rpp/protocol.hpp"

namespace rpp {

struct HostConfig {
    uint32_t initial_timeout_ms = 100;  // wait after first transmission
    uint32_t max_timeout_ms = 800;      // backoff cap (timeout doubles per retry)
    uint8_t max_attempts = 4;           // total transmissions (1 + retries), min 1
    Seq initial_seq = 1;

    // Upper bound on the duration of any transaction.
    uint32_t worst_case_duration_ms() const;
};

enum class Outcome : uint8_t {
    None,         // no transaction finished yet
    Success,      // response received, device status Ok
    DeviceError,  // response received, device reported an error status
    Timeout,      // retries exhausted, command is idempotent: nothing to worry
                  // about, safe to simply retry
    Uncertain,    // retries exhausted, command is NOT idempotent: it may or may
                  // not have been executed
};

struct TxResult {
    Outcome outcome = Outcome::None;
    Status status = Status::Ok;
    uint32_t value = 0;       // counter value from the response (Success/DeviceError)
    uint8_t attempts = 0;     // transmissions made
    uint32_t elapsed_ms = 0;  // begin() -> completion
};

struct HostStats {
    uint32_t transactions = 0;
    uint32_t retransmissions = 0;
    uint32_t ignored_frames = 0;  // stale / duplicate / unexpected responses
};

class Host {
public:
    Host(ITransport& port, const HostConfig& cfg);

    // Start a transaction. Returns false if one is already pending.
    bool begin(Command cmd, TimeMs now);
    // Process received frames and timers. Call at least once per millisecond tick.
    void poll(TimeMs now);

    bool pending() const { return state_ == State::Pending; }
    bool done() const { return state_ == State::Done; }
    const TxResult& result() const { return result_; }
    const HostStats& stats() const { return stats_; }

    static bool is_idempotent(Command c) { return c == Command::ReadCounter; }

private:
    enum class State : uint8_t { Idle, Pending, Done };
    void transmit(TimeMs now);
    void finish(Outcome o, TimeMs now);

    ITransport& port_;
    HostConfig cfg_;
    State state_ = State::Idle;
    Seq next_seq_;
    Seq cur_seq_ = 0;
    Command cur_cmd_ = Command::ReadCounter;
    uint32_t timeout_ = 0;
    TimeMs started_ = 0, deadline_ = 0;
    TxResult result_;
    HostStats stats_;
};

}  // namespace rpp
