# Great-Hole 封包结构与协议文档

> 基础设施文档，供 FEC 及后续功能设计参考。

## 1. Packet 缓冲区结构

**文件**: `src/core/Packet.hpp`

```
|<--   offset=2    -->|<----------- length ----------->|<-- unused back -->|
|   reserved_front    |              data               |                   |
|<--  byte 0    byte1 | byte2  ...  byte (2+length-1)  | ...  byte 2047   |
|<---------------------    capacity = 2048    ---------------------------->|
```

| 常量 | 值 | 行 | 说明 |
|------|:---:|-----|------|
| `kCapacity` | **2048** bytes | 16 | 内部缓冲区总大小 |
| `kReservedFront` | **2** bytes | 17 | 默认前置保留空间 |
| `_Offset` 初始值 | **2** | 19 | 构造函数默认 |
| `_Length` 初始值 | **2048** | 19 | 构造函数默认 |

逻辑数据窗口：`_Data[_Offset]` ~ `_Data[_Offset + _Length - 1]`。

### 容量计算

| 方法 | 公式 | 默认构造后 |
|------|------|:---:|
| `DataSize()` | `_Length` | 2048 |
| `FrontSpace()` | `_Offset` | **2** |
| `BackSpace()` | `_Data.capacity() - _Offset - _Length` | 0 |

### 操作对布局的影响

```cpp
// 在前端添加 1 字节 → 占用 reserved_front
PushFront(uint8_t value):       _Offset -= 1; _Length += 1;  // FrontSpace -= 1

// 移除前端字节 → 释放 reserved_front
PopFront(size_t size):          _Offset += size; _Length -= size;  // FrontSpace += size

// 在尾部添加（不能超过 capacity - offset - length）
PushBack(span):                 {写入尾部}; _Length += span.size();  // BackSpace -= span.size()
```

`PushFront` 用于写入包头，大端序（网络字节序）。`Data()` 返回的 `span` 仅包含 `[_Offset, _Offset+_Length)`，因此 XOR 等 Filter 不触及 reserved 区。

## 2. 三种模式对比

| | P2P (Udp) | Mux-Server (UdpMux) | DynMux (UdpDynMux) |
|---|---|---|---|
| **包头大小** | **0 字节** | **1 字节** | **2 字节** |
| **占用 reserved** | 0 / 2 | 1 / 2 | 2 / 2 |
| **多路方式** | 按 peer IP:port | 预配 Channel ID (0-255) | PSK 协商 + 动态 RxId |
| **认证** | 无 | 无 | 16-byte PSK |
| **连接状态** | 无状态 | 无状态 | kNegotiating → kRunning |

## 3. Wire 格式

### 3.1 P2P 模式 (EndpointUdp)

```
线路上:
  [payload N bytes]        ← 零开销，纯数据

发送:
  UdpChannel::Write(p) → Udp::WriteTo(peer, p)
  → _Socket.async_send_to(p.Data()的 span=offset..offset+length, peer)

接收:
  _Socket.async_receive_from(p, peer)  ← 填充 offset=2 开始
  → peer 查找 _Channels[peer] → UdpChannel::Read()
```

dispatch 键是 `boost::asio::ip::udp::endpoint`。

### 3.2 Mux-Server 模式 (EndpointUdpMux)

```
线路上:
  [1B ChannelId (uint8_t)][payload N bytes]

发送 (WriteTo):
  p.PushFront(id)           ← 占用 1 byte reserved front
  _Socket.async_send_to()

接收 (ReadLoop):
  p.PopFront(1) → id       ← 释放 reserved front
  _Channels[id] 查找
  channel->Send(p)
```

Channel 0 是合法 ID。对方地址自动学习 / 自动重映射。

### 3.3 DynMux 模式 (EndpointUdpDynMux)

```
线路上:
  控制包: [2B 0x0000][1B MsgType][payload]
  数据包: [2B RxId (BE)][payload N bytes]

发送数据 (Channel::Write):
  p.PushFront(_RemoteRxId)   ← 2 bytes BE, 占用全部 reserved front
  _Parent.WriteTo(peer, p)

接收 (ReadLoop):
  channelId = ReadUint16BE(p, offset=0)
  数据: p.PopFront(2) → 查找 _RxIdToChannel[channelId]
  控制: 读 byte[2] MsgType → 按 PSK 或 channelId 分发
```

#### DynMux 控制包 (MsgType)

| 类型 | 值 | 大小 | 格式 |
|------|:---:|:---:|------|
| INITIATE | 0x01 | **23B** | `[0000][01][16B PSK][2B MyRxId BE][2B PeerRxId BE]` |
| KEEPALIVE | 0x03 | **19B** | `[0000][03][16B PSK]` |
| KEEPALIVE_ACK | 0x04 | **19B** | `[0000][04][16B PSK]` |
| INVALID_PSK | 0x09 | **19B** | `[0000][09][16B PSK]` |
| INVALID_CHANNEL | 0x0A | **5B** | `[0000][0A][2B ChannelId BE]` |

## 4. 交互时序

### 4.1 P2P 模式

```
  Client                                Server
  ──────                                ──────
  UdpChannel                            UdpChannel
    │                                      │
    │  Write(p)                            │
    ├──→ Udp::WriteTo(peer, p)             │
    │      async_send_to ──────────────────→ async_receive_from
    │                                      │  _Channels[peer] → Send(p)
    │                                      │
    │  Read(p)  ←─ _InPipe.Consumer() ──── ReadLoop fiber
    │  ← app consumes                      │
```

双 Pipeline 完成双向通信：

```
  p1: App → XOR → UdpChannel(c1) → [::]:25525 → network
  p2: network → [::]:24252 → UdpChannel(c2) → XOR → App
```

### 4.2 Mux-Server 模式

```
  Client A (id=1)                  Server                 Client B (id=2)
  ──────────────                  ──────                 ──────────────
  Channel(1)::Write(p)            UdpMux                 Channel(2)
    │                                │
    │  PushFront(0x01)               │
    │  async_send_to ─────────────→  async_receive_from
    │                                │  PopFront(1) → id=1
    │                                │  _Channels[1] → Send(p)
    │                                │  → Channel(1)::Read()
    │                                │
    │                Channel(2)::Write(p)
    │                                │  PushFront(0x02)
    │                  ←──────────── async_send_to
    │  async_receive_from ←─────────
    │  PopFront(1) → id=2
    │  _Channels[2] → Send(p)
    │  → Channel(2)::Read()
```

多个 Channel 共享一个 UDP socket，按 1-byte ID 多路。

### 4.3 DynMux 模式（协商 + 数据传输）

```
  Client                                Server
  ──────                                ──────
                                         UdpDynMux (port P)
  create_channel(psk)
    │
    │  [kNegotiating]
    │  INITIATE(psk, rxId_C, 0) ─────────→
    │                                      │
    │                                      │  [kNegotiating]
    │           ←───────────── INITIATE(psk, rxId_S, rxId_C)
    │  [kRunning]                          │  [kRunning]
    │                                      │
    │  Data: PushFront(rxId_S)             │
    │  packet ──────────────────────────→  │  PopFront → rxId_S 匹配
    │                                      │  Send to Channel(data pipe)
    │                                      │
    │           ←────────────────── Data   │
    │  PopFront → rxId_C 匹配              │
    │                                      │
    │  KEEPALIVE(psk) ───── (30-60s) ────→ │
    │           ←──── KEEPALIVE_ACK(psk) ── │
    │                                      │
    │  180s 无响应 → [kNegotiating] 重协商  │
```

## 5. TUN 端点交互

### TUN 读/写

```cpp
// EndpointTun.cpp
// IFF_TUN | IFF_NO_PI → 纯 IP 包，无 4-byte PI 头

// 读: async_read_some(fd) → raw IP bytes → Pipeline::Write to App
// 写: App → Pipeline::Read → async_write_some(fd) → 注入 IP 协议栈
```

```
  App (TCP/UDP over IP)
     ↓↑
  Kernel IP stack
     ↓↑
  /dev/net/tun (TUN interface created by great-hole)
     ↓↑  raw IP packets
  Pipeline(Tun → XOR → UdpChannel → Udp → Network)
     ↓↑
  Remote Peer
```

### TUN + Pipeline 完整路径

```
  Local App 生成 IP 包
     ↓
  Linux IP 栈路由到 tun0
     ↓
  EndpointTun::Read() 读取原始 IP 包
     ↓
  Pipeline: Read(Tun) → FilterXor → Write(UdpChannel)
     ↓
  UdpChannel → Udp WriteTo → async_send_to
     ↓
  === UDP Datagram (raw payload, 0 header) ===
     ↓
  远端 Udp::ReadLoop → UdpChannel::Read
     ↓
  Pipeline: Read(UdpChannel) → FilterXor → Write(Tun)
     ↓
  EndpointTun::Write() → 注入远端 tun0
     ↓
  远端 Linux IP 栈路由到 App
```

## 6. 对 FEC 设计的影响

### 6.1 Reserved Front 使用情况

`Packet` 内部 reserved front = 2 bytes (`kReservedFront`)，位于 payload 之前。

**发包时**，各输出 endpoint 通过 `PushFront(n)` 将头部字节写入 reserved 区，然后 `async_send_to` 发送 `Data()`（包含 reserved 区的所有已填充字节 + payload）。

**收包时**，`async_receive_from` 填充 `_Offset=2` 开始的空间。输入 endpoint 的 `ReadLoop` 通过 `PopFront(n)` 剥离并处理字节，将 payload 传给 Pipeline。

```
初始:    |← reserved 2B →|←───────── payload ────────→|
                       _Offset=2

P2P:     |← reserved 2B →|←───────── payload ────────→|
                       _Offset=2                        0 byte used

Mux:     |← chan_id 1B →|←───────── payload ────────→|
                    _Offset=1                           1 byte used, 1B 空闲

DynMux:  |←── RxId 2B ──→|←──────── payload ──────→|
        _Offset=0                                      2 bytes used, 0B 空闲
```

| 模式 | reserved 总量 | 已用 | 剩余 | PushFront 顺序 |
|------|:---:|:---:|:---:|------|
| **P2P** | 2B | 0B | **2B** | 不 PushFront |
| **Mux** | 2B | 1B | **1B** | `PushFront(chan_id)` |
| **DynMux** | 2B | 2B | **0B** | `PushFront(RxId)` |

**剩余空间可继续 PushFront**：Mux 还剩 1B，P2P 还剩 2B。新的 Packet 头可通过 Pipeline 层在前端追加，再由 endpoint PushFront 覆盖最前面。PushFront 是**后进先出**——最后 Push 的字节在 wire 的最前端：

```
p.PushFront(fec_header);    // 先加 FEC 头 (7~11B)  → |FEC|payload|
p.PushFront(chan_id_1B);   // 再加 chan_id          → |chan|FEC|payload| ← wire order
```

### Wire 格式（完整）

```
P2P:
  |←────────── payload ────────────→|

P2P + FEC data (小 group, flags=0x00):
  [group_seq:3B][flags:1B][fb:1B][idx:1B][cnt:1B][IV:1~8B][XORed shard data]

P2P + FEC data (大 group, flags=0x01):
  [group_seq:3B][flags:1B][fb:1B][idx:2B][cnt:2B][IV:1~8B][XORed shard data]

P2P + PING (flags bit4=1):
  [group_seq:3B][flags:1B][fb:1B][timestamp:8B LE]

P2P + FEEDBACK_ONLY (flags bit5=1):
  [group_seq:3B][flags:1B][fb:1B]

P2P + REPEAT (flags bit6=1):
  [group_seq:3B][flags:1B][fb:1B][IV:1~8B][XORed raw packet]
  group_seq 正常递增, 无 shard_index/source_count

Mux + FEC data (小 group):
  [chan:1B][group_seq:3B][flags:1B][fb:1B][idx:1B][cnt:1B][IV:1~8B][XORed data]

DynMux + FEC data (小 group):
  [RxId:2B][group_seq:3B][flags:1B][fb:1B][idx:1B][cnt:1B][IV:1~8B][XORed data]
```

### 6.2 FEC Header (仅 FEC 启用时存在)

FEC 参数由 Lua 配置，双端对称（不协商）。

Packet 构造时 **预留 20B front space** (`Packet(shard_size, 20)`)，确保 PushFront 所有头不越界。
20B 覆盖：端点最大 2B (DynMux) + FEC header 最大 9B + IV 最大 8B + 1B 余量。

编码时 Pipeline 通过 PushFront 写入 FEC header，端点在上层再 PushFront 自己的 chan_id/RxId。
所有 PushFront 均为 little-endian 多字节 + 1字节字段。

#### Flags byte

```
bit 0:      字段宽度 (仅 FEC data)
                0 = shard_index / source_count 各 1B
                1 = shard_index / source_count 各 2B
bit 1-3:    IV 长度 (仅 FEC data / REPEAT)
                000=1B, 001=2B, ..., 111=8B
bit 4:      PING — 携带 8B LE timestamp, 用于 RTT 测量
bit 5:      FEEDBACK_ONLY — 纯反馈, 无 data/IV
bit 6:      REPEAT — 原始包重复, 无 RaptorQ, 无 blob
bit 7:      reserved
```

#### Packet 类型

| flags | 类型 | 内容 |
|:---:|------|------|
| bit4=0, bit5=0, bit6=0 | FEC data | 完整 FEC header + blob + RaptorQ |
| bit4=1 | PING | group_seq + flags + fb + 8B timestamp |
| bit5=1 | FEEDBACK_ONLY | group_seq + flags + fb |
| bit6=1 | REPEAT | group_seq + flags + fb + IV + raw packet (无 idx/cnt) |
| 多 bit 置位 | 非法 | 预留 |

#### 各字段

| 偏移 | 字段 | 大小 | 说明 |
|------|------|:---:|------|
| 0 | `group_seq` | 3B | 组序列号 LE |
| 3 | `flags` | 1B | 见上 |
| 4 | `feedback` | 1B | loss_rate × 250 (0~250, 精度 0.4%) |
| 5 | `shard_index` | 1B/2B | ESI (仅 FEC data), LE |
| */6 | `source_count` | 1B/2B | K (仅 FEC data), LE |
| — | `timestamp` | 8B | 发送时间戳 μs LE (仅 PING) |
| — | IV | 1~8B | 随机混淆密钥 (仅 FEC data) |
| — | data | N B | IV XOR 后的 shard data (仅 FEC data) |

#### 开销 (FEC data shard)

| mode | 小 group | 大 group |
|------|:---:|:---:|
| P2P | **7+iv** | **9+iv** |
| Mux | **8+iv** | **10+iv** |
| DynMux | **9+iv** | **11+iv** |

对比 UDPspeeder 固定 8B header（所有包、无 feedback）。

#### 字段宽度选择

| 条件 | flag bit0 | 说明 |
|------|:---:|------|
| K + ceil(K × overhead) ≤ 255 | 0 (小 group) | 覆盖大多数场景 |
| K + ceil(K × overhead) > 255 | 1 (大 group) | 极高吞吐或极端 overhead |

所有多字节字段使用 **little-endian**。`Packet` 需新增 `PushFrontLE()` 系列方法。

### 6.3 反馈闭环

每个 FEC 包的 `feedback` 字段携带收方观测的丢包率：
- `ping_interval_ms` (default 1000): PING 包发送间隔
- `feedback_timeout_ms` (default 2000): 无数据时反馈最大间隔
- `decode_timeout_ms` (default 200): 初始解码超时 (RTT 校准后覆盖)

发方收到 feedback 后 EWMA (α=0.3) 平滑，驱动 overhead 调整。

### 6.4 混淆 IV

IV 已耦合进 FEC header（见 §6.2 Flags byte bits 1-3）。
长度 1~8 字节。**全部 PushFront**，不单独追踪。

**处理流程**（FEC data，小 group）：

```
Encode:
  1. memcpy(p.Data(), shard_data, shard_size)
  2. 生成 iv_len 字节随机 IV
  3. XOR p.Data() with IV
  4. p.PushFrontLE(IV, iv_len)          → [IV][XOR data]
  5. p.PushFrontLE(source_count, 1)     → [cnt][IV][XOR data]
  6. p.PushFrontLE(shard_index, 1)      → [idx][cnt][IV][XOR data]
  7. p.PushFrontLE(feedback_byte)       → [fb][idx][cnt][IV][XOR data]
  8. p.PushFrontLE(flags)               → [flags][fb][idx][cnt][IV][XOR data]
  9. p.PushFrontLE(group_seq, 3)        → [group_seq][flags][fb][idx][cnt][IV][XOR data]
     → 端点 PushFront chan/RxId → send

Decode:
  1. PopFrontLE(3) → group_seq
  2. PopFrontLE(1) → flags
  3. PopFrontLE(1) → feedback
  4. PopFrontLE(1) → shard_index
  5. PopFrontLE(1) → source_count
  6. iv_len = ((flags >> 1) & 0x7) + 1
  7. PopFront(iv_len) → IV
  8. XOR remaining data with IV → 还原 shard_data
```

### 6.5 PING / RTT 测量

发送方定期发送 PING 包，`timestamp` 字段为发送时的 `now_us`。
接收方收到后，将 `timestamp_send` echo 回发——在下一个向该端发送的包（FEC data / FEEDBACK_ONLY / PING）中
写入 `echo_timestamp`。

发送方收到 echo 后：
```
rtt_sample = now_us - echo_timestamp
rtt_ewma = α × rtt_sample + (1-α) × rtt_ewma  (α=0.3)
```

初始 RTT = `encode_timeout_ms × 10`。decode_timeout 由 RTT EWMA 动态校准。
不需要独立的 PING_ACK 包类型，复用现有包的 `timestamp` 字段。

### 6.6 Filter 接口约束

`Filter::Pipe(Packet&, Cancel&)` 是 1-to-1 同步变换。以下功能可以做成 Filter：

- **混淆 (obfuscation)**: 1-to-1, 加随机 padding → ✅ Filter
- **去混淆 (deobfuscation)**: 1-to-1, 移除 padding → ✅ Filter

以下不能做成 Filter（需独立 fiber）：

- **FEC 编码**: N-to-M (batch 积累 + 编码 + 多输出) → ❌ 需要自己的 fiber
- **FEC 解码**: M-to-N (收集 + 解码 + 多输出) → ❌ 需要自己的 fiber

设计方案中 FEC 功能直接集成在 Pipeline fiber 内，batch 模式由 fec config 触发。
