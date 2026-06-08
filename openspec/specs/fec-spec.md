# FEC Integration Spec

## Status

Draft — 基于 UDPspeeder 调研和 great-hole 代码分析，design 阶段。
FEC 作为 Pipeline 的可配置功能，全部 PushFront，header 位于端点头与 IV+XORed payload 之间。

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
- 发方收到后：`ewma = α × new_loss + (1-α) × ewma` (α=0.3)
- 实际 overhead = `min(ewma/(1-ewma) + safety_margin, max_overhead)`

### 自适应 overhead

| 丢包率 p | overhead = p/(1-p) | +5% 安全 |
|:---:|:---:|:---:|
| 10% | 11.1% | 16.1% |
| 15% | 17.6% | 22.6% |
| 20% | 25.0% | 30.0% |

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
- AdaptiveOverhead

### Phase 5: 集成测试
- 本地 loopback: encode → 丢包仿真 → decode
- ali-osaka 实际链路

## Non-Goals

- 不协商 — 双方配置完全一致
- 不改变现有 Filter 接口
- DynMux 控制包不带 FEC

### 已解决的设计问题

- **Timer 集成**: `steady_timer` + `AsioUseFiber` + `Select` 在 omni-fiber 中完全可用。Pipeline 构造加 `io_context&`。
- **Pipeline batch 模式**: `Pipeline::virtual Process()` → `FecPipeline::Process()` override。
