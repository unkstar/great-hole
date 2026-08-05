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

- [x] 6.1 Loopback integration test: two great-hole instances, localhost, FEC encode→loss pattern→decode round-trip
- [x] 6.2 Build binary (Release) and deploy to ali + tokyo (great-hole-fec .deb, 2026-07-04)
- [x] 6.3 FEC config Lua scripts: `fec-tokyo-ali.lua`, `fec-tokyo.lua` (PI algo→Static, repeat_ratio=0)
- [x] 6.4 Test connectivity ali↔tokyo with FEC pipeline (ping 59ms, tunnel UP)
- [x] 6.5 Run `fec_matrix_test.py`: loopback isolated netns, all 192 combinations (done 2026-07-02)
- [x] 6.6 Verify all 8 algorithms converge under Bernoulli (1%/5%/10%/20% loss) — PI best
- [x] 6.7 Verify burst-aware algorithms outperform static under Gilbert (burst loss) — MIMD best
- [x] 6.8 Verify PI and Gradient algorithms handle Sinusoidal (slowly-varying loss)
- [x] 6.9 Verify Step pattern response (overshoot magnitude, convergence time)
- [x] 6.10 Compare effective throughput across all algorithms — PI=29.8Mbps, Static=30.2Mbps

## 6b. Phase 6b: Batching Debug (2026-07-04 ~ 2026-07-05)

- [x] 6b.1 Identified root cause: main fiber unconditionally waits timeout_ms per cycle → 26Mbps baseline
- [x] 6b.2 Two-fiber design: reader fiber continuously reads into batch_queue; main fiber drains with 100us poll when queue empty → 42Mbps
- [x] 6b.3 PING/FEEDBACK overhead analyzed: UDP async_send_to completes in microseconds, NOT RTT. No gating needed.
- [x] 6b.4 SendBatch copies=1 fast path: skip out_batch vector allocation
- [x] 6b.5 TryRead confirmed ~70% successful (NOT "always EAGAIN" as handoff claimed) — reader gets ~3.3 pkts per Read cycle
- [x] 6b.6 FEC overhead stats added (gated, default off): actual overhead ~5.7% at K≈17 (ceil(K*0.01)=1 → 1/17=5.9%)
- [x] 6b.7 Nested tunnel comparison: UDPspeeder+great-hole TCP = 62Mbps vs FEC TCP = 42Mbps. UDPspeeder CWND reaches 650KB; FEC CWND capped at 350KB.
- [x] 6b.8 Root cause conclusion: **batch delay, not FEC overhead, is the primary TCP throughput killer**. UDPspeeder (50% OH, 62Mbps) beats RaptorQ (5.7% OH, 42Mbps) because it doesn't add batch-induced delay/jitter to TCP flows.
- [x] 6b.9 Direct link tested: TCP 101Mbps (CWND 1MB), UDP 100Mbps 0% loss. Both directions work after opening GGC firewall ports 5001/5201/10086 TCP+UDP.
- [x] 6b.10 Boost.Asio epoll confirmed EPOLLET (edge-triggered); async_read_some does single speculative readv() -- correct per design.

### Key Performance Summary

| Metric | Direct | FEC (RaptorQ 1%) | Nested (UDPspeeder RS 50%) |
|--------|--------|------------------|---------------------------|
| TCP T→A | 101 Mbps | 42 Mbps | **62 Mbps** |
| TCP CWND | 1 MB | 350 KB | **650 KB** |
| TCP Retrans | 1523 | 166 | **3004** |
| UDP 80M T→A | 79.7 (0%) | 79.5 (0%) | 66.7 (**16% loss**) |
| UDP 100M T→A | 97.5 (0%) | 87.0 (0%) | 60.3 (**39% loss**) |
| FEC actual OH | - | 5.7% | 50% |

### Architectural Insight

UDPspeeder's batch-and-send approach (Mode 0, RS GF256) allows TCP CWND to grow to 650KB despite 3004 retransmissions. FEC's four-fiber batch pipeline (RaptorQ, 4ms deadline) eliminates retransmissions but caps CWND at 350KB via ACK compression and delay jitter. For TCP throughput, **batch-induced latency hurts more than FEC overhead or even packet loss**.

Future optimization: implement "send-immediately + repair-later" (UDPspeeder Mode 1 fast-send pattern) where data packets go out with zero delay and only repair symbols carry the batch latency.

## 7. Phase 7: Real-world Validation

- [ ] 7.1 Deploy to ali-osaka production-like config with FEC enabled (测试专用链路 fec-test 已搭好, 2026-08-05)
- [~] 7.2 iperf3 TCP 短时测试已跑 (FEC 13.7~16.1M vs 直连 36~92M), 1h+ 长时未做
- [~] 7.3 iperf3 UDP 短时测试已跑 (FEC ~25-27M vs 直连 76-95M), 1h+ 长时未做
- [x] 7.4 实际 overhead 与吞吐上限已测量: **1 vCPU 是 FEC 编码器瓶颈 (~27Mbps)** — 结果见 fec-spec.md 2026-08-05 基线
- [ ] 7.5 Document recommended algorithm + config for different link profiles
- [ ] 7.6 24h+ stability soak test (no memory leaks, no ring buffer issues)

## 9. Phase 9: RS Codec (Vandermonde GF256) — 可选 FEC 实现

> 动机: tokyo vCPU 上 RaptorQ 27M vs RS 标量实测 280M (K=17)。RS 无中间符号消元, 且系统化源分片即收即发 (零 batch 延迟, TCP 友好)。lcrq 保留为默认。

- [ ] 9.1 GF(256) 库: EXP/LOG 查表 + Vandermonde 系数构造 + fec_encode (逐 repair 生成) + fec_decode (k×k 高斯消元求逆) — 或引入 Rizzo fec.cpp (GPL-2.0 兼容)
- [ ] 9.2 FecConfig 加 `fec_codec` 字段 ("lcrq" 默认 / "rs"), Lua 绑定
- [ ] 9.3 RS 编码路径: systematic 源分片即发 + batch 窗口到期按 AdaptiveOverhead 补发 m 个 repair
- [ ] 9.4 RS 解码路径: 序号缺口检测 + repair 收集 + k×k 高斯消元恢复 (无丢包零开销)
- [ ] 9.5 wire format: repair 分片头 (batch_id 2B + repair_index 1B), 源分片复用 DWORD 头
- [ ] 9.6 max_batch 约束: k+m ≤ 255 (GF256), 建议 max_batch ≤ 100 (冗余上限 155 = 37% 覆盖)
- [ ] 9.7 矩阵测试: 8 算法 × 6 模式 × 4 速率跑 RS codec (复用 fec_matrix_test.py)
- [ ] 9.8 隧道实测: ali↔tokyo fec-test 链路 TCP/UDP vs lcrq 对比 (预期 UDP ~90M / TCP 接近 nofec)
- [ ] 9.9 spec 最终化: RS 实测数据回填 + 算法选型建议

## 8. Phase 8: Polish

- [x] 8.0 修复 lcrq 构建集成缺陷: 丢失的 libs/lcrq/CMakeLists.txt 从 ali 部署副本找回, 正式化为 cmake/lcrq.cmake + cmake/lcrq-install.sh (BUILD_IN_SOURCE + ar 打包 + 头文件安装), 全新 clone 可复现构建 (2026-08-05)
- [ ] 8.1 Add FEC-specific metrics/logging (overhead history, RTT EWMA, decode success rate)
- [ ] 8.2 Evaluate removing any algorithms that consistently underperform
- [ ] 8.3 Update `openspec/specs/fec-spec.md` with final decisions from test results
- [ ] 8.4 Archive this change: `openspec archive fec-implementation`
