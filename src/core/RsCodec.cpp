#include "RsCodec.hpp"

#include <algorithm>
#include <cstring>
#include <span>

#include <boost/log/trivial.hpp>

#include "AdaptiveOverhead.hpp"
#include "LossPattern.hpp"
#include "RS256.hpp"

namespace gh {

using namespace fec_wire;

RsCodec::RsCodec(const FecConfig& cfg, bool is_encoder, std::shared_ptr<FecSharedState> shared,
                 AdaptiveOverhead* overhead_ctrl, LossPattern* loss_pattern)
    : _Cfg(cfg), _IsEncoder(is_encoder), _Shared(std::move(shared)),
      _OverheadCtrl(overhead_ctrl), _LossPattern(loss_pattern) {}

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
            SendRsRepair(_RsBatch, _RsBatchStartSeq, out);
            _RsBatch.clear();
            _RsHaveBatch = false;
        }
    } else {
        AdvanceWatermark(now, out);
    }
}

void RsCodec::EncodePacket(Packet&& p, std::vector<Packet>& out) {
    const uint32_t T = RsSymbolSize(_Cfg);
    const size_t dlen = p.DataSize();
    if (dlen == 0) return;
    const float fb_loss = _Shared ? _Shared->latest_loss_rate : 0.0f;
    const uint8_t fb = static_cast<uint8_t>(std::min(fb_loss * 250.0f, 250.0f));

    // small packets (<256B payload) or oversized: direct send, no RS protection.
    // seq=0: small shards do NOT consume RS sequence space (RS batch seqs stay contiguous).
    if (dlen + 2 > T || dlen < 256) {
        Packet op; op._Length = 0;
        op.PushBack(p.Data());
        op.PushFrontLE(static_cast<uint16_t>(dlen));
        op.PushFrontLE(fb);
        op.PushFrontLE(BuildDword(0, kRsSmall));
        out.push_back(std::move(op));
        return;
    }

    // RS source shard: send immediately (zero batch delay), accumulate copy for repair
    uint32_t seq = (++_RsSeq) & 0xFFFFFF;
    if (seq == 0) seq = 1;
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
        SendRsRepair(_RsBatch, _RsBatchStartSeq, out);
        _RsBatch.clear();
        _RsHaveBatch = false;
    }
}

void RsCodec::SendRsRepair(const std::vector<Packet>& batch, uint32_t batch_start_seq,
                           std::vector<Packet>& out) {
    const uint32_t k = static_cast<uint32_t>(batch.size());
    if (k == 0) return;
    const uint32_t T = RsSymbolSize(_Cfg);
    float oh = _OverheadCtrl ? _OverheadCtrl->GetOverhead() : _Cfg.overhead;
    if (_OverheadCtrl && _Shared) _OverheadCtrl->Update(_Shared->latest_loss_rate);
    uint32_t m = static_cast<uint32_t>(std::ceil(k * oh));
    if (m > 255 - k) m = 255 - k;
    if (m == 0) return;

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
        Packet op; op._Length = 0;
        op.PushBack(std::span<const uint8_t>(repairs[j].data(), T));
        op.PushFrontLE(static_cast<uint8_t>(j));
        op.PushFrontLE(static_cast<uint8_t>(k));
        op.PushFrontLE(bid);
        op.PushFrontLE(fb);
        op.PushFrontLE(BuildDword(bid, kRsRepair));
        out.push_back(std::move(op));
    }
}

void RsCodec::DecodePacket(Packet&& p, std::vector<Packet>& out) {
    const uint32_t T = RsSymbolSize(_Cfg);
    if (p.DataSize() < 5) return;
    const uint32_t dw = p.PopFrontLE<uint32_t>();
    const uint32_t seq = dw & 0xFFFFFF;
    const uint8_t f = (dw >> 24) & 0xFF;

    const bool is_control = (f & kPing) || (f & kFeedback);
    if (!is_control && _LossPattern) {
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - _StartTime).count();
        if (_LossPattern->ShouldDrop(_TotalPackets, elapsed)) { _TotalPackets++; return; }
    }
    _TotalPackets++;
    const uint8_t fb = p.PopFrontLE<uint8_t>();
    if (_Shared && fb <= 250) { _Shared->latest_loss_rate = static_cast<float>(fb) / 250.0f; }
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
        if (p.DataSize() < 4) return;
        const uint16_t bid = p.PopFrontLE<uint16_t>();
        const uint8_t k = p.PopFrontLE<uint8_t>();
        const uint8_t idx = p.PopFrontLE<uint8_t>();
        if (p.DataSize() < T) return;
        auto& rep = _RsRepairs[bid & (kRsRepairSlots - 1)];
        if (!rep.valid || rep.bid != bid) { rep.bid = bid; rep.k = k; rep.valid = true; rep.data.clear(); }
        const size_t off = static_cast<size_t>(idx) * T;
        if (rep.data.size() < off + T) rep.data.resize(off + T);
        std::memcpy(rep.data.data() + off, p.Data().data(), T);
        rep.k = k;
        rep.time = std::chrono::steady_clock::now();
        RsTryRecover(bid, k);
        RsFlushDelivery(out);
        return;
    }

    // source shard (RS protected or small direct)
    if (p.DataSize() < 2) return;
    const uint16_t len = p.PopFrontLE<uint16_t>();
    if (len > p.DataSize()) return;
    if (f & kRsSmall) {
        Packet op; op._Length = 0;
        op.PushBack(std::span<const uint8_t>(p.Data().data(), len));
        out.push_back(std::move(op));
        return;
    }
    // RS source shard: cache padded payload, deliver in seq order.
    // Ring slot: O(1) insert, payload vector capacity reused across packets.
    auto& slot = _RsSrcs[seq & (kRsSrcSlots - 1)];
    slot.seq = seq;
    slot.valid = true;
    if (slot.payload.size() != T) slot.payload.resize(T);
    slot.payload[0] = static_cast<uint8_t>(len & 0xFF);
    slot.payload[1] = static_cast<uint8_t>((len >> 8) & 0xFF);
    std::memcpy(slot.payload.data() + 2, p.Data().data(), std::min<size_t>(len, p.DataSize()));
    if (!_RsHaveWatermark) { _RsHaveWatermark = true; _RsDeliverSeq = seq - 1; }
    RsFlushDelivery(out);
    AdvanceWatermark(std::chrono::steady_clock::now(), out);
}

void RsCodec::AdvanceWatermark(const std::chrono::steady_clock::time_point& now,
                               std::vector<Packet>& out) {
    // Global watermark stall guard: if delivery stalled past decode_timeout
    // (missing shard whose repairs never arrived), skip the gap and continue.
    if (!_RsHaveWatermark || now - _RsLastFlushTime <= std::chrono::milliseconds(_Cfg.decode_timeout_ms)) {
        return;
    }
    uint32_t gap_seq = 0;
    for (uint32_t i = 0; i < kRsSrcSlots; i++) {
        const auto& s = _RsSrcs[i];
        if (s.valid && s.seq >= _RsDeliverSeq + 1) { gap_seq = s.seq; break; }
    }
    if (gap_seq > _RsDeliverSeq + 1) {
        const uint32_t skipped = gap_seq - 1 - _RsDeliverSeq;
        _RsDeliverSeq = gap_seq - 1;
        BOOST_LOG_TRIVIAL(warning) << "RsCodec watermark stall: skipped " << skipped << " missing shards";
    }
    _RsLastFlushTime = now;

    // stale batch cleanup (throttled with the stall tick): force watermark
    // past batches that can no longer recover
    for (uint32_t i = 0; i < kRsRepairSlots; i++) {
        auto& r = _RsRepairs[i];
        if (!r.valid) continue;
        if (now - r.time > std::chrono::milliseconds(_Cfg.decode_timeout_ms)) {
            const uint32_t end = static_cast<uint32_t>(r.bid) + r.k;
            while (_RsHaveWatermark && _RsDeliverSeq + 1 < end) {
                const auto& s = _RsSrcs[(_RsDeliverSeq + 1) & (kRsSrcSlots - 1)];
                if (s.valid && s.seq == _RsDeliverSeq + 1) break;
                _RsDeliverSeq++;
            }
            r.valid = false;
            r.data.clear();
        }
    }
    RsFlushDelivery(out);
}

void RsCodec::RsFlushDelivery(std::vector<Packet>& out) {
    while (_RsHaveWatermark) {
        const uint32_t next = _RsDeliverSeq + 1;
        auto& slot = _RsSrcs[next & (kRsSrcSlots - 1)];
        if (!slot.valid || slot.seq != next) break;
        _RsLastFlushTime = std::chrono::steady_clock::now();
        const auto& payload = slot.payload;
        const uint16_t len = static_cast<uint16_t>(payload[0]) | (static_cast<uint16_t>(payload[1]) << 8);
        if (payload.size() < 2 + len) { slot.valid = false; _RsDeliverSeq++; continue; }
        Packet op; op._Length = 0;
        op.PushBack(std::span<const uint8_t>(payload.data() + 2, len));
        slot.valid = false;
        _RsDeliverSeq++;
        out.push_back(std::move(op));
    }
}

void RsCodec::RsTryRecover(uint32_t bid, uint32_t k) {
    const uint32_t T = RsSymbolSize(_Cfg);
    auto& rep = _RsRepairs[bid & (kRsRepairSlots - 1)];
    if (!rep.valid || rep.bid != bid) return;
    if (k == 0 || k > 255) return;

    // missing source shards in [bid, bid+k)
    std::vector<uint32_t> missing;
    for (uint32_t s = bid; s < bid + k; s++) {
        const auto& slot = _RsSrcs[s & (kRsSrcSlots - 1)];
        if (!(slot.valid && slot.seq == s)) missing.push_back(s);
    }
    if (missing.empty()) {
        rep.valid = false;
        rep.data.clear();
        return;
    }
    const size_t repairs_available = rep.data.size() / T;
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
        const size_t off = static_cast<size_t>(j) * T;
        if (off + T > rep.data.size()) break;
        known.push_back(std::vector<uint8_t>(rep_data + off, rep_data + off + T));
        rows.push_back(RS256::RepairRow(k, j));
    }
    if (known.size() < k) return;

    std::vector<std::vector<uint8_t>> out_src;
    if (!RS256::Decode(known, T, rows, out_src)) {
        BOOST_LOG_TRIVIAL(warning) << "RsCodec recover failed for batch " << bid;
        rep.valid = false;
        rep.data.clear();
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "RsCodec recovered " << missing.size() << " shards in batch " << bid;
    for (uint32_t s : missing) {
        auto& slot = _RsSrcs[s & (kRsSrcSlots - 1)];
        slot.seq = s;
        slot.valid = true;
        slot.payload = std::move(out_src[s - bid]);
    }
    rep.valid = false;
    rep.data.clear();
}

} // namespace gh
