# FEC Integration Spec

## Status

**Implemented & Tested** — 全部 Phase 1-4 代码实现完成，Phase 5-7 矩阵测试通过（192/192, 0 失败）。
8 种自适应算法在 100Mbps/100msRTT 下完成功能验证，PI 确认为最佳自适应算法。

## References

- `openspec/specs/fec-research.md` — RS / LDPC / RaptorQ 技术选型对比
- `openspec/specs/udpspeeder-analysis.md` — UDPspeeder 实现研判
- `openspec/specs/architecture-protocol.md` — Packet 结构、wire 格式（含最终 FEC header）

## Background

- great-hole Pipeline: `Read → filter chain → Write`，严格 1-1，单 fiber
- Packet 构造函数 `Packet(length, offset)`: `_Data` 分配 `length+offset` 字节，`_Offset=offset`
- PushFront: 减小 `_Offset`，数据在 wire 最前面。后 push 的先出现在 wire 上
- 所有 PushFront 都在 Pipeline fiber 内，无 coroutine 跨 fiber 问题

## Wire Format

### FEC data shard (flags bit4=0, bit5=0, bit6=0)

```
小 group (flags bit0=0):  [chan/RxId][seq+flags:4B][fb:1B][echo?:8B][idx:1B][cnt:1B][IV:1~8B][XORed shard]
大 group (flags bit0=1):  [chan/RxId][seq+flags:4B][fb:1B][echo?:8B][idx:2B][cnt:2B][IV:1~8B][XORed shard]

echo? = 仅 flags bit7=1 时存在 (8B LE μs timestamp)
```

### PING (flags bit4=1)

```
[chan/RxId][seq+flags:4B][fb:1B][echo?:8B][payload:8B]
```

PING 的 `echo?` 回传 RTT timestamp；`payload` 本次发送时间戳。均为 μs LE。

### FEEDBACK_ONLY (flags bit5=1)

```
[chan/RxId][seq+flags:4B][fb:1B][echo?:8B]
```

### REPEAT (flags bit6=1)

```
[chan/RxId][seq+flags:4B][fb:1B][echo?:8B][IV:1~8B][XORed raw packet]
```

不经过 RaptorQ，不经过 blob。payload 为原始 UDP 包。`group_seq` 正常递增。

## FEC Header 详细

### Flags byte

```
bit 0:      字段宽度 (仅 FEC data 时有效)
              0 = shard_index / source_count 各 1B
              1 = shard_index / source_count 各 2B
bit 1-3:    IV 长度 (仅 FEC data / REPEAT 时有效)
              000=1B, 001=2B, ..., 111=8B
              关闭 obfuscate 时 decoder 忽略 bits 1-3 (编码设为 000)
bit 4:      PING 包
bit 5:      FEEDBACK_ONLY (纯反馈)
bit 6:      REPEAT — 原始包多倍重复，绕过 RaptorQ
bit 7:      ECHO — optional, 1=后续 8B echo timestamp 存在
```

bit4/bit5/bit6 互斥，仅一个置位。

### 字段表（通用前缀）

| 偏移 | 字段 | 大小 | 说明 |
|------|------|:---:|------|
| 0 | `group_seq` + `flags` | 4B | LE DWORD: bits 0-23=seq, bits 24-31=flags |
| 4 | `feedback` | 1B | loss_rate × 250 (0~250) |
| 5 | `echo` | 8B | RTT echo timestamp μs LE (仅 flags bit7=1) |

### 字段表（FEC data，续）

| 偏移 | 字段 | 大小 | 说明 |
|------|------|:---:|------|
| 5/13 | `shard_index` | 1B/2B | ESI, LE (echo 无→5, echo 有→13) |
| * | `source_count` | 1B/2B | K, LE |
| * | IV | 1~8B | 随机混淆密钥 (obfuscate=true) |
| * | data | N B | shard data XOR IV |

### 字段表（PING，续）

| 偏移 | 字段 | 大小 | 说明 |
|------|------|:---:|------|
| 13 | `payload` | 8B | 本次发送时间戳 μs LE |

### 字段表（REPEAT，续）

| 偏移 | 字段 | 大小 | 说明 |
|------|------|:---:|------|
| 13 | IV | 1~8B | 随机混淆密钥 (obfuscate=true) |
| 13+iv | data | N B | 原始 UDP 包 (IV XOR 后) |

### DWORD 读写

```cpp
// Encode:
uint32_t dw = (group_seq & 0xFFFFFF) | ((uint32_t)flags << 24);
p.PushFrontLE(dw);  // wire: [group_seq:3B][flags:1B] LE

// Decode:
uint32_t dw = p.PopFrontLE<uint32_t>();
uint32_t group_seq = dw & 0xFFFFFF;
uint8_t flags = (dw >> 24) & 0xFF;
```

`Packet` 需新增 `PushFrontLE(uint16_t)` / `PushFrontLE(uint32_t)` 和对应的 `PopFrontLE<T>()`。

### Flags bit0 选择

| 条件 | flag bit0 | 说明 |
|------|:---:|------|
| symbol_count + ceil(symbol_count × overhead) ≤ 255 | **0 (小)** | 覆盖大多数场景 |
| symbol_count + ceil(symbol_count × overhead) > 255 | 1 (大) | 极高吞吐或极端 overhead |

### source_count 语义

header 中的 `source_count` = **RaptorQ 源符号数** (symbol_count = blob padding 后的 padded_size / symbol_size)。
blob 内部的 `u32 count` = **原始包数量** (pkt_count, 解码后拆分 blob 用)。
symbol_count ≥ pkt_count（padding 使符号数稍大于包数）。assert(symbol_count ≥ 1)。

### 开销

| mode | 小 group | 大 group |
|------|:---:|:---:|
| P2P | 15+iv | 17+iv |
| Mux | 16+iv | 18+iv |
| DynMux | 17+iv | 19+iv |

### 预留空间

FEC 编码时构造 `Packet(shard_size, 32)` 预留 32B front space (含 echo 8B)。

| 场景 | 总需 | 32B 是否够 |
|------|:---:|:---:|
| P2P 小 | 23 | **9B余** ✅ |
| P2P 大 | 25 | **7B余** ✅ |
| Mux 小 | 24 | **8B余** ✅ |
| Mux 大 | 26 | **6B余** ✅ |
| DynMux 小 | 25 | **7B余** ✅ |
| DynMux 大 | 27 | **5B余** ✅ |

### MTU 约束

**REPEAT 模式**: 原始包 + IV + header 不能超过 MTU。
```
max_raw = mtu - IP_UDP_overhead - endpoint - 13B(FEC_frame) - iv_len - (echo?8:0)
```
超出时跳过 REPEAT，以普通 FEC 编码处理（即使 pkt_count=1）。

**FEC data 模式**: blob 编码后由 RaptorQ 重分片为 symbol_size 的 shard。每个 shard + header 独立发包，不受原始包大小限制。

### symbol_size 计算

symbol_size 在 Pipeline Start 时根据 MTU 反算，lint 为固定值：

```
symbol_size = mtu - IP_UDP_overhead - max_wire_overhead
```

其中：
- `IP_UDP_overhead` = 20B (IP header) + 8B (UDP header) = 28B
- `max_wire_overhead` = endpoint + FEC_header_max + IV_max + echo_max

| 模式 | endpoint | FEC_header_max (小/大) | IV_max | echo_max | max_wire | symbol_size (mtu=1500, 小/大) |
|------|:---:|:---:|:---:|:---:|:---:|:---:|
| P2P | 0 | 7 / 9 | 8 | 8 | 23 / 25 | 1477 / 1475 |
| Mux | 1 | 8 / 10 | 8 | 8 | 25 / 27 | 1475 / 1473 |
| DynMux | 2 | 9 / 11 | 8 | 8 | 27 / 29 | 1473 / 1471 |

实际配置时用**小 group**的公式（即最常见的场景），取整到友好值（如 1450、1472 等）。

**配置验证**: `symbol_size + max_wire_overhead + IP_UDP_overhead ≤ mtu` 在 Pipeline Start 时检查，不满足则终止。

### IV 混淆

IV 不是独立的 Filter。通过 `fec_cfg.obfuscate = true/false` 控制。启用时 flags bits 1-3 编码 IV 长度 (1~8B)。
关闭时 flags bits 1-3=000，decoder 忽略，不读取 IV 字段。

### Ring Buffer

Decoder 维护固定大小的 ring buffer。buffer 满时丢弃最老的 group。

```
capacity = fec_cfg.decode_window (default 64, max 256)
eviction: 遍历所有 slot，丢弃 first_packet_time + decode_timeout 已超时的 group
           若均未超时 → 丢弃 first_packet_time 最早的 group
```

## Pipeline FEC 行为

### Encode (发送)

```
fiber loop:
  ┌─ accumulate: 从 _In 读包 → 存入 batch
  │   直到 timeout_ms 到期 或 batch.size() ≥ max_batch
  │   batch 中每个包保留原始 size; 0 字节包直接丢弃
  │   读操作与 encode_timeout timer 通过 Select 竞争
  ├─ 空 batch: 跳过, 回到 loop 开始
  ├─ filter chain: 对 batch 每个包 filter.Pipe()
  ├─ 选择 pkt_count:
  │   尝试 pkt_count = 1..batch_size()，对每个计算 wire_bytes
  │   选总 wire_bytes 最小的 pkt_count; n<1000 量级安全
  │
  ├─ if pkt_count == 1 (仅有 1 个原始包):
  │   if raw_size + iv_len + 14 + endpoint > mtu:
  │     跳过 REPEAT, 走正常 FEC 编码 (pkt_count=1 也做 blob+RaptorQ)
  │   repeat = 1 + ceil(repeat_ratio)
  │   对每个副本:
  │     p = Packet(data_size, 32)
  │     memcpy(p.Data(), raw_packet, data_size)
  │     if (obfuscate): 生成 iv_len 字节随机 IV; XOR p.Data() with IV; PushFrontLE(IV bytes, iv_len)
  │     if has_pending_echo: PushFrontLE(echo, 8); flags |= 0x80
  │     PushFrontLE(feedback_byte)        [fb 1B]
  │     uint32_t dw = (group_seq & 0xFFFFFF) | (0x40 << 24);
  │     PushFrontLE(dw)                   [seq+flags: 4B DWORD]
  │     co_await _Out->Write(p)
  │   continue loop
  │
  ├─ blob concat: [u32 count][u16 len][data] × pkt_count
  │   将 blob zero-pad 到 symbol_size 的整数倍:
  │   padded_size = ceil(blob_size / symbol_size) × symbol_size
  │   symbol_count = padded_size / symbol_size     ← source_count
  │   F = padded_size                              ← rq_init 的精确 F
  ├─ assert(symbol_count ≥ 1)
  ├─ rq_init(F, T=symbol_size) → symbol_count 个源符号
  ├─ M = ceil(symbol_count × overhead())
  │   rq_encode for ESI = 0..symbol_count+M-1
  └─ 对每个编码 shard:
       p = Packet(symbol_size, 32)
       memcpy(p.Data(), shard_data, symbol_size)
       if (obfuscate): 生成 iv_len 字节随机 IV; XOR; PushFrontLE(IV, iv_len)
       PushFrontLE(source_count, width)
       PushFrontLE(shard_index, width)
       if has_pending_echo: PushFrontLE(echo, 8); flags |= 0x80
       PushFrontLE(feedback_byte)
       dw = (group_seq & 0xFFFFFF) | (flags << 24);
       PushFrontLE(dw)
       co_await _Out->Write(p)
          └→ 端点 PushFront chan/RxId → send
```

### Decode (接收)

```
fiber loop:
  ┌─ co_await _In->Read(p)
  │   端点 PopFront chan/RxId
  ├─ uint32_t dw = PopFrontLE<uint32_t>()
  │  group_seq = dw & 0xFFFFFF; flags = (dw >> 24) & 0xFF
  ├─ PopFrontLE(1) → feedback
  ├─ if flags bit7: PopFrontLE(8) → echo_bytes [pending_echo = echo_bytes]
  │
  ├─ if bit4 (PING):
  │    PopFrontLE(8) → send_ts
  │    handle_ping(send_ts)  [设定 pending_echo = send_ts]
  │    continue loop
  │
  ├─ if bit5 (FEEDBACK_ONLY):
  │    continue loop
  │
  ├─ if bit6 (REPEAT):
  │    if (cfg.obfuscate) { iv_len = ((flags>>1)&0x7)+1; PopFront(iv_len)→IV; XOR data; }
  │    if 当前 group_seq 未交付过: filter chain → _Out->Write(p)
  │    else: 丢弃 (dedupe)
  │    continue loop
  │
  ├─ else (FEC data):
  │    width = flags bit0 ? 2 : 1
  │    PopFrontLE(width) → shard_index
  │    PopFrontLE(width) → source_count (= symbol_count)
  │    if (cfg.obfuscate) { iv_len = ...; PopFront(iv_len); XOR data; }
  │    │
  │    ├─ 按 group_seq 存入 ring buffer (记录 first_packet_time)
  │    ├─ 收够 symbol_count 个 shard_index → 解码
  │    │  或 now - first_packet_time > decode_timeout → 丢弃
  │    ├─ F = source_count × symbol_size
  │    ├─ assert(F > 0 && source_count ≥ 1)
  │    ├─ rq_init(F, T=symbol_size)
  │    ├─ rq_decode → 恢复 blob
  │    ├─ 更新丢包率统计 → 刷新 feedback = uint8_t(loss × 250)
  │    ├─ blob split: [u32 count][u16 len][data]×count → 原始 packets
  │    ├─ assert(count ≥ 1)
  │    ├─ filter chain: 对每个还原包 filter.Pipe()
  │    └─ 对每个原始包: co_await _Out->Write(p)
```

### 定时发送

Pipeline fiber 内用 `Select` 在 read 和 timer 间竞争：

```
每 ping_interval_ms 发送 PING (携带当前时间戳 + pending_echo + latest feedback)
超过 feedback_timeout_ms 未产生 FEC data → 发送 FEEDBACK_ONLY (携带 pending_echo + latest feedback)
```

### Decode timeout

```
decode_timeout = max(3 × rtt_ewma + timeout_ms, 50ms)
```

timeout_ms = encode_timeout (Lua 中的 `timeout` 参数)。初始 RTT: `timeout_ms × 10`。

### RTT 计算

PING 包携带发送时间戳 (payload 字段)。接收方收到后，将 `send_ts` 存入 `pending_echo`。
下一次向该端发送的**任何**包（FEC data / FEEDBACK_ONLY / PING / REPEAT）中，将 `pending_echo` 写入 `echo` 字段。
发送方收到 echo 后：`rtt_sample = now_us - echo_timestamp`。

### 反馈闭环

- 收方每 decode 一个 group → `feedback = uint8_t(loss_rate × 250)` (clamp 250)
- 写入包中 feedback 字段
- 发方收到后交给 `AdaptiveOverhead` 控制器处理
- `FecConfig` 新增 `algo` 字段选择算法

### 自适应 overhead 算法

RaptorQ 理论最小开销：给定丢包率 p，需 overhead ≥ p/(1-p) 才能恢复。
实际需加安全余量以覆盖：有限 block 的方差、丢包突发、RTT 反馈延迟。

以下列出全部候选算法，待实现后通过可控丢包测试（`test_drop_rate` 选项）对比选择。

---

#### 算法 0: Static（静态固定值）★ 当前实现

**原理**：不自适应，始终使用初始 `overhead` 值。

```
overhead = cfg.overhead  // 固定
```

**优点**：最简单，零计算开销。
**缺点**：无法响应链路变化，要么浪费带宽要么保护不足。
**用途**：作为 baseline 对照。

---

#### 算法 1: EWMA + Static Safety（带静态安全余量的指数平滑）★ 当前实现

**原理**：EWMA 平滑丢包率，加固定安全余量。

```
L_ewma[t] = α * L_sample + (1-α) * L_ewma[t-1]
overhead  = L_ewma / (1 - L_ewma) + safety_margin
```

| 参数 | 默认值 | 说明 |
|------|:---:|------|
| `alpha` | 0.3 | EWMA 平滑因子，越大反应越快但越抖 |
| `safety_margin` | 0.05 | 固定安全余量 5% |

**理论依据**：`p/(1-p)` 是无限 block size 下 Shannon 下界（3GPP TR 26.822）。
有限 K 需要额外余量（K=20 需 45%，K=100 需 24%，vs 理论 11.1%）。

**优点**：实现简单，平滑稳定。
**缺点**：反应滞后于丢包突变，安全余量不随 block 大小变化。

---

#### 算法 2: EWMA + Dynamic Safety（带动态安全余量的指数平滑）

**原理**：安全余量随丢包率波动自动缩放。

```
L_ewma[t]  = α * L_sample + (1-α) * L_ewma[t-1]
σ²[t]      = β * (L_sample - L_ewma)² + (1-β) * σ²[t-1]
safety     = safety_base + γ * sqrt(σ²)
overhead   = L_ewma / (1 - L_ewma) + safety
```

| 参数 | 默认值 | 说明 |
|------|:---:|------|
| `alpha` | 0.3 | EWMA 平滑因子 |
| `beta` | 0.2 | 方差平滑因子 |
| `safety_base` | 0.03 | 基础安全余量 |
| `gamma` | 2.0 | 波动放大系数 |

**理论依据**：当丢包率稳定时方差小 → 安全余量接近 safety_base，节省带宽。
当丢包率剧烈波动时方差大 → 安全余量自动扩大，应对突变。

**优点**：自动适应链路稳定性，优于固定余量。
**缺点**：增加一个平滑参数，调参略复杂。

---

#### 算法 3: PI Controller（比例-积分控制）

**原理**：经典控制论，以目标丢包率为 setpoint。

```
error     = L_target - L_measured     // 目标丢包 0，实际丢包 >0 则 error <0
integral  = clamp(integral + error * dt, -0.3, 0.3)  // 防积分饱和
overhead  = Kp * (-error) + Ki * integral
overhead  = clamp(overhead, 0.01, max_overhead)
```

| 参数 | 默认值 | 说明 |
|------|:---:|------|
| `Kp` | 1.5 | 比例增益 |
| `Ki` | 0.8 | 积分增益 |
| `L_target` | 0.01 | 目标丢包率（不为 0 避免永久补偿）|

**理论依据**：PID-FEC 机制（IJES 2019），Ziegler-Nichols 整定 Kp=1.52, Ki=1.43。
PI（去掉微分项）在测量噪声大时更鲁棒（INFOCOM 2017 PIA 控制器）。

**优点**：控制理论完备，稳态误差可消除，业界验证。
**缺点**：参数需整定，积分饱和需处理。

---

#### 算法 4: MIMD（乘性增加/乘性减少）

**原理**：解码失败→快速乘性增加，解码成功→缓慢乘性减少。

```
if decode_failed:
    overhead *= (1 + λ_up)      // 快速拉起
elif consecutive_success > N_stable:
    overhead *= (1 - λ_down)    // 缓慢回落
overhead = clamp(overhead, min_overhead, max_overhead)
```

| 参数 | 默认值 | 说明 |
|------|:---:|------|
| `lambda_up` | 0.50 | 失败时增加 50% |
| `lambda_down` | 0.05 | 成功时减少 5% |
| `N_stable` | 20 | 连续成功多少个 group 后才开始降 |

**理论依据**：类似 TCP 的 AIMD 但用乘法提速。RaptorQ 的 rateless 特性使得 overhead=0 也有 99.6% 成功率（p≤1% 时），因此 MIMD 可安全收敛到极小值。

**优点**：反应极快（丢包尖峰立刻拉高），稳态 overhead 自动收敛到最低值。
**缺点**：可能 overshoot（峰值 overhead 偏高），需 min_overhead 防止过低。

---

#### 算法 5: Quantile Target（分位数目标）

**原理**：用 P95/P99 丢包率代替均值，覆盖尖峰。

```
loss_window = queue<最近 N 个 group 的丢包率>
L_target    = percentile(loss_window, pct)  // 如 P95
overhead    = L_target / (1 - L_target) + safety_margin
```

| 参数 | 默认值 | 说明 |
|------|:---:|------|
| `window_size` | 64 | 滑动窗口 group 数 |
| `percentile` | 95 | 目标分位数 (90/95/99) |
| `safety_margin` | 0.03 | 在小分位数之上再加余量 |

**理论依据**：均值对丢包尖峰不敏感；P95 可覆盖 95% 的场景，避免为偶发尖峰过度补偿。
配合 RaptorQ rateless 特性，单次尖峰超出 overhead 时仅丢一个 group，影响可控。

**优点**：天然抗尖峰，不因偶发大丢包而过度反应。
**缺点**：窗口大小和分位数的选择需要经验调优。

---

#### 算法 6: Burst-Aware EWMA（突发感知指数平滑）

**原理**：区分背景丢包和突发丢包，分开统计。

```
if L_sample > L_ewma + burst_threshold:
    // 检测到突发
    L_burst[t] = α_fast * L_sample + (1-α_fast) * L_burst[t-1]
else:
    L_burst[t] = α_slow * L_burst[t-1]  // 缓慢衰减
L_bg[t]     = α_slow * L_sample + (1-α_slow) * L_bg[t-1]
overhead    = max(L_bg / (1-L_bg), L_burst / (1-L_burst)) + safety
```

| 参数 | 默认值 | 说明 |
|------|:---:|------|
| `alpha_slow` | 0.1 | 背景丢包平滑（慢） |
| `alpha_fast` | 0.6 | 突发丢包平滑（快） |
| `burst_threshold` | 0.05 | 超过 EWMA 多少判定为突发 |

**理论依据**：Gilbert 信道模型 — 丢包不是独立同分布，有"好状态"和"坏状态"。
分状态跟踪可避免突发结束后 overhead 回落过慢。

**优点**：对真实链路（含突发丢包）效果最好。
**缺点**：两个状态 + 阈值判断，实现稍复杂。

---

#### 算法 7: Gradient Throughput Optimization（梯度下降吞吐优化）

**原理**：直接优化目标函数 `throughput = (1-overhead) * (1-loss_rate)`。

```
// 观测: 上次 overhead 产生的实际 throughput
T_prev = (1 - overhead_prev) * (1 - L_measured)
// 微调 overhead，观测 throughput 变化
overhead_try = overhead_prev + δ
// 下一次测得的 throughput
T_try  = (1 - overhead_try) * (1 - L_new)
// 梯度方向
if T_try > T_prev:
    overhead = overhead_try          // 同方向继续
else:
    overhead = overhead_prev - δ    // 反向
```

| 参数 | 默认值 | 说明 |
|------|:---:|------|
| `delta` | 0.02 | 微调步长 |
| `eval_interval` | 500ms | 评估间隔 |

**理论依据**：TAROT（ACM MMSys 2026）— 优化驱动的 FEC 参数选择。
直接最大化有效吞吐而非最小化丢包率，避免"为消除最后 1% 丢包浪费 30% 带宽"的问题。

**优点**：理论上最优，不需要预设公式参数。
**缺点**：收敛慢，需在线探索（exploration cost）。

---

### 算法对照表

| 算法 | 反应速度 | 稳定性 | 实现复杂度 | 适用场景 |
|------|:---:|:---:|:---:|------|
| 0 Static | N/A | ★★★★★ | 最简单 | baseline 对照 |
| 1 EWMA+Static | ★★ | ★★★★ | 简单 | 当前实现，稳定链路 |
| 2 EWMA+Dynamic | ★★ | ★★★★ | 中等 | 丢包波动大的链路 |
| 3 PI | ★★★ | ★★★★ | 中等 | 需要稳态无差 |
| 4 MIMD | ★★★★★ | ★★★ | 简单 | 需要快速响应突变 |
| 5 Quantile | ★★ | ★★★★★ | 中等 | 偶发尖峰、稳定链路 |
| 6 Burst-Aware | ★★★★ | ★★★★ | 较复杂 | 真实链路含突发 |
| 7 Gradient | ★ | ★★★ | 复杂 | 理论最优、代价可接受 |

### 测试方案：可控丢包率

`FecConfig` 新增字段：

```cpp
struct FecConfig {
    // ... 现有字段 ...
    uint8_t algo = 1;            // 自适应算法 0~7
    float test_drop_rate = 0;    // 主动随机丢包率 0.0~1.0 (0=禁用)
    uint32_t test_drop_burst = 1; // 丢包突发长度 (1=随机独立丢包)
};
```

### 主动丢包模型（测试用）

主动丢包在 `FecPipeline::Process()` decode 路径入口实现：收到包后先经过 `LossPattern::ShouldDrop()`，
若返回 true 则丢弃（模拟丢包），否则送入 decoder。

`FecConfig` 字段：

```cpp
uint8_t  test_drop_pattern = 0;  // 丢包模型 0~6 (0=禁用)
float    test_drop_rate  = 0.0f; // 基础丢包率
float    test_drop_rate2 = 0.0f; // 辅助参数 (各模型含义不同)
uint32_t test_drop_burst = 1;    // 突发长度 / 周期
```

#### 模型 0: Disabled — 关闭主动丢包

#### 模型 1: Bernoulli（独立随机丢包）

每个包以概率 `p = test_drop_rate` 独立丢弃。**无记忆性**，相邻包丢包不相关。

```
ShouldDrop(): return rand() < p
```

最基础的 baseline。真实链路（尤其是无线链路）的丢包通常是突发的，
Bernoulli 模型无法体现这一点。

#### 模型 2: Gilbert（2 状态 Markov 突发丢包）

两个状态：Good（0% 丢包）和 Bad（100% 丢包）。

```
       p (Good→Bad)
    ┌──────────────►
  Good (0% loss)   Bad (100% loss)
    ◄──────────────
       r (Bad→Good)

平均突发长度 = 1/r
平均良好长度 = 1/p
稳态丢包率   = p / (p + r)
```

**参数映射**：
- `test_drop_rate` = 稳态丢包率目标
- `test_drop_burst` = 目标平均突发长度（packets）
- `r = 1.0 / test_drop_burst`
- `p = r * test_drop_rate / (1 - test_drop_rate)`

**适用范围**：无线衰落信道的一阶近似。丢包成簇出现，比 Bernoulli 更真实。

#### 模型 3: Gilbert-Elliott（2 状态 Markov 带背景丢包）

Gilbert 的扩展：Good 状态也有非零丢包率 k，Bad 状态丢包率 h<1。

```
       p (Good→Bad)
    ┌──────────────────────►
  Good (k% loss)        Bad (h% loss)
    ◄──────────────────────
       r (Bad→Good)
```

**参数映射**：
- `test_drop_rate` = 稳态丢包率目标
- `test_drop_rate2` = Good 状态丢包率 k（背景噪声，default 0.01）
- `test_drop_burst` = 目标平均突发长度
- `h = min(k + (test_drop_rate - k) / π_bad, 0.95)`
- `r = 1.0 / test_drop_burst`
- `p = r * π_bad / (1 - π_bad)`

**适用范围**：Wi-Fi、LTE/4G 等真实无线链路的 empirical 验证最佳模型。
标准文档广泛引用（str0m-netem, IEEE 802.11 仿真）。

#### 模型 4: Sinusoidal（正弦波动丢包）

丢包率随时间呈正弦变化，模拟周期性拥塞（如每天高峰时段、TCP 全局同步）。

```
loss_rate(t) = baseline + amplitude * sin(2π * t / period)
```

**参数映射**：
- `test_drop_rate` = 峰值丢包率（baseline + amplitude）
- `test_drop_rate2` = 谷值丢包率（baseline，默认 0.01）
- `test_drop_burst` = 周期（秒），默认 60

```
baseline  = test_drop_rate2
amplitude = test_drop_rate - test_drop_rate2
```

每个包仍按瞬时 loss_rate 做 Bernoulli 丢弃。

**适用范围**：测试算法对缓慢周期性变化的跟踪能力。

#### 模型 5: Step（阶跃突变丢包）

丢包率在指定时间点从低值突跳到高值（或反过来），模拟链路故障/恢复。

```
loss_rate(t) = rate_before  (t < step_time)
loss_rate(t) = rate_after   (t >= step_time)
```

**参数映射**：
- `test_drop_rate` = 突变后丢包率
- `test_drop_rate2` = 突变前丢包率（默认 0.01）
- `test_drop_burst` = 突变发生时间（秒），默认 30

**适用范围**：测试算法对突变的响应速度（反应延迟、overshoot）。

#### 模型 6: Congestion Wave（拥塞波丢包）

丢包率先线性爬升到峰值再线性回落，模拟真实拥塞事件（buffer 填满→排空）。

```
loss_rate(t) = min_rate + (max_rate - min_rate) * triangle(t / period)
```

其中 `triangle(x) = 2 * |2*(x mod 1) - 1|`（对称三角波）。

**参数映射**：
- `test_drop_rate` = 峰值丢包率
- `test_drop_rate2` = 基线丢包率（默认 0.01）
- `test_drop_burst` = 周期（秒），默认 120（2分钟爬升 + 2分钟回落 = 4分钟周期）

**适用范围**：最接近真实 Internet 拥塞模式。测试算法在丢包率持续变化下的表现。

### 丢包模型对照表

| 模型 | 名称 | 关键特征 | 测试目标 |
|:---:|------|------|------|
| 0 | Disabled | 无丢包 | 验证无丢包时 overhead 收敛到最小值 |
| 1 | Bernoulli | 独立随机 | baseline 对照 |
| 2 | Gilbert | 2状态突发 | 突发丢包适应能力 |
| 3 | Gilbert-Elliott | 2状态+背景噪声 | 真实无线链路模拟 |
| 4 | Sinusoidal | 周期性正弦 | 缓慢变化的跟踪能力 |
| 5 | Step | 阶跃突变 | 反应速度和 overshoot |
| 6 | Congestion Wave | 三角波拥塞 | 真实拥塞场景综合评估 |

### 测试流程

1. 同一机器启动两个 great-hole 实例（不同端口），loopback
2. 对每种丢包模型，设置不同丢包率档位 (1%, 5%, 10%, 20%)
3. 分别跑 8 种自适应算法，iperf3 测 TCP/UDP 吞吐
4. 记录每个测试的：有效吞吐、overhead 均值/峰值/稳态值、丢包恢复率、算法收敛时间
5. 按测试场景加权评分，汇总对比表供选择

### overhead 上限

overhead 上限由 `max_overhead` 控制（default 0.50）。

### 单包 REPEAT 比例

RaptorQ 的 rateless 特性不再需要 fine-grain 表：K≥2 时 overhead 由自适应系数决定。
仅 symbol_count=1 时用单独重复比例（不经 RaptorQ，纯复制）：

```lua
repeat_ratio = 4.0          -- 单包重复次数 = 1 + ceil(repeat_ratio) (=5)
```

`repeat_ratio = 0` 时单包不重复，直接走 FEC data 编码 (pkt_count=1 也做 blob+RaptorQ)。

## FecConfig 结构体

```cpp
struct FecConfig {
    uint32_t timeout_ms = 4;           // batch 最大等待时间 (min 1)
    float overhead = 0.15f;            // 初始冗余比例
    float max_overhead = 0.50f;        // 自适应上限
    float repeat_ratio = 4.0f;         // 单包 REPEAT 重复 = 1+ceil(ratio)
    uint32_t symbol_size = 0;          // RaptorQ 符号大小, 0=自动按 MTU 计算
    uint32_t mtu = 1500;               // 用于 symbol_size 计算
    uint32_t max_batch = 200;          // symbol_count 上限
    bool obfuscate = true;             // 启用 IV XOR
    uint8_t iv_len = 4;                // IV 字节数 1~8
    uint32_t decode_window = 64;       // ring buffer 容量 (max 256)
    uint32_t ping_interval_ms = 1000;  // PING 间隔
    uint32_t feedback_timeout_ms = 2000;  // 无数据反馈最大间隔
    uint32_t feedback_stale_ms = 10000;   // 无反馈回退 overhead 超时
    uint32_t ping_loss_threshold = 5;     // 连续丢 PING 阈值
    uint32_t decode_timeout_ms = 200;     // 初始解码超时 (RTT 校准后覆盖)

    // === 自适应算法 ===
    uint8_t algo = 1;                  // 算法选择 0~7

    // === 可控丢包测试 ===
    uint8_t test_drop_pattern = 0;     // 丢包模型 0~6 (0=禁用)
    float test_drop_rate = 0.0f;       // 基础丢包率 / 峰值
    float test_drop_rate2 = 0.0f;      // 辅助参数 (模型相关)
    uint32_t test_drop_burst = 1;      // 突发长度 / 周期 (模型相关)
};
```

## 配置验证

Pipeline Start 时检查，失败则终止：

| 验证 | 条件 | 行为 |
|------|------|------|
| `symbol_size + max_header ≤ mtu` | 硬约束 | 终止 |
| `symbol_size ≤ kCapacity - 32` | 硬约束 | 终止 |
| `iv_len ∈ 1..8` (obfuscate=true) | 或 obfuscate=false 忽略 | 终止 |
| `overhead ≤ max_overhead` | 自动 clamp | continue |

## 异常处理

| 场景 | 行为 |
|------|------|
| rq_encode 失败 | 丢弃 group, 记录 error, continue |
| rq_decode 失败 | 丢弃 group, 回收 ring buffer slot, continue |
| source_count = 0 | assert/terminate (不应发生) |
| feedback > 1.0 | clamp 到 250 |
| 空 batch | 跳过 |
| 无反馈超过 feedback_stale_ms | overhead 回退 +0.10 (cap max_overhead) |
| PING 连续丢 > ping_loss_threshold | 记录 warning |

## Lua API

```lua
fec_cfg = {
    timeout         = 4,
    overhead        = 0.15,
    max_overhead    = 0.50,
    repeat_ratio    = 4.0,
    symbol_size     = 0,            -- 0=自动按 MTU 计算
    mtu             = 1500,         -- 用于 symbol_size 计算
    max_batch       = 200,
    obfuscate       = true,
    iv_len          = 4,
    decode_window   = 64,
    ping_interval   = 1000,
    feedback_timeout = 2000,
    feedback_stale_ms = 10000,
    ping_loss_threshold = 5,
    decode_timeout  = 200,
    algo            = 1,            -- 自适应算法 0~7
    test_drop_rate  = 0.0,          -- 主动丢包率 (0=禁用)
    test_drop_burst = 1,            -- 丢包突发长度
}

-- FEC Pipeline
p_send = hole.fec_pipeline(app_chan, {xor_filter}, udp_chan, fec_cfg)
p_recv = hole.fec_pipeline(udp_chan, {xor_filter}, app_chan, fec_cfg)
```

`hole.fec_pipeline` 创建 `FecPipeline` 实例（`Pipeline` 的子类），自动判断 encode/decode 模式。

## Implementation Plan

### Phase 1: lcrq 集成 + Packet LE
- submodule `libs/lcrq`, CMake 集成
- C++ 封装 `rq_init/encode/decode`
- `Packet::PushFrontLE(uint16/32)`, `Packet::PopFrontLE<T>()`
- 单元测试: encode/decode round-trip

### Phase 2: Pipeline 重构 + FecConfig + Lua
- Pipeline: 提取 `virtual Process()` + 添加 `io_context&` 参数
- `FecPipeline : public Pipeline` — batch mode `Process()`
- `FecConfig` 结构体
- LuaLib: `hole.fec_pipeline()` 绑定

### Phase 3: FEC Encode / Decode
- Encode: batch 积累 + blob + rq_encode + header 写入
- Decode: ring buffer + group 超时 + rq_decode
- PING / FEEDBACK_ONLY / REPEAT 包
- 配置验证

### Phase 4: 混淆 + 反馈 + RTT
- IV XOR
- 丢包率统计 + feedback 闭环
- RTT echo 机制
- AdaptiveOverhead（算法 1 先实现）

### Phase 5: 可控丢包测试框架
- `test_drop_rate` / `test_drop_burst` 实现
- Gilbert 突发丢包模型
- 单机 loopback 测试脚本

### Phase 6: 多算法实现与对比
- 实现算法 0~7 共 8 种
- 同机可控丢包率测试 (0%, 1%, 5%, 10%, 20%)
- 每算法测 TCP + UDP 吞吐，记录 overhead 均值/峰值
- 汇总对比表，确定最终选择

### Phase 7: 真实链路验证
- ali-osaka / ali-tokyo 实际链路测试
- 与算法 0 (static) 对比提升幅度
- 长时稳定性测试 (24h+)

## Test Results — 8 算法矩阵测试

> **测试环境**: tokyo, netns 隔离, 100Mbps / 100ms RTT (tc netem on veth), iperf3 TCP 10s  
> **配置**: Static 基准 overhead=15%, 自适应算法 overhead=1% 起步, safety_margin=0.01  
> **矩阵**: 8 算法 × 6 丢包模式 × 4 丢包率 = 192 项, 零测试失败 (status=ok 192/192)  
> **日期**: 2026-07-02 ~ 2026-07-03

### 原始测试数据

完整 CSV: `tokyo:/tmp/regression.csv` (192 行)

### 8 算法对比矩阵 (recv Mbps, iperf3 TCP)

#### Static (15% overhead) — 基准对照
| Pattern | 1% | 5% | 10% | 20% |
|---------|:---:|:---:|:---:|:---:|
| Bernoulli | 52.6 | 13.6 | 11.1 | 0.3 |
| Gilbert | 18.9 | 0.9 | **0.0** | 0.1 |
| GElliott | 72.7 | 1.4 | 0.4 | 0.1 |
| Sine | 61.4 | 65.3 | 40.8 | 24.0 |
| Step | 64.9 | 54.9 | 7.3 | 8.0 |
| CongWave | 72.2 | 62.7 | 34.6 | 27.5 |
| **平均** | **57.1** | **33.1** | **15.7** | **10.0** |

#### PI (1%→自适应) — 最佳自适应
| Pattern | 1% | 5% | 10% | 20% |
|---------|:---:|:---:|:---:|:---:|
| Bernoulli | **57.8** | 6.9 | 1.5 | 0.3 |
| Gilbert | **39.5** | **13.0** | **1.7** | 0.0 |
| GElliott | 15.6 | **3.2** | 0.5 | 0.2 |
| Sine | **71.4** | 37.0 | **57.7** | 19.1 |
| Step | 61.8 | 28.8 | **28.6** | 9.8 |
| CongWave | 70.0 | 58.7 | **62.2** | **40.1** |
| **平均** | **52.7** | **24.6** | **25.4** | **11.6** |

#### MIMD (1%→即刻响应)
| Pattern | 1% | 5% | 10% | 20% |
|---------|:---:|:---:|:---:|:---:|
| Bernoulli | 10.8 | 1.1 | 0.8 | 0.2 |
| Gilbert | 12.4 | 1.5 | 0.7 | **0.2** |
| GElliott | 4.1 | 0.9 | **0.8** | **0.3** |
| Sine | 30.5 | 17.3 | 25.4 | 7.9 |
| Step | 7.2 | 12.7 | 10.2 | **11.8** |
| CongWave | 35.4 | 34.9 | 31.1 | 14.4 |
| **平均** | **16.7** | **11.4** | **11.5** | **5.8** |

#### Gradient (1%→梯度下降)
| Pattern | 1% | 5% | 10% | 20% |
|---------|:---:|:---:|:---:|:---:|
| Bernoulli | 52.9 | 2.4 | 0.7 | 0.2 |
| Gilbert | 11.4 | 1.0 | 0.6 | 0.4 |
| GElliott | 33.5 | 2.9 | 0.4 | 0.1 |
| Sine | 65.4 | **66.0** | 11.2 | 19.0 |
| Step | 13.4 | 5.1 | 5.3 | 4.0 |
| CongWave | 62.4 | 45.3 | 40.5 | 38.9 |
| **平均** | **39.8** | **20.5** | **9.8** | **10.4** |

#### EWMA+Stat / EWMA+Dyn / Quantile / BurstAware
| Algo | 平均 | 特点 |
|------|:---:|------|
| EWMA+Stat | 10.4 | 收敛过慢，10s 内效率低 |
| EWMA+Dyn | 9.4 | 动态余量在 Sine 有优势 |
| Quantile | 9.8 | 无显著差异 |
| BurstAware | 8.1 | 突发感知未发挥作用 (测试时间太短) |

### 最终排名

| # | 算法 | 平均 Mbps | 每格最优 | 零死格 | 推荐场景 |
|:--:|------|:---:|:---:|:---:|------|
| 1 | **PI** | 29.8 | **10/24** | 1 | **全场景最优自适应**, 积分控制收敛快 |
| 2 | Static (15%) | **30.2** | 9/24 | 1 | 稳定链路, 低丢包, 最简单 |
| 3 | Gradient | 20.1 | 2/24 | 0 | Sine/CongWave 时变丢包 |
| 4 | MIMD | 11.4 | 3/24 | **0** | **最可靠**, 突发丢包无死角 |

### 关键发现

1. **PI 是最佳自适应算法**: 平均 29.8 Mbps 接近 Static 的 30.2。Gilbert 10% 时 PI=1.7 vs Static=0.0（Static 死透）。集成控制 (Kp=1.5, Ki=0.8) 令其 10 秒内快速收敛。

2. **Static 15% 基准线可靠但脆弱**: 平稳丢包无敌，但 Gilbert 10% 跌至 0 Mbps——链路彻底断开。

3. **MIMD 零死格**: 唯一在所有 24 格都保持 >0 的算法。即时×1.50 反应优势在突发丢包中体现，但低丢包时 overhead 过高导致吞吐偏低。

4. **EWMA 类算法在短测试中无效**: 从 1% 起步，10 秒测试 + 100ms RTT 反馈延迟不足以让 alpha=0.3 的 EWMA 收敛到目标水平。

5. **丢包模式影响远大于丢包率**: Gilbert 10% (0 Mbps) 比 Bernoulli 20% (0.3 Mbps) 更致命。突发丢包导致 tunnel 断连。

6. **100ms RTT 对自适应不利**: 反馈环路延迟使收敛时间翻倍。MIMD 的即时反应在此场景下是正确选择。

### 自适应算法选型决策树

```
链路特征:
  ├─ 稳定, 丢包率 <5%       → Static (15% overhead)
  ├─ 偶尔突发丢包 (10-20%)   → MIMD (即时反应, 最可靠)
  ├─ 持续可变丢包 (5-15%)    → PI (积分控制, 收敛快)
  └─ 时变丢包 (正弦/拥塞波)  → Gradient 或 PI
```

### 已知局限

- **10 秒测试太短**: EWMA/Quantile/BurstAware 的慢收敛被放大。建议至少 30 秒 iperf3。
- **初始 overhead=1% 偏低**: 从 1% 起步对 EWMA 过于苛刻。建议默认从 5% 起步。
- **Static 不应算作"自适应"**: Static 15% 是固定基准，非自适应。后续测试应分离 Static 和自适应算法的初始 overhead。
- **100ms RTT 是模拟值**: 实际 ali↔tokyo 约 60ms RTT。本地 netns 测试用于功能验证，性能对比以远程实测为准。

## Non-Goals

- 不协商 — 双方配置完全一致
- 不改变现有 Filter 接口
- DynMux 控制包不带 FEC

### 已解决的设计问题

- **Timer 集成**: `steady_timer` + `AsioUseFiber` + `Select` 在 omni-fiber 中完全可用。Pipeline 构造加 `io_context&`。
- **Pipeline batch 模式**: `Pipeline::virtual Process()` → `FecPipeline::Process()` override。

## 实测性能总结 (2026-07-05)

### 测试环境

- Ali (39.108.136.48) ↔ Tokyo (202.144.195.145), RTT ~60ms
- 直连带宽: TCP 101 Mbps, UDP 100 Mbps 零丢包
- FEC 配置: `timeout_ms=4, max_batch=20, overhead=0.01, algo=0 (Static)`
- 编译器: Debian 13, clang-19, Boost 1.83

### 吞吐量对比

| 测试 | 直连 | FEC (RaptorQ) | 嵌套 (UDPspeeder RS 50%OH) |
|------|:---:|:---:|:---:|
| TCP T→A | 101 Mbps | **42 Mbps** | 62 Mbps |
| TCP A→T | 95.7 Mbps | - | 49.7 Mbps |
| TCP CWND 峰值 | 1 MB | 350 KB | 650 KB |
| TCP 重传 | 1523 | **166** | 3004 |
| UDP 80M | 79.7 (0%) | **79.5 (0%)** | 66.7 (16% loss) |
| UDP 100M | 97.5 (0%) | **87.0 (0%)** | 60.3 (39% loss) |

### 架构分析

**FEC 编码器最终设计 (Two-fiber)**:

```
Reader fiber: co_await TUN Read() → TryRead loop (~70% hit rate, ~3.3 pkts/cycle)
                                  → push to batch_queue

Main fiber:   queue empty → 100us poll timer
              queue data  → drain to batch
              batch full (max_batch=20) or timeout (4ms) → SendBatch
              SendBatch: pkt_count==1 → REPEAT copies=N (fast path)
                         pkt_count>1  → RaptorQ K symbols + ceil(K*oh) repair
```

**关键发现**:

1. **TryRead 成功率 ~70%**（非 handoff 中声称的"总是 EAGAIN"）。reader 每周期产出 ~3.3 个包。
2. **FEC 实际开销 5.7%**（非配置的 1%）。原因: `ceil(K×0.01)` 取整，K≈17 时 ceil(0.17)=1，`1/17=5.9%`。需 K≥100 才能实现真正的 1%。
3. **Batch 延迟是 TCP 吞吐杀手**（非 FEC 开销）。UDPspeeder (50% OH, 62 Mbps) 比 RaptorQ (5.7% OH, 42 Mbps) 快 48%，因为 UDPspeeder 不制造 ACK 压缩/延迟抖动。
4. **FEC 消除丢包但对 TCP CWND 增长有抑制作用**：FEC TCP 重传 166 vs 直连 1523，但 CWND 仅 350 KB vs 直连 1 MB。
5. **Boos.Asio epoll 为 EPOLLET（边沿触发）**。`async_read_some` 投机执行单次 `readv()`，配合 TryRead 循环排空缓冲。
6. **PING/FEEDBACK 开销微秒级**（UDP async_send_to 立即完成），非 RTT 延迟。无需隔离到 batch 之间。

### 未来改进方向

1. **即发后补 (send-immediately + repair-later)**: 数据包到达即发送（REPEAT copies=1，零延迟），凑够 K 个后补发 RaptorQ 修复符号。需改造 wire format（包对齐 symbol 边界）和 decoder（REPEAT + repair 符号混合解码）。
2. **减小 max_batch + 增大 timeout**: 减少 batch 突发度，降低 ACK 压缩效应。
3. **解码端 pace 输出**: 解码后的原始包以微间隔输出到 TUN，避免 TCP ACK 爆发。

## 追加发现 (2026-07-05 深夜调试)

### FEC 版 Pipeline 基类改动导致 nofec 性能退化 3 倍

使用原始 great-hole 二进制（Ali-Osaka 版本）和 FEC 版 great-hole-fec 二进制，运行**完全相同的 nofec 配置**对比：

| 版本 | 配置 | 吞吐 | CWND |
|------|------|------|------|
| 原始 great-hole (v0.2.0) | XOR + Pipeline | **93.9 Mbps** | 596-748 KB |
| great-hole-fec (当前分支) | XOR + Pipeline (同配置) | **27.7 Mbps** | 287-321 KB |
| great-hole-fec | FecPipeline (RaptorQ) | 38-45 Mbps | 246-361 KB |

**结论：FEC 分支对 Pipeline 基类的改动（`virtual Process()` + `io_context&`）破坏了普通 Pipeline 的性能，即使不使用 FecPipeline 也受影响。** FecPipeline 自身的 batch 延迟进一步降低了吞吐。原始二进制恢复后隧道吞吐从 27.7 恢复到 93.9 Mbps。

### ER-X 硬件瓶颈

Ali↔ER-X 间 WireGuard 性能非对称：
- ER-X **接收** (Ali→ER-X): 145 Mbps — 解密快
- ER-X **发送** (ER-X→Ali): **48 Mbps** — MT7621A CPU 加密上限

Google 测速下载 47.7 Mbps 即受限于 ER-X WireGuard 发送能力。上传 7.5 Mbps 是因为 Google 选中香港服务器（325ms RTT）导致 TCP BDP 受限。

### 出口切换持久化

`switch-exit` 脚本已更新：切换出口时写入 `/etc/great-hole/fec/exit-target`，`wg0.conf` PostUp 读取此文件决定使用 table 101 (Osaka) 或 table 102 (Tokyo)。WireGuard 重启/服务器重启后出口选择保持不变。

## 实测性能基线 (2026-08-05, 新 Tokyo 1vCPU)

> **测试环境**: Ali (39.108.136.48) ↔ Tokyo (202.144.195.103, 2026-08-04 重建), RTT ~65ms
> **Tokyo 规格**: Debian 13, **1 vCPU** (AMD EPYC-Rome), 1.9GB RAM, ~23% steal time
> **测试链路**: fec-test 专用隧道 (UDP 20086 直连, 不经 speederv2), TUN 172.31.40.0/30, MTU 1420
> **FEC 配置**: timeout_ms=1, max_batch=20, overhead=1% PI (algo=3), symbol_size=1440

| 测试 | 直连 | FEC 隧道 | 说明 |
|------|:---:|:---:|------|
| TCP T→A | 36~69 Mbps | **16.1 Mbps** | 直连波动大 (链路质量波动) |
| TCP A→T | 91.7 Mbps | **13.7 Mbps** | 方向不对称 |
| UDP 80M T→A | 76.4 (0%) | **27.0 Mbps** | |
| UDP 100M T→A | 95.4 (0%) | - | |
| UDP 80M A→T | - | **25.1 Mbps** | iperf3 lost% 因 FEC 重排失真 |

### 关键结论

1. **新 Tokyo 链路直连能力完好** (UDP 95M / TCP 91.7M A→T)，重建实例无带宽损失。
2. **FEC 编码器吞吐上限 ~27 Mbps (双向一致)** — 1 vCPU 是硬瓶颈 (RaptorQ 编码 + 双 fiber + 23% steal)。历史 42Mbps 数据来自旧 Tokyo 多核实例。
3. **调参有效**: timeout_ms 4→1 + max_batch 200→20 使 FEC TCP 3.8→16.1 Mbps (×4)。
4. **直连 TCP 方向不对称** (T→A 36~69 vs A→T 91.7) — 国际链路波动，非隧道问题。
5. FEC UDP 与直连的差距 (27 vs 95M) 全部来自编码 CPU，非链路或配置。

## Batch 延迟假说验证 (2026-08-05) — 旧结论修正

> 旧结论 (2026-07-05): "Batch 延迟是 TCP 吞吐杀手" — **实测证伪，已修正**

### 代码事实

`SendBatch` FEC 路径 (FecPipeline.cpp:327-363): `BuildBlob`（攒全部包）→ 一次性 `rq.Encode(blob)` → 循环 `GenerateSymbol` → 最后 `WriteBatch` 整体发出。**第一个符号确实必须等整组编码完成** — "攒够全组才能开始生成第一个包"属实。

### 实测对照 (tokyo 1vCPU, 2026-08-05)

| 配置 | 组延迟 | TCP | UDP 80M |
|------|:---:|:---:|:---:|
| timeout=1ms, max_batch=20 | 低 | 16.1M | 27.0M |
| **max_batch=1** (单包 REPEAT 快路径) | **零** | **16.7M** | **53.4M** |
| timeout=8ms, max_batch=200 | 高 | **21.3M** | - |

### 结论

1. **组延迟与 TCP 吞吐无相关性** (batch=1 零延迟 TCP 仍 16.7M；8ms 长延迟反而 21.3M) — "batch 延迟导致 TCP 差"证伪。
2. **TCP 瓶颈 = 单核 CPU 饱和**: TCP 测试中 great-hole-fec 进程 CPU 达 99.9% (双向数据+ACK 都要 FEC 编解码)。UDP 单向编码 → CPU 限制点不同。
3. **RaptorQ 每包开销 > speederv2 GF256 档位编码**: 生产嵌套 (speederv2, 小包 1:1 复制) TCP 38.9M vs great-hole FEC 16-24M，同为单核。
4. UDP 受编码 CPU 限制: batch=20 时 RaptorQ 编码 27M；batch=1 REPEAT 无编码 53.4M — 印证 RaptorQ 编码是 CPU 大头。
5. **推论**: 提升 FEC TCP 吞吐的正路 = 减每包 CPU 开销 (ACK/小包走 REPEAT copies=1 直发, 仅大包 RaptorQ) + 多核，而非调 batch 延迟。

## lcrq 复测 (2026-08-05) — 研究数据失真确认

> 研究阶段 (fec-research.md) 记录: osaka 563Mbps / ali 1769Mbps (K=32K symbols, T=1024)
> **复测 (tokyo 1vCPU AMD EPYC-Rome, lcrq v0.3.1, 官方 examples/speedtest.c):**

| K | T | 编码 | 解码 | 备注 |
|:--:|:--:|:---:|:---:|------|
| 17 | 1440 | **31.4 Mbps** | 30.2 Mbps | 我们的实际组大小 (max_batch=20) |
| 17 | 1440 | 34.2 Mbps (-O3) | 30.8 | 优化级别无影响 |
| 200 | 1024 | 31.0 Mbps | 30.3 | |
| 1000 | 1024 | 8.9 Mbps | - | K 增大 init 开销 O(K²) 反噬 |
| 32768 | 1024 | (init >20min 未完成) | - | 研究参数在本机不可行 |

### 结论

1. **lcrq 小 K (K≤200) 在 tokyo 单核实测 ~30 Mbps** — 与我们 FEC 隧道 UDP 27M **完全吻合**。集成无额外损失，瓶颈就是 lcrq 编码本身。
2. **研究数据失真**: 563/1769 Mbps 来自 **AVX-512 CPU (osaka Cascadelake / ali Xeon Platinum) + K=32K 摊薄**。tokyo EPYC-Rome **无 AVX-512**，且 K=32K 的 rq_init O(K²) 矩阵预计算在本机 >20 分钟 — 研究参数在部署环境不可复现。
3. **-Og 与 -O3 无差异** (31.4 vs 34.2M) — 优化级别不是原因。
4. **修正归因**: "batch 延迟"假说证伪 → "CPU 饱和"现象属实 → 根因 = **lcrq 小 K 编码吞吐 (无 AVX-512 时 ~30Mbps)**。REPEAT 快路径 (batch=1, UDP 53.4M) 绕过 RaptorQ 是当前唯一有效提速手段。
5. **推论**: FEC 隧道吞吐上限 = min(链路, lcrq 小K编码吞吐)。换 AVX-512 实例或减少 RaptorQ 组 (ACK/小包 REPEAT 直发) 才能突破。

## lcrq 瓶颈深挖 (2026-08-05) — 优化与 AVX-512 排除

### 此前结论修正

1. **"-O3 与 -Og 一致"是假象**: configure 的 CFLAGS 只写入顶层 Makefile，`make -C src` 子目录 make 不继承 → 两次测试实际都是 **-O0** 编译 (编译命令 `cc -fPIC -I.` 无 -O 标志确认)。用 `make CFLAGS='-O3 -march=native'` 真正重编后 K=17 仍 32.7M — **优化级别不是瓶颈**。
2. **AVX-512 排除**: tokyo (AMD EPYC-Rome) 与 osaka (Xeon) 的 /proc/cpuinfo **均无 avx512 标志** (云 vCPU 未透传)，且两台机器 K=17 speedtest **实测一致 (31.4 vs 31.9 Mbps)** — 机器差异不是瓶颈。

### 微基准定位 (tokyo, K=17, T=1440, 逐阶段计时)

```
rq_init     0.001 ms   (可忽略)
rq_encode   5.0-11.3 ms  ← 占 99%，瓶颈所在
rq_symbol ×18  0.06-0.10 ms (可忽略)
```

**rq_encode = RFC 6330 中间符号计算 (高斯消元 phase0-3 + 矩阵求解)**, 每批一次, O(L²·T) 字节运算, 数学必须开销。K=17 时约 5-11ms/组 → 编码上限 ~32Mbps。

### 最终归因

- FEC 隧道 UDP 27M = lcrq K≤200 编码上限 (~30M) − UDP I/O 开销, **集成无额外损失**。
- 研究数据 (osaka 563M / ali 1769M, "K=32K symbols") 在两台机器当前构建下**不可复现** (K=1000 已崩至 8.9M, K=32K init >20min; K=17~200 全区间 ~30M) — 研究数据存疑。
- **突破路径不变**: REPEAT 快路径 (batch=1 UDP 53.4M 已证) 或换 AVX-512 实例 (未验证, 云 vCPU 普遍不暴露 avx512)。

## AVX-512 实锤 (2026-08-05) — 同一二进制三机对照

### 决定性实验: tokyo 编译 (-O3 -march=native) 的同一 lcrq-speedtest 二进制在 3 台机器跑 K=17

| 机器 | CPU | AVX-512 | K=17 编码 | 倍数 |
|------|-----|:---:|:---:|:---:|
| tokyo | AMD EPYC-Rome 1vCPU | 无 (云未透传) | 31.4 Mbps | 1× |
| osaka | Xeon (Cascadelake) | 无 | 31.9 Mbps | 1× |
| **ali** | Xeon Platinum | **有 (f/bw/cd/dq/vl)** | **241.5 Mbps** | **7.7×** |

ali K=200: 183.1 Mbps。

### 为什么 -O3 不是瓶颈（信服解释）

1. lcrq 热路径 = **手写 SIMD intrinsics** (gf256_avx2.c / matrix_avx512.c)。intrinsics 编译为固定 SIMD 指令，**编译器 -O 级别不影响 intrinsics 执行** — 这是 -O0/-O3 无差异的根本原因。
2. 无 AVX-512 时走标量/查表路径，编译器优化对查表+位操作提升有限 (31.4→32.7M, +4%)。
3. 同一二进制在 ali 快 7.7 倍 = 纯指令集差异。**瓶颈 = AVX-512 可用性，不是优化级别**。

### 研究数据 (1769M) 溯源

- ali (AVX-512) 是研究基准机 — 数据真实但仅代表 AVX-512 机器 + K=32K 摊薄。
- tokyo/osaka 云 vCPU 未透传 avx512 → 实际部署场景只有 ~30M。
- **fec-research.md 的吞吐数据必须标注"仅 AVX-512 机器有效"**。

### 对隧道的影响

- tokyo 端编/解码均 ~30M (无 AVX-512) → FEC 隧道双向都受 tokyo 限制 (~27M UDP) — 与实测一致。
- ali 端编码 241M → 若 tokyo 换 AVX-512 实例, FEC 隧道可提升 ~7.7×。
- REPEAT 快路径 (53.4M) 仍是无 AVX-512 环境下的唯一现实突破。

### 构建文件修复 (同批提交)

- 丢失的 `libs/lcrq/CMakeLists.txt` 从 ali 部署副本找回 → 正式化为 `cmake/lcrq.cmake` (ExternalProject + IMPORTED target)。
- 主 CMakeLists.txt: `add_subdirectory(libs/lcrq)` → `include(cmake/lcrq.cmake)`。
- tokyo 全新建树验证可复现。

## 终极归因 (2026-08-05) — tokyo vCPU 算力配额是唯一瓶颈

### 决定性实验链

1. **AVX2 vs AVX512 无差异**: ali 同机同 -O0 库, 仅切换调度路径 — avx512 216.0M vs avx2-only 212.6M (≈相同)。lcrq 的 shuffle 查表 SIMD 瓶颈在内存访问, 不在向量宽度。**"avx2 重写提速"不成立 — AVX2 已启用且与 AVX512 等价**。
2. **机器算力差距 6.6 倍**: 同一库 (avx2-only, -O0): tokyo 32.5M vs ali 216M。
3. **CPU 基准**: tokyo 标称 2794 MHz 但 30M 整数循环 9.8s vs ali (2500 MHz) 3.8s — **tokyo vCPU 实际算力只有 ali 的 ~38%** (2.6×), SIMD/内存密集任务放大到 6.6×。

### 结论

- lcrq 无 avx512 时回退 **AVX2** (非 SSE2, cpu.c 逐级检测 + matrix.c 调度正确, 已实证)。
- **FEC 27M 瓶颈 = GGC tokyo vCPU 算力配额** (超售/节流), 与指令集、优化级别、batch 延迟、集成均无关。
- **提速正路**: 换更高算力 vCPU (同 ali 算力即可 ~200M, 无需 AVX-512), 或减少 RaptorQ 使用 (REPEAT 快路径 53.4M)。
- 客服邮件应诉求: CPU 配额/节点超售, 而非 AVX-512。
