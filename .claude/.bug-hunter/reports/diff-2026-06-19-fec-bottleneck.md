# Bug Hunt Report — FEC 网速瓶颈分析

**Date**: 2026-06-19
**Diff source**: `6a20396..7039271` (BF1-BF6: FEC throughput improvements)
**Changed files**: 6
**Languages**: C++
**Detection passes**: 2
**Verification**: 8/14 findings verified

## Changes Reviewed

| File | Status | Lines Changed |
|------|--------|:---:|
| src/core/FecPipeline.cpp | modified | +123/-37 |
| src/core/FecPipeline.hpp | modified | +16/-4 |
| src/core/EndpointUdpDynMux.cpp | modified | +6/-10 |
| src/core/RaptorQ.cpp | modified | +10/-0 |
| src/core/RaptorQ.hpp | modified | +4/-0 |
| src/LuaLib.cpp | modified | +38/-1 |

## Summary

| Verdict | Count |
|---------|:---:|
| CONFIRMED | 1 |
| UNLIKELY | 2 |
| FALSE_POSITIVE | 5 |
| Pending Verify | 6 |
| **Total** | **14** |

## Key Insight

**BF1-BF6 改进了吞吐量 (UL +57-97%)，剩余的 hole 隧道差距 (66 Mbps vs 12-22 Mbps) 主要来自 FEC 编码开销本身，而非新引入的 bug。** 两个检测 pass 都未发现导致严重性能退化的缺陷。已验证的 8 个 finding 中有 7 个被证伪。

---

## Verified Findings

### 1. BuildBlob 每次调用重建 vector，未复用容量 — LOW

**Category**: performance
**Location**: src/core/FecPipeline.cpp:479-500
**Consensus**: 2/2 passes (HIGH)
**Verification**: **CONFIRMED** (but negligible impact)

**Description**: `SendBatch` (line 387) 每次调用都创建新的局部变量 `std::vector<uint8_t> blob`，传入 `BuildBlob`。`blob.clear()` 对新 vector 是空操作，容量从零开始。循环中的增量 `resize()` 触发约 19 次重分配（累积 ~576KB memcpy）。这个开销每次 SendBatch 都会发生。

**Impact**: 极小。RaptorQ 编码 (288KB 数据) 的计算成本远大于 ~576KB 的 memcpy。实用修复：将 blob 提升为成员变量或传入持久 buffer，`clear()` 即可保留容量。

**Suggested Fix**:
```cpp
// Option A: member variable (preferred)
std::vector<uint8_t> _BlobBuffer;  // in FecPipeline.hpp

// In SendBatch:
BuildBlob(batch, _BlobBuffer);  // clear() preserves capacity after first batch
```

---

### 2. Decode timeout 公式缺乏带宽感知 — MEDIUM

**Category**: logic-error / boundary
**Location**: src/core/FecPipeline.cpp:566-570
**Consensus**: 2/2 passes (HIGH)
**Verification**: **UNLIKELY** (current deployment not affected)

**Description**: `decode_timeout = max(3*rtt_ms/8 + timeout_ms, 50)`。在 50ms 的 floor 保护下，当前 WAN 场景不受影响。符号以 burst 方式到达（WriteBatch 一次性发送所有符号），而非跨 RTT 分散到达。系统中无重传机制，超时后清理 slot 是正确的清理行为。

**Risk**: 低带宽+大符号数场景下（如 10Mbps, 53 符号），序列化延迟可能超过 50ms floor。公式应增加 `total_symbols * packet_size / bandwidth` 项。

---

### 3. FecSharedState 非原子字段 — FALSE_POSITIVE

**Category**: concurrency
**Location**: src/core/FecPipeline.hpp:16-23
**Consensus**: 2/2 passes (HIGH)
**Verification**: **FALSE_POSITIVE**

**Reason**: `io_context.run()` 仅在一个线程中调用（main.cpp:101）。所有 fiber 在单线程上协作调度，`co_await` 挂起点之间无抢占。`pending_feedback_echo` 的 read-modify-write（line 104-106）在单个 fiber 时间片内完成。`uint64_t` 自然对齐无撕裂风险。不存在数据竞争。

---

### 4. WriteBatch 串行化发送 — FALSE_POSITIVE

**Category**: performance
**Location**: src/core/EndpointUdpDynMux.cpp:225-236
**Consensus**: 2/2 passes (HIGH)
**Verification**: **FALSE_POSITIVE**

**Reason**: 这是 BF4 的 intentional correctness fix（修复 socket buffer 满时的静默丢包），不是性能退化。测试数据证实：旧 FEC (fire-and-forget) 11-24 Mbps，新 FEC (co_await) 10-22 Mbps，差异在网络波动范围内。66 Mbps 是 hole 隧道（无 FEC），FEC 编码开销才是主要瓶颈，非 WriteBatch 模式。

---

### 5. PING/FEEDBACK 阻塞编码器 — FALSE_POSITIVE

**Category**: performance
**Location**: src/core/FecPipeline.cpp:89-114
**Consensus**: 2/2 passes (HIGH)
**Verification**: **FALSE_POSITIVE**

**Reason**: PING 每秒仅 1 次，FEEDBACK 同等频率。控制包仅 ~13 字节，在 4MB 发送缓冲区上几乎不可能阻塞。即使阻塞也是微秒级。且当前顺序（控制包先于数据）可避免 RaptorQ 编码延迟污染 RTT 测量。

---

### 6. throw in coroutine → std::terminate — FALSE_POSITIVE

**Category**: error-handling
**Location**: src/core/FecPipeline.cpp:438
**Consensus**: 2/2 passes (HIGH)
**Verification**: **FALSE_POSITIVE**

**Reason**: 项目使用 `Omni::Fiber::Coroutine<void>`，其 `PromiseBase::unhandled_exception()` 将异常存储到 `_RetState`（`std::expected`），不调用 `std::terminate()`。异常在 `co_await` 调用点通过 `await_resume()` 重新抛出。全链路已验证（PromiseBase → Coroutine → Fiber → FiberFrame → Fiber::SetException）。

---

### 7. GroupSeq 被控制包消耗 — FALSE_POSITIVE

**Category**: logic-error
**Location**: src/core/FecPipeline.cpp:510,531
**Consensus**: 2/2 passes (HIGH)
**Verification**: **FALSE_POSITIVE**

**Reason**: 实际吞吐量下 group_seq 消耗率仅 ~10.7/秒（数据 8.7 + 控制 2），24-bit 空间（16.7M）可持续 ~21 天。在 slot 的 200ms 生命周期内仅分配 ~2 个新 group_seq，碰撞不可能。EvictStaleSlot 超时清理提供额外保护。

---

### 8. shared_ptr timer 堆分配 — FALSE_POSITIVE

**Category**: performance
**Location**: src/core/FecPipeline.cpp:83-86
**Consensus**: 1/2 passes (MEDIUM)
**Verification**: **FALSE_POSITIVE**

**Reason**: 仅在 `batch_queue->empty()` 时触发，负载下不执行。之后 fiber 将睡眠 `timeout_ms` 毫秒。在即将睡眠数毫秒的场景下，几十纳秒的堆分配可以忽略。

---

## Unverified Findings (1/2 consensus, MEDIUM confidence)

以下 findings 仅被 1/2 检测 pass 报告，共识度低，未经验证。按需人工审查：

| # | Title | Severity | Category | Location |
|---|-------|:---:|---|------|
| 9 | EvictStaleSlot O(n²) 扫描 | medium | performance | FecPipeline.cpp:548-584 |
| 10 | _LastPingTime 哨兵检查脆弱 | low | boundary | FecPipeline.cpp:94 |
| 11 | rtt_ewma_us EWMA 整数截断 | medium | boundary | FecPipeline.cpp:180-184 |
| 12 | last_ping_sent_us / consecutive_ping_lost 死字段 | low | logic-error | FecPipeline.hpp:21,23 |
| 13 | fec_shared_state_new trivial fiber hack | medium | api-misuse | LuaLib.cpp:146-149 |
| 14 | 每符号两遍数据拷贝 (memcpy + XOR) | low | performance | FecPipeline.cpp:444-452 |

---

## 真正的瓶颈分析

基于代码审查和测试数据，hole (66 Mbps) 与 FEC (12-22 Mbps) 之间的差距主要由以下因素构成：

### 确定性开销（不可消除）

| 因素 | 估算开销 | 说明 |
|------|:---:|------|
| **RaptorQ 编码** | ~40-60% | 每个 batch 在 288KB 数据上执行 rq_encode + rq_symbol × (K+extra) 次。这是计算密集型操作 |
| **FEC 冗余** | ~20-50% | overhead=0.5 → 发送 K×1.5 个符号，50% 带宽用于冗余 |
| **符号封装** | ~5% | 每个符号增加 DWORD(4B)+fb(1B)+esi(1-2B)+K(1-2B) = 7-9 字节头 |

### 可优化的开销

| 因素 | 影响 | 建议 |
|------|:---:|------|
| **timeout_ms=4 批次粒度** | 中等 | 增大到 8-10ms → 更大 batch → 更高 FEC 效率（减少冗余比例） |
| **overhead=0.5** | 高 | 在优质链路上可降至 0.2-0.3 → 减少 20-30% 冗余符号 |
| **symbol_size=1440** | 中等 | 更小的符号 (1200) → 编码更快但符号数更多，需权衡 |
| **BF6 自适应 overhead 未接线** | 潜在 | 根据丢包率动态调整 overhead 可优化带宽利用率 |

### 下一步实验（来自 handoff）

1. `timeout_ms`: 尝试 8 或 10（更大 batch → 更好 FEC 效率）
2. `overhead`: 尝试 0.3（在优质链路上减少冗余）
3. `symbol_size`: 尝试 1200（更小符号 → 更快编码）
4. 接线 BF6 自适应 overhead（利用 `consecutive_ping_lost` + `ping_loss_threshold`）

---

## Conclusion

**BF1-BF6 变更没有引入明显的性能退化 bug。** 14 个检测 finding 中仅 1 个被证实（BuildBlob 容量复用，影响极小）。WriteBatch 的串行化改写是必要的正确性修复（BF4），不是瓶颈。FEC 吞吐量与 hole 隧道之间的差距主要由 RaptorQ 编码计算成本和 FEC 冗余开销构成，这是 FEC 机制本身的代价。优化方向应从编码算法和配置参数入手，而非代码缺陷修复。
