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
#include "RS256.hpp"

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
    if (_Cfg.fec_codec == "rs") {
        if (_IsEncoder) {
            co_await ProcessRsEncode();
        } else {
            co_await ProcessRsDecode();
        }
        co_return;
    }
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

// ============================ RS CODEC (Vandermonde GF256) ============================

static uint32_t rs_symbol_size(const FecConfig& cfg) {
    uint32_t T = cfg.symbol_size;
    if (T == 0) { T = cfg.mtu - 28 - 20; if (T < 64) T = 64; }
    return T;
}

Omni::Fiber::Coroutine<void> FecPipeline::ProcessRsEncode() {
    const uint32_t T = rs_symbol_size(_Cfg);
    const auto timeout = std::chrono::milliseconds(_Cfg.timeout_ms);

    while (!_Stop.IsTriggered()) {
        // PING/FEEDBACK — same scheduling as the lcrq path
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

        Packet p;
        auto err = co_await _In->Read(p, _Stop);
        if (err) {
            if (err == ErrorCode{AppErrorCategory::kOperationAborted, kAppError}) continue;
            if (err == ErrorCode{AppErrorCategory::kEndOfStream, kAppError}) break;
            if (IsCritical(err)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") RS read error: " << err.message(); throw SystemError(err, "FecPipeline RS read error"); }
            BOOST_LOG_TRIVIAL(warning) << "FecPipeline(" << this << ") RS read warning: " << err.message(); continue;
        }
        co_await RsHandleEncodePacket(std::move(p));
        // Drain the TUN (EPOLLET): repeated async_read_some on the same
        // edge-triggered socket would hang if the buffer is not emptied.
        while (!_Stop.IsTriggered()) {
            Packet p2;
            if (_In->TryRead(p2)) break;
            co_await RsHandleEncodePacket(std::move(p2));
        }
        (void)timeout;
    }
    if (_RsHaveBatch && !_RsBatch.empty()) {
        co_await SendRsRepair(_RsBatch, _RsBatchStartSeq);
        _RsBatch.clear();
        _RsHaveBatch = false;
    }
    co_return;
}

Omni::Fiber::Coroutine<void> FecPipeline::RsHandleEncodePacket(Packet&& p) {
    const uint32_t T = rs_symbol_size(_Cfg);
    const auto timeout = std::chrono::milliseconds(_Cfg.timeout_ms);
    const size_t dlen = p.DataSize();
    if (dlen > 0) {
        const uint8_t* d = p.Data().data();
        BOOST_LOG_TRIVIAL(info) << "RS-ENC in=" << dlen << " hdr=" << std::hex
            << (int)d[0] << "," << (int)d[1] << "," << (int)d[2] << "," << (int)d[3] << ","
            << (int)d[12] << "," << (int)d[13] << "," << (int)d[14] << "," << (int)d[15] << std::dec;
    }
    if (dlen == 0) co_return;
    const float fb_loss = _Shared ? _Shared->latest_loss_rate : 0.0f;
    const uint8_t fb = static_cast<uint8_t>(std::min(fb_loss * 250.0f, 250.0f));

    // small packets (<256B payload) or oversized: direct send, no RS protection.
    // seq=0: small shards do NOT consume RS sequence space (RS batch seqs stay contiguous).
    if (dlen + 2 > T || dlen < 256) {
        Packet out; out._Length = 0;
        out.PushBack(p.Data());
        out.PushFrontLE(static_cast<uint16_t>(dlen));
        out.PushFrontLE(fb);
        out.PushFrontLE(BuildDword(0, kRsSmall));
        auto werr = co_await _Out->Write(out, _Stop);
        if (werr && IsCritical(werr)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") RS write error: " << werr.message(); throw SystemError(werr, "FecPipeline RS write error"); }
        co_return;
    }

    // RS source shard: send immediately (zero batch delay), accumulate copy for repair
    uint32_t seq = (++_RsSeq) & 0xFFFFFF;
    if (seq == 0) seq = 1;
    {
        Packet out; out._Length = 0;
        out.PushBack(p.Data());
        out.PushFrontLE(static_cast<uint16_t>(dlen));
        out.PushFrontLE(fb);
        out.PushFrontLE(BuildDword(seq, 0));
        const size_t pad = T - out.DataSize();
        if (pad > 0) {
            out._Data.resize(out._Offset + out._Length + pad);
            std::memset(out._Data.data() + out._Offset + out._Length, 0, pad);
            out._Length += pad;
        }
        auto werr = co_await _Out->Write(out, _Stop);
        if (werr && IsCritical(werr)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") RS write error: " << werr.message(); throw SystemError(werr, "FecPipeline RS write error"); }
    }

    // batch window bookkeeping
    const auto now = std::chrono::steady_clock::now();
    if (!_RsHaveBatch) {
        _RsHaveBatch = true;
        _RsBatchStartSeq = seq;
        _RsBatchStartTime = now;
    }
    _RsBatch.push_back(std::move(p));
    const bool full = _RsBatch.size() >= _Cfg.max_batch;
    const bool timed_out = (now - _RsBatchStartTime) >= timeout;
    if (full || timed_out) {
        co_await SendRsRepair(_RsBatch, _RsBatchStartSeq);
        _RsBatch.clear();
        _RsHaveBatch = false;
    }
    co_return;
}

Omni::Fiber::Coroutine<void> FecPipeline::SendRsRepair(const std::vector<Packet>& batch, uint32_t batch_start_seq) {
    const uint32_t k = static_cast<uint32_t>(batch.size());
    if (k == 0) co_return;
    const uint32_t T = rs_symbol_size(_Cfg);
    float oh = _OverheadCtrl ? _OverheadCtrl->GetOverhead() : _Cfg.overhead;
    if (_OverheadCtrl && _Shared) _OverheadCtrl->Update(_Shared->latest_loss_rate);
    uint32_t m = static_cast<uint32_t>(std::ceil(k * oh));
    if (m > 255 - k) m = 255 - k;
    if (m == 0) co_return;

    std::vector<std::vector<uint8_t>> srcv(k, std::vector<uint8_t>(T, 0));
    for (uint32_t i = 0; i < k; i++) {
        auto d = batch[i].Data();
        std::memcpy(srcv[i].data(), d.data(), std::min<size_t>(d.size(), T));
    }
    std::vector<std::vector<uint8_t>> repairs;
    RS256::EncodeRepair(srcv, T, RS256::BuildCoeffs(k, m), repairs);

    const uint16_t bid = static_cast<uint16_t>(batch_start_seq & 0xFFFF);
    const float fb_loss = _Shared ? _Shared->latest_loss_rate : 0.0f;
    const uint8_t fb = static_cast<uint8_t>(std::min(fb_loss * 250.0f, 250.0f));
    for (uint32_t j = 0; j < m; j++) {
        Packet out; out._Length = 0;
        out.PushBack(std::span<const uint8_t>(repairs[j].data(), T));
        out.PushFrontLE(static_cast<uint8_t>(j));
        out.PushFrontLE(static_cast<uint8_t>(k));
        out.PushFrontLE(bid);
        out.PushFrontLE(fb);
        out.PushFrontLE(BuildDword(bid, kRsRepair));
        auto werr = co_await _Out->Write(out, _Stop);
        if (werr && IsCritical(werr)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") RS repair write error: " << werr.message(); throw SystemError(werr, "FecPipeline RS repair write error"); }
    }
    BOOST_LOG_TRIVIAL(info) << "FecPipeline(" << this << ") RS batch k=" << k << " repairs=" << m;
    co_return;
}

Omni::Fiber::Coroutine<void> FecPipeline::ProcessRsDecode() {
    while (!_Stop.IsTriggered()) {
        Packet p;
        auto err = co_await _In->Read(p, _Stop);
        if (err) {
            if (err == ErrorCode{AppErrorCategory::kOperationAborted, kAppError}) continue;
            if (err == ErrorCode{AppErrorCategory::kEndOfStream, kAppError}) break;
            if (IsCritical(err)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") RS decode read error: " << err.message(); throw SystemError(err, "FecPipeline RS decode read error"); }
            BOOST_LOG_TRIVIAL(warning) << "FecPipeline(" << this << ") RS decode read warning: " << err.message(); continue;
        }
        co_await RsHandleDecodePacket(std::move(p));
        // Drain the UDP endpoint (EPOLLET): same hang avoidance as encode side.
        while (!_Stop.IsTriggered()) {
            Packet p2;
            if (_In->TryRead(p2)) break;
            co_await RsHandleDecodePacket(std::move(p2));
        }
    }
    co_return;
}

Omni::Fiber::Coroutine<void> FecPipeline::RsHandleDecodePacket(Packet&& p) {
    const uint32_t T = rs_symbol_size(_Cfg);
    BOOST_LOG_TRIVIAL(info) << "RS-DEC in=" << p.DataSize();
    if (p.DataSize() < 5) co_return;
    const uint32_t dw = p.PopFrontLE<uint32_t>();
    const uint32_t seq = dw & 0xFFFFFF;
    const uint8_t f = (dw >> 24) & 0xFF;

    const bool is_control = (f & kPing) || (f & kFeedback);
    if (!is_control && _LossPattern) {
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - _StartTime).count();
        if (_LossPattern->ShouldDrop(_TotalPackets, elapsed)) { _TotalPackets++; co_return; }
    }
    _TotalPackets++;
    const uint8_t fb = p.PopFrontLE<uint8_t>();
    if (_Shared && fb <= 250) { _Shared->latest_loss_rate = static_cast<float>(fb) / 250.0f; }
    bool has_echo = (f & kEcho) != 0;
    if (has_echo) { if (p.DataSize() < 8) co_return; uint64_t echo = p.PopFrontLE<uint64_t>(); if ((f & kFeedback) && _Shared) { auto now_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); uint64_t rtt_us = (now_us > echo) ? (now_us - echo) : 0; if (rtt_us > 0) { _Shared->rtt_ewma_us = (_Shared->rtt_ewma_us == 0) ? rtt_us : (_Shared->rtt_ewma_us * 7 + rtt_us) / 8; } } }
    if (f & kPing) {
        if (p.DataSize() >= 8 && _Shared) { _Shared->pending_feedback_echo = p.PopFrontLE<uint64_t>(); }
        co_return;
    }
    if (f & kFeedback) co_return;

    if (f & kRsRepair) {
        if (p.DataSize() < 4) co_return;
        const uint16_t bid = p.PopFrontLE<uint16_t>();
        const uint8_t k = p.PopFrontLE<uint8_t>();
        const uint8_t idx = p.PopFrontLE<uint8_t>();
        if (p.DataSize() < T) co_return;
        auto& rep = _RsRepairs[bid];
        const size_t off = static_cast<size_t>(idx) * T;
        if (rep.size() < off + T) rep.resize(off + T);
        std::memcpy(rep.data() + off, p.Data().data(), T);
        _RsBatchK[bid] = k;
        _RsBatchTime[bid] = std::chrono::steady_clock::now();
        RsTryRecover(bid, k);
        co_await RsFlushDelivery();
        co_return;
    }

    // source shard (RS protected or small direct)
    if (p.DataSize() < 2) co_return;
    const uint16_t len = p.PopFrontLE<uint16_t>();
    if (len > p.DataSize()) co_return;
    if (f & kRsSmall) {
        Packet out; out._Length = 0;
        out.PushBack(std::span<const uint8_t>(p.Data().data(), len));
        auto werr = co_await _Out->Write(out, _Stop);
        if (werr && IsCritical(werr)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") RS write error: " << werr.message(); throw SystemError(werr, "FecPipeline RS write error"); }
        co_return;
    }
    // RS source shard: cache padded payload, deliver in seq order
    std::vector<uint8_t> payload(T, 0);
    payload[0] = static_cast<uint8_t>(len & 0xFF);
    payload[1] = static_cast<uint8_t>((len >> 8) & 0xFF);
    std::memcpy(payload.data() + 2, p.Data().data(), std::min<size_t>(len, p.DataSize()));
    _RsSrcs[seq] = std::move(payload);
    if (!_RsHaveWatermark) { _RsHaveWatermark = true; _RsDeliverSeq = seq - 1; }
    co_await RsFlushDelivery();

    // Global watermark stall guard: if delivery stalled past decode_timeout
    // (missing shard whose repairs never arrived), skip the gap and continue.
    const auto now = std::chrono::steady_clock::now();
    if (_RsHaveWatermark && now - _RsLastFlushTime > std::chrono::milliseconds(_Cfg.decode_timeout_ms)) {
        auto gap = _RsSrcs.lower_bound(_RsDeliverSeq + 1);
        if (gap != _RsSrcs.end() && gap->first > _RsDeliverSeq + 1) {
            const uint32_t skipped = gap->first - 1 - _RsDeliverSeq;
            _RsDeliverSeq = gap->first - 1;
            BOOST_LOG_TRIVIAL(warning) << "FecPipeline(" << this << ") RS watermark stall: skipped " << skipped << " missing shards";
        }
        _RsLastFlushTime = now;
    }

    // stale batch cleanup: force watermark past batches that can no longer recover
    for (auto it = _RsBatchTime.begin(); it != _RsBatchTime.end();) {
        if (now - it->second > std::chrono::milliseconds(_Cfg.decode_timeout_ms)) {
            const uint32_t k = _RsBatchK.count(it->first) ? _RsBatchK[it->first] : 0;
            const uint32_t end = static_cast<uint32_t>(it->first) + k;
            while (_RsHaveWatermark && _RsDeliverSeq + 1 < end && !_RsSrcs.count(_RsDeliverSeq + 1)) {
                _RsDeliverSeq++;
            }
            _RsRepairs.erase(it->first);
            _RsBatchK.erase(it->first);
            it = _RsBatchTime.erase(it);
        } else {
            ++it;
        }
    }
    co_await RsFlushDelivery();
    co_return;
}

Omni::Fiber::Coroutine<void> FecPipeline::RsFlushDelivery() {
    while (_RsHaveWatermark) {
        const uint32_t next = _RsDeliverSeq + 1;
        auto it = _RsSrcs.find(next);
        if (it == _RsSrcs.end()) break;
        _RsLastFlushTime = std::chrono::steady_clock::now();
        const auto& payload = it->second;
        const uint16_t len = static_cast<uint16_t>(payload[0]) | (static_cast<uint16_t>(payload[1]) << 8);
        if (payload.size() < 2 + len) { _RsSrcs.erase(it); _RsDeliverSeq++; continue; }
        Packet out; out._Length = 0;
        out.PushBack(std::span<const uint8_t>(payload.data() + 2, len));
        _RsSrcs.erase(it);
        _RsDeliverSeq++;
        auto werr = co_await _Out->Write(out, _Stop);
        if (werr && IsCritical(werr)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") RS write error: " << werr.message(); throw SystemError(werr, "FecPipeline RS write error"); }
    }
    co_return;
}

void FecPipeline::RsTryRecover(uint32_t bid, uint32_t k) {
    const uint32_t T = rs_symbol_size(_Cfg);
    auto rep_it = _RsRepairs.find(bid);
    if (rep_it == _RsRepairs.end()) return;
    if (k == 0 || k > 255) return;

    // missing source shards in [bid, bid+k)
    std::vector<uint32_t> missing;
    for (uint32_t s = bid; s < bid + k; s++) {
        if (!_RsSrcs.count(s)) missing.push_back(s);
    }
    if (missing.empty()) {
        _RsRepairs.erase(rep_it);
        _RsBatchK.erase(bid);
        _RsBatchTime.erase(bid);
        return;
    }
    const size_t repairs_available = rep_it->second.size() / T;
    if (missing.size() > repairs_available) return;  // wait for more repairs

    // assemble known shards: source unit rows + repair Vandermonde rows
    std::vector<std::vector<uint8_t>> known, rows;
    for (uint32_t s = bid; s < bid + k; s++) {
        auto sit = _RsSrcs.find(s);
        if (sit == _RsSrcs.end()) continue;
        std::vector<uint8_t> row(k, 0);
        row[s - bid] = 1;
        known.push_back(sit->second);
        rows.push_back(std::move(row));
    }
    const uint8_t* rep_data = rep_it->second.data();
    for (uint32_t j = 0; j < k && known.size() < k; j++) {
        const size_t off = static_cast<size_t>(j) * T;
        if (off + T > rep_it->second.size()) break;
        known.push_back(std::vector<uint8_t>(rep_data + off, rep_data + off + T));
        rows.push_back(RS256::RepairRow(k, j));
    }
    if (known.size() < k) return;

    std::vector<std::vector<uint8_t>> out_src;
    if (!RS256::Decode(known, T, rows, out_src)) {
        BOOST_LOG_TRIVIAL(warning) << "FecPipeline(" << this << ") RS recover failed for batch " << bid;
        _RsRepairs.erase(rep_it);
        _RsBatchK.erase(bid);
        _RsBatchTime.erase(bid);
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "FecPipeline(" << this << ") RS recovered " << missing.size() << " shards in batch " << bid;
    for (uint32_t s : missing) {
        _RsSrcs[s] = std::move(out_src[s - bid]);
    }
    _RsRepairs.erase(rep_it);
    _RsBatchK.erase(bid);
    _RsBatchTime.erase(bid);
}

} // namespace gh
