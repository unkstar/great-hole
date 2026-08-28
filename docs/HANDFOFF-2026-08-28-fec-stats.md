# Handoff: FEC 统计信息系统 — LLM-CSV 模式 (2026-08-28)

**状态**: Spec 设计阶段(待实现)
**背景**: fec-test 隧道 344 万 rx_dropped(35% 接收)+ 22:04 事件 50% 分片丢失(分钟级 speedtest 暴跌),现有日志无法归因(静默放弃无统计、write 失败无日志、百万级事件无法逐条记日志)

---

## 一、设计参考:UnrealEngine LLM 的 CSV 模式

参考 UE 的 **Low Level Memory Tracker(LLM)** 的持久化设计:

| UE LLM 特性 | 本项目映射 |
|---|---|
| 周期性全量快照 | 周期采样 FEC/传输/系统全维度快照 |
| 行式 CSV(行=时间点, 列=tag) | 行=时间点, 列=统计维度 |
| 可配置采样间隔 | lua 配置 `stats_interval_ms` |
| 低成本(采样期间累计, 周期输出) | 原子计数 + 周期汇总写 |
| 文件轮转/清理 | 按天轮转 + 保留天数 |
| tag 分层(EngineMisc/Physics/...) | 模块分层(FEC/传输/系统) |

**核心差异**: LLM 是内存分配追踪,本项目是隧道健康统计——但持久化模式(周期快照 CSV)相同。

## 二、设计目标

1. **归因能力**: 丢包发生时,能区分"内核丢(tun rx_dropped)"、"链路丢(隧道)"、"FEC 静默放弃"——三者对照
2. **零刷屏**: 百万级事件 = 内存计数,日志每分钟 ≤1 行
3. **可回溯**: CSV 时间序列保留历史,事件后可离线分析
4. **低成本**: 原子计数 + 10s 采样 + 60s 汇总,CPU/内存可忽略
5. **自包含**: 系统计数器(/sys//proc)由进程自行读取,无外部 cron 依赖

## 三、架构

```
统计源(埋点)                    采样器                    输出
┌─────────────┐              ┌──────────────┐        ┌──────────────┐
│ RsCodec     │──原子计数──→ │  FecStats    │──60s──→│ CSV 文件      │
│ Tun::Write  │              │  (内存聚合)   │──事件──→│ 事件日志      │
│ WriteBatch  │              │  环形缓冲 1h  │        │ journal      │
│ UdpDynMux   │              └──────────────┘        └──────────────┘
│ SystemSampler│──10s 采样──→( /sys /proc )
└─────────────┘
```

## 四、统计点清单(全模块)

### 4.1 FEC 解码侧
| 维度 | 埋点 |
|---|---|
| dec_src_pkts / dec_src_missing / dec_loss_rate | RsCodec::DecodePacket / UpdateLossRate |
| recover_attempts / recover_success(shards) / recover_failed | RsCodec::RsTryRecover |
| **recover_abandoned**(静默放弃,等待超时) | RsCodec::CleanupStaleBatches(新增统计) |
| reorder_early(repair 先到误判) | RsTryRecover missing 扫描(源槽随后被填) |
| decode_timeout_cleanups | CleanupStaleBatches |
| dup_pkts(小包去重) | DecodePacket kRsSmall 路径 |
| ctrl_pkts(ping/feedback 收发) | DecodePacket 控制分支 |

### 4.2 FEC 编码侧
| 维度 | 埋点 |
|---|---|
| enc_src_pkts / enc_small_pkts / enc_repair_pkts | EncodePacket / SendRsRepair |
| batch_size_dist / flush_timeout / flush_full | EncodePacket 批 flush 原因 |
| overhead_current / overhead_max_hits | AdaptiveOverhead::GetOverhead |
| deadband_suppressed(repair 跳过数) | SendRsRepair loss_deadband 分支 |

### 4.3 传输层
| 维度 | 埋点 |
|---|---|
| write_fail_total / write_fail_errno[] / write_fail_len_buckets | Tun::Write |
| write_eagain_count(队列压力) | Tun::Write EAGAIN 路径 |
| write_partial(部分写) | Tun::Write bytes != len |
| batch_abort_count / batch_abort_lost_pkts | WriteBatch 中断 |
| q_depth_peak(输入队列峰值) | FecPipeline reader 循环 |
| out_batch_size_dist | FecPipeline out.size |

### 4.4 协商/反馈
| 维度 | 埋点 |
|---|---|
| rtt_ewma_us / peer_loss_rate / latest_loss_rate | FecSharedState |
| ping_timeout_count / feedback_timeout_count | SendPing / SendFeedback |
| mux_state_changes / tx_mismatch / invalid_channel / session_timeout | EndpointUdpDynMux |

### 4.5 系统对照(自读)
| 维度 | 来源 |
|---|---|
| rx_packets / rx_dropped / tx_packets / tx_dropped(增量) | /sys/class/net/<iface>/statistics/ |
| vm_rss_kb | /proc/self/status |
| mem_available_kb | /proc/meminfo |
| cpu_usage | /proc/stat delta |

## 五、CSV 输出设计(核心, 参考 UE LLM)

### 5.1 文件布局
```
/var/log/great-hole/stats/fec-test-<yyyymmdd>.csv
    ↑ lua 可配置 stats_dir
    ↑ 按天轮转, 保留 stats_keep_days(默认 7)自动清理
```

### 5.2 行格式(每 60s 一行, 全维度快照)
```
ts,enc_src_pkts,enc_repair_pkts,enc_small_pkts,batch_size_avg,overhead,
dec_src_pkts,dec_missing,dec_loss_rate,recover_attempts,recover_success,
recover_shards,recover_failed,recover_abandoned,reorder_early,dup_pkts,
write_fail,write_eagain,write_partial,batch_abort,batch_abort_lost,q_depth_peak,
rtt_us,peer_loss,ping_timeout,mux_state_changes,
sys_rx_pkts,sys_rx_dropped,sys_tx_pkts,sys_tx_dropped,vm_rss_kb,mem_avail_kb,cpu_pct
```
*注: 除 rate 类外, 均为累计值(CSV 分析时做差分), 避免采样抖动*

### 5.3 写入策略(低开销)
- 内存缓冲 10 行 → 批量 append(单次 fwrite, 无逐行 flush)
- 采样 tick 挂 FecPipeline 已有 Tick 协程(非新线程)
- 文件打开失败/磁盘满 → 静默降级(计数到 stats_disk_error, 不阻塞数据面)

### 5.4 大小预算
```
60s 一行 × 1440 行/天 × ~200B/行 ≈ 300KB/天/接口 → 7 天 ≈ 2MB(可忽略)
```

## 六、事件日志(极低频)

| 事件 | 级别 | 条件 |
|---|---|---|
| recover_abandoned > 0 | warning | 丢包超 FEC 能力(关键!) |
| recover_failed > 0 | warning | 解码失败 |
| overhead 触及 max(0.50) | warning | 修复能力饱和 |
| write_fail 速率 > 1000/s 持续 5s | warning | 内核丢包风暴 |
| rx_dropped 增量异常(> 1%/周期) | warning | 内核丢包 |
| errno 变化 | warning | 错误类型切换 |
| mux 状态异常(INVALID_CHANNEL/超时) | warning | 协商异常 |

## 七、内存环形缓冲(快速回溯)
- 按秒存最近 3600 个快照(全维度压缩: 速率 + 关键计数)
- 大小: ~3600 × 64B ≈ 230KB(可忽略)
- 事件日志可附带最近 N 秒摘要

## 八、配置(lua, 全部可选, 默认值)
```lua
stats = {
    enabled = true,
    interval_ms = 60000,      -- 采样汇总周期 (LLM 风格, 可调)
    csv = true,               -- 启用 CSV
    csv_dir = "/var/log/great-hole/stats",
    csv_keep_days = 7,
    sys_interface = "fec-test",  -- 系统计数器接口名
    event_log = true,         -- 事件日志
    ring_buffer_sec = 3600,
}
```

## 九、性能预算
| 项 | 开销 |
|---|---|
| 原子计数(热路径) | ~2ns/次, 数据面可忽略 |
| 10s 系统采样(/sys /proc read) | <0.1ms/次 |
| 60s CSV 写(200B 行) | <0.1ms/次 |
| 事件日志 | 仅异常, 无常态开销 |

## 十、实施计划

| 步骤 | 文件 | 内容 |
|---|---|---|
| 1 | `src/core/FecStats.hpp/.cpp`(新) | 统计结构 + 原子计数 + 采样器 + CSV 写入 + 环形缓冲 |
| 2 | `src/core/RsCodec.cpp` | 埋点(dec/enc/recover/abandoned/dup) |
| 3 | `src/core/AdaptiveOverhead.cpp` | overhead 值采样(GetOverhead 快照) |
| 4 | `src/core/EndpointTun.cpp` | write 失败/部分写/EAGAIN 计数 |
| 5 | `src/core/FecPipeline.cpp` | 周期 tick 挂载 + out_batch/q_depth |
| 6 | `src/core/EndpointUdpDynMux.cpp` | mux 状态计数 |
| 7 | `src/LuaInterface.cpp` | lua stats 配置绑定 |
| 8 | `src/core/FecConfig.hpp` | 配置项 |
| 9 | 两端部署 | fec-test 隧道, 对照 rx_dropped 归因验证 |

## 十一、验收标准
1. 100 万级 write_fail 事件 → CSV 行数不变(计数), 日志 0 行常态
2. 22:04 类事件重现 → CSV 可精确回溯: 丢包起点/速率/归因(内核 vs 链路 vs FEC 放弃)
3. 数据面性能无退化(80M 隧道 iperf3 前后对比)
4. CSV 文件轮转/清理正常, 磁盘占用 < 5MB/周
5. lua 可完整开关

## 十二、待定(实现时决策)
- CSV 用累计值还是增量值?(建议累计, 分析端差分——避免采样丢失造成的数据缺口)
- 事件日志是否需要附带环形缓冲摘要?(建议带最近 60s 的 loss/recover 摘要, 一行内)
- 多接口支持?(当前 fec-test 单接口, 结构预留 iface 维度)
