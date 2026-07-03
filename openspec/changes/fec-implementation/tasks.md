## 1. Phase 1: lcrq Integration + Packet LE

- [x] 1.1 Add lcrq submodule (`libs/lcrq`) — commit 82a8522
- [x] 1.2 CMake integration: autotools + ar static .a build — commit a9dc0fe
- [x] 1.3 RaptorQ C++ wrapper (`RaptorQ.hpp/cpp`) — `rq_init`/`Encode`/`GenerateSymbol`/`SubmitSymbol`/`TryDecode`
- [x] 1.4 `Packet::PushFrontLE(uint16/32)`, `Packet::PopFrontLE<T>()`
- [x] 1.5 Unit tests: 9/9 RaptorQ round-trip tests pass (`TestLcrq.cpp`)

## 2. Phase 2: Pipeline Refactor + FecConfig + Lua

- [x] 2.1 Extract `Pipeline::Process()` as virtual method
- [x] 2.2 Add `io_context&` to Pipeline (`_Io` member)
- [x] 2.3 `FecConfig` struct with all fields (timeout, overhead, symbol_size, obfuscate, algo, etc.)
- [x] 2.4 `FecPipeline` skeleton class extending Pipeline
- [x] 2.5 Lua binding: `hole.fec_pipeline()` + `hole.fec_shared_state()` in `LuaLib.cpp`

## 3. Phase 3: FEC Encode / Decode

- [x] 3.1 Encode path: batch accumulation with fiber timer + `timeout_ms` Select pattern
- [x] 3.2 Blob construction: `[u32 count][u16 len][data]...` with symbol_size zero-padding
- [x] 3.3 RaptorQ encode: `rq.Encode(blob)` → `GenerateSymbol(esi)` × (K + extra)
- [x] 3.4 Wire format header: DWORD (24-bit seq + 8-bit flags) + fb byte + optional echo
- [x] 3.5 Decode path: ring buffer with `decode_window` slots + `bit_ceil` alignment
- [x] 3.6 Ring buffer slot management: FindSlot(), EvictStaleSlot(), decode timeout (3×RTT + timeout)
- [x] 3.7 Blob split: read `[u32 count]` then `[u16 len][data]...` pairs after decode
- [x] 3.8 PING packet: `[DWORD + kPing][fb][echo?][payload:8B µs]`
- [x] 3.9 FEEDBACK_ONLY packet: `[DWORD + kFeedback + kEcho][fb][echo:8B]`
- [x] 3.10 REPEAT packet: `[DWORD + kRepeat][fb][echo?][IV][raw_data]`, adaptive ratio

## 4. Phase 4: Obfuscation + Feedback + RTT

- [x] 4.1 IV XOR: generate `iv_len` random bytes, XOR entire payload, prepend IV, flags bits 1-3
- [x] 4.2 Decode-side deobfuscation: read IV length from flags, extract IV, XOR payload
- [x] 4.3 Loss rate stat: `loss_window_groups` sliding window + `loss_alpha` IIR smoothing
- [x] 4.4 Feedback byte: `uint8_t(loss_rate × 250)` in every packet, decoded as `fb / 250.0`
- [x] 4.5 RTT measurement: PING carries send µs → decoder stores echo → encoder sends FEEDBACK with echo → EWMA RTT update
- [x] 4.6 `AdaptiveOverhead` base class + factory (`Create(algo, ...)`)
- [x] 4.7 Algorithm 0: Static (always returns configured overhead)
- [x] 4.8 Algorithm 1: EWMA + Static Safety (`ewma/(1-ewma) + safety_margin`)
- [x] 4.9 Algorithm 2: EWMA + Dynamic Safety (variance-scaled safety)
- [x] 4.10 Algorithm 3: PI Controller (Kp, Ki, integral anti-windup)
- [x] 4.11 Algorithm 4: MIMD (×1.50 on loss, ×0.95 on sustained success)
- [x] 4.12 Algorithm 5: Quantile Target (Pxx sliding window)
- [x] 4.13 Algorithm 6: Burst-Aware EWMA (fast burst track + slow background track)
- [x] 4.14 Algorithm 7: Gradient Throughput Optimization (ε-greedy exploration)
- [x] 4.15 Adaptive REPEAT ratio: interpolate `repeat_ratio_min..max` by overhead fraction
- [x] 4.16 `FecConfig` validation: MTU constraint, IV length, `overhead ≤ max_overhead`
- [x] 4.17 Configurable `safety_margin`, `loss_window_groups`, `loss_alpha` in `FecConfig`

## 5. Phase 5: Loss Pattern Models (Testing)

- [x] 5.1 `LossPattern` base class + factory (`Create(pattern, rate, rate2, burst)`)
- [x] 5.2 Pattern 0: Disabled (never drops)
- [x] 5.3 Pattern 1: Bernoulli (independent random, probability p)
- [x] 5.4 Pattern 2: Gilbert (2-state Markov, Good=0% / Bad=100%, target burst length)
- [x] 5.5 Pattern 3: Gilbert-Elliott (non-zero loss in both states)
- [x] 5.6 Pattern 4: Sinusoidal (time-varying `baseline + amplitude × sin(2πt/period)`)
- [x] 5.7 Pattern 5: Step (abrupt jump at `test_drop_burst` seconds)
- [x] 5.8 Pattern 6: Congestion Wave (triangular wave: climb + descent)
- [x] 5.9 Control packet bypass: PING/FEEDBACK never dropped by loss pattern
- [x] 5.10 `fec_matrix_test.py`: automated 8(algo)×6(pattern)×4(rate) = 192 test matrix

## 6. Phase 6: Integration Testing

- [ ] 6.1 Loopback integration test: two great-hole instances, localhost, FEC encode→loss pattern→decode round-trip
- [ ] 6.2 Build binary (Release) and deploy to ali + osaka
- [ ] 6.3 FEC config Lua scripts: `fec-tunnel-osaka.lua`, `fec-tunnel-ali.lua`
- [ ] 6.4 Test connectivity ali↔osaka with FEC pipeline (ping, basic throughput)
- [ ] 6.5 Run `fec_matrix_test.py`: loopback isolated netns, all 192 combinations
- [ ] 6.6 Verify all 8 algorithms converge under Bernoulli (1%/5%/10%/20% loss)
- [ ] 6.7 Verify burst-aware algorithms outperform static under Gilbert (burst loss)
- [ ] 6.8 Verify PI and Gradient algorithms handle Sinusoidal (slowly-varying loss)
- [ ] 6.9 Verify Step pattern response (overshoot magnitude, convergence time)
- [ ] 6.10 Compare effective throughput: `(1-overhead)×(1-loss_rate)` across all algorithms

## 7. Phase 7: Real-world Validation

- [ ] 7.1 Deploy to ali-osaka production-like config with FEC enabled
- [ ] 7.2 Long-duration iperf3 TCP throughput test (1h+) with FEC vs baseline
- [ ] 7.3 Long-duration iperf3 UDP test with FEC vs baseline
- [ ] 7.4 Measure actual overhead in steady state for best algorithm
- [ ] 7.5 Document recommended algorithm + config for different link profiles
- [ ] 7.6 24h+ stability soak test (no memory leaks, no ring buffer issues)

## 8. Phase 8: Polish

- [ ] 8.1 Add FEC-specific metrics/logging (overhead history, RTT EWMA, decode success rate)
- [ ] 8.2 Evaluate removing any algorithms that consistently underperform
- [ ] 8.3 Update `openspec/specs/fec-spec.md` with final decisions from test results
- [ ] 8.4 Archive this change: `openspec archive fec-implementation`
