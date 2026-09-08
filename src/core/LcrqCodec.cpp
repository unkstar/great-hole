#include "LcrqCodec.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <span>

#include <boost/log/trivial.hpp>

#include "AdaptiveOverhead.hpp"
#include "LossPattern.hpp"
#include "RaptorQ.hpp"

namespace gh {

using namespace fec_wire;

LcrqCodec::LcrqCodec(const FecConfig& cfg, bool is_encoder, std::shared_ptr<FecSharedState> shared,
                     AdaptiveOverhead* overhead_ctrl, LossPattern* loss_pattern)
    : _Cfg(cfg), _IsEncoder(is_encoder), _Shared(std::move(shared)),
      _OverheadCtrl(overhead_ctrl), _LossPattern(loss_pattern) {
    _Batch.reserve(_Cfg.max_batch);
    uint32_t window = static_cast<uint32_t>(std::bit_ceil(cfg.decode_window));
    _RingBuffer.resize(window);
    _RingMask = window - 1;
}

uint8_t LcrqCodec::BuildFlags() const {
    uint8_t flags = 0;
    if (_Cfg.iv_len > 0 && _Cfg.obfuscate) {
        flags |= (_Cfg.iv_len & 0x07) << 1;
    }
    return flags;
}

uint32_t LcrqCodec::BuildDword(uint32_t group_seq, uint8_t flags) const {
    return (group_seq & 0xFFFFFF) | (static_cast<uint32_t>(flags) << 24);
}

void LcrqCodec::OnPacket(Packet&& p, std::vector<Packet>& out) {
    if (_IsEncoder) {
        EncodePacket(std::move(p), out);
    } else {
        DecodePacket(std::move(p), out);
    }
}

void LcrqCodec::Tick(std::vector<Packet>& out) {
    if (!_IsEncoder) return;
    if (_HaveBatch && std::chrono::steady_clock::now() - _BatchStart >=
                          std::chrono::milliseconds(_Cfg.timeout_ms)) {
        SendBatch(out);
    }
}

void LcrqCodec::EncodePacket(Packet&& p, std::vector<Packet>& out) {
    if (!_HaveBatch) {
        _HaveBatch = true;
        _BatchStart = std::chrono::steady_clock::now();
    }
    _Batch.push_back(std::move(p));
    if (_Batch.size() >= _Cfg.max_batch) {
        SendBatch(out);
    }
}

void LcrqCodec::SendBatch(std::vector<Packet>& out) {
    assert(!_Batch.empty());
    uint32_t pkt_count = static_cast<uint32_t>(_Batch.size());
    uint32_t group_seq = ++_GroupSeq;
    uint32_t flags = BuildFlags();
    uint32_t out_symbols = 0;  // set in each path for stats
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
        auto& pkt = _Batch[0];
        std::vector<uint8_t> iv;
        if (flags & 0x0E) {
            iv.resize(_Cfg.iv_len);
            for (auto& b : iv) b = static_cast<uint8_t>(std::rand() & 0xFF);
        }
        float fb_loss = _Shared ? _Shared->latest_loss_rate : 0.0f;
        uint8_t fb = static_cast<uint8_t>(std::min(fb_loss * 250.0f, 250.0f));
        uint32_t repeat_dw = (group_seq & 0xFFFFFF) | static_cast<uint32_t>(flags | kRepeat) << 24;
        for (uint32_t i = 0; i < copies; i++) {
            Packet op; op._Length = 0;
            op.PushBack(pkt.Data());
            if (!iv.empty()) {
                for (size_t j = 0; j < op.DataSize(); j++) op._Data[op._Offset + j] ^= iv[j % iv.size()];
                op.PushFront(std::span<const uint8_t>(iv));
            }
            op.PushFrontLE(fb);
            op.PushFrontLE(repeat_dw);
            out.push_back(std::move(op));
        }
    } else {
        std::vector<uint8_t> blob;
        BuildBlob(_Batch, blob);
        uint32_t symbol_size = _Cfg.symbol_size;
        if (symbol_size == 0) {
            symbol_size = _Cfg.mtu - 28 - 20;
            if (symbol_size < 16) symbol_size = 16;
        }
        uint64_t F = blob.size();
        if (F % symbol_size != 0) {
            size_t pad = symbol_size - (F % symbol_size);
            blob.insert(blob.end(), pad, 0);
            F = blob.size();
        }
        uint16_t T = static_cast<uint16_t>(symbol_size);
        RaptorQ rq(F, T);
        rq.Encode(blob.data(), blob.size());
        uint32_t K = rq.K();
        float current_overhead = _OverheadCtrl ? _OverheadCtrl->GetOverhead() : _Cfg.overhead;
        uint32_t extra = static_cast<uint32_t>(std::ceil(K * current_overhead));
        if (_Cfg.loss_deadband >= 0.0f && _Shared &&
            _Shared->peer_loss_rate <= _Cfg.loss_deadband) {
            extra = 0;
        }
        uint32_t total_symbols = K + extra;
        out_symbols = total_symbols;
        if (total_symbols > 65535) total_symbols = 65535;
        bool use_2b = (total_symbols > 255) || (K > 255);
        uint8_t final_flags = static_cast<uint8_t>(flags);
        if (use_2b) final_flags |= kWidth2B;
        std::vector<uint8_t> iv;
        if (final_flags & 0x0E) {
            iv.resize(_Cfg.iv_len);
            for (auto& b : iv) b = static_cast<uint8_t>(std::rand() & 0xFF);
        }
        uint32_t final_dw = BuildDword(group_seq, final_flags);
        auto sym_buf = std::make_unique<uint8_t[]>(T);
        for (uint32_t esi = 0; esi < total_symbols; esi++) {
            if (!rq.GenerateSymbol(esi, sym_buf.get())) {
                throw std::runtime_error("RaptorQ: GenerateSymbol failed for ESI " + std::to_string(esi));
            }
            Packet op; op._Length = 0;
            op.PushBack(std::span<const uint8_t>(sym_buf.get(), T));
            if (!iv.empty()) {
                for (size_t j = 0; j < op.DataSize(); j++) op._Data[op._Offset + j] ^= iv[j % iv.size()];
                op.PushFront(std::span<const uint8_t>(iv));
            }
            if (use_2b) {
                op.PushFrontLE(static_cast<uint16_t>(K));
                op.PushFrontLE(static_cast<uint16_t>(esi));
            } else {
                op.PushFrontLE(static_cast<uint8_t>(K));
                op.PushFrontLE(static_cast<uint8_t>(esi));
            }
            float fb_loss2 = _Shared ? _Shared->latest_loss_rate : 0.0f;
            uint8_t fb2 = static_cast<uint8_t>(std::min(fb_loss2 * 250.0f, 250.0f));
            op.PushFrontLE(fb2);
            op.PushFrontLE(final_dw);
            out.push_back(std::move(op));
        }
    }
    // FEC overhead stats (100-batch sliding window). Set LOG_N>0 to enable.
    {
        static constexpr int W = 100, LOG_N = 0;
        static uint64_t wi = 0, wo = 0, ti = 0, to = 0, wn = 0, ln = 0;
        wi += pkt_count;
        wo += out_symbols;
        ti += pkt_count;
        to += out_symbols;
        if (++wn >= W) {
            if (LOG_N > 0 && ++ln >= LOG_N) {
                ln = 0;
                float wr = (float)wo / (float)wi, ar = (float)to / (float)ti;
                BOOST_LOG_TRIVIAL(info) << "FEC-STAT win" << W << " in=" << wi << " out=" << wo
                                        << " oh=" << (wr - 1.0f) * 100 << "% | total in=" << ti
                                        << " out=" << to << " oh=" << (ar - 1.0f) * 100 << "%";
            }
            wi = 0;
            wo = 0;
            wn = 0;
        }
    }
    _Batch.clear();
    _HaveBatch = false;
}

void LcrqCodec::BuildBlob(const std::vector<Packet>& batch, std::vector<uint8_t>& blob) {
    blob.clear();
    uint32_t count = static_cast<uint32_t>(batch.size());
    blob.push_back(static_cast<uint8_t>(count & 0xFF));
    blob.push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
    blob.push_back(static_cast<uint8_t>((count >> 16) & 0xFF));
    blob.push_back(static_cast<uint8_t>((count >> 24) & 0xFF));
    for (auto& pkt : batch) {
        uint16_t len = static_cast<uint16_t>(pkt.DataSize());
        blob.push_back(static_cast<uint8_t>(len & 0xFF));
        blob.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        size_t pos = blob.size();
        blob.resize(pos + len);
        std::memcpy(blob.data() + pos, pkt.Data().data(), len);
    }
}

void LcrqCodec::DecodePacket(Packet&& p, std::vector<Packet>& out) {
    if (p.DataSize() < 5) return;
    uint32_t dw = p.PopFrontLE<uint32_t>();
    uint32_t group_seq = dw & 0xFFFFFF;
    uint8_t f = (dw >> 24) & 0xFF;
    bool is_control = (f & kPing) || (f & kFeedback);
    if (!is_control && _LossPattern) {
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - _StartTime).count();
        if (_LossPattern->ShouldDrop(_TotalPackets, elapsed)) { _TotalPackets++; return; }
    }
    _TotalPackets++;
    uint8_t fb = p.PopFrontLE<uint8_t>();
    if (_Shared && fb <= 250) { _Shared->peer_loss_rate = static_cast<float>(fb) / 250.0f; }
    uint64_t echo = 0;
    bool has_echo = (f & kEcho) != 0;
    if (has_echo) {
        if (p.DataSize() < 8) return;
        echo = p.PopFrontLE<uint64_t>();
    }
    bool is_ping = (f & kPing) != 0;
    bool is_feedback = (f & kFeedback) != 0;
    bool is_repeat = (f & kRepeat) != 0;
    if (is_ping) {
        if (p.DataSize() >= 8 && _Shared) { _Shared->pending_feedback_echo = p.PopFrontLE<uint64_t>(); }
        return;
    }
    if (is_feedback) {
        if (has_echo && echo > 0 && _Shared) {
            auto now_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            uint64_t rtt_us = (now_us > echo) ? (now_us - echo) : 0;
            if (rtt_us > 0) { _Shared->rtt_ewma_us = (_Shared->rtt_ewma_us == 0) ? rtt_us : (_Shared->rtt_ewma_us * 7 + rtt_us) / 8; }
        }
        return;
    }
    if (is_repeat) {
        auto& rslot = SlotFor(group_seq);
        bool seen = rslot.group_seq == group_seq;
        if (!seen) {
            rslot.group_seq = group_seq;
            uint8_t iv_len = (f >> 1) & 0x07;
            if (iv_len > 0 && _Cfg.obfuscate) {
                if (p.DataSize() < iv_len) return;
                auto iv_span = p.PopFront(iv_len);
                for (size_t i = 0; i < p.DataSize(); i++) { p._Data[p._Offset + i] ^= iv_span[i % iv_len]; }
            }
            out.push_back(std::move(p));
        }
        return;
    }
    // FEC data shard
    uint8_t width = (f & kWidth2B) ? 2 : 1;
    uint32_t shard_index, source_count;
    if (width == 1) {
        if (p.DataSize() < 2) return;
        shard_index = p.PopFrontLE<uint8_t>();
        source_count = p.PopFrontLE<uint8_t>();
    } else {
        if (p.DataSize() < 4) return;
        shard_index = p.PopFrontLE<uint16_t>();
        source_count = p.PopFrontLE<uint16_t>();
    }
    uint8_t iv_len = (f >> 1) & 0x07;
    if (iv_len > 0 && _Cfg.obfuscate) {
        if (p.DataSize() < iv_len) return;
        auto iv_span = p.PopFront(iv_len);
        for (size_t i = 0; i < p.DataSize(); i++) { p._Data[p._Offset + i] ^= iv_span[i % iv_len]; }
    }

    auto& slot = SlotFor(group_seq);
    if (slot.group_seq == 0) {
        slot.group_seq = group_seq;
        slot.source_count = source_count;
        slot.first_time = std::chrono::steady_clock::now();
    }
    if (slot.source_count != source_count) {
        slot.Reset();
        slot.group_seq = group_seq;
        slot.source_count = source_count;
        slot.first_time = std::chrono::steady_clock::now();
    }
    // esi-indexed shard storage: O(1) insert, no linear scan
    if (slot.shards.size() <= shard_index) slot.shards.resize(shard_index + 1);
    if (slot.shards[shard_index].empty()) slot.symbol_count++;
    slot.shards[shard_index].assign(p._Data.data() + p._Offset, p._Data.data() + p._Offset + p._Length);
    if (shard_index >= slot.max_esi) slot.max_esi = shard_index;
    if (slot.symbol_count >= slot.source_count) {
        uint32_t symbol_size = _Cfg.symbol_size;
        if (symbol_size == 0) symbol_size = _Cfg.mtu - 28 - 20;
        uint64_t F = static_cast<uint64_t>(source_count) * symbol_size;
        RaptorQ rq(F, static_cast<uint16_t>(symbol_size));
        for (uint32_t esi = 0; esi < slot.shards.size(); esi++) {
            auto& shard = slot.shards[esi];
            if (shard.empty()) continue;
            if (shard.size() < symbol_size) shard.resize(symbol_size, 0);
            rq.SubmitSymbol(shard.data(), symbol_size, esi);
        }
        _LossGroupCount++;
        std::vector<uint8_t> decoded(F);
        if (rq.TryDecode(decoded.data(), F)) {
            uint32_t window = _Cfg.loss_window_groups > 0 ? _Cfg.loss_window_groups : 50;
            if (_LossGroupCount >= window && _Shared) {
                float fail_rate = static_cast<float>(_LossFailCount) / static_cast<float>(_LossGroupCount);
                float alpha = _Cfg.loss_alpha > 0 ? _Cfg.loss_alpha : 0.1f;
                _Shared->latest_loss_rate = alpha * fail_rate + (1.0f - alpha) * _Shared->latest_loss_rate;
                _LossGroupCount = 0;
                _LossFailCount = 0;
            }
            BOOST_LOG_TRIVIAL(info) << "LcrqCodec decoded group " << group_seq << " with "
                                    << slot.symbol_count << "/" << (slot.max_esi + 1) << " symbols";
            size_t pos = 0;
            if (pos + 4 > decoded.size()) { slot.Reset(); return; }
            uint32_t pkt_count = static_cast<uint32_t>(decoded[pos]) | (static_cast<uint32_t>(decoded[pos + 1]) << 8) |
                                 (static_cast<uint32_t>(decoded[pos + 2]) << 16) | (static_cast<uint32_t>(decoded[pos + 3]) << 24);
            pos += 4;
            for (uint32_t i = 0; i < pkt_count && pos + 2 <= decoded.size(); i++) {
                uint16_t pkt_len = static_cast<uint16_t>(decoded[pos]) | (static_cast<uint16_t>(decoded[pos + 1]) << 8);
                pos += 2;
                if (pos + pkt_len > decoded.size()) break;
                Packet op; op._Length = 0;
                op.PushBack(std::span<const uint8_t>(decoded.data() + pos, pkt_len));
                pos += pkt_len;
                out.push_back(std::move(op));
            }
        } else {
            _LossFailCount++;
            BOOST_LOG_TRIVIAL(warning) << "LcrqCodec decode failed for group " << group_seq << " with "
                                       << slot.symbol_count << " symbols";
        }
        slot.Reset();
    }
}

LcrqCodec::RingSlot& LcrqCodec::SlotFor(uint32_t group_seq) {
    // Direct-indexed ring: O(1) home-slot lookup (no linear scan). If the home
    // slot holds a stale group, evict it; otherwise overwrite (same behavior
    // as the original round-robin overwrite at ring capacity).
    auto& slot = _RingBuffer[group_seq & _RingMask];
    if (slot.group_seq == group_seq) return slot;
    uint64_t decode_timeout = _Cfg.decode_timeout_ms;
    if (_Shared && _Shared->rtt_ewma_us > 0) {
        uint64_t rtt_based = (3 * _Shared->rtt_ewma_us / 8000) + _Cfg.timeout_ms;
        decode_timeout = std::max(rtt_based, static_cast<uint64_t>(50));
    }
    if (slot.group_seq != 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - slot.first_time)
                           .count();
        if (static_cast<uint64_t>(elapsed) > decode_timeout) {
            slot.Reset();  // stale: evict (shard buffers keep capacity)
        }
    }
    return slot;
}

} // namespace gh
