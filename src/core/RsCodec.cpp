#include "RsCodec.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <span>

#include <boost/log/trivial.hpp>

#include "AdaptiveOverhead.hpp"
#include "FecStats.hpp"
#include "LossPattern.hpp"
#include "RS256.hpp"

namespace gh {

using namespace fec_wire;

RsCodec::RsCodec(const FecConfig& cfg, bool is_encoder, std::shared_ptr<FecSharedState> shared,
                 AdaptiveOverhead* overhead_ctrl, LossPattern* loss_pattern)
    : _Cfg(cfg), _IsEncoder(is_encoder), _Shared(std::move(shared)),
      _OverheadCtrl(overhead_ctrl), _LossPattern(loss_pattern) {
    if (_Cfg.stats) _Stats = _Cfg.stats.get();
}

uint32_t RsCodec::RsSymbolSize(const FecConfig& cfg) {
    uint32_t T = cfg.symbol_size;
    if (T == 0) {
        T = cfg.mtu - 28 - 20;
        if (T < 64) T = 64;
    }
    return T;
}

uint8_t RsCodec::BuildFlags() const {
    return 0;
}

uint32_t RsCodec::BuildDword(uint32_t seq, uint8_t flags) const {
    return (seq & 0xFFFFFF) | (static_cast<uint32_t>(flags) << 24);
}

void RsCodec::OnPacket(Packet&& p, std::vector<Packet>& out) {
    if (_IsEncoder) {
        EncodePacket(std::move(p), out);
    } else {
        DecodePacket(std::move(p), out);
    }
}

void RsCodec::Tick(std::vector<Packet>& out) {
    const auto now = std::chrono::steady_clock::now();
    if (_IsEncoder) {
        // batch deadline must still fire while the queue is idle
        if (_RsHaveBatch && now - _RsBatchStartTime >= std::chrono::milliseconds(_Cfg.timeout_ms)) {
            if (_Stats) { _Stats->BatchFlushTimeout(); _Stats->AddBatchSize(_RsBatch.size()); }
            SendRsRepair(_RsBatch, _RsBatchStartSeq);
            _RsBatch.clear();
            _RsHaveBatch = false;
        }
        // spread pending repairs: one per millisecond so they never burst
        // together with the batch tail shard (UDPspeeder -t equivalent)
        if (!_RsPendingRepairs.empty() &&
            now - _RsLastRepairSend >= std::chrono::milliseconds(1)) {
            out.push_back(std::move(_RsPendingRepairs.front()));
            _RsPendingRepairs.pop_front();
            _RsLastRepairSend = now;
        }
    } else {
        UpdateLossRate();
        CleanupStaleBatches(now);
    }
}

void RsCodec::UpdateLossRate() {
    // Raw-loss accounting, shard-granular (one event per slot eviction), so
    // it works with zero repairs in flight — the repair-slot-driven batch
    // accounting latched at 0 when loss_deadband suppressed repairs (no
    // repair slot created -> no measurement -> deadband never exits).
    // Window scaled by max_batch to keep the update cadence equivalent to
    // the old loss_window_groups batches (~50 x 20 shards).
    if (!_Shared) return;
    const uint32_t window = (_Cfg.loss_window_groups > 0 ? _Cfg.loss_window_groups : 50) *
                            std::max<uint32_t>(_Cfg.max_batch, 1);
    if (_RsLossGroups < window) return;
    const float fail_rate = static_cast<float>(_RsLossFails) / static_cast<float>(_RsLossGroups);
    const float alpha = _Cfg.loss_alpha > 0 ? _Cfg.loss_alpha : 0.1f;
    _Shared->latest_loss_rate = alpha * fail_rate + (1.0f - alpha) * _Shared->latest_loss_rate;
    if (_Stats) _Stats->SetLossRate(_Shared->latest_loss_rate);
    BOOST_LOG_TRIVIAL(debug) << "RsLossRate: fails=" << _RsLossFails << "/" << _RsLossGroups
                             << " fail_rate=" << fail_rate
                             << " -> rate=" << _Shared->latest_loss_rate;
    _RsLossGroups = 0;
    _RsLossFails = 0;
}

void RsCodec::EncodePacket(Packet&& p, std::vector<Packet>& out) {
    const uint32_t T = RsSymbolSize(_Cfg);
    const size_t dlen = p.DataSize();
    if (dlen == 0) return;
    const float fb_loss = _Shared ? _Shared->latest_loss_rate : 0.0f;
    const uint8_t fb = static_cast<uint8_t>(std::min(fb_loss * 250.0f, 250.0f));

    // small packets (<256B payload) or oversized: direct send with redundancy
    // (like the lcrq REPEAT path). seq=0: small shards do NOT consume RS
    // sequence space (RS batch seqs stay contiguous).
    if (dlen + 2 > T || dlen < 256) {
        if (_Stats) _Stats->EncSmall();
        // redundancy copies: 1 + ceil(repeat_ratio), adapted like lcrq
        float adaptive_ratio = _Cfg.repeat_ratio;
        if (_OverheadCtrl) {
            float oh = _OverheadCtrl->GetOverhead();
            float frac = (oh - _Cfg.safety_margin) / (_Cfg.max_overhead - _Cfg.safety_margin);
            frac = std::clamp(frac, 0.0f, 1.0f);
            adaptive_ratio = _Cfg.repeat_ratio_min + (_Cfg.repeat_ratio_max - _Cfg.repeat_ratio_min) * frac;
        }
        uint32_t copies = static_cast<uint32_t>(std::ceil(adaptive_ratio)) + 1;
        for (uint32_t i = 0; i < copies; i++) {
            Packet op; op._Length = 0;
            op.PushBack(p.Data());
            op.PushFrontLE(static_cast<uint16_t>(dlen));
            op.PushFrontLE(fb);
            op.PushFrontLE(BuildDword(0, kRsSmall));
            out.push_back(std::move(op));
        }
        return;
    }

    // RS source shard: send immediately (zero batch delay), accumulate copy for repair
    uint32_t seq = (++_RsSeq) & 0xFFFFFF;
    if (seq == 0) seq = 1;
    if (_Stats) _Stats->EncSrc();
    {
        Packet op; op._Length = 0;
        op.PushBack(p.Data());
        op.PushFrontLE(static_cast<uint16_t>(dlen));
        op.PushFrontLE(fb);
        op.PushFrontLE(BuildDword(seq, 0));
        const size_t pad = T - op.DataSize();
        if (pad > 0) {
            op._Data.resize(op._Offset + op._Length + pad);
            std::memset(op._Data.data() + op._Offset + op._Length, 0, pad);
            op._Length += pad;
        }
        out.push_back(std::move(op));
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
    const bool timed_out = (now - _RsBatchStartTime) >= std::chrono::milliseconds(_Cfg.timeout_ms);
    if (full || timed_out) {
        if (_Stats) {
            if (full) _Stats->BatchFlushFull();
            else _Stats->BatchFlushTimeout();
            _Stats->AddBatchSize(_RsBatch.size());
        }
        SendRsRepair(_RsBatch, _RsBatchStartSeq);
        _RsBatch.clear();
        _RsHaveBatch = false;
    }
}

void RsCodec::SendRsRepair(const std::vector<Packet>& batch, uint32_t batch_start_seq) {
    const uint32_t k = static_cast<uint32_t>(batch.size());
    if (k == 0) return;
    const uint32_t T = RsSymbolSize(_Cfg);
    float oh = _OverheadCtrl ? _OverheadCtrl->GetOverhead() : _Cfg.overhead;
    if (_OverheadCtrl && _Shared) _OverheadCtrl->Update(_Shared->peer_loss_rate);
    // loss_deadband: while the measured loss sits at/below the configured
    // clean-link baseline, skip repairs entirely — ceil() quantization would
    // otherwise send >=1 shard per batch (5% at max_batch=20) even on a
    // loss-free line. Crossing the deadband restores m>=1 immediately (via
    // the safety margin) and the controller ramps up from there.
    if (_Cfg.loss_deadband >= 0.0f && _Shared &&
        _Shared->peer_loss_rate <= _Cfg.loss_deadband) {
        if (_Stats) _Stats->DeadbandSuppressed();
        return;
    }
    uint32_t m = static_cast<uint32_t>(std::ceil(k * oh));
    if (m > 255 - k) m = 255 - k;
    if (m == 0) return;
    if (_Stats) {
        _Stats->SetOverhead(oh);
        _Stats->EncRepair(m);
    }

    std::vector<std::vector<uint8_t>> srcv(k, std::vector<uint8_t>(T, 0));
    for (uint32_t i = 0; i < k; i++) {
        auto d = batch[i].Data();
        std::memcpy(srcv[i].data(), d.data(), std::min<size_t>(d.size(), T));
    }
    std::vector<std::vector<uint8_t>> repairs;
    RS256::EncodeRepair(srcv, T, RS256::BuildCoeffs(k, m), repairs);

    // full 24-bit batch start seq on the wire: a 16-bit truncation would
    // break repair recovery once seq > 65535 (~94MB of traffic)
    const uint32_t bid = batch_start_seq & 0xFFFFFF;
    const float fb_loss = _Shared ? _Shared->latest_loss_rate : 0.0f;
    const uint8_t fb = static_cast<uint8_t>(std::min(fb_loss * 250.0f, 250.0f));
    for (uint32_t j = 0; j < m; j++) {
        Packet op; op._Length = 0;
        op.PushBack(std::span<const uint8_t>(repairs[j].data(), T));
        op.PushFrontLE(static_cast<uint8_t>(j));
        op.PushFrontLE(static_cast<uint8_t>(k));
        // Wire bytes are [lo16 | hi] = standard 24-bit LE. PushFrontLE
        // prepends, so push the HIGH byte first, then the low 16 bits.
        // (bbf1455 pushed low-16 first: wire became [hi lo0 lo1], the
        // decoder rebuilt hi + (lo0<<8) + (lo1<<16) — a scrambled batch id
        // on every packet, so repairs keyed to the wrong batch and
        // recovery NEVER triggered.)
        op.PushFrontLE(static_cast<uint8_t>((bid >> 16) & 0xFF));
        op.PushFrontLE(static_cast<uint16_t>(bid & 0xFFFF));
        op.PushFrontLE(fb);
        op.PushFrontLE(BuildDword(bid, kRsRepair));
        _RsPendingRepairs.push_back(std::move(op));  // spread out in Tick
    }
}

void RsCodec::DecodePacket(Packet&& p, std::vector<Packet>& out) {
    const uint32_t T = RsSymbolSize(_Cfg);
    if (p.DataSize() < 5) return;
    const uint32_t dw = p.PopFrontLE<uint32_t>();
    const uint32_t seq = dw & 0xFFFFFF;
    const uint8_t f = (dw >> 24) & 0xFF;

    const bool is_control = (f & kPing) || (f & kFeedback);
    if (_Stats && is_control) _Stats->DecCtrl();
    if (!is_control && _LossPattern) {
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - _StartTime).count();
        if (_LossPattern->ShouldDrop(_TotalPackets, elapsed)) { _TotalPackets++; return; }
    }
    _TotalPackets++;
    const uint8_t fb = p.PopFrontLE<uint8_t>();
    if (_Shared && fb <= 250) { _Shared->peer_loss_rate = static_cast<float>(fb) / 250.0f; }
    bool has_echo = (f & kEcho) != 0;
    if (has_echo) {
        if (p.DataSize() < 8) return;
        uint64_t echo = p.PopFrontLE<uint64_t>();
        if ((f & kFeedback) && _Shared) {
            auto now_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            uint64_t rtt_us = (now_us > echo) ? (now_us - echo) : 0;
            if (rtt_us > 0) { _Shared->rtt_ewma_us = (_Shared->rtt_ewma_us == 0) ? rtt_us : (_Shared->rtt_ewma_us * 7 + rtt_us) / 8; }
        }
    }
    if (f & kPing) {
        if (p.DataSize() >= 8 && _Shared) { _Shared->pending_feedback_echo = p.PopFrontLE<uint64_t>(); }
        return;
    }
    if (f & kFeedback) return;

    if (f & kRsRepair) {
        if (p.DataSize() < 5) return;
        const uint32_t bid = (p.PopFrontLE<uint16_t>() | (static_cast<uint32_t>(p.PopFrontLE<uint8_t>()) << 16)) & 0xFFFFFF;
        const uint8_t k = p.PopFrontLE<uint8_t>();
        const uint8_t idx = p.PopFrontLE<uint8_t>();
        if (p.DataSize() < T) return;
        auto& rep = _RsRepairs[bid & (kRsRepairSlots - 1)];
        if (!rep.valid || rep.bid != bid) {
            rep.bid = bid; rep.k = k; rep.valid = true; rep.completed = false;
            rep.data.clear(); rep.mask.fill(0);
        }
        const size_t off = static_cast<size_t>(idx) * T;
        if (rep.data.size() < off + T) rep.data.resize(off + T);
        std::memcpy(rep.data.data() + off, p.Data().data(), T);
        rep.mask[idx >> 3] |= static_cast<uint8_t>(1u << (idx & 7));
        rep.k = k;
        rep.time = std::chrono::steady_clock::now();
        RsTryRecover(bid, k, out);
        return;
    }

    // source shard (RS protected or small direct)
    if (p.DataSize() < 2) return;
    const uint16_t len = p.PopFrontLE<uint16_t>();
    if (len > p.DataSize()) return;
    if (f & kRsSmall) {
        // dedup: redundancy copies of the same small packet (identical
        // payload, back-to-back) must be delivered once, like the lcrq
        // REPEAT seen check. Duplicate delivery would flood TCP with
        // duplicate ACKs (cwnd collapse) and skew UDP datagram counts.
        const auto* d = p.Data().data();
        if (_Stats) _Stats->DecSmall();
        if (_HaveLastSmall && _LastSmall.size() == len &&
            std::memcmp(_LastSmall.data(), d, len) == 0) {
            if (_Stats) _Stats->DecDup();
            return;
        }
        _HaveLastSmall = true;
        if (_LastSmall.size() != len) _LastSmall.resize(len);
        std::memcpy(_LastSmall.data(), d, len);
        Packet op; op._Length = 0;
        op.PushBack(std::span<const uint8_t>(d, len));
        out.push_back(std::move(op));
        return;
    }
    // RS source shard: cache padded payload, then deliver IMMEDIATELY.
    // Out-of-order delivery is fine (TCP reorders; UDPspeeder mode-1 style):
    // a gap never blocks later shards, and is never "skipped" into permanent
    // loss by a watermark guard. The ring keeps a copy so repair recovery can
    // fill gaps when the repairs arrive (recovered shards are delivered from
    // RsTryRecover).
    auto& slot = _RsSrcs[seq & (kRsSrcSlots - 1)];
    // Raw-loss accounting at slot eviction, independent of repair arrival
    // (repairs may be disabled by loss_deadband). When this shard claims the
    // slot, the previous occupant should be seq - kRsSrcSlots (one full ring
    // cycle ago — a generous grace period for reordering). If it never
    // arrived, it is a loss. One event per shard, so fails/groups are
    // shard-granular.
    if (slot.valid) {
        const uint32_t expected = (seq - kRsSrcSlots) & 0xFFFFFF;
        if (slot.seq != expected) {
            _RsLossFails++;
            if (_Stats) _Stats->DecMissing();
        }
        _RsLossGroups++;
    }
    slot.seq = seq;
    slot.valid = true;
    if (_Stats) _Stats->DecSrc();
    // 乱序检测: 该分片所属批已恢复完成(repair 先到), 源分片后到 = 乱序误判
    if (_Stats) {
        auto& r = _RsRepairs[seq & (kRsRepairSlots - 1)];
        if (r.valid && r.completed && r.bid <= seq &&
            seq < static_cast<uint32_t>(r.bid) + r.k) {
            _Stats->ReorderEarly();
        }
    }
    if (slot.payload.size() != T) slot.payload.resize(T);
    slot.payload[0] = static_cast<uint8_t>(len & 0xFF);
    slot.payload[1] = static_cast<uint8_t>((len >> 8) & 0xFF);
    std::memcpy(slot.payload.data() + 2, p.Data().data(), std::min<size_t>(len, p.DataSize()));
    // Retry recovery on shard arrival: repairs can arrive while the batch's
    // tail shards are still in flight (missing scan overcounts), and a
    // "wait for more repairs" batch never gets rescanned once its repairs
    // have all arrived. A shard arriving for a batch that has repairs
    // re-triggers the scan.
    {
        auto& rep = _RsRepairs[seq & (kRsRepairSlots - 1)];
        if (rep.valid && rep.bid <= seq && seq < static_cast<uint32_t>(rep.bid) + rep.k) {
            RsTryRecover(rep.bid, rep.k, out);
        }
    }
    Packet op; op._Length = 0;
    op.PushBack(std::span<const uint8_t>(slot.payload.data() + 2, len));
    out.push_back(std::move(op));
}

void RsCodec::CleanupStaleBatches(const std::chrono::steady_clock::time_point& now) {
    // Repair slot release. Loss accounting no longer lives here: it is
    // shard-granular at source-slot eviction (DecodePacket), so it keeps
    // working while loss_deadband suppresses repairs.
    for (uint32_t i = 0; i < kRsRepairSlots; i++) {
        auto& r = _RsRepairs[i];
        if (!r.valid) continue;
        if (now - r.time <= std::chrono::milliseconds(_Cfg.decode_timeout_ms)) continue;
        if (_Stats) {
            _Stats->DecodeTimeoutCleanup();
            // 等待超时仍未完成的批 = 静默放弃 (缺失 > repair 能力)
            if (!r.completed) _Stats->RecoverAbandoned();
        }
        r.valid = false;
        r.data.clear();
    }
}

void RsCodec::RsTryRecover(uint32_t bid, uint32_t k, std::vector<Packet>& out) {
    const uint32_t T = RsSymbolSize(_Cfg);
    auto& rep = _RsRepairs[bid & (kRsRepairSlots - 1)];
    if (!rep.valid || rep.bid != bid) return;
    if (k == 0 || k > 255) return;

    if (_Stats) _Stats->RecoverAttempt();
    // missing source shards in [bid, bid+k)
    std::vector<uint32_t> missing;
    for (uint32_t s = bid; s < bid + k; s++) {
        const auto& slot = _RsSrcs[s & (kRsSrcSlots - 1)];
        if (!(slot.valid && slot.seq == s)) missing.push_back(s);
    }
    if (missing.empty()) {
        rep.completed = true;  // no gaps → ok (counted at slot release)
        return;
    }
    // count only repairs actually received: an unarrived repair in the
    // middle leaves a zero-filled slot that would otherwise decode as a
    // real symbol (garbage output or a false "success")
    size_t repairs_available = 0;
    for (const uint8_t m : rep.mask) repairs_available += std::popcount(m);
    if (missing.size() > repairs_available) return;  // wait for more repairs

    // assemble known shards: source unit rows + repair Vandermonde rows
    std::vector<std::vector<uint8_t>> known, rows;
    for (uint32_t s = bid; s < bid + k; s++) {
        const auto& slot = _RsSrcs[s & (kRsSrcSlots - 1)];
        if (!(slot.valid && slot.seq == s)) continue;
        std::vector<uint8_t> row(k, 0);
        row[s - bid] = 1;
        known.push_back(slot.payload);
        rows.push_back(std::move(row));
    }
    const uint8_t* rep_data = rep.data.data();
    for (uint32_t j = 0; j < k && known.size() < k; j++) {
        if (!(rep.mask[j >> 3] & (1u << (j & 7)))) continue;  // never received
        const size_t off = static_cast<size_t>(j) * T;
        if (off + T > rep.data.size()) continue;
        known.push_back(std::vector<uint8_t>(rep_data + off, rep_data + off + T));
        rows.push_back(RS256::RepairRow(k, j));
    }
    if (known.size() < k) return;

    std::vector<std::vector<uint8_t>> out_src;
    if (!RS256::Decode(known, T, rows, out_src)) {
        BOOST_LOG_TRIVIAL(warning) << "RsCodec recover failed for batch " << bid;
        if (_Stats) _Stats->RecoverFailed();
        return;  // keep slot: later shard arrivals retry; outcome counted at release
    }
    BOOST_LOG_TRIVIAL(info) << "RsCodec recovered " << missing.size() << " shards in batch " << bid;
    if (_Stats) _Stats->RecoverSuccess(static_cast<uint32_t>(missing.size()));
    for (uint32_t s : missing) {
        auto& slot = _RsSrcs[s & (kRsSrcSlots - 1)];
        slot.seq = s;
        slot.valid = true;
        slot.payload = std::move(out_src[s - bid]);
        // deliver the recovered shard now (out-of-order, like the originals)
        const uint16_t rlen = static_cast<uint16_t>(slot.payload[0]) |
                              (static_cast<uint16_t>(slot.payload[1]) << 8);
        if (slot.payload.size() >= 2 + rlen) {
            Packet op; op._Length = 0;
            op.PushBack(std::span<const uint8_t>(slot.payload.data() + 2, rlen));
            out.push_back(std::move(op));
        }
    }
    rep.completed = true;  // batch fully recovered → ok (counted at slot release)
}

} // namespace gh
