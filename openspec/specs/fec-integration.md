# FEC Integration — RaptorQ via lcrq

## Summary

为本工程增加 RaptorQ (RFC 6330) 前向纠错 + 随机填充混淆。
代替当前使用的 UDPspeeder 嵌套隧道，直接集成到 great-hole 的 Pipeline 中。

## What We Keep from UDPspeeder

| 功能 | 决策 | 原因 |
|------|:---:|------|
| XOR 加密 | **舍弃** | great-hole 自身已有加密能力 |
| fine-grained FEC | **继承 + 增强** | 用 RaptorQ rateless 特性做得更灵活 |
| timeout 机制 | **继承** | 控制 FEC 引入的最大延迟 |
| 随机填充混淆 | **继承** | 隐藏 FEC 冗余，防 DPI 识别 |
| mode 0 (blob) | **继承** | 省带宽，无 MTU 问题 |

## What We Add

| 功能 | 说明 |
|------|------|
| **RaptorQ 编码** | 代替 RS GF(256)，O(k) 复杂度，SIMD 加速 |
| **双向丢包反馈** | 收方通过带内消息告知发方当前丢包率 |
| **动态 overhead 调整** | 根据实时丢包率 + EWMA 平滑自适应冗余度 |
| **更大 block** | 从 RS 的 255 限制 → 56403 符号 |

## Design

### Fine-Grained FEC with RaptorQ

保留 UDPspeeder 的 `x:y` 参数语法作为**初始配置**，但运行时自动根据丢包率调整。

```
配置: -f 1:3,2:4,10:6,20:10
         ↑ 小 batch 高冗余    ↑ 大 batch 低冗余

运行时:
  丢包 2%  → overhead 向 10% 收敛
  丢包 10% → overhead 向 20% 收敛
  丢包 20% → overhead 向 35% 收敛（含安全余量）
```

由于 RaptorQ 是 rateless，不需要预定义冗余比例。可以：

1. 先发送 K 个源符号
2. 根据当前丢包率估算需要多少额外符号
3. 接收方缺几个就发信号补几个（精确补偿，不浪费）

### Timeout 机制

```cpp
// 与 UDPspeeder 相同逻辑，但 block 符号数由丢包率动态决定
struct FecGroup {
    std::vector<Packet> pending;     // 积攒的数据包
    Timer timeout;                    // 单次定时器
    float current_overhead;          // 当前冗余率

    // 触发条件（任一满足即编码）:
    // 1. timeout 到期
    // 2. pending.size() >= max_batch
    // 3. 预估编码后总大小 >= MTU
};
```

### 随机填充混淆

从 UDPspeeder 移植，作为独立的 `PacketObfuscator` Filter：

```
发送路径:  original_payload
  → 生成 4~32 字节随机 IV
  → IV XOR original_payload
  → 追加 IV + iv_len_byte
  → 输出混淆后的包

接收路径:  obfuscated_payload
  → 读最后一个字节获取 iv_len
  → 提取 IV
  → IV XOR payload → 还原
```

### 双向丢包反馈

#### 收方 → 发方反馈通道

利用上行链路定期发送 `LossReport` 控制消息（嵌入到下一个上行 FEC block 中）：

```cpp
struct LossReport {
    uint32_t seq_base;           // 统计起始序号
    float loss_rate;             // 观测丢包率
    uint32_t symbols_received;   // 已收符号数
    uint32_t symbols_expected;   // 应收符号数
    uint32_t timestamp_ms;       // 发送时间戳
};
```

- 每收到一个完整 block 或每 100ms 发送一次
- 发方的 `LossRateEstimator` 用 EWMA 平滑

#### 发方自适应

```cpp
class AdaptiveOverhead {
    float ewma_loss = 0.0;
    const float alpha = 0.3;      // EWMA 平滑系数
    const float safety = 0.05;    // 安全余量 5%
    const float max_overhead = 0.50; // 上限 50%

public:
    void feed(float measured_loss) {
        ewma_loss = alpha * measured_loss + (1 - alpha) * ewma_loss;
    }

    float overhead() {
        return std::min(ewma_loss + safety, max_overhead);
    }

    uint32_t encoded_count(uint32_t K) {
        return K * (1.0 + overhead());
    }
};
```

### Pipeline 集成位置

```
Send path:
  Source → ... → FecEncodeFilter → PacketObfuscator → UdpSend → Network

Recv path:
  Network → UdpRecv → PacketDeobfuscator → FecDecodeFilter → ... → Sink
```

`FecDecodeFilter` 同时提取 `LossReport` 反馈给对端的 `FecEncodeFilter`。

## Implementation Plan

### Phase 1: lcrq 集成
1. 添加 submodule `libs/lcrq`
2. CMake 构建集成
3. C++ 封装 `FecEncoder` / `FecDecoder`
4. 单元测试（编解码 round-trip + 丢包仿真）

### Phase 2: 核心 FEC Filter
1. `FecEncodeFilter` — timeout + 编码
2. `FecDecodeFilter` — 收集 + 解码
3. 集成到 Pipeline
4. 集成测试（ali-osaka 实际链路）

### Phase 3: 混淆 + 自适应
1. `PacketObfuscator` / `PacketDeobfuscator` Filter
2. `LossReport` 消息格式
3. `AdaptiveOverhead` 控制器
4. 双向丢包反馈闭环
5. 端到端测试

## Non-Goals

- 不实现 UDPspeeder 的 interval/jitter 延迟分散（当前已满足需求）
- 不实现 XOR 加密（great-hole 已有）
- 不实现 FIFO 动态调参（自适应取代手动调参）
