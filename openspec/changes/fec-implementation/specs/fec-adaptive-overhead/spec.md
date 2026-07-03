## ADDED Requirements

### Requirement: Adaptive Overhead Algorithm Selection
The system SHALL support 8 overhead algorithms selectable via `FecConfig::algo` (0-7).

#### Scenario: Algorithm index maps to implementation
- **WHEN** `cfg.algo = N` (0 ≤ N ≤ 7)
- **THEN** `AdaptiveOverhead::Create(N, ...)` SHALL return the corresponding algorithm instance
- **AND** unknown indices SHALL fall back to Algorithm 1 (EWMA+StaticSafety)

### Requirement: Algorithm 0 — Static
The system SHALL provide a static overhead algorithm that always returns the configured initial overhead.

#### Scenario: Static overhead never changes
- **WHEN** Algorithm 0 (Static) is selected and `Update()` is called with any loss sample
- **THEN** `GetOverhead()` SHALL always return the configured initial overhead value

### Requirement: Algorithm 1 — EWMA + Static Safety
The system SHALL provide an EWMA-smoothed loss rate algorithm with a fixed safety margin.

#### Scenario: EWMA convergence
- **WHEN** Algorithm 1 receives a steady stream of loss samples at rate L
- **THEN** the EWMA estimate SHALL converge to L at rate α
- **AND** `GetOverhead()` SHALL return `ewma / (1 - ewma) + safety_margin` clamped to [0, max_overhead]

### Requirement: Algorithm 2 — EWMA + Dynamic Safety
The system SHALL provide an EWMA algorithm with safety margin proportional to loss rate variance.

#### Scenario: Variance-based safety scaling
- **WHEN** Algorithm 2 receives highly variable loss samples
- **THEN** the dynamic safety term SHALL increase proportionally to sqrt(variance)
- **AND** when loss rate stabilizes, safety SHALL approach `safety_base`

### Requirement: Algorithm 3 — PI Controller
The system SHALL provide a Proportional-Integral controller algorithm.

#### Scenario: Integral windup prevention
- **WHEN** the PI integral term reaches `i_max` (default 0.3)
- **THEN** it SHALL be clamped to prevent windup
- **AND** the output overhead SHALL stabilize at the value that achieves `target_loss`

### Requirement: Algorithm 4 — MIMD
The system SHALL provide a Multiplicative Increase / Multiplicative Decrease algorithm.

#### Scenario: Fast increase on loss
- **WHEN** a nonzero loss sample is received
- **THEN** overhead SHALL immediately multiply by (1 + λ_up) and reset the success streak

#### Scenario: Slow decrease on sustained success
- **WHEN** `N_stable` consecutive zero-loss samples are received
- **THEN** overhead SHALL multiply by (1 - λ_down) on each subsequent zero-loss sample

### Requirement: Algorithm 5 — Quantile Target
The system SHALL provide a quantile-based algorithm using a sliding window.

#### Scenario: Percentile calculation
- **WHEN** Algorithm 5 maintains a window of recent loss samples
- **THEN** `GetOverhead()` SHALL use the Pxx percentile of the window as the effective loss rate
- **AND** `overhead = Pxx / (1 - Pxx) + safety_margin`

### Requirement: Algorithm 6 — Burst-Aware EWMA
The system SHALL provide a burst-aware algorithm with separate background and burst EWMA tracks.

#### Scenario: Burst detection
- **WHEN** a loss sample exceeds `L_bg + burst_threshold`
- **THEN** the burst state SHALL be activated and the fast EWMA track SHALL be updated
- **AND** `GetOverhead()` SHALL use `max(L_bg, L_burst)` as the effective loss rate

#### Scenario: Burst decay
- **WHEN** loss samples fall back below the burst threshold
- **THEN** the burst EWMA SHALL decay at the slow rate (α_slow)
- **AND** the background EWMA SHALL continue tracking at the slow rate

### Requirement: Algorithm 7 — Gradient Throughput Optimization
The system SHALL provide a gradient-based algorithm that optimizes throughput directly.

#### Scenario: Exploration vs exploitation
- **WHEN** every `eval_interval` ticks
- **THEN** the algorithm SHALL evaluate `throughput = (1 - overhead) × (1 - loss_rate)` at the current candidate
- **AND** compare against the best throughput seen, adjusting direction if needed

#### Scenario: Periodic best-value exploitation
- **WHEN** 10 × `eval_interval` ticks have elapsed
- **THEN** the algorithm SHALL reset to the best-seen overhead and reverse exploration direction

### Requirement: Feedback Byte Encoding
The system SHALL encode loss rate as a single byte in each packet header.

#### Scenario: Loss rate to feedback byte
- **WHEN** the latest measured loss rate is `L ∈ [0, 1]`
- **THEN** the feedback byte SHALL be `uint8_t(min(L × 250, 250))`
- **AND** the receiver SHALL decode it as `L = fb / 250.0`

### Requirement: Adaptive REPEAT Ratio
The system SHALL scale the REPEAT ratio with current overhead level.

#### Scenario: REPEAT ratio tracks overhead
- **WHEN** current overhead `oh` ranges from `safety_margin` to `max_overhead`
- **THEN** the REPEAT ratio SHALL interpolate linearly between `repeat_ratio_min` and `repeat_ratio_max`
- **AND** single-packet copies SHALL be `1 + ceil(adaptive_ratio)`

### Requirement: Configurable Safety Margin
All adaptive algorithms SHALL accept a configurable `safety_margin` parameter via `FecConfig`.

#### Scenario: Safety margin applies to overhead floor
- **WHEN** `safety_margin = 0.01` and measured loss rate is zero
- **THEN** `overhead ≥ safety_margin` for all algorithms except Static and MIMD

### Requirement: Configurable Loss Window and Alpha
The system SHALL use `loss_window_groups` and `loss_alpha` from `FecConfig` for decode-side loss rate estimation.

#### Scenario: Smoothed loss reporting
- **WHEN** `loss_window_groups = 50` and `loss_alpha = 0.1`
- **THEN** every 50 decoded groups, the decoder SHALL update shared loss rate with IIR smoothing: `new = α × fail_rate + (1-α) × old`
