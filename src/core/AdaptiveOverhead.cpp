#include "AdaptiveOverhead.hpp"

#include <algorithm>
#include <numeric>

namespace gh {

// ==================== Factory ====================

std::unique_ptr<AdaptiveOverhead> AdaptiveOverhead::Create(uint8_t algo, float initial_overhead,
                                                            float max_overhead, float safety,
                                                            float alpha) {
    switch (algo) {
    case 0:
        return std::make_unique<AlgoStatic>(initial_overhead);
    case 1:
        return std::make_unique<AlgoEwmaStatic>(initial_overhead, max_overhead, alpha, safety);
    case 2:
        return std::make_unique<AlgoEwmaDynamic>(initial_overhead, max_overhead, alpha, 0.2f,
                                                  safety * 0.6f, 2.0f);
    case 3: {
        float Kp = 1.5f, Ki = 0.8f, target = safety * 0.5f, i_max = 0.3f;
        return std::make_unique<AlgoPI>(initial_overhead, max_overhead, Kp, Ki, target, i_max);
    }
    case 4: {
        float min_oh = safety, lambda_up = 0.50f, lambda_down = 0.05f;
        uint32_t n_stable = 20;
        return std::make_unique<AlgoMIMD>(initial_overhead, max_overhead, min_oh, lambda_up,
                                           lambda_down, n_stable);
    }
    case 5: {
        size_t window = 64;
        int pct = 95;
        return std::make_unique<AlgoQuantile>(initial_overhead, max_overhead, window, pct, safety);
    }
    case 6: {
        float a_slow = 0.1f, a_fast = 0.6f, b_thresh = 0.05f;
        return std::make_unique<AlgoBurstAware>(initial_overhead, max_overhead, a_slow, a_fast,
                                                 b_thresh, safety);
    }
    case 7: {
        float delta = 0.02f;
        uint32_t eval = 50;
        return std::make_unique<AlgoGradient>(initial_overhead, max_overhead, delta, eval);
    }
    default:
        return std::make_unique<AlgoEwmaStatic>(initial_overhead, max_overhead, alpha, safety);
    }
}

// ==================== Algorithm 1: EWMA + Static Safety ====================

AlgoEwmaStatic::AlgoEwmaStatic(float initial_overhead, float max_overhead, float alpha,
                                 float safety)
    : _MaxOverhead(max_overhead), _Alpha(alpha), _Safety(safety), _EwmaLoss(0.0f),
      _Initialized(false), _SampleCount(0) {
    // Seed EWMA with initial overhead assumption: overhead ≈ loss/(1-loss) + safety
    // Reverse: loss ≈ (overhead - safety) / (1 + overhead - safety)
    float implied_loss = (initial_overhead - safety) / (1.0f + initial_overhead - safety);
    if (implied_loss > 0.0f) {
        _EwmaLoss = implied_loss;
        _Initialized = true;
    }
}

void AlgoEwmaStatic::Update(float loss_sample) {
    loss_sample = std::clamp(loss_sample, 0.0f, 1.0f);
    if (!_Initialized) {
        _EwmaLoss = loss_sample;
        _Initialized = true;
    } else {
        _EwmaLoss = _Alpha * loss_sample + (1.0f - _Alpha) * _EwmaLoss;
    }
    _SampleCount++;
}

float AlgoEwmaStatic::GetOverhead() const {
    if (_EwmaLoss >= 0.999f) return _MaxOverhead;
    float oh = _EwmaLoss / (1.0f - _EwmaLoss) + _Safety;
    return std::clamp(oh, 0.0f, _MaxOverhead);
}

void AlgoEwmaStatic::Reset() {
    _EwmaLoss = 0.0f;
    _Initialized = false;
    _SampleCount = 0;
}

// ==================== Algorithm 2: EWMA + Dynamic Safety ====================

AlgoEwmaDynamic::AlgoEwmaDynamic(float initial_overhead, float max_overhead, float alpha,
                                   float beta, float safety_base, float gamma)
    : _MaxOverhead(max_overhead), _Alpha(alpha), _Beta(beta), _SafetyBase(safety_base),
      _Gamma(gamma), _EwmaLoss(0.0f), _Variance(0.0f), _Initialized(false) {
    float implied_loss =
        (initial_overhead - safety_base) / (1.0f + initial_overhead - safety_base);
    if (implied_loss > 0.0f) {
        _EwmaLoss = implied_loss;
        _Initialized = true;
    }
}

void AlgoEwmaDynamic::Update(float loss_sample) {
    loss_sample = std::clamp(loss_sample, 0.0f, 1.0f);
    if (!_Initialized) {
        _EwmaLoss = loss_sample;
        _Variance = 0.0f;
        _Initialized = true;
        return;
    }
    float old_ewma = _EwmaLoss;
    _EwmaLoss = _Alpha * loss_sample + (1.0f - _Alpha) * _EwmaLoss;
    float deviation = loss_sample - old_ewma;
    _Variance = _Beta * (deviation * deviation) + (1.0f - _Beta) * _Variance;
}

float AlgoEwmaDynamic::GetOverhead() const {
    if (_EwmaLoss >= 0.999f) return _MaxOverhead;
    float dynamic_safety = _SafetyBase + _Gamma * std::sqrt(_Variance);
    float oh = _EwmaLoss / (1.0f - _EwmaLoss) + dynamic_safety;
    return std::clamp(oh, 0.0f, _MaxOverhead);
}

void AlgoEwmaDynamic::Reset() {
    _EwmaLoss = 0.0f;
    _Variance = 0.0f;
    _Initialized = false;
}

// ==================== Algorithm 3: PI Controller ====================

AlgoPI::AlgoPI(float initial_overhead, float max_overhead, float Kp, float Ki, float target_loss,
               float i_max)
    : _MaxOverhead(max_overhead), _InitialOverhead(initial_overhead), _Kp(Kp), _Ki(Ki),
      _TargetLoss(target_loss), _IMax(i_max), _Integral(0.0f), _Overhead(initial_overhead) {}

void AlgoPI::Update(float loss_sample) {
    loss_sample = std::clamp(loss_sample, 0.0f, 1.0f);
    float error = _TargetLoss - loss_sample; // positive when loss < target
    _Integral = std::clamp(_Integral + error, -_IMax, _IMax);
    float overhead = _Kp * (-error) + _Ki * _Integral;
    // Floor at the configured initial overhead: the formula alone goes
    // negative at loss=0 (cold start with no measurement yet), which clamped
    // to 0 and forced a ~60-batch re-climb — an eternity on a slow TCP
    // stream, leaving batches with m=0-1 repairs.
    _Overhead = std::clamp(overhead, _InitialOverhead, _MaxOverhead);
}

float AlgoPI::GetOverhead() const { return _Overhead; }

void AlgoPI::Reset() {
    _Integral = 0.0f;
    _Overhead = 0.15f;
}

// ==================== Algorithm 4: MIMD ====================

AlgoMIMD::AlgoMIMD(float initial_overhead, float max_overhead, float min_overhead,
                     float lambda_up, float lambda_down, uint32_t n_stable)
    : _MaxOverhead(max_overhead), _MinOverhead(min_overhead), _LambdaUp(lambda_up),
      _LambdaDown(lambda_down), _NStable(n_stable), _Overhead(initial_overhead), _SuccessStreak(0) {
}

void AlgoMIMD::Update(float loss_sample) {
    loss_sample = std::clamp(loss_sample, 0.0f, 1.0f);
    if (loss_sample > 0.0f) {
        // Loss detected: fast increase
        _Overhead *= (1.0f + _LambdaUp);
        if (_Overhead > _MaxOverhead) _Overhead = _MaxOverhead;
        _SuccessStreak = 0;
    } else {
        _SuccessStreak++;
        if (_SuccessStreak >= _NStable) {
            _Overhead *= (1.0f - _LambdaDown);
            if (_Overhead < _MinOverhead) _Overhead = _MinOverhead;
        }
    }
}

float AlgoMIMD::GetOverhead() const { return _Overhead; }

void AlgoMIMD::Reset() {
    _Overhead = 0.15f;
    _SuccessStreak = 0;
}

// ==================== Algorithm 5: Quantile Target ====================

AlgoQuantile::AlgoQuantile(float initial_overhead, float max_overhead, size_t window_size,
                             int percentile, float safety)
    : _MaxOverhead(max_overhead), _WindowSize(window_size), _Percentile(percentile),
      _Safety(safety) {
    // Seed with implied loss
    float implied_loss = (initial_overhead - safety) / (1.0f + initial_overhead - safety);
    if (implied_loss > 0.0f) {
        for (size_t i = 0; i < _WindowSize; i++)
            _Window.push_back(implied_loss);
    }
}

void AlgoQuantile::Update(float loss_sample) {
    loss_sample = std::clamp(loss_sample, 0.0f, 1.0f);
    if (_Window.size() >= _WindowSize) _Window.pop_front();
    _Window.push_back(loss_sample);
}

float AlgoQuantile::computePercentile() const {
    if (_Window.empty()) return 0.0f;
    std::vector<float> sorted(_Window.begin(), _Window.end());
    std::sort(sorted.begin(), sorted.end());
    size_t idx = static_cast<size_t>(std::ceil(_Percentile / 100.0f * sorted.size())) - 1;
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
}

float AlgoQuantile::GetOverhead() const {
    float pct_loss = computePercentile();
    if (pct_loss >= 0.999f) return _MaxOverhead;
    float oh = pct_loss / (1.0f - pct_loss) + _Safety;
    return std::clamp(oh, 0.0f, _MaxOverhead);
}

void AlgoQuantile::Reset() { _Window.clear(); }

// ==================== Algorithm 6: Burst-Aware EWMA ====================

AlgoBurstAware::AlgoBurstAware(float initial_overhead, float max_overhead, float alpha_slow,
                                 float alpha_fast, float burst_threshold, float safety)
    : _MaxOverhead(max_overhead), _AlphaSlow(alpha_slow), _AlphaFast(alpha_fast),
      _BurstThreshold(burst_threshold), _Safety(safety), _BgEwma(0.01f), _BurstEwma(0.0f),
      _InBurst(false), _Initialized(false) {}

void AlgoBurstAware::Update(float loss_sample) {
    loss_sample = std::clamp(loss_sample, 0.0f, 1.0f);
    if (!_Initialized) {
        _BgEwma = loss_sample;
        _Initialized = true;
        return;
    }

    if (loss_sample > _BgEwma + _BurstThreshold) {
        // Burst detected
        if (!_InBurst) {
            _BurstEwma = loss_sample;
            _InBurst = true;
        } else {
            _BurstEwma = _AlphaFast * loss_sample + (1.0f - _AlphaFast) * _BurstEwma;
        }
    } else {
        _InBurst = false;
        _BurstEwma = _AlphaSlow * _BurstEwma; // decay burst estimate
    }
    _BgEwma = _AlphaSlow * loss_sample + (1.0f - _AlphaSlow) * _BgEwma;
}

float AlgoBurstAware::GetOverhead() const {
    float effective_loss = std::max(_BgEwma, _BurstEwma);
    if (effective_loss >= 0.999f) return _MaxOverhead;
    float oh = effective_loss / (1.0f - effective_loss) + _Safety;
    return std::clamp(oh, 0.0f, _MaxOverhead);
}

void AlgoBurstAware::Reset() {
    _BgEwma = 0.01f;
    _BurstEwma = 0.0f;
    _InBurst = false;
    _Initialized = false;
}

// ==================== Algorithm 7: Gradient Throughput Opt ====================

AlgoGradient::AlgoGradient(float initial_overhead, float max_overhead, float delta,
                             uint32_t eval_interval)
    : _MaxOverhead(max_overhead), _Delta(delta), _EvalInterval(eval_interval),
      _Overhead(initial_overhead), _BestOverhead(initial_overhead), _BestThroughput(0.0f),
      _LastLoss(0.0f), _Tick(0), _Direction(1) {}

void AlgoGradient::Update(float loss_sample) {
    loss_sample = std::clamp(loss_sample, 0.0f, 1.0f);
    _LastLoss = loss_sample;
    _Tick++;
    if (_Tick % _EvalInterval != 0) return;

    // Evaluate: throughput = (1 - overhead) * (1 - loss)
    float current_throughput = (1.0f - _Overhead) * (1.0f - loss_sample);
    if (current_throughput > _BestThroughput) {
        _BestThroughput = current_throughput;
        _BestOverhead = _Overhead;
    }

    // Explore: try adjusting overhead
    float candidate = _Overhead + _Direction * _Delta;
    if (candidate < 0.0f || candidate > _MaxOverhead) {
        _Direction = -_Direction;   // reverse
        candidate = _Overhead + _Direction * _Delta;
    }
    _Overhead = std::clamp(candidate, 0.0f, _MaxOverhead);

    // Every 10 evaluations, check if current is better than best (exploit)
    if (_Tick % (10 * _EvalInterval) == 0 && _BestThroughput > 0) {
        _Overhead = _BestOverhead;
        _Direction = (_Direction > 0) ? -1 : 1; // try other direction
    }
}

float AlgoGradient::GetOverhead() const { return _Overhead; }

void AlgoGradient::Reset() {
    _Overhead = 0.15f;
    _BestOverhead = 0.15f;
    _BestThroughput = 0.0f;
    _LastLoss = 0.0f;
    _Tick = 0;
    _Direction = 1;
}

} // namespace gh
