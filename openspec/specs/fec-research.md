# FEC 技术选型调研

## 背景

本项目需要为 UDP 流增加前向纠错 (FEC) 能力，以对抗 10%~20% 丢包率的链路，同时保证低延迟、尽可能少重传。

## 候选方案对比

### Reed-Solomon

| 维度 | 评估 |
|------|------|
| 复杂度 | 编解码 O(k·m) GF(256) 运算 |
| 最大块数 | 受 GF(256) 限制，通常 ≤255 |
| 码率灵活性 | 固定，不可动态调整 |
| 软件吞吐 | 50~500 Mbps (优化后) |
| 优势 | 实现简单，工业界成熟 |
| 适用场景 | 小数据块 (k≤256) |

### LDPC (quantumgizmos/ldpc)

| 维度 | 评估 |
|------|------|
| 复杂度 | BP 迭代解码 O(n)，OSD 有 O(n³) 高斯消元 |
| 吞吐量 (实测) | ali: 0.5~6.7 Mbps, osaka: 0.1~1.8 Mbps |
| SIMD | 无，纯标量 C++ 实现 |
| CPU 占用 | 100%（单核） |
| 结论 | 实现无 SIMD/多线程优化，吞吐远不能满足需求 |

### RaptorQ (lcrq) ★ 选定

| 维度 | 评估 |
|------|------|
| 标准 | RFC 6330 |
| 码率 | Rateless（无速率），可动态调整 |
| 复杂度 | 编解码均 O(k)，线性复杂度 |
| SIMD | **SSSE3/SSE2/AVX2/AVX-512** 全支持 |
| 吞吐量 (实测) | ali: 1.4~1.8 Gbps, osaka: 450~563 Mbps |
| 外部依赖 | 零（纯 C，无第三方依赖） |
| 编译 | 30 秒，configure + make |
| 许可证 | GPL-2.0 / GPL-3.0 |

## 实测数据

测试环境：
- **ali**: 阿里云 ECS, Intel Xeon Platinum 2C, 406MB RAM, AVX-512
- **osaka**: VPS, Intel Xeon Cascadelake 2C, 1GB RAM, AVX

### RaptorQ (lcrq) 吞吐量

```
参数: K=32K symbols, T=1024 bytes, 1000 iterations

         | 编码       | 解码       | SIMD 级别
---------|------------|------------|----------
ali      | 1769 Mbps  | 2060 Mbps  | AVX-512
osaka    | 563 Mbps   | 559 Mbps   | AVX
```

### 有效吞吐量（含丢包冗余）

丢包时需要多发额外符号覆盖丢包。每符号编解码开销恒定：

| 丢包率 | 编码冗余 | osaka 有效 | ali 有效 |
|--------|:---:|:---:|:---:|
| 0%   | 0%   | 563 Mbps | 1769 Mbps |
| 10%  | +11% | 507 Mbps | 1592 Mbps |
| 20%  | +25% | 450 Mbps | 1415 Mbps |

解码始终为接收 K 个符号的计算量，不受丢包率影响。

## 自适应策略

### 双维自适应

```
丢包率高  → overhead ↑  (多编 FEC)
丢包率低  → overhead ↓  (少编 FEC)
延迟敏感  → block ↓     (小 block，快到达)
带宽优先  → block ↑     (大 block，开销效率高)
```

| 网络状态 | block 大小 | overhead | 行为 |
|----------|:---:|:---:|------|
| 良好 | 256KB | 5% | 高效率 |
| 中等 | 128KB | 15% | 平衡 |
| 差 | 64KB | 30% | 小 block 快速恢复 |
| 极差 | 32KB | 40% | 保证送达 |

### RaptorQ 的 Rateless 特性

- 可从 K 个源符号生成**无限多**个编码符号
- 接收方只要收到**任意 K 个**即可解码
- 不需要提前协商码率，可在发送过程中动态调整
- 丢包补偿策略：发送 N=K×(1+overhead) 个符号，参数 overhead 根据反馈实时调整

## 集成方案 (lcrq)

- 仓库: https://git.sr.ht/~librecast/lcrq
- GitHub 镜像: https://github.com/Librecast/lcrq
- 加入方式: git submodule → `libs/lcrq`
- 构建: autotools (configure + make)，需集成到 CMake 构建
- API: C 接口，`rq_init()` / `rq_encode()` / `rq_decode()` / `rq_free()`
