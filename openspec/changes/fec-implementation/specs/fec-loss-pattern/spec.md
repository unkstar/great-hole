## ADDED Requirements

### Requirement: Loss Pattern Model Selection
The system SHALL support 6 active loss models selectable via `FecConfig::test_drop_pattern` (1-6), with 0 meaning disabled.

#### Scenario: Pattern index maps to implementation
- **WHEN** `cfg.test_drop_pattern = N` (1 ≤ N ≤ 6) and `test_drop_rate > 0`
- **THEN** `LossPattern::Create(N, ...)` SHALL return the corresponding loss pattern instance
- **AND** when `test_drop_pattern = 0`, no loss pattern SHALL be created

#### Scenario: Control packets are never dropped
- **WHEN** a packet with PING or FEEDBACK flag is received
- **THEN** the active loss pattern SHALL NOT apply (control packets bypass loss simulation)

### Requirement: Pattern 1 — Bernoulli
The system SHALL provide independent random packet loss with probability p.

#### Scenario: Independent loss events
- **WHEN** Pattern 1 (Bernoulli) is active with rate p
- **THEN** each packet SHALL be dropped independently with probability p
- **AND** adjacent packets SHALL have uncorrelated loss outcomes

### Requirement: Pattern 2 — Gilbert
The system SHALL provide a 2-state Markov burst loss model (Good=0% loss, Bad=100% loss).

#### Scenario: Burst loss behavior
- **WHEN** Pattern 2 (Gilbert) is active with target rate and burst length
- **THEN** loss SHALL occur in contiguous bursts with average length = `test_drop_burst`
- **AND** the steady-state loss rate SHALL converge to `test_drop_rate`

#### Scenario: State transitions
- **WHEN** in the Good state
- **THEN** transition to Bad with probability `p = r × π_bad / (1 - π_bad)`
- **WHEN** in the Bad state
- **THEN** transition to Good with probability `r = 1.0 / test_drop_burst`

### Requirement: Pattern 3 — Gilbert-Elliott
The system SHALL provide a 2-state Markov model with non-zero loss in both states.

#### Scenario: Background noise in Good state
- **WHEN** Pattern 3 (GilbertElliott) is active with bg_loss = `test_drop_rate2`
- **THEN** Good state SHALL drop packets with probability bg_loss
- **AND** Bad state SHALL drop packets with probability `h = min(k + (rate - k) / π_bad, 0.95)`

### Requirement: Pattern 4 — Sinusoidal
The system SHALL provide a sinusoidal time-varying loss pattern.

#### Scenario: Periodic loss oscillation
- **WHEN** Pattern 4 (Sinusoidal) is active with peak rate and trough rate
- **THEN** the instantaneous loss rate SHALL follow `baseline + amplitude × sin(2π × t / period)`
- **AND** each packet SHALL be dropped via Bernoulli at the instantaneous rate

### Requirement: Pattern 5 — Step
The system SHALL provide a step-function loss pattern (abrupt change at a specified time).

#### Scenario: Sudden loss rate change
- **WHEN** Pattern 5 (Step) is active and elapsed time < `test_drop_burst` seconds
- **THEN** loss rate SHALL be `test_drop_rate2` (before)
- **WHEN** elapsed time ≥ `test_drop_burst` seconds
- **THEN** loss rate SHALL jump to `test_drop_rate` (after)

### Requirement: Pattern 6 — Congestion Wave
The system SHALL provide a triangular-wave loss pattern simulating real congestion.

#### Scenario: Gradual congestion and recovery
- **WHEN** Pattern 6 (CongestionWave) is active
- **THEN** loss rate SHALL follow a symmetric triangle wave: `min_rate + (max_rate - min_rate) × 2 × |2×(t/period mod 1) - 1|`
- **AND** the period represents a full climb+descent cycle

### Requirement: Time-based Loss Rate Query
All time-based patterns SHALL report their instantaneous loss rate via `CurrentRate()`.

#### Scenario: Current rate for display
- **WHEN** `CurrentRate()` is called
- **THEN** for Bernoulli/Gilbert/GilbertElliott it SHALL return the steady-state rate
- **AND** for Sinusoidal/Step/CongestionWave it SHALL return the instantaneous rate at the current time
