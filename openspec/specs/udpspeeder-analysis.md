# UDPspeeder 实现研判

## 用户当前使用情况

UDPSpeeder 目前在 ali 和 osaka 之间作为 great-hole 的嵌套隧道使用，效果满意。
配置特点：低 timeout + fine-grained FEC 参数。

## 架构概述

UDPSpeeder 是双边 UDP 加速工具，使用 Reed-Solomon FEC + XOR 加密 + 随机填充混淆。

```
Application UDP
  → [conv] 附加连接ID
  → [crc32 + obscure + encrypt]  包级别混淆加密
  → [FEC encode]                RS 编码
  → [interval/jitter]           延迟分散
  → wire
```

加密和混淆在 FEC 之外：每个包（包括 FEC 冗余包）独立加密混淆，DPI 无法识别冗余数据。

## FEC 实现

### Reed-Solomon 库

使用 Luigi Rizzo 的 FEC 库 (`lib/fec.cpp`, 版权 1997-98)，**Vandermonde 矩阵 GF(256)**。

**代码位置**：
- `lib/fec.cpp` — GF(256) 查表运算, Vandermonde 矩阵构造
- `lib/fec.h` — `fec_new()`, `fec_encode()`, `fec_decode()`, `fec_free()`
- `lib/rs.cpp` — 薄封装 + 编码缓存 `void* table[256][256]`

**GF(256) 实现**：
```c
#define GF_BITS 8  // GF(2^8), 不可约多项式: 1+x^2+x^3+x^4+x^8
// gf_mul_table[256][256] — 预计算乘法表 (64KB)
// addmul1() — 循环展开 UNROLL=16
```
**无 SIMD 优化，纯 C 查表实现**。同一时期有 longhair 等 AVX2 优化的 RS 库。

### 参数限制

- 最大总数: 255 (GF(256)+1)
- 推荐: x+y < 50（CPU 原因）
- 格式: `-f x:y` 或 fine-grained `-f 1:3,2:4,10:6,20:10`

### 两种 FEC 模式

| 模式 | 行为 | 延迟 | 带宽 | MTU |
|------|------|:---:|:---:|:---:|
| Mode 0 (blob) | 多包拼接成 blob → 分片 → FEC | 略高 | **省** | 无问题 |
| Mode 1 (packet) | 逐包定长分片 → FEC | 略低 | 高 | 需关注 |

**用户使用 Mode 0**。

### FEC 包头格式

每个 FEC 编码后的包都有一个 **8 字节** 固定头 (`fec_manager.cpp:319-348`)：

```
Offset  Size  Field          Description
─────────────────────────────────────────────
  0      4B    seq (u32 BE)  组序列号，单调递增
  4      1B    mode          0=blob, 1=per-packet
  5      1B    data_num      数据分片数 K
  6      1B    redundant_num 冗余分片数 M
  7      1B    index         此分片在组中的序号 (0..K+M-1)
─────────────────────────────────────────────
Total: 8 bytes
```

**Mode 0 的 payload 格式**（blob 模式）：
```
[4B packet_count (u32)] [for each: 2B length (u16) | data bytes]
```
多个原始包拼接为 blob → 按 `data_num` 分成等长 shard。解码时从 blob 拆回各原始包。

**Mode 1 的 payload 格式**：（每个 shard 携带一个原始包）
```
[2B payload_length (u16)] [data bytes]
```
每个原始包先加 2B 长度前缀，再按 FEC 分组编码。

**与我们的设计对比**：

| | UDPspeeder | 我们的方案 |
|---|:---:|:---:|
| header 大小 | 8B (固定) | P2P: 5B, Mux: 5B, DynMux: 6B (仅 FEC 包) |
| FEC 标志 | 无（靠 header 内容判断） | 1 bit flag，无 FEC 时零开销 |
| 组管理 | 4B seq + data_num + redundant_num | group_seq(2B) + total(1B) + index(1B) |
| Mode 0 blob | packet_count(4B) + per-packet length(2B) | 同样逻辑，复用 blob\_concat/split |

## Fine-Grained FEC（用户的关键配置）

允许对不同的数据包数量使用不同的冗余比例：

```
-f 1:3,2:4,10:6,20:10
```

含义：
- 1 个数据包时：发 3 个冗余（4倍，强保护）
- 2 个数据包时：发 4 个冗余（3倍）
- 10 个数据包时：发 6 个冗余（1.6倍）
- 20 个数据包时：发 10 个冗余（1.5倍）

中间值线性插值（向上取整）。发送时自动选择带宽效率最优的 x:y 组合。

## Timeout 机制

```c
// fec_manager.cpp line 174-183
if (counter == 0) {
    first_packet_time = get_current_time_us();
    ev_timer_set(&timer, fec_par.timeout / 1000000.0, 0);
    ev_timer_start(loop, &timer);
}
```

收到第一个包时启动 one-shot 定时器。超时后立即 FEC 编码。FEC 也可能提前触发（queue 满或接近 MTU）。

| 场景 | timeout | 引入延迟 |
|------|:---:|:---:|
| 非游戏 | 8ms | ≤16ms |
| 游戏 | 1ms | ≤2ms |
| 零延迟 | 0 | 0ms |

## 推荐配置总结

| 场景 | FEC 参数 | timeout | 流量倍数 | 延迟 |
|------|:---:|:---:|:---:|:---:|
| 日常 | `-f20:10` | 8ms | 1.5x | ≤16ms |
| 折衷 | `-f10:6` | 3ms | 1.6x | ≤6ms |
| 游戏 | `-f2:4` | 1ms | 3x | ≤2ms |
| 游戏(零延迟) | `-f2:4` | 0ms | >3x | 0ms |

## 加密与混淆

### XOR 加密

```
packet.cpp:33-40:
encrypt_0(char *input, int &len, char *key) {
    for (i=0,j=0; i<len; i++,j++) {
        if (key[j]==0) j=0;
        input[i] ^= key[j];
    }
}
```
简单循环 XOR，自逆（加密=解密）。用于改变协议特征，防运营商识别。

### 随机填充混淆

```
packet.cpp:78-107:
1. 生成 4~32 字节随机 IV
2. 用 IV XOR 原始数据
3. 在数据末尾追加 IV + IV 长度字节
```

使包内容看起来随机，隐藏 FEC 冗余特征。

### 层次关系

```
加密/混淆 → 逐包执行 → 先于 FEC
FEC → 组包执行 → 后于加密混淆
```

每个 FEC 编码后的包（含数据包和冗余包）都独立携带自己的 XOR+混淆。DPI 无法从包内容判断哪些是原始数据包、哪些是 FEC 冗余包。

## 与 RaptorQ 方案对比

| 维度 | UDPspeeder (RS GF256) | 拟定方案 (RaptorQ/lcrq) |
|------|----------------------|------------------------|
| **FEC 算法** | Vandermonde RS, GF(256) | RaptorQ, RFC 6330 |
| **码率** | 固定比例 | **Rateless** (无速率) |
| **SIMD** | **无**，纯查表 | **SSE2/SSSE3/AVX2/AVX-512** |
| **最大 shard 数** | 255 | **56403** |
| **编码复杂度** | O(k·m) | **O(k)** |
| **解码复杂度** | O(k·m) | **O(k)** |
| **动态调整** | FIFO 命令，粗粒度 | **每 block 独立，原生支持** |
| **加密** | 简单 XOR | 无（需外挂） |
| **混淆** | 内置随机填充 | 无（需外挂） |
| **CPU 开销** | 中等~高（纯 C 查表） | **低**（SIMD 加速） |
| **吞吐量 (osaka)** | 未测，预计 ~50-100 Mbps | **560 Mbps** |
| **吞吐量 (ali)** | 未测，预计 ~100-200 Mbps | **1770 Mbps** |

## 方案建议

### 从 UDPspeeder 继承

1. **fine-grained FEC 参数语法** `-f 1:3,2:4,10:6,20:10` — 用 RaptorQ rateless 特性做得更灵活
2. **timeout 机制** — 控制 FEC 引入的最大延迟
3. **随机填充混淆** → `PacketObfuscator` Filter
4. **mode 0 (blob)** — 省带宽，无 MTU 问题

### 不需要移植的

- **XOR 加密** — great-hole 自身已有加密能力

### FEC 替换 + 增强

将 UDPspeeder 的 RS 编码替换为 RaptorQ (lcrq)，并增加：

1. 保留 fine-grained 参数语法作为初始配置
2. 保留 timeout 机制（控制最大延迟）
3. **双向丢包反馈** — 收方告知发方实际丢包率
4. **动态 overhead 调整** — EWMA 平滑自适应
5. 底层编码从 `fec_encode()` 替换为 `rq_encode()`

### 优势

- **吞吐量提升 5~20x**（SIMD 加速 + O(k) 复杂度）
- **支持更大 block**（56403 vs 255 符号）
- **CPU 占用更低**（同样吞吐量下）
- **自适应 overhead**（无需手动调参）
- **精确补偿**（缺几个符号补几个，不浪费带宽）
