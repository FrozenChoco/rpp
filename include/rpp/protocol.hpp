// Wire-level types shared by host, peripheral and transport.
#pragma once
#include <cstdint>

namespace rpp {

using TimeMs = uint32_t;  // monotonic millisecond clock; wraps after ~49 days
using Seq = uint16_t;     // transaction sequence number; wraps

enum class FrameType : uint8_t { Request = 1, Response = 2 };
enum class Command : uint8_t { Increment = 1, ReadCounter = 2 };
enum class Status : uint8_t { Ok = 0, UnknownCommand = 1 };

// Logical frame. The transport carries frames by value; there is no byte-level
// encoding or CRC (see docs/DESIGN.md, "Known limitations").
struct Frame {
    FrameType type = FrameType::Request;
    Command cmd = Command::ReadCounter;
    Seq seq = 0;
    Status status = Status::Ok;  // responses only
    uint32_t value = 0;          // responses only: counter value
};

// True if `a` is strictly newer than `b` in modular (wrap-safe) order.
// Valid while the two are less than 32768 apart, which holds because the host
// runs one transaction at a time and advances seq by 1 per transaction.
inline bool seq_newer(Seq a, Seq b) {
    return static_cast<int16_t>(static_cast<Seq>(a - b)) > 0;
}

// True if `now` has reached or passed `t` (wrap-safe for spans < ~24 days).
inline bool time_reached(TimeMs now, TimeMs t) {
    return static_cast<int32_t>(now - t) >= 0;
}

// Interface one endpoint uses to talk to the (unreliable) link.
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual void send(const Frame& f, TimeMs now) = 0;
    // Returns true and fills `out` if a frame is deliverable at `now`.
    virtual bool receive(Frame& out, TimeMs now) = 0;
};

}  // namespace rpp
