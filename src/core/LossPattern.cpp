#include "LossPattern.hpp"

#include <algorithm>
#include <numbers>

namespace gh {

// ==================== Factory ====================

std::unique_ptr<LossPattern> LossPattern::Create(uint8_t pattern, float rate, float rate2,
                                                  uint32_t burst_or_period) {
    switch (pattern) {
    case 0:
        return std::make_unique<PatternDisabled>();
    case 1:
        return std::make_unique<PatternBernoulli>(rate);
    case 2:
        return std::make_unique<PatternGilbert>(rate, burst_or_period);
    case 3:
        return std::make_unique<PatternGilbertElliott>(rate, rate2, burst_or_period);
    case 4:
        return std::make_unique<PatternSinusoidal>(rate, rate2, burst_or_period);
    case 5:
        return std::make_unique<PatternStep>(rate, rate2, burst_or_period);
    case 6:
        return std::make_unique<PatternCongestionWave>(rate, rate2, burst_or_period);
    default:
        return std::make_unique<PatternDisabled>();
    }
}

// ==================== Pattern 1: Bernoulli ====================

PatternBernoulli::PatternBernoulli(float p)
    : _P(std::clamp(p, 0.0f, 1.0f)), _Rng(std::random_device{}()), _Dist(0.0f, 1.0f) {}

bool PatternBernoulli::ShouldDrop(uint64_t /*packet_seq*/, double /*seconds*/) {
    return _Dist(_Rng) < _P;
}

// ==================== Pattern 2: Gilbert ====================

PatternGilbert::PatternGilbert(float target_rate, uint32_t target_burst_len)
    : _InBad(false), _Rng(std::random_device{}()), _Dist(0.0f, 1.0f) {
    target_rate = std::clamp(target_rate, 0.0f, 1.0f);
    _SteadyRate = target_rate;
    if (target_burst_len < 1) target_burst_len = 1;

    // r = 1 / avg_burst_len (probability of exiting Bad each packet)
    // p = r * π_bad / (1 - π_bad), where π_bad = steady_state_loss
    float r = 1.0f / static_cast<float>(target_burst_len);
    if (target_rate >= 0.999f) {
        _P_BG = 0.0f;
        _P_GB = 1.0f;
    } else if (target_rate <= 0.001f) {
        _P_BG = 1.0f;
        _P_GB = 0.0f;
    } else {
        _P_BG = r;
        _P_GB = r * target_rate / (1.0f - target_rate);
        // Clamp to valid probabilities
        _P_BG = std::clamp(_P_BG, 0.0f, 1.0f);
        _P_GB = std::clamp(_P_GB, 0.0f, 1.0f);
    }
}

bool PatternGilbert::ShouldDrop(uint64_t /*packet_seq*/, double /*seconds*/) {
    float r = _Dist(_Rng);
    if (_InBad) {
        if (r < _P_BG) _InBad = false; // transition to Good
    } else {
        if (r < _P_GB) _InBad = true; // transition to Bad
    }
    return _InBad;
}

float PatternGilbert::CurrentRate() const { return _SteadyRate; }

// ==================== Pattern 3: Gilbert-Elliott ====================

PatternGilbertElliott::PatternGilbertElliott(float target_rate, float bg_loss,
                                               uint32_t target_burst_len)
    : _InBad(false), _Rng(std::random_device{}()), _Dist(0.0f, 1.0f) {
    target_rate = std::clamp(target_rate, 0.0f, 1.0f);
    _K = std::clamp(bg_loss, 0.0f, 0.3f);
    _SteadyRate = target_rate;
    if (target_burst_len < 1) target_burst_len = 1;

    float r = 1.0f / static_cast<float>(target_burst_len);
    if (target_rate <= _K + 0.001f) {
        _P_BG = 1.0f;
        _P_GB = 0.0f;
        _H = _K;
    } else {
        // π_bad = (target_rate - k) / (h - k), choose h = 0.8 (typical high loss)
        _H = 0.8f;
        float pi_bad = (target_rate - _K) / (_H - _K);
        pi_bad = std::clamp(pi_bad, 0.0f, 1.0f);
        _P_BG = r;
        if (pi_bad < 0.999f) {
            _P_GB = r * pi_bad / (1.0f - pi_bad);
        } else {
            _P_GB = 1.0f;
        }
        _P_BG = std::clamp(_P_BG, 0.0f, 1.0f);
        _P_GB = std::clamp(_P_GB, 0.0f, 1.0f);
    }
}

bool PatternGilbertElliott::ShouldDrop(uint64_t /*packet_seq*/, double /*seconds*/) {
    float r = _Dist(_Rng);
    if (_InBad) {
        if (r < _P_BG) _InBad = false;
    } else {
        if (r < _P_GB) _InBad = true;
    }
    // Within-state loss
    float loss_prob = _InBad ? _H : _K;
    return _Dist(_Rng) < loss_prob;
}

float PatternGilbertElliott::CurrentRate() const { return _SteadyRate; }

// ==================== Pattern 4: Sinusoidal ====================

PatternSinusoidal::PatternSinusoidal(float peak_rate, float trough_rate, uint32_t period_sec)
    : _Baseline(std::clamp(trough_rate, 0.0f, 1.0f)),
      _Amplitude(std::clamp(peak_rate - trough_rate, 0.0f, 1.0f)),
      _PeriodSec(period_sec > 0 ? period_sec : 60), _CurrentRate(_Baseline),
      _Rng(std::random_device{}()), _Dist(0.0f, 1.0f) {}

bool PatternSinusoidal::ShouldDrop(uint64_t /*packet_seq*/, double seconds) {
    double phase = 2.0 * std::numbers::pi * seconds / static_cast<double>(_PeriodSec);
    _CurrentRate = _Baseline + _Amplitude * static_cast<float>(std::sin(phase));
    _CurrentRate = std::max(0.0f, _CurrentRate);
    return _Dist(_Rng) < _CurrentRate;
}

float PatternSinusoidal::CurrentRate() const { return _CurrentRate; }

// ==================== Pattern 5: Step ====================

PatternStep::PatternStep(float after_rate, float before_rate, uint32_t step_time_sec)
    : _BeforeRate(std::clamp(before_rate, 0.0f, 1.0f)),
      _AfterRate(std::clamp(after_rate, 0.0f, 1.0f)),
      _StepTimeSec(step_time_sec > 0 ? step_time_sec : 30),
      _Rng(std::random_device{}()), _Dist(0.0f, 1.0f) {}

bool PatternStep::ShouldDrop(uint64_t /*packet_seq*/, double seconds) {
    float rate = (seconds >= static_cast<double>(_StepTimeSec)) ? _AfterRate : _BeforeRate;
    return _Dist(_Rng) < rate;
}

float PatternStep::CurrentRate() const { return _AfterRate; }

// ==================== Pattern 6: Congestion Wave ====================

PatternCongestionWave::PatternCongestionWave(float peak_rate, float base_rate,
                                               uint32_t period_sec)
    : _MinRate(std::clamp(base_rate, 0.0f, 1.0f)),
      _MaxRate(std::clamp(peak_rate, base_rate, 1.0f)),
      _PeriodSec(period_sec > 0 ? period_sec : 120), _CurrentRate(_MinRate),
      _Rng(std::random_device{}()), _Dist(0.0f, 1.0f) {}

double PatternCongestionWave::triangle(double x) {
    double t = std::fmod(x, 1.0);
    if (t < 0.0) t += 1.0;
    return 2.0 * std::abs(2.0 * t - 1.0);
}

bool PatternCongestionWave::ShouldDrop(uint64_t /*packet_seq*/, double seconds) {
    double x = seconds / static_cast<double>(_PeriodSec);
    double tri = triangle(x);
    _CurrentRate =
        _MinRate + (_MaxRate - _MinRate) * static_cast<float>(1.0 - tri); // starts low, peaks at middle
    return _Dist(_Rng) < _CurrentRate;
}

float PatternCongestionWave::CurrentRate() const { return _CurrentRate; }

} // namespace gh
