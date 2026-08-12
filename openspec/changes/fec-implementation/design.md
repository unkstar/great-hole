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

### 7. fec_codec 双编解码器: RS (Vandermonde GF256) 作为 lcrq 部署替选 (2026-08-05 新增)

**选择**: `FecCodec` 策略接口 + `fec_codec` 配置二选一 ("lcrq" 默认 / "rs")，AdaptiveOverhead / LossPattern / wire 头复用。
**原因**: tokyo 部署环境 vCPU 算力配额低，lcrq 小 K 编码实测 ~27-30M (无 AVX-512 透传)；RS 标量实测 280M (K=17)。RS 系统化源分片即收即发 (零 batch 延迟) 对 TCP 更友好。
**替代方案**: 换更高算力 vCPU / AVX-512 实例 → 不可控 (云未透传)；REPEAT 快路径 → 无冗余保护。

### 8. RS 乱序投递 (2026-08-08, TCP 停滞根因修复)

**选择**: 解码端缺口不阻塞后续分片，直接交付 (乱序)，repair 到达后补投缺口；encoder 侧 repair 以 1ms 间隔摊开发送。
**原因**: watermark 保序曾把"暂时缺口"变成"永久丢失" (guard 跳过)，引发 dup ACK 风暴 → 重传 burst → 中间设备丢 burst → 自维持循环 (TCP 0.2-14M)。乱序交付 + repair 摊开后 TCP 88.5M。
**替代方案**: 保序 + 更长超时 → 延迟放大，ACK 压缩；UDPspeeder mode 1 + -t 已被验证为正确模式。

## Risks / Trade-offs

- **[RTT 延迟]** PING/FEEDBACK 闭环有 1 RTT 延迟，丢包突变时前几个 group 可能保护不足 → 用 REPEAT 兜底单包场景，MIMD 算法快速响应
- **[lcrq 许可证]** GPL-2.0 → 本项目也需 GPL 兼容。已确认 great-hole 是 GPL-3.0；RS256 为自研实现，无此问题
- **[大 K 内存]** K=56403 时 RaptorQ 内部矩阵可达数百 MB → `max_batch` 限制 K ≤ 200（约 300KB symbol×200=60MB blob），实测够用
- **[ring buffer 溢出]** `decode_window=64` 限制并发 group 数 → 超时驱逐机制兜底，burst 场景可能丢旧 group
- **[定时器精度]** fiber timer 基于 `steady_timer` + `AsioUseFiber`，精度受 io_context 调度影响 → 3×RTT+timeout 的 decode_timeout 已留余量
- **[REPEAT MTU]** 原始包 + IV + header 可能超过 MTU → `SendBatch` 中检查，超出时跳过 REPEAT 走 FEC 编码
- **[PI 零丢包积分漂移]** PI 在零丢包线路上把 overhead 推到 15.5% 高位 (积分漂移是设计行为，floor 只挡下限) → 零丢包链路用 algo=1 EWMA (实测补偿率 5.0%)
- **[RS repair bid 截断]** bid 用 16-bit 会在 >94MB 流量后错乱，且 PushFrontLE 逆序 prepend 曾致字节序错乱 (f082528 修复) → bid 全 24-bit，push 顺序: 高字节先、低 16 后
- **[RS 乱序投递副作用]** TCP dup ACK 快速重传偏高 (dl ~1105-1953)，数据全到无害 → 可再调优 repair 摊开间隔
