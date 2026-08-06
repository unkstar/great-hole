#include "FecPipeline.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <utility>

#include <boost/asio/steady_timer.hpp>
#include <boost/log/trivial.hpp>

#include "AdaptiveOverhead.hpp"
#include "Asio.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentFiber.hpp"
#include "LossPattern.hpp"
#include "Packet.hpp"
#include "RemoteCall.hpp"

namespace gh {

using namespace fec_wire;

FecPipeline::FecPipeline(boost::asio::io_context& io, std::shared_ptr<EndpointInput> in,
                         const std::vector<std::shared_ptr<Filter>>& filters,
                         std::shared_ptr<EndpointOutput> out, FecConfig cfg, bool is_encoder,
                         std::shared_ptr<FecSharedState> shared)
    : Pipeline(io, in, filters, out), _Cfg(cfg), _IsEncoder(is_encoder), _Shared(std::move(shared)) {
    if (is_encoder) {
        _OverheadCtrl = AdaptiveOverhead::Create(cfg.algo, cfg.overhead, cfg.max_overhead,
                                                 cfg.safety_margin);
        BOOST_LOG_TRIVIAL(info) << "FecPipeline(" << this << ") adaptive overhead: "
                                << _OverheadCtrl->Name() << " algo=" << (int)cfg.algo;
    } else {
        if (cfg.test_drop_pattern > 0) {
            _LossPattern = LossPattern::Create(cfg.test_drop_pattern, cfg.test_drop_rate,
                                               cfg.test_drop_rate2, cfg.test_drop_burst);
            BOOST_LOG_TRIVIAL(info) << "FecPipeline(" << this << ") loss pattern: "
                                    << _LossPattern->Name() << " rate=" << cfg.test_drop_rate;
        }
    }
}

FecPipeline::~FecPipeline() = default;

uint8_t FecPipeline::BuildFlags() const {
    uint8_t flags = 0;
    if (_Cfg.iv_len > 0 && _Cfg.obfuscate) {
        flags |= (_Cfg.iv_len & 0x07) << 1;
    }
    return flags;
}

uint32_t FecPipeline::BuildDword(uint32_t group_seq, uint8_t flags) const {
    return (group_seq & 0xFFFFFF) | (static_cast<uint32_t>(flags) << 24);
}

Omni::Fiber::Coroutine<void> FecPipeline::Process() {
    auto& fiber = co_await Omni::Fiber::GetCurrentFiber();

    // Codec strategy: pure synchronous processor. The transport owns all
    // reads/writes and fiber scheduling.
    auto codec = FecCodec::Create(_Cfg, _IsEncoder, _Shared, _OverheadCtrl.get(), _LossPattern.get());

    // Reader fiber: keeps an async read pending on the input at all times so
    // the descriptor stays registered with the reactor (no edge-loss window
    // while the worker processes or writes). Packets go into a deque.
    auto q = std::make_shared<std::deque<Packet>>();
    auto reader_done = std::make_shared<bool>(false);
    fiber.Spawn("fec-reader", [this, q, reader_done]() -> Omni::Fiber::Coroutine<void> {
        while (!_Stop.IsTriggered()) {
            Packet p;
            auto err = co_await _In->Read(p, _Stop);
            if (err) {
                if (err == ErrorCode{AppErrorCategory::kEndOfStream, kAppError}) { *reader_done = true; break; }
                if (err == ErrorCode{AppErrorCategory::kOperationAborted, kAppError}) continue;
                if (IsCritical(err)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") read error: " << err.message(); throw SystemError(err, "FecPipeline read error"); }
                BOOST_LOG_TRIVIAL(warning) << "FecPipeline(" << this << ") read warning: " << err.message(); continue;
            }
            q->push_back(std::move(p));
            // drain the input into the queue. EPOLLET (edge-triggered)
            // invariant: the fd's buffer MUST be fully drained to EAGAIN or
            // the next edge never fires for already-pending data — the reader
            // then suspends while the device queue overflows and drops
            // packets (observed: tun RX drops at both ends during TCP tests).
            // The q grows unbounded anyway (the main loop paces consumption),
            // so a q-size cap here only breaks the drain.
            while (!_Stop.IsTriggered()) {
                Packet p2;
                if (_In->TryRead(p2)) break;  // EAGAIN: fd empty
                q->push_back(std::move(p2));
            }
        }
    });

    auto poll_timer = std::make_shared<boost::asio::steady_timer>(_Io);
    std::vector<Packet> out;  // codec output; capacity reused across calls
    out.reserve(64);

    while (!_Stop.IsTriggered()) {
        // PING/FEEDBACK — transport-level control, any time
        if (_Shared) {
            if (_Cfg.ping_interval_ms > 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _LastPingTime).count();
                if (_LastPingTime.time_since_epoch().count() == 0 || elapsed >= _Cfg.ping_interval_ms) {
                    _LastPingTime = now;
                    auto now_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
                    co_await SendPing(now_us);
                }
            }
            if (_Shared->pending_feedback_echo != 0) {
                uint64_t echo = _Shared->pending_feedback_echo;
                _Shared->pending_feedback_echo = 0;
                co_await SendFeedback(echo);
            }
        }

        // idle tick: codec time-based work (repair flush, watermark recovery)
        out.clear();
        codec->Tick(out);
        if (!out.empty()) {
            auto werr = co_await _Out->WriteBatch(out, _Stop);
            if (werr && IsCritical(werr)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") write error: " << werr.message(); throw SystemError(werr, "FecPipeline write error"); }
        }

        if (q->empty()) {
            if (*reader_done) break;
            poll_timer->expires_after(std::chrono::microseconds(100));
            co_await poll_timer->async_wait(Omni::Fiber::AsioUseFiber);
            continue;
        }
        Packet p = std::move(q->front());
        q->pop_front();
        out.clear();
        codec->OnPacket(std::move(p), out);
        if (!out.empty()) {
            auto werr = co_await _Out->WriteBatch(out, _Stop);
            if (werr && IsCritical(werr)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") write error: " << werr.message(); throw SystemError(werr, "FecPipeline write error"); }
        }
    }
    co_return;
}

Omni::Fiber::Coroutine<void> FecPipeline::SendPing(uint64_t timestamp_us) {
    Packet out; out._Length = 0;
    uint8_t flags = BuildFlags() | kPing;
    float fb_loss_ping = _Shared ? _Shared->latest_loss_rate : 0.0f;
    uint8_t fb_ping = static_cast<uint8_t>(std::min(fb_loss_ping * 250.0f, 250.0f));
    out.PushFrontLE(timestamp_us);
    out.PushFrontLE(fb_ping);
    out.PushFrontLE(BuildDword(++_GroupSeq, flags));
    auto err = co_await _Out->Write(out, _Stop);
    if (err && IsCritical(err)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") SendPing write error: " << err.message(); throw SystemError(err, "FecPipeline SendPing write error"); }
    co_return;
}

Omni::Fiber::Coroutine<void> FecPipeline::SendFeedback(uint64_t echo_us) {
    Packet out; out._Length = 0;
    uint8_t flags = BuildFlags() | kFeedback | kEcho;
    float fb_loss_fb = _Shared ? _Shared->latest_loss_rate : 0.0f;
    uint8_t fb_fb = static_cast<uint8_t>(std::min(fb_loss_fb * 250.0f, 250.0f));
    out.PushFrontLE(echo_us);
    out.PushFrontLE(fb_fb);
    out.PushFrontLE(BuildDword(++_GroupSeq, flags));
    auto err = co_await _Out->Write(out, _Stop);
    if (err && IsCritical(err)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") SendFeedback write error: " << err.message(); throw SystemError(err, "FecPipeline SendFeedback write error"); }
    co_return;
}

} // namespace gh
