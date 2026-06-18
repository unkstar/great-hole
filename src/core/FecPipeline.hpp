#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "FecConfig.hpp"
#include "Pipeline.hpp"

namespace gh {

// Shared state between encoder and decoder on the same side.
// Used for PING/FEEDBACK loop and RTT measurement.
struct FecSharedState {
    // Decoder writes when PING received, encoder reads & sends FEEDBACK then clears
    uint64_t pending_feedback_echo = 0;
    // Decoder updates when FEEDBACK received (RTT EWMA in microseconds)
    uint64_t rtt_ewma_us = 0;
    // Encoder writes timestamp of last sent PING
    uint64_t last_ping_sent_us = 0;
    // Consecutive PINGs lost (encoder increments on timeout)
    uint32_t consecutive_ping_lost = 0;
};

class FecPipeline : public Pipeline {
public:
    FecPipeline(boost::asio::io_context& io, std::shared_ptr<EndpointInput> in,
                const std::vector<std::shared_ptr<Filter>>& filters,
                std::shared_ptr<EndpointOutput> out, FecConfig cfg, bool is_encoder,
                std::shared_ptr<FecSharedState> shared = nullptr);
    ~FecPipeline() override = default;

protected:
    Omni::Fiber::Coroutine<void> Process() override;

private:
    // Wire format flags
    enum Flags : uint8_t {
        kWidth1B = 0,
        kWidth2B = 1 << 0,
        kFlagsMask = 0x0F,
        kPing = 1 << 4,
        kFeedback = 1 << 5,
        kRepeat = 1 << 6,
        kEcho = 1 << 7,
    };

    uint8_t BuildFlags() const;
    uint32_t BuildDword(uint32_t group_seq, uint8_t flags) const;

    // Encode helpers
    Omni::Fiber::Coroutine<void> SendBatch(std::vector<Packet>& batch);
    void BuildBlob(const std::vector<Packet>& batch, std::vector<uint8_t>& blob);
    Omni::Fiber::Coroutine<void> SendPing(uint64_t timestamp_us);
    Omni::Fiber::Coroutine<void> SendFeedback(uint64_t echo_us);

    // Decode helpers
    struct RingSlot {
        uint32_t group_seq = 0;
        uint32_t source_count = 0;
        uint32_t symbol_count = 0;
        std::chrono::steady_clock::time_point first_time;

        struct ShardEntry {
            std::vector<uint8_t> data;
            uint32_t esi;
        };
        std::vector<ShardEntry> shards;
    };

    std::vector<RingSlot> _RingBuffer;
    size_t _RingNext = 0;
    size_t _RingMask = 0;

    RingSlot& FindSlot(uint32_t group_seq);
    bool EvictStaleSlot();

    FecConfig _Cfg;
    bool _IsEncoder;

    // Encode state
    uint32_t _GroupSeq = 0;
    std::chrono::steady_clock::time_point _LastPingTime;

    // Shared state between encoder and decoder (PING/FEEDBACK)
    std::shared_ptr<FecSharedState> _Shared;
};

} // namespace gh
