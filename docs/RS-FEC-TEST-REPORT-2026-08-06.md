# RS FEC 开发与测试报告 (2026-08-06)

**状态**: RS (Vandermonde GF256) codec 完成架构重构并验证;**P2P 直连与 dyn_mux 双模式全部跑通**。
**分支**: `fec-writebatch` (HEAD = 17133bf)

---

## 一、最终测试矩阵(实测数据)

链路: tokyo (202.144.195.103) ↔ ali (39.108.136.48), 共享 100M GGC 线路(含生产流量), RTT ~57ms。
配置: `fec_codec=rs`, `timeout_ms=20`(repair 攒批), `overhead=0.01`(静态, algo=0), `symbol_size=1440`, `max_batch=20`, 4MB socket 缓冲。

### P2P 直连模式(端口 20086 直连 UDP)

| 测试 | 结果 | 丢包 | 抖动 | 备注 |
|---|---|---|---|---|
| UDP 20M × 30s | 20.0 Mbit/s | 0/54826 (0%) | 0.051 ms | `iperf Done.` |
| UDP 50M × 30s | 49.9 Mbit/s | 0/137063 (0%) | 0.099 ms | `iperf Done.` |
| UDP 80M × 30s | 79.8 Mbit/s | 0/219299 (0%) | 0.068 ms | `iperf Done.` |
| TCP 10s | 22.8 / 19.6 Mbit/s | 121 retr | - | `iperf Done.` |
| TCP 30s | 17.1 / 16.5 Mbit/s | 1952 retr | - | `iperf Done.` |

### dyn_mux 模式(PSK 协商,端口 20086)

| 测试 | 结果 | 丢包 | 抖动 | 备注 |
|---|---|---|---|---|
| 协商收敛 | 对称握手: tokyo 主动 initiate → ali 回复 → rx matched → running | - | - | tokyo 侧需配置 peer 地址 |
| ping | 59ms, 3/3 (0%) | - | - | 隧道 UP |
| UDP 50M × 30s | 49.9 Mbit/s | 0/137062 (0%) | 0.102 ms | `iperf Done.` |
| UDP 80M × 30s | 79.8 Mbit/s | 0/219302 (0%) | 0.135 ms | `iperf Done.` |
| TCP 10s | 21.9 / 19.6 Mbit/s | 416 retr | - | `iperf Done.` |
| TCP 30s | 31.3 / 30.6 Mbit/s | 1193 retr | - | `iperf Done.` |

### 基线对比

| 链路 | TCP | UDP | 来源 |
|---|---|---|---|
| 直连(无隧道) | 36-92 Mbit/s | ~95 Mbit/s | 历史实测 |
| lcrq (tokyo 1vCPU) | 16 Mbit/s | 27 Mbit/s | 编码器 CPU 瓶颈 |
| **RS (本轮)** | **17-31 Mbit/s** | **80 Mbit/s 零丢包** | 本报告 |
| RS 标量基准 | - | 280 Mbit/s (K=17) | 单机 |

## 二、问题与修复记录(按时间)

1. **UDP socket 默认缓冲过小 (208KB)**: 85Mbps 突发流下 tokyo 侧 53.9k 次 send-buffer errors、ali 侧 401k 次 receive-buffer drops,链路冻结。
   → 修复: `EndpointUdp`/`EndpointUdpMux` 设置 4MB send/recv 缓冲(与 UdpDynMux 一致),up.sh 提升 `wmem_max/rmem_max` 到 16MB。
2. **per-packet 调试日志风暴**: RS-ENC/RS-DEC 每包一条 BOOST_LOG,7,300 pps 时 journald 饱和,1-2 vCPU 机器 CPU 被日志吃光(ali 曾因此宕机)。
   → 修复: 移除两条 per-packet 日志,保留低频率 batch/recover/watermark 日志。
3. **RS 解码热路径 std::map 劣化**: 每包 O(log n) 插入/删除 + 每包 2 次堆分配(map 节点 + payload vector),加上每包 O(n) 的 batch 清理循环;解码吞吐在 ~2,000 pps 后崩溃。
   → 修复: 定长环形槽数组(`seq & (N-1)` 直接索引 + 槽内 seq 校验),payload vector 容量跨包复用 → **稳态零分配**。修复后 UDP 50M/80M 零丢包。
4. **TCP 冻结(根因,架构级)**: 每轮 TCP 测试在 3-6s 处编码端停止读 TUN,客户端 cwnd 塌缩至 1,控制交换丢失。UDP(连续 7,000 pps)永不触发,TCP(稀疏 SYN/ACK/JSON)每次触发。
   → 根因: **RS 单 fiber 循环结构** (read → co_await write(挂起) → drain → 下次注册)。协程在 UDP 写挂起期间,输入 fd 未注册到 reactor;EPOLLET 边沿落进该窗口即永久丢失(内核不排队未注册 fd 的边沿,EPOLLET 不重放)。lcrq 主线是双 fiber(reader fiber 常驻 pending read),从未有此问题;RS 重造了有缺陷的单 fiber 变体。
   → 修复(架构重构): **FecCodec 策略模式** — 传输层唯一循环(reader fiber 永远挂着一个 async read → `std::deque` → worker fiber → `codec->OnPacket` 同步处理 → 统一 WriteBatch);codec 纯同步(零 I/O、零挂起)。`FecPipeline` 不再包含任何编解码逻辑。
5. **lcrq 主线同类容器问题**(审计修复): `FindSlot` 按内容线性扫描整个 ring;shard 存储线性查找 + 无容量复用;传输队列 vector 头部 O(n) erase。
   → 修复: ring 槽直接索引(`group_seq & mask` + 槽内校验);shard 按 esi 直接索引 + 容量保留;队列改 `std::deque`。
6. **dyn_mux 协商死锁**: 两端 create_channel 均无 peer 地址 → 都处于被动等待对方 initiate → 死锁。
   → 修复: 配置层 — 主动侧(tokyo)显式传 peer 地址触发 initiate;被动侧回复 initiate(对称握手,de9c1d4/b12e965 已提交)。

## 三、架构(重构后)

```
FecPipeline (传输层,唯一实现)
├─ reader fiber: 常驻 pending async read → std::deque<Packet>
├─ worker fiber: 100us tick → codec->Tick(out) → WriteBatch
│                deque pop_front (O(1)) → codec->OnPacket(p, out) → WriteBatch
├─ PING/FEEDBACK: 传输层控制包
└─ FecCodec 策略 (纯同步, 零 I/O 零挂起)
   ├─ RsCodec   (Vandermonde GF256): 源分片即发 + 窗口 repair + 水位交付
   │   └─ 定长槽 ring: seq/bid & (N-1), payload 容量复用
   └─ LcrqCodec (RaptorQ): blob 攒批 + REPEAT 单包冗余
       └─ ring 槽直接索引 + shard esi 索引
```

## 四、性能观察与后续优化方向

- **UDP**: RS 编解码不再是瓶颈(80M 零丢包,接近共享链路容量上限)。上限 = 100M 链路 − 生产流量 − repair 开销(~1%)。
- **TCP (17-31M vs 直连 36-92M)**: 主要受线路固有丢包(~1.6%)影响 — 每 batch 只有 1 个 repair,同一 batch 丢 2 个分片时无法恢复 → 水位跳过 → TCP 重传。优化方向:
  1. `overhead` 提到 2-3%(增加 repair 覆盖,减少跳过)
  2. 缩短 `decode_timeout_ms`(更快跳过,减少 TCP 等待)
  3. 已验证: 控制交换与冻结问题已消除,重传行为正常
- **待办**: TCP 吞吐调优、24h+ 稳定性 soak、openspec Phase 7/9 任务状态更新。

## 五、关键文件

- `src/core/FecCodec.hpp/cpp` — 策略接口 + 工厂 + wire flags
- `src/core/RsCodec.hpp/cpp` — RS codec(ring 槽、批次、水位、repair)
- `src/core/LcrqCodec.hpp/cpp` — lcrq codec(同步化 + 容器修复)
- `src/core/FecPipeline.hpp/cpp` — 传输层唯一循环
- `src/core/EndpointUdp.cpp` — 4MB socket 缓冲
- `configs/fec-test-{tokyo,ali}.lua` — P2P 直连测试配置
- `configs/fec-test-dynmux-{tokyo,ali}.lua` — dyn_mux 测试配置(tokyo 侧含 peer 地址)
- `openspec/changes/fec-implementation/specs/fec-pipeline/spec.md` — 架构 spec(已更新)
