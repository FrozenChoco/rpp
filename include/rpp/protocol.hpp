// Wire-level types shared by host, peripheral and transport.
#pragma once
#include <cstdint>

namespace rpp {

using TimeMs = uint32_t;  // monotonic millisecond clock; wraps after ~49 days
using Seq = uint16_t;     // transaction sequence number; wraps

enum class FrameType : uint8_t { Request = 1, Response = 2 };
enum class Command : uint8_t { Increment = 1, ReadCounter = 2 };
enum class Status : uint8_t { Ok = 0, UnknownCommand = 1 };

// FEC data types
struct FecBlock {
    FrameType type = FrameType::Response;
    Command cmd = Command::ReadCounter;
    Seq seq = 0;
    Status status = Status::Ok;
    uint32_t value = 0;
};

// Logical frame. Peripheral responses use three identical copies.
struct Frame {
    FrameType type = FrameType::Request;
    Command cmd = Command::ReadCounter;
    Seq seq = 0;
    Status status = Status::Ok;
    uint32_t value = 0;
    bool fec_present = false;
    FecBlock fec_a;
    FecBlock fec_b;
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

// Populate the three response copies at the peripheral.
inline void encode_response_fec(Frame& f) {
    f.fec_present = f.type == FrameType::Response;
    if (!f.fec_present) return;
    FecBlock b;
    b.type = f.type; b.cmd = f.cmd; b.seq = f.seq;
    b.status = f.status; b.value = f.value;
    f.fec_a = b;
    f.fec_b = b;
}

inline bool fec_blocks_equal(const FecBlock& a, const FecBlock& b) {
    return a.type == b.type && a.cmd == b.cmd && a.seq == b.seq &&
           a.status == b.status && a.value == b.value;
}

// Correct a response if one of the three copies is damaged. If no two copies
// agree, the frame is rejected rather than guessed.
inline bool decode_response_fec(Frame& f) {
    if (!f.fec_present || f.type != FrameType::Response) return false;
    FecBlock primary;
    primary.type = f.type; primary.cmd = f.cmd; primary.seq = f.seq;
    primary.status = f.status; primary.value = f.value;

    if (fec_blocks_equal(primary, f.fec_a)) {
        f.status = primary.status; f.value = primary.value;
        f.cmd = primary.cmd; f.seq = primary.seq; f.type = primary.type;
        return true;
    }
    if (fec_blocks_equal(primary, f.fec_b)) {
        f.type = f.fec_b.type; f.cmd = f.fec_b.cmd; f.seq = f.fec_b.seq;
        f.status = f.fec_b.status; f.value = f.fec_b.value;
        return true;
    }
    if (fec_blocks_equal(f.fec_a, f.fec_b)) {
        f.type = f.fec_a.type; f.cmd = f.fec_a.cmd; f.seq = f.fec_a.seq;
        f.status = f.fec_a.status; f.value = f.fec_a.value;
        return true;
    }
    return false;
}

}  // namespace rpp
