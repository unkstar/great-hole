#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "FecCodec.hpp"
#include "FecConfig.hpp"
#include "Pipeline.hpp"

namespace gh {

class AdaptiveOverhead;
class FecStats;
class LossPattern;

// Transport layer for FEC. Owns all reads/writes and fiber scheduling; the
// FEC algorithm is a pluggable FecCodec strategy (pure synchronous processor,
// no I/O, no suspension). A dedicated reader fiber keeps an async read
// pending on the input at all times (descriptor always registered with the
// reactor); a worker fiber drains the deque and calls codec->OnPacket, then
// writes the codec's output.
class FecPipeline : public Pipeline {
public:
    FecPipeline(boost::asio::io_context& io, std::shared_ptr<EndpointInput> in,
                const std::vector<std::shared_ptr<Filter>>& filters,
                std::shared_ptr<EndpointOutput> out, FecConfig cfg, bool is_encoder,
                std::shared_ptr<FecSharedState> shared = nullptr);
    ~FecPipeline() override;

protected:
    Omni::Fiber::Coroutine<void> Process() override;

private:
    uint8_t BuildFlags() const;
    uint32_t BuildDword(uint32_t group_seq, uint8_t flags) const;

    // Transport control packets
    Omni::Fiber::Coroutine<void> SendPing(uint64_t timestamp_us);
    Omni::Fiber::Coroutine<void> SendFeedback(uint64_t echo_us);

    FecConfig _Cfg;
    bool _IsEncoder;
    FecStats* _Stats = nullptr;  // 统计系统 (可空, FecConfig::stats 注入)

    // Adaptive overhead controller (encode side)
    std::unique_ptr<AdaptiveOverhead> _OverheadCtrl;

    // Active loss pattern for testing (decode side)
    std::unique_ptr<LossPattern> _LossPattern;

    // Transport state
    uint32_t _GroupSeq = 0;
    std::chrono::steady_clock::time_point _LastPingTime;

    // Shared state between encoder and decoder (PING/FEEDBACK)
    std::shared_ptr<FecSharedState> _Shared;
};

} // namespace gh
