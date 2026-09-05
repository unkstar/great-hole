# great-hole 使用说明(中文)

> great-hole 是一个基于 **Lua 配置驱动**的 FEC(前向纠错)隧道框架。它将一个 TUN 接口与一个 UDP 传输配对,通过 **RS(Reed-Solomon)/RaptorQ 前向纠错 + 小包重复 + 自适应冗余率**在丢包链路上提供可靠传输,专为跨境/高丢包链路设计。
>
> 适用场景:双端固定公网 IP 的点对点隧道(如 VPS 之间)、高丢包国际链路、嵌套进 WireGuard/OpenVPN 下的传输层。

---

## 一、总体架构

```
┌─────────────┐     ┌────────────────────────────┐     ┌─────────────┐
│  TUN 接口    │ ──► │  FecPipeline (编码器 encoder) │ ──► │  UDP 传输    │
│ (IP 包进出)  │ ◄── │  FecPipeline (解码器 decoder) │ ◄── │ (对端 20086) │
└─────────────┘     └────────────────────────────┘     └─────────────┘
```

- **编码器(encoder)**:从 TUN 读 IP 包 → 切成 RS 符号 → 分批 + 生成修复包(repair)→ 经 UDP 发出;小包(<256B,如 TCP ACK)走"直接发送 + 冗余重复"路径。
- **解码器(decoder)**:从 UDP 收包 → 恢复源符号顺序 → 对缺失符号用 repair 做 RS 解码补全 → 写入 TUN。
- 控制面:**PING/FEEDBACK** 心跳承载 RTT 测量与双向丢包率反馈,驱动自适应冗余率。
- 统计面:**FecStats**(可选)按周期采样,输出 CSV/事件日志。

一个典型的最小配置(Lua):

```lua
-- 传输: 固定端口 UDP(点对点直连,无协商)
u = hole.udp(20086)                       -- 绑定本端 20086
c = u:create_channel("203.0.113.5", 20086) -- 指向对端(注意: 两端都要绑定固定端口)
t = hole.tun("fec0")                      -- TUN 接口 fec0

local fec_cfg = { fec_codec = "rs", timeout_ms = 20, overhead = 0.03, ... }

local shared = hole.fec_shared_state()    -- 编码/解码共享状态(RTT/丢包率)
p_enc = hole.fec_pipeline(t, {}, c, fec_cfg, true,  shared)  -- 编码器: TUN→UDP
p_dec = hole.fec_pipeline(c, {}, t, fec_cfg, false, shared)  -- 解码器: UDP→TUN

hole.wait_for_exit()
p_dec:stop(); p_enc:stop(); t:stop(); c:stop(); u:stop()
```

---

## 二、Lua API 参考

### 2.1 `hole.udp([port])` — 裸 UDP 传输(推荐直连模式)

创建 UDP 端点。**无参数**:不绑定端口(仅作发送端);**`hole.udp(port)`**:绑定固定端口。

| 方法 | 说明 |
|---|---|
| `u:create_channel(host, port)` | 创建到 `host:port` 的通道(UDP 直连,**无 PSK/无协商**) |
| `u:create_channel("psk")` | 按 PSK 创建(需要与 mux 服务端配合) |
| `u:stop()` | 停止 |

> ⚠️ **重要**:`Udp` 通道按 `IP:port` **精确匹配**入站包。两端都必须绑定固定端口(`hole.udp(port)`),否则对端(源端口随机)的包会被丢弃,表现为"隧道建不起来"。

### 2.2 `hole.udp_dyn_mux(port)` — 动态复用 UDP(多路复用 + PSK 协商)

一个 UDP 端口承载多个逻辑通道,带 PSK 挑战-应答协商与 keepalive。适合:一端地址不固定(如拨号)、或需要轻量加密握手。

| 方法 | 说明 |
|---|---|
| `m:create_channel("0123456789abcdef")` | 被动侧:监听,等待对端 initiate |
| `m:create_channel("0123456789abcdef", "203.0.113.5", 20086)` | 主动侧:指定对端地址发起协商 |
| `m:stop()` | 停止 |

> 两端固定公网 IP 时优先用 `hole.udp` 直连(更简单、少一层协商);`udp_dyn_mux` 用于需要对端感知/NAT 场景。

### 2.3 `hole.udp_mux_server(port)` / `hole.tun(name)`

- `udp_mux_server`:老式多路复用服务端(按 PSK 建通道),一般用 `udp_dyn_mux` 替代。
- `hole.tun("fec0")`:打开/创建名为 `fec0` 的 TUN 设备(需要 `CAP_NET_ADMIN`,systemd 下用 `AmbientCapabilities=` 授予)。

### 2.4 `hole.filter_xor(key)` — XOR 混淆过滤器

对包做 XOR 异或混淆(需要 ≥32 字节 key)。可作为 pipeline 的 filter 传入:`fec_pipeline(t, {f}, c, ...)`。

### 2.5 `hole.fec_pipeline(in, filters, out, cfg, is_encoder, [shared], [stats])`

创建 FEC 处理管线。参数:

| # | 参数 | 说明 |
|---|---|---|
| 1 | `in` | 输入端点(编码器=TUN;解码器=UDP 通道) |
| 2 | `filters` | 过滤器表(可为 `{}`) |
| 3 | `out` | 输出端点(编码器=UDP 通道;解码器=TUN) |
| 4 | `cfg` | FEC 配置表(见第三章) |
| 5 | `is_encoder` | `true`=编码器,`false`=解码器 |
| 6 | `shared`(可选) | `fec_shared_state()` 返回的共享状态;**编码器与解码器必须共用同一个** |
| 7 | `stats`(可选) | `fec_stats({...})` 返回的统计对象(见第六章) |

方法:`p:stop()`。

> ⚠️ 控制包(PING/FEEDBACK)只由**编码器侧**(out=UDP 通道)发出。若把控制包发给 TUN 输出端,会被内核以 EINVAL 拒绝并破坏 RTT 反馈。

### 2.6 `hole.fec_shared_state()`

创建编码器/解码器共享的状态对象(RTT EWMA、丢包率、echo 待发等)。**同一条隧道的 encoder/decoder 必须传同一个对象。**

### 2.7 `hole.fec_stats(config)` — 统计系统

见第六章。

### 2.8 `hole.wait_for_exit()`

阻塞直到进程收到退出信号。

---

## 三、FEC 配置项全解(`fec_cfg` 表)

### 3.1 编解码器与符号

| 字段 | 默认 | 说明 |
|---|---|---|
| `fec_codec` | `"lcrq"` | `"rs"`:Vandermonde GF(256) 里德-所罗门;`"lcrq"`:RaptorQ(RFC 6330)。**实测推荐 `"rs"`** |
| `symbol_size` | 0(自动) | RS 符号大小(字节)。0 = 由 `mtu` 自动计算(通常 = MTU) |
| `mtu` | 1500 | 用于自动计算符号大小的链路 MTU。**必须 ≤ 实际路径 MTU**。典型:物理 1492/隧道内 1380 |
| `max_batch` | 200 | 一批的最大源符号数(RS 每批修复能力 = 批内 repair 数)。**典型 20**(见 3.4 批量说明) |

### 3.2 冗余率(repair 量)

| 字段 | 默认 | 说明 |
|---|---|---|
| `overhead` | 0.15 | 初始冗余率。每批 repair 数 `m = ceil(批内符号数 × overhead)`。**15% 是保守默认;低丢包链路可设 0.03** |
| `max_overhead` | 0.50 | 自适应冗余率上限(50%)。设太高会挤占有效带宽 |
| `repeat_ratio` | 4.0 | 小包(<256B,如 ACK)直接发送路径的重复系数:`副本数 = 1 + ceil(ratio)`(4.0 → 5 份) |
| `repeat_ratio_min` | 1.0 | 重复系数下限(最低副本数 2) |
| `repeat_ratio_max` | 5.0 | 重复系数上限(最高副本数 6) |

**冗余率语义**:丢包率 `L` 下,需要的冗余 ≈ `L/(1-L)`。自适应算法会按实测丢包率调整,`overhead` 是无丢包时的收敛值/初始值。

### 3.3 自适应补偿算法

| 字段 | 默认 | 说明 |
|---|---|---|
| `algo` | 1 | 算法编号 0~7,见第四章 |
| `loss_alpha` | 0.1 | 丢包率 EWMA 平滑系数(越小越平滑、响应越慢) |
| `loss_window_groups` | 50 | 丢包率更新窗口(组数;约 0.5~1s 一次更新) |
| `safety_margin` | 0.01 | 自适应冗余率的安全下限(额外加的余量,1%) |
| `loss_deadband` | -1.0(禁用) | ≥0 时:实测丢包 ≤ 该值时**不发任何 repair**(干净链路的零冗余模式)。**-1 = 禁用,保留每批 m=1 的底补偿**(推荐,见 3.5) |

### 3.4 批处理与延迟

| 字段 | 默认 | 说明 |
|---|---|---|
| `timeout_ms` | 4 | 批的最大等待时间:源符号凑批,满 `max_batch` 或超时即发出。**越小延迟越低、批越小修复效率越低** |
| `decode_timeout_ms` | 200 | 解码器等待 repair 的最长时间(超时未补全的批记 abandoned)。内部会按 RTT 校准 |
| `decode_window` | 64 | 解码环形缓冲容量(最大 256) |

**批量机制**:编码器把连续源符号凑成一批(≤`max_batch` 个或 `timeout_ms` 超时),批尾生成 `m` 个 repair 一起发。修复粒度 = 批:一批内丢 ≤ m 个符号时,解码器用 repair 全部找回。**高丢包突发场景下,批越小越容易被"一批丢超过 m 个"击穿;`max_batch=20` + `timeout_ms=20` 是实测折中。**

### 3.5 控制面(RTT/丢包率反馈)

| 字段 | 默认 | 说明 |
|---|---|---|
| `ping_interval_ms` | 1000 | PING 发送间隔(ms);0 = 关闭 |
| `feedback_timeout_ms` | 2000 | 无数据时最长等待反馈的时间 |
| `feedback_stale_ms` | 10000 | 反馈失效回退时间:超过该时长无反馈,冗余率回落到初始值 |
| `ping_loss_threshold` | 5 | 连续丢 PING 阈值(用于判定链路断开) |

**反馈链路**:编码器发 PING(带本端测量丢包率)→ 对端解码器收到后回 FEEDBACK(echo)→ 编码器算 RTT、更新"对端方向丢包率"→ 自适应算法据此调冗余率。**编码器保护的方向 = 对端反馈的丢包率(`peer_loss_rate`),不是本端测的丢包率。**

### 3.6 混淆

| 字段 | 默认 | 说明 |
|---|---|---|
| `obfuscate` | true | 是否启用 IV XOR 混淆(抗特征识别) |
| `iv_len` | 4 | IV 长度(1~8 字节) |

### 3.7 测试用可控丢包(生产不要开)

| 字段 | 默认 | 说明 |
|---|---|---|
| `test_drop_pattern` | 0 | 0=关闭;1=Bernoulli(独立随机);2=Gilbert(两态马尔可夫);3=GElliott;4=Sine(正弦周期);5=Step(阶跃);6=CongWave(拥塞波) |
| `test_drop_rate` | 0.0 | 主丢包率参数 |
| `test_drop_rate2` | 0.0 | 次参数(与模式相关) |
| `test_drop_burst` | 1 | 突发长度/周期(与模式相关) |

---

## 四、自适应补偿算法(`algo` 0~7)

所有算法输入为丢包率采样 [0,1],输出推荐冗余率 [0,`max_overhead`]。

| # | 名称 | 原理 | 适用 |
|---|---|---|---|
| 0 | **Static** | 固定 `overhead`,永不调整 | 链路特性已知、不想自适应 |
| 1 | **EWMA+StaticSafety** | `L_ewma = α·L + (1-α)·L_ewma`;`oh = L/(1-L) + safety` | 通用默认(平滑、保守) |
| 2 | **EWMA+DynamicSafety** | 同上,但 safety 随丢包采样方差缩放(波动大时加余量) | 丢包率波动明显的链路 |
| 3 | **PI 控制器** | 比例积分反馈,把丢包率控制到目标值 `L_target` | 需要精确控制目标丢包率 |
| 4 | **MIMD** | 解码失败 → 冗余率乘增(`×1+λ_up`);连续成功 `N` 次 → 乘减(`×1-λ_down`) | 突发丢包快速响应 |
| 5 | **Quantile** | 滑动窗口丢包率的分位数估计(如 P95) | 需要覆盖极端丢包 |
| 6 | **BurstAware** | 双 EWMA:慢速跟踪背景、快速跟踪突发;采样超过背景+阈值 → 激活快速通道 | **突发丢包链路(实测 5% 突发下最优)** |
| 7 | **Gradient** | 以吞吐 `(1-oh)(1-L)` 为目标做梯度搜索(ε-贪心探索两个候选冗余率) | 需要吞吐最优 |

> 2026-08 实测(64 项矩阵:8 算法 × 4 丢包模式 × 2 速率):**repair 充足时算法间差异 <3%**,瓶颈在于冗余率是否跟得上丢包;`BurstAware` 在 5% 突发丢包下表现最好。生产(突发国际链路)推荐 `algo=6`。

---

## 五、共享状态与线缆格式(进阶)

- **FecSharedState 字段**:`pending_feedback_echo`(待回显时间戳)、`rtt_ewma_us`(RTT EWMA)、`last_ping_sent_us`、`consecutive_ping_lost`、`latest_loss_rate`(本端解码器测得的丢包率)、`peer_loss_rate`(**对端方向的丢包率,自适应算法的输入**)。
- **线上包 flags**(首 4 字节 dword 的高 8 位):`kPing=0x10`、`kFeedback=0x20`、`kRepeat=0x40`(lcrq 重复)、`kEcho=0x80`;RS 模式:`kRsSmall=0x08`(小包直发)、`kRsRepair=0x40`(修复包)。
- RS repair 包格式:`[dword: bid|kRsRepair<<24][fb][bid lo16][bid hi8][k][idx][repair 符号]`。

---

## 六、统计系统(`hole.fec_stats`,LLM-CSV 模式)

### 6.1 配置项

```lua
local st = hole.fec_stats({
    enabled = true,          -- 总开关(默认 false,不影响数据面)
    interval_ms = 60000,     -- 汇总/CSV 周期(ms)
    csv = true,              -- 写 CSV
    csv_dir = "/var/log/great-hole/stats",  -- CSV 目录
    csv_keep_days = 7,       -- 保留天数(按天轮转)
    sys_interface = "fec-test", -- 要采样 /sys 统计的接口名
    event_log = true,        -- 事件日志(异常时写 journal)
    ring_buffer_sec = 3600,  -- 环形缓冲秒数(最近 N 秒速率快照)
})
```

### 6.2 CSV 列(44 列,行 = 周期快照;累计值分析时做差分)

| 列 | 含义 | 列 | 含义 |
|---|---|---|---|
| ts | Unix 秒 | enc_src | 编码源包数 |
| dec_src | 解码源包数 | enc_small | 编码小包数 |
| dec_missing | 解码缺失源符号(链路丢包) | enc_repair | 编码修复符号数 |
| dec_small | 解码小包数 | deadband_supp | 死区抑制的 repair 批数 |
| dec_ctrl | 解码控制包数 | overhead | 当前冗余率 |
| dec_dup | 解码重复包(去重) | overhead_max_hits | 冗余率达上限次数 |
| recover_attempts | 恢复尝试次数 | batch_full / batch_timeout | 满批 / 超时批次数 |
| recover_success | 恢复成功批数 | batch_size_avg | 平均批大小 |
| recover_shards | 恢复符号数 | write_fail / write_eagain / write_partial | TUN 写失败 / 队列满 / 部分写 |
| recover_failed | 解码失败批数 | batch_abort / batch_abort_lost | 批中断 / 连带丢失 |
| recover_abandoned | 超时放弃批数(**FEC 能力不足**) | q_depth_peak / out_batch_avg | 队列峰值 / 平均输出批 |
| reorder_early | 乱序早到 | rtt_us / peer_loss | RTT / 对端丢包率 |
| decode_timeouts | 解码超时清理 | ping/feedback_timeout | 控制超时 |
| loss_rate | 本端丢包率 | mux_* | 动态复用状态计数 |
| — | — | sys_rx_pkts/dropped、tx_pkts/dropped | **内核接口计数(累计值)** |
| — | — | vm_rss_kb / mem_avail_kb / cpu_pct | 进程内存 / 系统内存 / CPU |

**归因三件套**:`sys_rx_dropped`(内核丢:进程没及时读)→ `dec_missing`(链路丢:对端发出后丢失)→ `recover_abandoned`(FEC 能力不足:repair 不够)。一次"用户感受到的丢包"必落在这三者之一。

**注意**:
- `/sys` 接口计数是**历史累计**(进程重启不归零),首行即基线;分析时做行间差分。
- CSV 按天轮转(文件名 `fec-test-YYYYMMDD.csv`),**行级 flush**(60s/行,崩溃不丢最近数据)。
- 文件前缀当前硬编码为 `fec-test-`。

---

## 七、构建与部署

### 7.1 编译

```bash
# 依赖: Clang >= 16 (C++23) + Boost >= 1.80 + CMake
cmake -DCMAKE_BUILD_TYPE=Release -B build   # 必须显式 Release!
cmake --build build -j$(nproc)
```

> ⚠️ **务必显式 `-DCMAKE_BUILD_TYPE=Release`**:不设则默认无优化(-O0),RS 编解码热路径慢 2~3 倍(实测 45M vs 92M)。验证:`grep CXX_FLAGS build/src/CMakeFiles/great-hole.dir/flags.make` 应含 `-O3 -DNDEBUG`。

### 7.2 systemd 部署模板

```ini
[Unit]
Description=great-hole FEC tunnel (side A)
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
ExecStartPre=/etc/great-hole/fec/fec-up.sh      # 建 TUN + IP
ExecStart=/usr/bin/great-hole --startlua /etc/great-hole/fec/tunnel.lua
StandardOutput=journal
StandardError=journal
Restart=always
RestartSec=5
AmbientCapabilities=CAP_SYS_ADMIN CAP_NET_ADMIN CAP_NET_RAW

[Install]
WantedBy=multi-user.target
```

启动前 up 脚本示例:

```bash
ip tuntap add dev fec0 mode tun 2>/dev/null || true
ip addr add 172.31.40.1/32 peer 172.31.40.2/32 dev fec0 2>/dev/null || true
ip link set fec0 mtu 1380 up
ip route replace 172.31.40.2 dev fec0 2>/dev/null || true
```

### 7.3 出口切换(`scripts/switch-tunnel.sh`)

```bash
ALI_SUDO_PW='<sudo 密码>' bash scripts/switch-tunnel.sh {fec|prod|status}
```

- `fec` = RS FEC 直连隧道;`prod` = 嵌套隧道(fec-tokyo + speederv2)。
- 模式持久化在两端 `/etc/great-hole/fec/tunnel-mode`,up/down 脚本据此决定是否接管出口路由(防止机器重启后出口被错误改回)。
- 固定 IP 两端用 `hole.udp` 直连;出口选择(osaka/tokyo)用 `scripts/switch-exit`。

### 7.4 经验教训速查

| 坑 | 说明 |
|---|---|
| 编译不优化 | CMake 必须 `-DCMAKE_BUILD_TYPE=Release`(见 7.1) |
| tun 部分写入 | 生产(NDEBUG)下旧断言失效 = 静默截断;已改为循环写全(2026-08 修复) |
| 控制包写 TUN | 解码器 out=TUN 时发 PING 会被 EINVAL 拒;控制包只在编码器侧发 |
| UDP 直连源端口 | 通道按 IP:port 精确匹配,两端都要固定端口 |
| 恢复不触发 | repair 先于源包到达时旧逻辑永不重试(已改为周期重试,2026-08 修复) |
| CSV 缓冲 | 必须行级 flush,否则崩溃丢数据 |

---

## 八、诊断速查

| 症状 | 查什么 |
|---|---|
| 图片/网页卡但 ping 通 | 丢包在数据流(限速),不在 ICMP;查 CSV `dec_missing`/`loss_rate` |
| 单侧丢包 | 查 `peer_loss`(对端反馈)与 `loss_rate`(本端)方向是否吻合 |
| FEC 补不过来 | `recover_abandoned` 增长 = repair 不足:链路丢包率 > 冗余率×(1-丢包率) |
| 内核丢包 | `sys_rx_dropped` 差分增长 = 进程读 TUN 不及时(或写侧队列满) |
| 速度锯齿 | 上行 TCP 丢包 → 重传 → cwnd 坍缩;确认 repair 是否跟上(`overhead` 是否爬升) |
| overhead 不爬升 | 查 `peer_loss` 反馈是否为 0(控制包链路) |
