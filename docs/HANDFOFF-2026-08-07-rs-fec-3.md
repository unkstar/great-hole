# Handoff: RS FEC 第三阶段 (2026-08-07)

**状态**: 三个根因已修复并提交 (`f082528`);UDP 双方向 0% 丢包;TCP 仍 ~10-12M dl / 0.2-2M ul,残余 stall 待查。
**分支**: `fec-writebatch` (HEAD = f082528)

---

## 一、本次已提交的修复 (`f082528`)

| # | 修复 | 文件 | 验证 |
|---|---|---|---|
| 1 | **repair 24 位 bid 字节序**(bbf1455 引入的回归):编码端先推低位 16bit 再推高位 8bit,PushFrontLE 前插导致线上 `[hi lo0 lo1]`,解码端重建 `hi + (lo0<<8) + (lo1<<16)` —— 每个 repair 的 bid 都被打乱 → 恢复永不触发("RS recovered 0 次" 根因,逐字节模拟确认)。改为先推高位 → 线上标准 24 位 LE,解码端不变 | `RsCodec.cpp` | 恢复开始触发(单次 recovered 4 shards) |
| 2 | **dynmux initiate 乒乓风暴**:HandleControlPacket 无条件回复 initiate,即使已完全匹配 —— 互相触发无限互回(每秒 ~34 个,持续数小时)。kNegotiating 期间数据包被丢弃 → 周期性 burst 丢失。改为仅在未完全匹配时回复 | `EndpointUdpDynMux.cpp` | 风暴消失(34/s → 0) |
| 3 | **repair 冗余 + 恢复重试**:每个 repair 发 2 份(解码端幂等,同 idx 覆盖);repair 槽位掩码追踪实际收到的 idx(未到的中间 repair 不再以零填充符号参与解码);shard 到达时重触发 RsTryRecover(消除 "wait for more repairs" 死锁) | `RsCodec.cpp/.hpp` | repair 100% 到达(676 线上 = 677 解码) |
| 4 | **FecPipeline 读取器 drain 到 EAGAIN**:原 q-size 64 上限破坏 EPOLLET 排空不变量,fd 挂起数据不再触发边缘,读取器挂起 → tun 队列溢出丢包(两端测试期间均观察到) | `FecPipeline.cpp` | — |

## 二、验证结果(最终干净构建)

- **UDP 50M 双方向 × 15-30s: 0% 丢包**(真 0,68530/68530)
- **repair 到达 100%**、恢复触发(每次测试 recovered 2-5 次,修复前 0)
- **TCP dl 10-12M(271-399 重传)、TCP ul 0.2-2M** —— 仍差

## 三、剩余问题(TCP 停滞,下一会话主线)

**现象**: 解码端 watermark stall 仍持续(每 200ms 1 次,skipped 1-3),缺口 shard **之后到达**(诊断确认:stall 时缺口 seq 槽位 `valid=true` —— 数据没丢,是延迟 > decode_timeout(200ms) 被 guard 抢先跳过)。

**缺口模式**: 每 8-9 个 seq 缺 1 个(~12%),延迟 1-3 秒。

**主导假说(未验证)**: 编码端发送端 burst 背压 —— TCP 重传风暴的瞬时 burst 打满发送缓冲(4MB 设置可能被内核 clamp 到 ~200KB),WriteBatch 的 awaited send 阻塞 → 后续 shard 延迟发出 → 解码端 guard 抢先跳过。UDP 稳态无 burst 所以 0% 丢。

**建议排查**:
1. 验证内核实际 send buffer:`ss -mup` / `sysctl net.core.wmem_max`(4MB 设置可能被 clamp)
2. 编码端加发送延迟统计(WriteBatch await 时长直方图)
3. 试把 `timeout_ms` 从 20 提到 50(批次更大,repair 更集中,减少 repair 流量)
4. 试 `algo=0`(静态 overhead)排除 PI 自适应因素

**其他已知项**(来自上一 handoff,未动):
- 反向 80M 丢包(修复后未重测 —— UDP 50M 已 0%,80M 待验证)
- 24h soak 未做
- openspec Phase 7/9 状态更新未做

## 四、测试纪律教训(本次新增)

1. **日志本身会拖垮系统**: 每包 debug 日志 × 单线程 fiber = journald pipe 阻塞 → 解码积压 → stall 雪崩。诊断日志必须节流(≥2s 一条)或彻底移除
2. **journald 会吞日志**: rate limit(1000 行/30s)+ 长行显示为 `[xxB blob data]`。`journalctl -o cat` 看完整内容
3. **tcpdump 解析字段偏移**: dynmux 包有 2B RxId 前缀;repair 字段偏移 [6]=fb [7..9]=bid [10]=k [11]=idx
4. **scp 保留源 mtime**: 部署后 make 可能不重编(源 mtime ≤ obj mtime)。部署前 `touch` 源文件
5. **部署链命令要分步验证**: `cmd1 && cmd2 && ...` 链中 make 管道(tail)吞掉错误,后续 cp/restart 可能没执行。验证:pid 变化 + 二进制 mtime
6. **测试前必须预热**: 服务重启后 PI 积分清零,oh 从 0.01 爬升(m=1 脆弱期 ~60 批 ≈ 1-2 秒),冷启动测试结果无意义

## 五、服务器状态

| 机器 | 服务 | 配置 | 说明 |
|---|---|---|---|
| tokyo | `great-hole-fec-test-tokyo` | dynmux + algo=3, decode_timeout_ms=200(已恢复默认) | 运行 f082528 最终构建 |
| ali | `great-hole-fec-test-ali` | 同上 | 同上 |

- 部署流程不变: tokyo obj2 构建 → 传 ali → 两端重启
- 注意: 部署后先 `touch` 源文件再 make(见教训 4)
