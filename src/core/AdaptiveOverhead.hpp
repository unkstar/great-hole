#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <queue>
#include <random>
#include <stdexcept>
#include <string>

namespace gh {

// ==================== Adaptive Overhead Algorithms ====================
//
// All algorithms receive loss sample [0..1] via Update() and return
// recommended overhead [0..max_overhead] via GetOverhead().
//
// Base class: algorithm 0 (Static) — always returns fixed config value.

class AdaptiveOverhead {
public:
    virtual ~AdaptiveOverhead() = default;

    // Feed a new loss rate sample [0..1]
    virtual void Update(float loss_sample) = 0;

    // Get current recommended overhead
    virtual float GetOverhead() const = 0;

    // Reset state
    virtual void Reset() = 0;

    // Human-readable name
    virtual std::string Name() const = 0;

    // Factory: create by algo index
    static std::unique_ptr<AdaptiveOverhead> Create(uint8_t algo, float initial_overhead,
                                                     float max_overhead,
                                                     float safety = 0.01f,
                                                     float alpha = 0.1f);
};

// ==================== Algorithm 0: Static ====================
//
// Never adapts. Always returns the configured overhead.

class AlgoStatic : public AdaptiveOverhead {
public:
    explicit AlgoStatic(float overhead) : _Overhead(overhead) {}
    void Update(float) override {}
    float GetOverhead() const override { return _Overhead; }
    void Reset() override {}
    std::string Name() const override { return "Static"; }

private:
    float _Overhead;
};

// ==================== Algorithm 1: EWMA + Static Safety ====================
//
// L_ewma[t] = alpha * L_sample + (1-alpha) * L_ewma[t-1]
// overhead  = L_ewma / (1 - L_ewma) + safety_margin
// Clamped to [0, max_overhead].

class AlgoEwmaStatic : public AdaptiveOverhead {
public:
    AlgoEwmaStatic(float initial_overhead, float max_overhead, float alpha, float safety);
    void Update(float loss_sample) override;
    float GetOverhead() const override;
    void Reset() override;
    std::string Name() const override { return "EWMA+StaticSafety"; }

private:
    float _MaxOverhead;
    float _Alpha;
    float _Safety;
    float _EwmaLoss;
    bool _Initialized;
    uint32_t _SampleCount;
};

// ==================== Algorithm 2: EWMA + Dynamic Safety ====================
//
// Safety margin scales with stddev of loss samples.
// variance[t] = beta * (sample - mean)² + (1-beta) * variance[t-1]
// safety      = safety_base + gamma * sqrt(variance)
// overhead    = ewma / (1 - ewma) + safety

class AlgoEwmaDynamic : public AdaptiveOverhead {
public:
    AlgoEwmaDynamic(float initial_overhead, float max_overhead, float alpha, float beta,
                    float safety_base, float gamma);
    void Update(float loss_sample) override;
    float GetOverhead() const override;
    void Reset() override;
    std::string Name() const override { return "EWMA+DynamicSafety"; }

private:
    float _MaxOverhead;
    float _Alpha;
    float _Beta;
    float _SafetyBase;
    float _Gamma;
    float _EwmaLoss;
    float _Variance;
    bool _Initialized;
};

// ==================== Algorithm 3: PI Controller ====================
//
// error    = L_target - L_measured  (positive when loss is below target)
// integral = clamp(integral + error * dt, -i_max, i_max)
// overhead = Kp * (-error) + Ki * integral
//
// Uses fixed dt=1 (per-sample).

class AlgoPI : public AdaptiveOverhead {
public:
    AlgoPI(float initial_overhead, float max_overhead, float Kp, float Ki, float target_loss,
           float i_max);
    void Update(float loss_sample) override;
    float GetOverhead() const override;
    void Reset() override;
    std::string Name() const override { return "PI"; }

private:
    float _MaxOverhead;
    float _InitialOverhead;
    float _Kp;
    float _Ki;
    float _TargetLoss;
    float _IMax;
    float _Integral;
    float _Overhead;
};

// ==================== Algorithm 4: MIMD ====================
//
// On decode failure: overhead *= (1 + lambda_up)      — fast increase
// On success streak > N_stable: overhead *= (1 - lambda_down) — slow decrease
// Clamp to [min_overhead, max_overhead].

class AlgoMIMD : public AdaptiveOverhead {
public:
    AlgoMIMD(float initial_overhead, float max_overhead, float min_overhead, float lambda_up,
             float lambda_down, uint32_t n_stable);
    void Update(float loss_sample) override;
    float GetOverhead() const override;
    void Reset() override;
    std::string Name() const override { return "MIMD"; }

private:
    float _MaxOverhead;
    float _MinOverhead;
    float _LambdaUp;
    float _LambdaDown;
    uint32_t _NStable;
    float _Overhead;
    uint32_t _SuccessStreak;
};

// ==================== Algorithm 5: Quantile Target ====================
//
// Maintains a sliding window of recent loss samples.
// overhead = Pxx(loss_window) / (1 - Pxx(loss_window)) + safety

class AlgoQuantile : public AdaptiveOverhead {
public:
    AlgoQuantile(float initial_overhead, float max_overhead, size_t window_size, int percentile,
                 float safety);
    void Update(float loss_sample) override;
    float GetOverhead() const override;
    void Reset() override;
    std::string Name() const override { return "Quantile"; }

private:
    float computePercentile() const;

    float _MaxOverhead;
    size_t _WindowSize;
    int _Percentile;
    float _Safety;
    std::deque<float> _Window;
};

// ==================== Algorithm 6: Burst-Aware EWMA ====================
//
// Two EWMA tracks: background (slow) and burst (fast).
// When sample > bg + threshold → burst detected, fast track activated.
// overhead = max(bg_rate, burst_rate) + safety

class AlgoBurstAware : public AdaptiveOverhead {
public:
    AlgoBurstAware(float initial_overhead, float max_overhead, float alpha_slow,
                   float alpha_fast, float burst_threshold, float safety);
    void Update(float loss_sample) override;
    float GetOverhead() const override;
    void Reset() override;
    std::string Name() const override { return "BurstAware"; }

private:
    float _MaxOverhead;
    float _AlphaSlow;
    float _AlphaFast;
    float _BurstThreshold;
    float _Safety;
    float _BgEwma;
    float _BurstEwma;
    bool _InBurst;
    bool _Initialized;
};

// ==================== Algorithm 7: Gradient Throughput Opt ====================
//
// Objective: maximize throughput = (1-overhead) * (1-loss_rate).
// Maintains two candidate overheads, evaluates which gives higher throughput.
// epsilon-greedy exploration.

class AlgoGradient : public AdaptiveOverhead {
public:
    AlgoGradient(float initial_overhead, float max_overhead, float delta, uint32_t eval_interval);
    void Update(float loss_sample) override;
    float GetOverhead() const override;
    void Reset() override;
    std::string Name() const override { return "Gradient"; }

private:
    float _MaxOverhead;
    float _Delta;
    uint32_t _EvalInterval;
    float _Overhead;
    float _BestOverhead;
    float _BestThroughput;
    float _LastLoss;
    uint32_t _Tick;
    int _Direction; // +1 exploring up, -1 exploring down, 0 committed
};

} // namespace gh
