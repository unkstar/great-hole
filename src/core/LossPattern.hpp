#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <string>

namespace gh {

// ==================== Loss Pattern Models ====================
//
// Simulates different packet loss patterns for testing adaptive overhead.
// All patterns implement ShouldDrop(seq) → bool.

class LossPattern {
public:
    virtual ~LossPattern() = default;

    // Returns true if the packet at this point should be dropped.
    // `seconds` is elapsed time since start (for time-based patterns).
    virtual bool ShouldDrop(uint64_t packet_seq, double seconds) = 0;

    // Returns the instantaneous loss probability (for display/logging).
    virtual float CurrentRate() const = 0;

    // Human-readable name.
    virtual std::string Name() const = 0;

    // Factory: create by pattern index.
    static std::unique_ptr<LossPattern> Create(uint8_t pattern, float rate, float rate2,
                                                uint32_t burst_or_period);
};

// ==================== Pattern 0: Disabled ====================
//
// Never drops packets.

class PatternDisabled : public LossPattern {
public:
    bool ShouldDrop(uint64_t, double) override { return false; }
    float CurrentRate() const override { return 0.0f; }
    std::string Name() const override { return "Disabled"; }
};

// ==================== Pattern 1: Bernoulli ====================
//
// Independent random loss with probability p.

class PatternBernoulli : public LossPattern {
public:
    PatternBernoulli(float p);
    bool ShouldDrop(uint64_t packet_seq, double seconds) override;
    float CurrentRate() const override { return _P; }
    std::string Name() const override { return "Bernoulli"; }

private:
    float _P;
    std::mt19937 _Rng;
    std::uniform_real_distribution<float> _Dist;
};

// ==================== Pattern 2: Gilbert ====================
//
// 2-state Markov: Good (0% loss), Bad (100% loss).
// Parameters derived from target loss rate and burst length.

class PatternGilbert : public LossPattern {
public:
    // rate = steady-state loss probability, burst_len = avg burst length in packets
    PatternGilbert(float target_rate, uint32_t target_burst_len);
    bool ShouldDrop(uint64_t packet_seq, double seconds) override;
    float CurrentRate() const override;
    std::string Name() const override { return "Gilbert"; }

private:
    bool _InBad;
    float _P_GB; // Good → Bad transition prob
    float _P_BG; // Bad → Good transition prob
    float _SteadyRate;
    std::mt19937 _Rng;
    std::uniform_real_distribution<float> _Dist;
};

// ==================== Pattern 3: Gilbert-Elliott ====================
//
// 2-state Markov with non-zero loss in both states.
// Good state: k% loss (background noise).
// Bad state:  h% loss (high loss, but < 100%).

class PatternGilbertElliott : public LossPattern {
public:
    // rate = steady-state loss probability, burst_len = avg burst length (pkts)
    // bg_loss = loss rate in Good state (default 0.01)
    PatternGilbertElliott(float target_rate, float bg_loss, uint32_t target_burst_len);
    bool ShouldDrop(uint64_t packet_seq, double seconds) override;
    float CurrentRate() const override;
    std::string Name() const override { return "GilbertElliott"; }

private:
    bool _InBad;
    float _P_GB;
    float _P_BG;
    float _K; // loss prob in Good
    float _H; // loss prob in Bad
    float _SteadyRate;
    std::mt19937 _Rng;
    std::uniform_real_distribution<float> _Dist;
};

// ==================== Pattern 4: Sinusoidal ====================
//
// Loss rate oscillates sinusoidally over time.
// loss_rate(t) = baseline + amplitude * sin(2π * t / period)

class PatternSinusoidal : public LossPattern {
public:
    // peak = max loss rate, trough = min loss rate, period_sec = cycle period
    PatternSinusoidal(float peak_rate, float trough_rate, uint32_t period_sec);
    bool ShouldDrop(uint64_t packet_seq, double seconds) override;
    float CurrentRate() const override;
    std::string Name() const override { return "Sinusoidal"; }

private:
    float _Baseline;
    float _Amplitude;
    uint32_t _PeriodSec;
    float _CurrentRate;
    std::mt19937 _Rng;
    std::uniform_real_distribution<float> _Dist;
};

// ==================== Pattern 5: Step ====================
//
// Loss rate jumps from before_rate to after_rate at step_time seconds.

class PatternStep : public LossPattern {
public:
    // before_rate, after_rate, step_time_sec
    PatternStep(float after_rate, float before_rate, uint32_t step_time_sec);
    bool ShouldDrop(uint64_t packet_seq, double seconds) override;
    float CurrentRate() const override;
    std::string Name() const override { return "Step"; }

private:
    float _BeforeRate;
    float _AfterRate;
    uint32_t _StepTimeSec;
    std::mt19937 _Rng;
    std::uniform_real_distribution<float> _Dist;
};

// ==================== Pattern 6: Congestion Wave ====================
//
// Triangular wave: loss rate linearly increases to peak then decreases.
// rate(t) = min_rate + (max_rate - min_rate) * triangle(t / period)
// triangle(x) = 2 * |2*(x mod 1) - 1|

class PatternCongestionWave : public LossPattern {
public:
    PatternCongestionWave(float peak_rate, float base_rate, uint32_t period_sec);
    bool ShouldDrop(uint64_t packet_seq, double seconds) override;
    float CurrentRate() const override;
    std::string Name() const override { return "CongestionWave"; }

private:
    static double triangle(double x);

    float _MinRate;
    float _MaxRate;
    uint32_t _PeriodSec;
    float _CurrentRate;
    std::mt19937 _Rng;
    std::uniform_real_distribution<float> _Dist;
};

} // namespace gh
