# Handoff: loss_deadband — 零丢包 0% 补偿 (2026-08-13)

**状态**: 功能实现并验证;**最终决策 = 禁用**(loss_deadband = -1),保留每批 m=1 底补偿。测量环路修复保留。
**分支**: `fec-writebatch` (**HEAD = ca8bec9**),两端 fec-test 隧道运行此构建。

**决策理由** (2026-08-13 实测):
- TCP 为主场景,5% 底补偿零成本: 88M payload + 5% repair ≈ 99M wire < 100M 网口
- 底补偿使单分片丢包对 TCP 隐形;deadband=0 时丢包要等 ~1s 检测窗口,期间 TCP 吃 dup-ACK
- 5% 的真实代价只在饱和 UDP (95M 发送时超网口 ~5%),非常态
- 已证明 TCP 速率与补偿无关 (88.5M@5% vs 88.4M@0%,两个不同瓶颈恰好落在同一数字)

---

## 一、动机

实测发现 fec-test 的 5.0% 补偿率**不是丢包驱动的**,而是量化下限:
`oh = ewma_loss/(1-ewma_loss) + safety_margin(0.01)` → 零丢包时 oh=0.01 →
`m = ceil(k × oh)`,k≤100 时恒 ≥1 → 每批必发 1 个 repair,max_batch=20 → 1/20 = 5%。

## 二、改动

1. **`loss_deadband` 配置** (3e4631f): `-1`=禁用(默认,旧行为);`≥0` = 实测丢包 ≤ 该值时编码器不发 repair。RS + lcrq 双 codec 生效;两侧各自配置(本侧 encoder 管本侧发送方向,不对称链路可设不同值)。
2. **测量解耦修复 1 — 槽驱逐原始丢包统计** (3ebf7b5): 旧统计完全依赖 repair 分片到达来创建 batch 记账槽;deadband 下不发 repair → 永不统计 → fb 恒 0 → deadband 永不退出(latch)。改为源分片槽驱逐统计: 新分片认领环槽(4096)时,预期前住户 = seq-4096,未到则计 1 丢。整环周期宽限天然容忍乱序;统计的是**原始线路丢包**(而非 FEC 后残余,无反馈振荡)。
3. **测量解耦修复 2 — peer_loss_rate 字段拆分** (94f26de): `latest_loss_rate` 一个字段身兼二职(本地测量 + 对端 fb 回读,回读每包覆盖写),干净反向流会把丢包方向的测量清零。拆为 `latest_loss_rate`(本地测量,供 fb 外发)与 `peer_loss_rate`(对端测量,供本地编码器控制器与 deadband 门控)。

## 三、验证 (fec-test tokyo↔ali, 双侧独立配置 0.005)

| 场景 | repair/shard | 说明 |
|---|---|---|
| 干净线路 (旧二进制基线) | 4.79% | 与 handoff 5.0% 一致 |
| 干净线路 (deadband) | **0.0000%** | TCP 80.2M 不降反升 (vs 基线 44.2M, 受当时生产流量竞争) |
| 真实丢包突发出现 | 5-14%, ~1-2s 响应 | deadband 及时退出 |
| 突发结束 | 0% @ 全速 | 自动归零,环路闭合 |
| 3% Step 丢包 (LossPattern) | m=1 恢复, ali 日志 **116 次 recovered** | 测量→fb→退出→repair→恢复全链路 ✓ |

## 四、本次调查中的两个环境事故 (与代码无关)

1. **tokyo /tmp (tmpfs 963M) 被测试抓包占满 → iperf3 崩溃**: iperf3 3.18 的流缓冲 = mmap 的 /tmp 临时文件;tmpfs 满 → 文件无法增长 → read EFAULT → `readentropy` 失败 → `iperf_errexit(NULL)` 无脑解引用 `test->print_mutex` (NULL+0x10) → SIGSEGV。**教训: 抓包一律写 /home (磁盘),不写 /tmp; 每次 iperf3 需要 ~128KB tmpfs 空间**。上轮遗留 /tmp/rs.log(166M)+cap*.pcap(~480M) 未删(非本次产物,留待决定)。
2. **Step 测试时序**: Step 计时从服务启动起算(非测试开始),重启 ali 与开测之间隔过长会错过干净窗口。正确做法: 重启后立即开测。

## 五、遗留/已知限制

1. **检测延迟与速率相关**: 槽驱逐统计要等一个环周期 (4096 shards)。满载 ~0.5s;低速率(坍塌后 ~100 shard/s)时 ~40s。健康隧道丢包后 ~1-2s 内恢复,不构成问题;坍塌态检测慢是 EWMA 爬升慢(alpha=0.1, 窗口 50×max_batch)的老问题,可后续调优。
2. 3% Step 尾段 TCP 坍塌未恢复: 合成丢包 + 真实线路拥塞叠加,EWMA 慢爬 m=1 档保护不足。非 deadband 回归,是已知的 EWMA 反应特性(fec-spec.md 有记录)。
3. tokyo /tmp 需清理(见上),否则下次 iperf3 测试会再崩。
4. 反向 80M UDP、24h soak 仍未做(原遗留)。

## 六、关键文件

- `src/core/FecConfig.hpp` — loss_deadband 字段
- `src/core/FecCodec.hpp` — peer_loss_rate 字段
- `src/core/RsCodec.cpp` — deadband 门控 + 槽驱逐统计 + peer_loss_rate
- `src/core/LcrqCodec.cpp` — lcrq deadband 门控
- `configs/fec-test-dynmux-*.lua` — 部署实际使用配置, loss_deadband=0.005
