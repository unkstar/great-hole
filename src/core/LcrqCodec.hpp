#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "FecCodec.hpp"
#include "FecConfig.hpp"

namespace gh {

// RaptorQ (lcrq) codec strategy — the original mainline FEC implementation.
//
// Encode batches packets into a blob and emits K + ceil(K×overhead) RaptorQ
// symbols; single-packet batches use the REPEAT redundancy path. Decode
// buffers shards by group_seq in a direct-indexed ring (O(1) slot lookup,
// no linear scan) and decodes when enough symbols arrive.
class LcrqCodec final : public FecCodec {
public:
    LcrqCodec(const FecConfig& cfg, bool is_encoder, std::shared_ptr<FecSharedState> shared,
              AdaptiveOverhead* overhead_ctrl, LossPattern* loss_pattern);
    ~LcrqCodec() override = default;

    const char* Name() const override { return "lcrq"; }
    void OnPacket(Packet&& p, std::vector<Packet>& out) override;
    void Tick(std::vector<Packet>& out) override;
    size_t MaxBatch() const override { return _Cfg.max_batch; }

private:
    struct RingSlot {
        uint32_t group_seq = 0;
        uint32_t source_count = 0;
        uint32_t symbol_count = 0;
        uint32_t max_esi = 0;
        std::chrono::steady_clock::time_point first_time;
        // shards indexed by esi (O(1) lookup, no linear scan); empty = missing
        std::vector<std::vector<uint8_t>> shards;

        void Reset() {
            group_seq = 0;
            source_count = 0;
            symbol_count = 0;
            max_esi = 0;
            for (auto& s : shards) s.clear();  // retain per-shard capacity
        }
    };

    const FecConfig& _Cfg;
    bool _IsEncoder;
    std::shared_ptr<FecSharedState> _Shared;
    AdaptiveOverhead* _OverheadCtrl;
    LossPattern* _LossPattern;
    uint64_t _TotalPackets = 0;
    std::chrono::steady_clock::time_point _StartTime = std::chrono::steady_clock::now();

    // encode state
    uint32_t _GroupSeq = 0;
    std::vector<Packet> _Batch;  // accumulating batch, reserved to max_batch
    std::chrono::steady_clock::time_point _BatchStart;
    bool _HaveBatch = false;

    // decode state — direct-indexed ring (group_seq & mask) with in-slot
    // validation; shard entry buffers retain capacity across reuse.
    std::vector<RingSlot> _RingBuffer;
    size_t _RingMask = 0;

    uint32_t _LossGroupCount = 0;
    uint32_t _LossFailCount = 0;

    uint8_t BuildFlags() const;
    uint32_t BuildDword(uint32_t group_seq, uint8_t flags) const;

    void EncodePacket(Packet&& p, std::vector<Packet>& out);
    void SendBatch(std::vector<Packet>& out);
    void BuildBlob(const std::vector<Packet>& batch, std::vector<uint8_t>& blob);
    void DecodePacket(Packet&& p, std::vector<Packet>& out);
    RingSlot& SlotFor(uint32_t group_seq);
    bool EvictStaleSlot();
};

} // namespace gh
