#include "FecPipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <stdexcept>

#include <boost/log/trivial.hpp>

#include "AdaptiveOverhead.hpp"
#include "Cancel.hpp"
#include "Asio.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentFiber.hpp"
#include "LossPattern.hpp"
#include "Packet.hpp"
#include "RaptorQ.hpp"

namespace gh {

FecPipeline::~FecPipeline() = default;

FecPipeline::FecPipeline(boost::asio::io_context& io, std::shared_ptr<EndpointInput> in,
                         const std::vector<std::shared_ptr<Filter>>& filters, std::shared_ptr<EndpointOutput> out,
                         FecConfig cfg, bool is_encoder,
                         std::shared_ptr<FecSharedState> shared)
    : Pipeline(io, in, filters, out), _Cfg(cfg), _IsEncoder(is_encoder), _Shared(std::move(shared)) {
    if (is_encoder) {
        _OverheadCtrl = AdaptiveOverhead::Create(cfg.algo, cfg.overhead, cfg.max_overhead,
                                                   cfg.safety_margin);
        BOOST_LOG_TRIVIAL(info) << "FecPipeline(" << this << ") adaptive overhead: "
                               << _OverheadCtrl->Name() << " algo=" << (int)cfg.algo;
    } else {
        uint32_t window = static_cast<uint32_t>(std::bit_ceil(cfg.decode_window));
        _RingBuffer.resize(window);
        _RingMask = window - 1;
        if (cfg.test_drop_pattern > 0) {
            _LossPattern = LossPattern::Create(cfg.test_drop_pattern, cfg.test_drop_rate,
                                                cfg.test_drop_rate2, cfg.test_drop_burst);
            BOOST_LOG_TRIVIAL(info) << "FecPipeline(" << this << ") loss pattern: "
                                   << _LossPattern->Name() << " rate=" << cfg.test_drop_rate;
        }
    }
}

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

    if (_IsEncoder) {
        // Two-fiber: reader pushes to batch_queue, main drains with deadline.
        // 100us poll when queue empty during batch accumulation.
        // PING/FEEDBACK anytime — UDP Write overhead is microseconds, not RTT.
        auto batch_queue = std::make_shared<std::vector<Packet>>();
        auto reader_done = std::make_shared<bool>(false);

        fiber.Spawn("fec-reader", [this, batch_queue, reader_done]() -> Omni::Fiber::Coroutine<void> {
            while (!_Stop.IsTriggered()) {
                Packet p;
                auto err = co_await _In->Read(p, _Stop);
                if (err) {
                    if (err == ErrorCode{AppErrorCategory::kEndOfStream, kAppError}) { *reader_done = true; break; }
                    if (err == ErrorCode{AppErrorCategory::kOperationAborted, kAppError}) continue;
                    if (IsCritical(err)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline read error: " << err.message(); throw SystemError(err, "FecPipeline read error"); }
                    BOOST_LOG_TRIVIAL(warning) << "FecPipeline read warning: " << err.message(); continue;
                }
                if (_Filters.empty()) {
                    batch_queue->push_back(std::move(p));
                    while (batch_queue->size() < _Cfg.max_batch && !_Stop.IsTriggered()) {
                        Packet p2; if (_In->TryRead(p2)) break;
                        batch_queue->push_back(std::move(p2));
                    }
                } else {
                    for (auto& f : _Filters) { auto fe = co_await f->Pipe(p, _Stop); if (fe) { if (fe == ErrorCode{AppErrorCategory::kOperationAborted, kAppError}) break; if (IsCritical(fe)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline filter error: " << fe.message(); throw SystemError(fe, "FecPipeline filter error"); } break; } }
                    batch_queue->push_back(std::move(p));
                }
            }
        });

        auto poll_timer = std::make_shared<boost::asio::steady_timer>(_Io);
        auto timeout = std::chrono::milliseconds(_Cfg.timeout_ms);
        std::vector<Packet> batch;
        batch.reserve(_Cfg.max_batch);
        std::chrono::steady_clock::time_point batch_start;
        bool have_batch = false;

        while (!_Stop.IsTriggered()) {
            // PING/FEEDBACK — any time (overhead is microseconds, not RTT)
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

            if (have_batch) {
                while (!batch_queue->empty() && batch.size() < _Cfg.max_batch) {
                    batch.push_back(std::move(batch_queue->front()));
                    batch_queue->erase(batch_queue->begin());
                }
                bool full = batch.size() >= _Cfg.max_batch;
                bool timed_out = (std::chrono::steady_clock::now() - batch_start) >= timeout;
                if (full || timed_out) {
                    if (_OverheadCtrl && _Shared) _OverheadCtrl->Update(_Shared->latest_loss_rate);
                    static int bl = 0; ++bl;
                    if (bl <= 5 || bl % 50 == 1)
                        BOOST_LOG_TRIVIAL(info) << "FecPipeline(" << this << ") ENC batch=" << batch.size()
                            << " oh=" << (_OverheadCtrl ? _OverheadCtrl->GetOverhead() : _Cfg.overhead)
                            << " loss=" << (_Shared ? _Shared->latest_loss_rate : 0.0f);
                    co_await SendBatch(batch);
                    batch.clear(); have_batch = false;
                } else if (batch_queue->empty()) {
                    // Batch open, queue empty — brief yield to reader
                    poll_timer->expires_after(std::chrono::microseconds(100));
                    co_await poll_timer->async_wait(Omni::Fiber::AsioUseFiber);
                }
                continue;
            }

            if (batch_queue->empty()) {
                if (*reader_done) break;
                poll_timer->expires_after(std::chrono::microseconds(100));
                co_await poll_timer->async_wait(Omni::Fiber::AsioUseFiber);
                continue;
            }
            batch_start = std::chrono::steady_clock::now();
            have_batch = true;
        }
        if (!batch.empty()) co_await SendBatch(batch);
    } else {
        // ============ DECODE PATH ============
        while (!_Stop.IsTriggered()) {
            Packet p;
            auto err = co_await _In->Read(p, _Stop);
            if (err) {
                if (err == ErrorCode{AppErrorCategory::kOperationAborted, kAppError}) continue;
                if (err == ErrorCode{AppErrorCategory::kEndOfStream, kAppError}) break;
                if (IsCritical(err)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") decode read error: " << err.message(); throw SystemError(err, "FecPipeline decode read error"); }
                BOOST_LOG_TRIVIAL(warning) << "FecPipeline(" << this << ") decode read warning: " << err.message(); continue;
            }
            if (p.DataSize() < 5) continue;
            uint32_t dw = p.PopFrontLE<uint32_t>();
            uint32_t group_seq = dw & 0xFFFFFF;
            uint8_t f = (dw >> 24) & 0xFF;
            bool is_control = (f & kPing) || (f & kFeedback);
            if (!is_control && _LossPattern) {
                auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - _StartTime).count();
                if (_LossPattern->ShouldDrop(_TotalPackets, elapsed)) { _TotalPackets++; continue; }
            }
            _TotalPackets++;
            uint8_t fb = p.PopFrontLE<uint8_t>();
            if (_Shared && fb <= 250) { _Shared->latest_loss_rate = static_cast<float>(fb) / 250.0f; }
            uint64_t echo = 0;
            bool has_echo = (f & kEcho) != 0;
            if (has_echo) { if (p.DataSize() < 8) continue; echo = p.PopFrontLE<uint64_t>(); }
            bool is_ping = (f & kPing) != 0;
            bool is_feedback = (f & kFeedback) != 0;
            bool is_repeat = (f & kRepeat) != 0;
            if (is_ping) {
                if (p.DataSize() >= 8 && _Shared) { _Shared->pending_feedback_echo = p.PopFrontLE<uint64_t>(); }
                continue;
            }
            if (is_feedback) {
                if (has_echo && echo > 0 && _Shared) {
                    auto now_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                    uint64_t rtt_us = (now_us > echo) ? (now_us - echo) : 0;
                    if (rtt_us > 0) { _Shared->rtt_ewma_us = (_Shared->rtt_ewma_us == 0) ? rtt_us : (_Shared->rtt_ewma_us * 7 + rtt_us) / 8; }
                }
                continue;
            }
            if (is_repeat) {
                bool seen = false;
                for (size_t ri = 0; ri < _RingBuffer.size(); ri++) { if (_RingBuffer[ri].group_seq == group_seq) { seen = true; break; } }
                if (!seen) {
                    auto& rslot = FindSlot(group_seq); rslot.group_seq = group_seq;
                    uint8_t iv_len = (f >> 1) & 0x07;
                    if (iv_len > 0 && _Cfg.obfuscate) {
                        if (p.DataSize() < iv_len) continue;
                        auto iv_span = p.PopFront(iv_len);
                        for (size_t i = 0; i < p.DataSize(); i++) { p._Data[p._Offset + i] ^= iv_span[i % iv_len]; }
                    }
                    auto err_write = co_await _Out->Write(p, _Stop);
                    if (err_write && IsCritical(err_write)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") write error: " << err_write.message(); throw SystemError(err_write, "FecPipeline write error"); }
                }
                continue;
            }
            // FEC data shard
            uint8_t width = (f & kWidth2B) ? 2 : 1;
            uint32_t shard_index, source_count;
            if (width == 1) {
                if (p.DataSize() < 2) continue;
                shard_index = p.PopFrontLE<uint8_t>();
                source_count = p.PopFrontLE<uint8_t>();
            } else {
                if (p.DataSize() < 4) continue;
                shard_index = p.PopFrontLE<uint16_t>();
                source_count = p.PopFrontLE<uint16_t>();
            }
            uint8_t iv_len = (f >> 1) & 0x07;
            if (iv_len > 0 && _Cfg.obfuscate) {
                if (p.DataSize() < iv_len) continue;
                auto iv_span = p.PopFront(iv_len);
                for (size_t i = 0; i < p.DataSize(); i++) { p._Data[p._Offset + i] ^= iv_span[i % iv_len]; }
            }
            auto& slot = FindSlot(group_seq);
            if (slot.group_seq == 0) { slot.group_seq = group_seq; slot.source_count = source_count; slot.first_time = std::chrono::steady_clock::now(); }
            if (slot.source_count != source_count) { slot = RingSlot{}; slot.group_seq = group_seq; slot.source_count = source_count; slot.first_time = std::chrono::steady_clock::now(); }
            RingSlot::ShardEntry entry;
            entry.data.assign(p._Data.data() + p._Offset, p._Data.data() + p._Offset + p._Length);
            entry.esi = shard_index;
            slot.shards.push_back(std::move(entry));
            slot.symbol_count = static_cast<uint32_t>(slot.shards.size());
            if (shard_index >= slot.max_esi) slot.max_esi = shard_index;
            if (slot.symbol_count >= slot.source_count) {
                uint32_t symbol_size = _Cfg.symbol_size;
                if (symbol_size == 0) symbol_size = _Cfg.mtu - 28 - 20;
                uint64_t F = static_cast<uint64_t>(source_count) * symbol_size;
                RaptorQ rq(F, static_cast<uint16_t>(symbol_size));
                for (auto& shard : slot.shards) {
                    if (shard.data.size() < symbol_size) shard.data.resize(symbol_size, 0);
                    rq.SubmitSymbol(shard.data.data(), symbol_size, shard.esi);
                }
                _LossGroupCount++;
                std::vector<uint8_t> decoded(F);
                if (rq.TryDecode(decoded.data(), F)) {
                    uint32_t window = _Cfg.loss_window_groups > 0 ? _Cfg.loss_window_groups : 50;
                    if (_LossGroupCount >= window && _Shared) {
                        float fail_rate = static_cast<float>(_LossFailCount) / static_cast<float>(_LossGroupCount);
                        float alpha = _Cfg.loss_alpha > 0 ? _Cfg.loss_alpha : 0.1f;
                        _Shared->latest_loss_rate = alpha * fail_rate + (1.0f - alpha) * _Shared->latest_loss_rate;
                        _LossGroupCount = 0; _LossFailCount = 0;
                    }
                    BOOST_LOG_TRIVIAL(info) << "FecPipeline(" << this << ") decoded group " << group_seq << " with " << slot.symbol_count << "/" << (slot.max_esi + 1) << " symbols";
                    size_t pos = 0;
                    if (pos + 4 > decoded.size()) { slot = RingSlot{}; continue; }
                    uint32_t pkt_count = static_cast<uint32_t>(decoded[pos]) | (static_cast<uint32_t>(decoded[pos + 1]) << 8) | (static_cast<uint32_t>(decoded[pos + 2]) << 16) | (static_cast<uint32_t>(decoded[pos + 3]) << 24);
                    pos += 4;
                    for (uint32_t i = 0; i < pkt_count && pos + 2 <= decoded.size(); i++) {
                        uint16_t pkt_len = static_cast<uint16_t>(decoded[pos]) | (static_cast<uint16_t>(decoded[pos + 1]) << 8);
                        pos += 2;
                        if (pos + pkt_len > decoded.size()) break;
                        Packet out; out._Length = 0;
                        out.PushBack(std::span<const uint8_t>(decoded.data() + pos, pkt_len));
                        pos += pkt_len;
                        auto err_write = co_await _Out->Write(out, _Stop);
                        if (err_write && IsCritical(err_write)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") decoded write error: " << err_write.message(); throw SystemError(err_write, "FecPipeline decoded write error"); }
                    }
                } else {
                    _LossFailCount++;
                    BOOST_LOG_TRIVIAL(warning) << "FecPipeline(" << this << ") decode failed for group " << group_seq << " with " << slot.symbol_count << " symbols";
                }
                slot = RingSlot{};
            }
        }
    }
    co_return;
}

Omni::Fiber::Coroutine<void> FecPipeline::SendBatch(std::vector<Packet>& batch) {
    assert(!batch.empty());
    uint32_t pkt_count = static_cast<uint32_t>(batch.size());
    uint32_t group_seq = ++_GroupSeq;
    uint32_t flags = BuildFlags();
    uint32_t out_symbols = 0; // set in each path for stats
    float adaptive_ratio = _Cfg.repeat_ratio;
    if (_OverheadCtrl) {
        float oh = _OverheadCtrl->GetOverhead();
        float frac = (oh - _Cfg.safety_margin) / (_Cfg.max_overhead - _Cfg.safety_margin);
        frac = std::clamp(frac, 0.0f, 1.0f);
        adaptive_ratio = _Cfg.repeat_ratio_min + (_Cfg.repeat_ratio_max - _Cfg.repeat_ratio_min) * frac;
    }

    if (pkt_count == 1) {
        uint32_t copies = static_cast<uint32_t>(std::ceil(adaptive_ratio)) + 1;
        out_symbols = copies;
        auto& pkt = batch[0];
        std::vector<uint8_t> iv;
        if (flags & 0x0E) { iv.resize(_Cfg.iv_len); for (auto& b : iv) b = static_cast<uint8_t>(std::rand() & 0xFF); }
        float fb_loss = _Shared ? _Shared->latest_loss_rate : 0.0f;
        uint8_t fb = static_cast<uint8_t>(std::min(fb_loss * 250.0f, 250.0f));
        uint32_t repeat_dw = (group_seq & 0xFFFFFF) | static_cast<uint32_t>(flags | kRepeat) << 24;
        if (copies == 1) {
            // Fast path: single copy — write directly, no out_batch vector
            Packet out; out._Length = 0;
            out.PushBack(pkt.Data());
            if (!iv.empty()) { for (size_t j = 0; j < out.DataSize(); j++) out._Data[out._Offset + j] ^= iv[j % iv.size()]; out.PushFront(std::span<const uint8_t>(iv)); }
            out.PushFrontLE(fb);
            out.PushFrontLE(repeat_dw);
            auto err = co_await _Out->Write(out, _Stop);
            if (err && IsCritical(err)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") REPEAT write error: " << err.message(); throw SystemError(err, "FecPipeline REPEAT write error"); }
        } else {
            std::vector<Packet> out_batch;
            for (uint32_t i = 0; i < copies && !_Stop.IsTriggered(); i++) {
                Packet out; out._Length = 0;
                out.PushBack(pkt.Data());
                if (!iv.empty()) { for (size_t j = 0; j < out.DataSize(); j++) out._Data[out._Offset + j] ^= iv[j % iv.size()]; out.PushFront(std::span<const uint8_t>(iv)); }
                out.PushFrontLE(fb);
                out.PushFrontLE(repeat_dw);
                out_batch.push_back(std::move(out));
            }
            if (!out_batch.empty()) { auto err = co_await _Out->WriteBatch(out_batch, _Stop); if (err && IsCritical(err)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") REPEAT write error: " << err.message(); throw SystemError(err, "FecPipeline REPEAT write error"); } }
        }
    } else {
        std::vector<uint8_t> blob;
        BuildBlob(batch, blob);
        uint32_t symbol_size = _Cfg.symbol_size;
        if (symbol_size == 0) { symbol_size = _Cfg.mtu - 28 - 20; if (symbol_size < 16) symbol_size = 16; }
        uint64_t F = blob.size();
        if (F % symbol_size != 0) { size_t pad = symbol_size - (F % symbol_size); blob.insert(blob.end(), pad, 0); F = blob.size(); }
        uint16_t T = static_cast<uint16_t>(symbol_size);
        RaptorQ rq(F, T); rq.Encode(blob.data(), blob.size());
        uint32_t K = rq.K();
        float current_overhead = _OverheadCtrl ? _OverheadCtrl->GetOverhead() : _Cfg.overhead;
        uint32_t extra = static_cast<uint32_t>(std::ceil(K * current_overhead));
        uint32_t total_symbols = K + extra;
        out_symbols = total_symbols;
        if (total_symbols > 65535) total_symbols = 65535;
        bool use_2b = (total_symbols > 255) || (K > 255);
        uint8_t final_flags = static_cast<uint8_t>(flags);
        if (use_2b) final_flags |= kWidth2B;
        std::vector<uint8_t> iv;
        if (final_flags & 0x0E) { iv.resize(_Cfg.iv_len); for (auto& b : iv) b = static_cast<uint8_t>(std::rand() & 0xFF); }
        uint32_t final_dw = BuildDword(group_seq, final_flags);
        std::vector<Packet> out_batch;
        auto sym_buf = std::make_unique<uint8_t[]>(T);
        for (uint32_t esi = 0; esi < total_symbols && !_Stop.IsTriggered(); esi++) {
            if (!rq.GenerateSymbol(esi, sym_buf.get())) throw std::runtime_error("RaptorQ: GenerateSymbol failed for ESI " + std::to_string(esi));
            Packet out; out._Length = 0;
            out.PushBack(std::span<const uint8_t>(sym_buf.get(), T));
            if (!iv.empty()) { for (size_t j = 0; j < out.DataSize(); j++) out._Data[out._Offset + j] ^= iv[j % iv.size()]; out.PushFront(std::span<const uint8_t>(iv)); }
            if (use_2b) { out.PushFrontLE(static_cast<uint16_t>(K)); out.PushFrontLE(static_cast<uint16_t>(esi)); }
            else { out.PushFrontLE(static_cast<uint8_t>(K)); out.PushFrontLE(static_cast<uint8_t>(esi)); }
            float fb_loss2 = _Shared ? _Shared->latest_loss_rate : 0.0f;
            uint8_t fb2 = static_cast<uint8_t>(std::min(fb_loss2 * 250.0f, 250.0f));
            out.PushFrontLE(fb2);
            out.PushFrontLE(final_dw);
            out_batch.push_back(std::move(out));
        }
        if (!out_batch.empty()) { auto err = co_await _Out->WriteBatch(out_batch, _Stop); if (err && IsCritical(err)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") FEC write error: " << err.message(); throw SystemError(err, "FecPipeline FEC write error"); } }
    }
    // FEC overhead stats (100-batch sliding window). Set LOG_N>0 to enable.
    { static constexpr int W=100, LOG_N=0; static uint64_t wi=0,wo=0,ti=0,to=0,wn=0,ln=0;
      wi+=pkt_count; wo+=out_symbols; ti+=pkt_count; to+=out_symbols;
      if (++wn>=W) { if (LOG_N>0 && ++ln>=LOG_N) { ln=0;
          float wr=(float)wo/(float)wi, ar=(float)to/(float)ti;
          BOOST_LOG_TRIVIAL(info)<<"FEC-STAT win"<<W<<" in="<<wi<<" out="<<wo
            <<" oh="<<(wr-1.0f)*100<<"% | total in="<<ti<<" out="<<to
            <<" oh="<<(ar-1.0f)*100<<"%"; }
          wi=0;wo=0;wn=0; } }
    co_return;
}

void FecPipeline::BuildBlob(const std::vector<Packet>& batch, std::vector<uint8_t>& blob) {
    blob.clear();
    uint32_t count = static_cast<uint32_t>(batch.size());
    blob.push_back(static_cast<uint8_t>(count & 0xFF)); blob.push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
    blob.push_back(static_cast<uint8_t>((count >> 16) & 0xFF)); blob.push_back(static_cast<uint8_t>((count >> 24) & 0xFF));
    for (auto& pkt : batch) {
        uint16_t len = static_cast<uint16_t>(pkt.DataSize());
        blob.push_back(static_cast<uint8_t>(len & 0xFF)); blob.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        size_t pos = blob.size(); blob.resize(pos + len);
        std::memcpy(blob.data() + pos, pkt.Data().data(), len);
    }
}

Omni::Fiber::Coroutine<void> FecPipeline::SendPing(uint64_t timestamp_us) {
    Packet out; out._Length = 0;
    uint8_t flags = BuildFlags() | kPing;
    float fb_loss_ping = _Shared ? _Shared->latest_loss_rate : 0.0f;
    uint8_t fb_ping = static_cast<uint8_t>(std::min(fb_loss_ping * 250.0f, 250.0f));
    out.PushFrontLE(timestamp_us); out.PushFrontLE(fb_ping);
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
    out.PushFrontLE(echo_us); out.PushFrontLE(fb_fb);
    out.PushFrontLE(BuildDword(++_GroupSeq, flags));
    auto err = co_await _Out->Write(out, _Stop);
    if (err && IsCritical(err)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") SendFeedback write error: " << err.message(); throw SystemError(err, "FecPipeline SendFeedback write error"); }
    co_return;
}

FecPipeline::RingSlot& FecPipeline::FindSlot(uint32_t group_seq) {
    for (size_t i = 0; i < _RingBuffer.size(); i++) { if (_RingBuffer[i].group_seq == group_seq) return _RingBuffer[i]; }
    EvictStaleSlot();
    return _RingBuffer[_RingNext++ & _RingMask];
}

bool FecPipeline::EvictStaleSlot() {
    auto now = std::chrono::steady_clock::now();
    uint64_t decode_timeout = _Cfg.decode_timeout_ms;
    if (_Shared && _Shared->rtt_ewma_us > 0) {
        uint64_t rtt_based = (3 * _Shared->rtt_ewma_us / 8000) + _Cfg.timeout_ms;
        decode_timeout = std::max(rtt_based, static_cast<uint64_t>(50));
    }
    for (size_t i = 0; i < _RingBuffer.size(); i++) {
        if (_RingBuffer[i].group_seq != 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _RingBuffer[i].first_time).count();
            if (static_cast<uint64_t>(elapsed) > decode_timeout) { _RingBuffer[i] = RingSlot{}; return true; }
        }
    }
    return false;
}

} // namespace gh
