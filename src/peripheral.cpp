#include "rpp/peripheral.hpp"

namespace rpp {

Frame Peripheral::execute(const Frame& req) {
    Frame rsp;
    rsp.type = FrameType::Response;
    rsp.cmd = req.cmd;
    rsp.seq = req.seq;
    switch (req.cmd) {
        case Command::Increment: ++counter_; rsp.status = Status::Ok; break;
        case Command::ReadCounter: rsp.status = Status::Ok; break;
        default: rsp.status = Status::UnknownCommand; break;
    }
    rsp.value = counter_;
    ++stats_.executed;
    return rsp;
}

bool Peripheral::on_request(const Frame& req, Frame& rsp) {
    if (req.type != FrameType::Request) return false;

    if (has_last_ && req.seq == last_seq_) {
        // Retransmission or network duplicate: replay, never re-execute.
        rsp = last_rsp_;
        ++stats_.duplicates_served;
        return true;
    }
    if (has_last_ && !seq_newer(req.seq, last_seq_)) {
        // Delayed/reordered request from an earlier transaction. The host has
        // moved on, so executing it would be wrong and replying is pointless.
        ++stats_.stale_ignored;
        return false;
    }
    last_rsp_ = execute(req);
    last_seq_ = req.seq;
    has_last_ = true;
    rsp = last_rsp_;
    return true;
}

void Peripheral::service(ITransport& port, TimeMs now) {
    Frame req, rsp;
    while (port.receive(req, now))
        if (on_request(req, rsp)) port.send(rsp, now);
}

}  // namespace rpp
