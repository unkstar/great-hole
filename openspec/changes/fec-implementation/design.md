## Context

great-hole 当前通过 UDPspeeder 嵌套隧道获得 FEC 保护，但 UDPspeeder 的 GF(256) Reed-Solomon 码有 symbol 数 ≤255 的上限，且无 SIMD 加速。本项目将 RaptorQ (RFC 6330) 直接集成到 Pipeline 中，lcrq 库提供 SSSE3/SSE2/AVX2/AVX-512 加速，实测编码吞吐 563~1769 Mbps。

Pipeline 架构是单 fiber 的 Read→Filters→Write 链路。FEC 需要 batch 模式（积累→编码→批量写），因此 FecPipeline override `Process()` 实现不同的数据流模型。Wire format 使用 DWORD 头（24-bit seq + 8-bit flags），支持 FEC data / PING / FEEDBACK_ONLY / REPEAT 四种包类型。

## Goals / Non-Goals

**Goals:**
- 用 RaptorQ 替换 UDPspeeder 的 RS 码，消除 symbol 数上限
- 双向丢包反馈闭环：收方测丢包率→带内反馈→发方自适应 overhead
- 8 种自适应算法可切换，通过可控丢包测试选出最优
- Lua 可配置全部参数，支持 `hole.fec_pipeline()` 一行创建
- REPEAT 模式处理单包场景（symbol_count=1 时绕过 RaptorQ）

**Non-Goals:**
- 不协商：收发双方配置完全一致
- 不改变现有 Filter 接口
- DynMux 控制包不经过 FEC
- 不实现 UDPspeeder 的 interval/jitter 延迟分散
- 不移除现有 XOR 加密（great-hole 自身已有）

## Decisions

### 1. FecPipeline 继承 Pipeline 而非 Filter

**选择**: `FecPipeline : public Pipeline`，override `Process()` 实现 batch 模式。
**原因**: FEC 需要积累多包→编码→批量输出，这改变了 Pipeline 的核心循环模式。Filter 接口是 1-1 映射，无法满足。`Process()` 是 virtual 的，天然支持 override。
**替代方案**: 做成 Filter + 外部 timer → 无法在 Filter::Pipe() 中用 fiber timer。

### 2. Wire format: DWORD 头 (24-bit seq + 8-bit flags)

**选择**: 4B DWORD 小端序包头，低 24-bit 为 group_seq，高 8-bit 为 flags。
**原因**: 节省空间（vs 分开的 seq+flags），且一次 PopFrontLE 即可解析。flags 的 bit4/5/6 互斥（PING/FEEDBACK/REPEAT），bit0 控制字段宽度，bit1-3 编码 IV 长度。
**替代方案**: varint → 节省 1B 但增加复杂度，不值。

### 3. 自适应算法放在 encode 侧，decode 侧仅统计丢包率

**选择**: `AdaptiveOverhead` 在 encode 侧运行，`Update()` 接收来自 decode 侧的 feedback。Decode 侧每 `loss_window_groups` 个 group 更新一次 loss rate 到 shared state。
**原因**: 关注点分离。Encode 侧负责"发多少冗余"，Decode 侧负责"观测丢了多少"。通过 `FecSharedState` 共享状态。
**替代方案**: Decode 侧直接计算 overhead → 增加 decode 侧复杂性，且 encode 侧仍需读取 feedback 来调整。

### 4. 8 种算法 + 6 种丢包模型的测试驱动选择

**选择**: 先全部实现，再通过 `fec_matrix_test.py` 遍历 8(algo)×6(pattern)×4(rate)=192 组合，用实际数据决定最终选择的算法。
**原因**: 没有完美算法——Static 最简单但无法自适应，Gradient 理论上最优但收敛慢。真实链路（含突发丢包）下 BurstAware 可能最佳，但需要实测验证。
**替代方案**: 只实现 2-3 种 → 缺乏对照，无法确定最优。

### 5. REPEAT 模式绕过 RaptorQ

**选择**: symbol_count=1 时走 REPEAT（纯复制 × N），不经 RaptorQ 编码。
**原因**: K=1 时 RaptorQ 退化为复制，但 RaptorQ 编解码本身有开销。直接复制更简单高效。
**替代方案**: 即使 K=1 也走 RaptorQ → 增加无意义的编解码开销。

### 6. lcrq 静态库集成

**选择**: CMake `ExternalProject` / `add_custom_command` 调用 autotools 编译 lcrq，产出 `liblcrq.a`，静态链接。
**原因**: lcrq 是 autotools 项目，无 CMake 支持。静态链接避免运行时依赖。
**替代方案**: 手工移植 lcrq 源码到 CMake → 维护成本高，上游更新困难。

## Risks / Trade-offs

- **[RTT 延迟]** PING/FEEDBACK 闭环有 1 RTT 延迟，丢包突变时前几个 group 可能保护不足 → 用 REPEAT 兜底单包场景，MIMD 算法快速响应
- **[lcrq 许可证]** GPL-2.0 → 本项目也需 GPL 兼容。已确认 great-hole 是 GPL-3.0
- **[大 K 内存]** K=56403 时 RaptorQ 内部矩阵可达数百 MB → `max_batch` 限制 K ≤ 200（约 300KB symbol×200=60MB blob），实测够用
- **[ring buffer 溢出]** `decode_window=64` 限制并发 group 数 → 超时驱逐机制兜底，burst 场景可能丢旧 group
- **[定时器精度]** fiber timer 基于 `steady_timer` + `AsioUseFiber`，精度受 io_context 调度影响 → 3×RTT+timeout 的 decode_timeout 已留余量
- **[REPEAT MTU]** 原始包 + IV + header 可能超过 MTU → `SendBatch` 中检查，超出时跳过 REPEAT 走 FEC 编码
