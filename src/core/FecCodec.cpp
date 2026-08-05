#include "FecCodec.hpp"

#include <memory>
#include <utility>

#include "FecConfig.hpp"
#include "LcrqCodec.hpp"
#include "RsCodec.hpp"

namespace gh {

std::unique_ptr<FecCodec> FecCodec::Create(const FecConfig& cfg, bool is_encoder,
                                           std::shared_ptr<FecSharedState> shared,
                                           AdaptiveOverhead* overhead_ctrl,
                                           LossPattern* loss_pattern) {
    if (cfg.fec_codec == "rs") {
        return std::make_unique<RsCodec>(cfg, is_encoder, std::move(shared), overhead_ctrl,
                                         loss_pattern);
    }
    return std::make_unique<LcrqCodec>(cfg, is_encoder, std::move(shared), overhead_ctrl,
                                       loss_pattern);
}

} // namespace gh
