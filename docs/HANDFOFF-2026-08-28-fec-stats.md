# Handoff: FEC 统计信息系统 — LLM-CSV 模式 (2026-08-28)

**状态**: ✅ 已实现并上线 (2026-08-29, 生产 fec-test 升级, 部署细节见第十三章)
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

---

## 十三、实施经验教训 (2026-08-29 实测部署后)

### 1. 构建陷阱: CMAKE_BUILD_TYPE 为空 = -O0
**现象**: 统计二进制 iperf3 只有 41/45M, 生产(8/12 构建)92M, 同配置同路径。
**根因**: 本项目 CMakeLists 未设 `CMAKE_BUILD_TYPE`, 默认空 → 编译 flags 只有 `-std=c++23`,
**无优化 (-O0) 且断言启用**。生产二进制是 Release(-O3 -DNDEBUG, 断言被编译掉)。
RS 编解码热路径 -O0 vs -O3 差 2-3 倍。
**规则**: 任何部署构建必须显式 `cmake -DCMAKE_BUILD_TYPE=Release .`(验证 flags.make 含 `-O3 -DNDEBUG`)。
调试构建才有价值: 断言抓到了下面的真实 bug。

### 2. Tun::Write 部分写入 = 生产静默截断路径 (真实丢包来源之一)
**现象**: 调试构建启动即断言崩溃 `p._Length == bytes_transferred`。
**根因**: tun 非阻塞写队列满时 `async_write_some` 返回 would_block/部分写入; 原代码 assert 在
生产 NDEBUG 下失效 → **包尾静默丢弃, 上层以为写成功**。这正是此前无法归因的丢包路径之一。
**修复**: 循环写到完, would_block 重新注册等可写 edge(EPOLLET 安全), 部分进度计 WritePartial,
0 进度防御死循环。`EndpointTun.cpp`。
**教训**: NDEBUG 编译掉 assert 的代码里, "不可能发生"的假设 = 静默损坏。

### 3. 控制包必须只从 encoder(传输侧)发送
**现象**: CSV `write_fail` 每 60s +100(≈1/s), 且 `rtt_us=0` 永远。
**根因**: FecPipeline 主循环无条件发 PING/FEEDBACK。decoder 的 _Out 是 tun, 16B 非 IP 控制包
写 tun → 内核 EINVAL; 且 RTT 回显(pending_feedback_echo)被写进 tun 丢失 → 双向 RTT 测不到。
**修复**: `if (_IsEncoder && _Shared)` 才发控制包。decoder 仍处理入站控制并计算 RTT。
**教训**: 两个 pipeline 共享 io_context 但方向不同, 控制面必须绑定传输侧。

### 4. CSV 必须行级 flush
**现象**: 服务跑 10+ 分钟 CSV 仍 0 字节(表头都没落盘)。
**根因**: `fopen("a")` 全缓冲 4KB; 原设计每 10 行(10 分钟)flush 一次。
**修复**: 表头后立即 flush + 每行 flush(60s/行 ~200B, 开销可忽略)。
**教训**: 统计系统的价值是"崩溃后仍可读最近数据", 缓冲延迟落盘违背目的。

### 5. 测速/验证时 CPU 竞争会污染结果
**现象**: Release 二进制首测仍 64/54M(编译进程还在后台编 asan 目标抢 CPU)。
**修复**: 等编译完全结束后重测 → 73/94M, 与生产持平。
**教训**: 共享 VM 上任何基准测试前先确认无后台编译/高负载。

### 6. SSH 管理环境 (Windows)
- **必须 `unset SSH_AUTH_SOCK SSH_AGENT_PID`** 再用 `C:\Windows\System32\OpenSSH\ssh.exe`:
  残留的 Git Bash agent socket 会让 Windows ssh 连不上 Windows agent(有全部 key)。
- 残留的 Git Bash `ssh-agent` 进程(C:\Program Files\Git\usr\bin)会抢占 `\.\pipe\openssh-ssh-agent`,
  导致 publickey 被拒 —— 杀掉即可。
- PowerShell 里 `ssh`/`ssh-add` 默认解析到 Git 版本, 用全路径调 Windows 版。
- **Ali sudo 密码**: 记录在 `D:\tmp\network-topo\gz\gz-er4-deploy-handover-20260826.md`
  (`echo '密码' | sudo -S`, 单引号保字面量)。注意: 变量里的管道不重新解析,
  `PW='echo x | sudo -S'; $PW cmd` 只会 echo —— 必须写完整显式管道。

### 7. 固定 IP 两端用直连, 不用 dyn_mux (用户决策)
- `hole.udp(port)` + `u:create_channel(peer_host, port)`; **两端都必须绑定固定端口**,
  Udp channel 按 `IP:port` 精确匹配(EndpointUdp.cpp ReadLoop), 随机源端口会被对端丢弃
  (表现为"协商没反应"+ 日志 packet from unknown peer)。
- 直连 vs dyn_mux 性能无差异(对照实验验证), 直连更简单、无 PSK 协商开销。

### 8. 测试隧道部署模式(fec-test2 流程, 已撤销)
- 新二进制名 `great-hole-fec-test2`、新端口 20087、新 tun 接口 fec-test2、systemd 单元镜像生产。
- 二进制先停服务再 cp(Text file busy)。
- 统计 CSV 文件名硬编码 `fec-test-` 前缀 —— 测试与生产共用同一 CSV 文件(ts 可区分)。
  **后续优化**: 文件名改为 `sys_interface` 前缀, 避免混淆。

### 9. 性能结论
统计插桩(原子计数 + 60s Tick + 每行 flush)在 Release 下**零性能代价**:
fec-test2 73/94M vs 生产 81/92M(同轮波动内)。验收标准 3 达成。
