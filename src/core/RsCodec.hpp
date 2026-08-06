#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "FecCodec.hpp"
#include "FecConfig.hpp"

namespace gh {

// RS (Vandermonde GF256) codec strategy.
//
// Source shards are sent immediately (zero batch latency); repairs are
// generated per batch window and flushed via Tick/OnPacket batch deadlines.
// Decode delivers in seq order through a watermark; missing shards are
// recovered from repairs (Vandermonde submatrix inversion) or skipped by the
// watermark stall guard after decode_timeout.
class RsCodec final : public FecCodec {
public:
    RsCodec(const FecConfig& cfg, bool is_encoder, std::shared_ptr<FecSharedState> shared,
            AdaptiveOverhead* overhead_ctrl, LossPattern* loss_pattern);
    ~RsCodec() override = default;

    const char* Name() const override { return "rs"; }
    void OnPacket(Packet&& p, std::vector<Packet>& out) override;
    void Tick(std::vector<Packet>& out) override;
    size_t MaxBatch() const override { return _Cfg.max_batch; }

private:
    // Fixed slot rings: O(1) lookup by seq/bid & (N-1) with in-slot seq
    // validation. Slot count >> max in-flight shards/batches (in-flight ≈
    // RTT × rate, < 1k at 100Mbps over the test link). Payload vectors
    // retain capacity across packets — zero steady-state allocation.
    static constexpr uint32_t kRsSrcSlots = 4096;    // power of 2
    static constexpr uint32_t kRsRepairSlots = 256;  // power of 2
    struct RsSrcSlot {
        uint32_t seq = 0;
        bool valid = false;
        std::vector<uint8_t> payload;  // T-byte [len|data|pad], capacity reused
    };
    struct RsRepairSlot {
        uint32_t bid = 0;  // full 24-bit batch start seq
        uint8_t k = 0;
        bool valid = false;
        std::chrono::steady_clock::time_point time;
        std::vector<uint8_t> data;  // idx*T slots, capacity reused
    };

    const FecConfig& _Cfg;
    bool _IsEncoder;
    std::shared_ptr<FecSharedState> _Shared;
    AdaptiveOverhead* _OverheadCtrl;
    LossPattern* _LossPattern;
    uint64_t _TotalPackets = 0;
    std::chrono::steady_clock::time_point _StartTime = std::chrono::steady_clock::now();

    // encode state
    uint32_t _RsSeq = 0;  // source shard sequence (24-bit)
    std::vector<Packet> _RsBatch;  // windowed source packets (copies)
    uint32_t _RsBatchStartSeq = 0;
    std::chrono::steady_clock::time_point _RsBatchStartTime;
    bool _RsHaveBatch = false;

    // decode state
    std::array<RsSrcSlot, kRsSrcSlots> _RsSrcs;
    std::array<RsRepairSlot, kRsRepairSlots> _RsRepairs;
    uint32_t _RsDeliverSeq = 0;
    bool _RsHaveWatermark = false;
    std::chrono::steady_clock::time_point _RsLastFlushTime = std::chrono::steady_clock::now();

    // small-packet dedup: redundancy copies of one small packet arrive
    // back-to-back; deliver only the first (like the lcrq REPEAT seen check).
    std::vector<uint8_t> _LastSmall;
    bool _HaveLastSmall = false;

    uint8_t BuildFlags() const;
    uint32_t BuildDword(uint32_t seq, uint8_t flags) const;
    static uint32_t RsSymbolSize(const FecConfig& cfg);

    void EncodePacket(Packet&& p, std::vector<Packet>& out);
    void SendRsRepair(const std::vector<Packet>& batch, uint32_t batch_start_seq,
                      std::vector<Packet>& out);
    void DecodePacket(Packet&& p, std::vector<Packet>& out);
    void AdvanceWatermark(const std::chrono::steady_clock::time_point& now, std::vector<Packet>& out);
    void RsFlushDelivery(std::vector<Packet>& out);
    void RsTryRecover(uint32_t bid, uint32_t k);
};

} // namespace gh
