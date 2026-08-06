# Handoff: RS FEC 第二阶段 (2026-08-06 深夜)

**状态**: 架构重构完成,双模式(直连/dynmux)基本跑通;**剩余: TCP 偶发停滞(255-skip 根因未决)+ 反向 80M 丢包**。
**分支**: `fec-writebatch` (HEAD = bbf1455)

---

## 一、已完成并验证(全部已提交)

| 提交 | 内容 |
|---|---|
| `17133bf` | **FecCodec 策略模式重构**: 传输层唯一循环(reader fiber 常驻 pending read → deque → worker → codec.OnPacket 同步处理 → WriteBatch);RsCodec/LcrqCodec 纯同步零 I/O;lcrq 容器修复(FindSlot 直接索引、shard esi 索引、deque) |
| `b0c31ce` | 测试报告 `docs/RS-FEC-TEST-REPORT-2026-08-06.md` + dynmux 配置 peer 修复 |
| `bbf1455` | RS 三修复: 小包冗余+去重、repair bid 24位、stall guard 最小 seq 扫描 |

**验证结果**(重构后,algo=0 静态):
- UDP 50M/80M × 30s: 0% 丢包,双模式 ✓
- TCP 10s/30s: 完整通过("iperf Done.",17-42M)✓

## 二、剩余问题(下一会话主线)

### 1. TCP 偶发停滞 + 周期性 255-skip(最关键)
- 现象: TCP 多数通过,但偶发中途数据停滞;解码端日志 `RsCodec watermark stall: skipped 255 missing shards` 每 ~2s 一次
- **255 ≈ 在途窗口**(RTT 57ms × ~4,200 pps ≈ 240-260): 每次停滞,水位把整个在途窗口都跳过 → 数据丢失 → TCP 重传风暴 → cwnd 塌缩
- **repair 恢复 0 次**(`RS recovered` / `recover failed` 日志均为 0)——repairs 在线上(3,662 个),但从未触发恢复
- 已排查排除: overhead/PI 本身(lcrq+PI 的 TCP 正常)、epoll、缓冲、日志、map、小包冗余
- 下一步: **查 repair 解码为什么没触发 RsTryRecover**(wire 1452B = 修复后格式;解析逻辑需与编码端 3B bid 逐字节核对;可能的坑: `p.DataSize() < T` 边界、mux RxId 剥离后的长度)

### 2. 反向路径(ali→tokyo)高带宽丢包
- 反向 80M: RS 99% 丢包 / lcrq 59% —— 两 codec 都丢,但 RS 更差
- 反向解码端在 tokyo(1 vCPU)——可能与 CPU 有关,但 lcrq 同机器 59% vs RS 99% 说明 RS 解码更差,值得查
- 反向 20M/50M: 正常或偶发

### 3. 其他待办(报告中)
- TCP 吞吐 17-42M vs 直连 36-92M: 受线路 ~1.6% 丢包影响
- 24h 稳定性 soak
- openspec Phase 7/9 任务状态更新 + 最终 spec 回填

## 三、服务器状态

| 机器 | 测试服务 | 配置 | 说明 |
|---|---|---|---|
| tokyo | `great-hole-fec-test-tokyo` | `/etc/great-hole/fec/fec-test-tokyo.lua` | **当前跑 dynmux 配置 + algo=3(PI)** |
| ali | `great-hole-fec-test-ali` | `/etc/great-hole/fec/fec-test-ali.lua` | 同上 |

- 部署流程: tokyo `obj2` 构建 → 拷贝 `/usr/bin/great-hole-fec-test` → scp 到 ali → 两端重启服务
- 切换直连/dynmux: 换 lua 文件(仓库 `configs/` 有全部 4 个版本)+ 重启
- 生产链路(fec-tokyo 10086)未受影响
- iperf3 server: 用 `-1` 单次模式,每次测试前重启(单线程 server 会卡死级联)

## 四、SSH/环境(重要变化)

- **SSH 已切换到 Windows 自带 OpenSSH**: `C:/Windows/System32/OpenSSH/ssh.exe`(agent 为其自带服务,密钥已加载)
- 不再使用 msysgit ssh + `/tmp/ssh-nLiVXPjFur2A/agent.761`(旧 agent socket 已失效)
- 所有 ssh 命令必须带 `-o BatchMode=yes`(否则 Windows 对带口令密钥弹窗)
- tunnel-test 技能脚本已适配: `~/.claude/skills/tunnel-test/scripts/tunnel_test.py`(Windows ssh + 每测试重启 `-1` server + BatchMode)
- ali sudo: `echo 'whatRUfooling$' | sudo -S ...`;tokyo sudo 免密

## 五、关键文件

- `src/core/FecCodec.hpp/cpp` — 策略接口 + 工厂 + wire flags
- `src/core/RsCodec.hpp/cpp` — RS codec(ring 槽、批次、水位、repair、小包冗余/去重)
- `src/core/LcrqCodec.hpp/cpp` — lcrq codec(同步化 + 容器修复)
- `src/core/FecPipeline.hpp/cpp` — 传输层唯一循环
- `configs/fec-test-{tokyo,ali}.lua` — 直连;`configs/fec-test-dynmux-{tokyo,ali}.lua` — dynmux(tokyo 侧含 peer 地址)
- `docs/RS-FEC-TEST-REPORT-2026-08-06.md` — 测试报告(含完整矩阵)
- `openspec/changes/fec-implementation/specs/fec-pipeline/spec.md` — 架构 spec(策略模式 + 容器纪律)

## 六、调试纪律(本会话教训)

1. **回归优先**: lcrq 是已知可用的基线——RS 的任何问题先与 lcrq 对比,不要怀疑环境(epoll/PI/overhead 都是先被怀疑后排除的)
2. 容器纪律: 热路径禁 map/线性扫描,用定长槽直接索引 + 容量复用
3. 修复前先更新 spec
4. 远端命令全部带超时,避免长阻塞
