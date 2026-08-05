# Handoff: RS Codec 实现与调试 (2026-08-05)

**状态**: RS (Vandermonde GF256) codec 完整实现已提交; P2P ping 通; **iperf3 大流量卡住未解决**。
**分支**: `fec-writebatch` (HEAD = fbaffad)

---

## 背景 (一句话)

RaptorQ 在 tokyo (1vCPU 无 AVX-512 云 vCPU) 上编码仅 27Mbps 是 CPU 瓶颈; RS (GF256) 标量实测 280Mbps (K=17) → 新增 `fec_codec="rs"` 可选实现, lcrq 保留为默认。

## 已完成 (全部已推送)

1. **RS256 库** (`src/core/RS256.hpp/cpp`): EXP/LOG 查表, Vandermonde 系数行, EncodeRepair (逐 repair 生成), Decode (k×k 高斯消元求逆)
2. **FecPipeline RS 路径**: `ProcessRsEncode/Decode` + `RsHandleEncodePacket/DecodePacket` + `SendRsRepair` + `RsFlushDelivery` + `RsTryRecover`
   - 编码: 源分片**即收即发** (零 batch 延迟, TCP 友好), 窗口到期按 AdaptiveOverhead 补发 repair
   - 解码: seq 水位交付 + 乱序缓存 + 缺口恢复 (Vandermonde 子矩阵求逆) + 水位 stall guard
   - wire: 源分片 `[DWORD seq][fb][len:2B][data pad T]`, repair `[DWORD bid][fb][bid:2B][k][idx][T data]`, 小包(<256B)直发 `[DWORD 0+kRsSmall][fb][len][data]`
3. **配置**: `FecConfig.fec_codec` + Lua 绑定 (`configs/fec-test-tokyo.lua` / `fec-test-ali.lua`, P2P 直连 UDP 20086)
4. **调试中修复的 bug** (按时间):
   - `2279161`: EPOLLET 排空 (Read 后 TryRead 循环, 否则挂起)
   - `651f176`: 小包不消耗 RS seq 空间; 水位 stall guard
   - `b12e965`/`de9c1d4`: dyn_mux 协商死锁 (对称握手: 无条件回 initiate, myRx mismatch 返回 kNegotiating)
   - `fbaffad`: **TryRead 条件反转** (lcrq 用 `if(TryRead) break;` 我写成 `if(!TryRead) break;` → EAGAIN 时处理残留 2048B 缓冲无限循环) — **这是 2048B 幽灵包根因**
5. **lcrq 构建集成修复** (更早, a133d78 相关): `cmake/lcrq.cmake` + `cmake/lcrq-install.sh` (BUILD_IN_SOURCE + ar 打包 + 头文件)

## 当前卡点 (iperf3 大流量)

**现象**:
- ping 通 (57ms, 小包直发路径正常)
- iperf3 TCP: 握手完成 (ESTAB), 37B 数据段 (iperf3 协议头) 发出, **1380B 数据段从未进 TUN** (RS-ENC 日志只有 52-96B 小包); ACK 12 秒才回 → TCP 停滞
- iperf3 UDP: 同样卡 (无输出)
- eth0 抓包: 只有 13/59/67/96B 包, 无大包

**推论**: ≥256B 的大包路径无流量 — 嫌疑: 编码端 RS 源分片发送 或 TUN 读大包; **下一步: UDP 20M 短测 + 看 RS-ENC 是否出现大包 + RS batch 日志** (调试日志已在代码里: `RS-ENC in=<size> hdr=<ip头>` / `RS-DEC in=<size>`)

**注意**: 代码里 925acbc/72454fe 是**临时调试日志**, 问题解决后需清理。

## 服务器状态

| 机器 | IP | 角色 | 关键信息 |
|---|---|---|---|
| tokyo | 202.144.195.103 (SSH 63916, ggcuser) | 测试编码端 + 生产嵌套隧道 | 1vCPU 无 AVX-512; sudo 免密 |
| ali | 39.108.136.48 (SSH 22, unkstar) | 测试解码端 + 生产汇聚 | 2C AVX-512; sudo 需密码 `whatRUfooling$` |
| osaka | 23.106.140.137 (29832, unkstar) | 备用出口 | 与测试无关 |

- 生产链路: ali↔tokyo 嵌套 tunnel (speederv2 + great-hole nofec), **正常运行, 未受影响**
- `/usr/bin/great-hole-fec` = 8/4 原版 (已恢复, 生产回退二进制); `/usr/bin/great-hole-fec-test` = 当前修复版 (测试用)
- 测试服务: `great-hole-fec-test-tokyo.service` / `great-hole-fec-test-ali.service` (ExecStart 指向 great-hole-fec-test, ExecStartPre 重建 TUN)
- 测试链路: P2P UDP 20086 直连 (无 dyn_mux 协商), TUN 172.31.40.0/30 (tokyo .2 / ali .1), 配置在 `/etc/great-hole/fec/fec-test-{tokyo,ali}.lua`
- 两端防火墙 20086 入站已开 (用户确认)

## 调试命令速查

```bash
# 日志 (两端)
sudo journalctl -u great-hole-fec-test-tokyo -n 20 --no-pager | grep -E 'RS-ENC|RS-DEC|RS batch'
sudo journalctl -u great-hole-fec-test-ali -n 20 --no-pager | grep -E 'RS-ENC|RS-DEC'   # ali 需 sudo -S

# 重启 (tokyo)
sudo systemctl restart great-hole-fec-test-tokyo
# ali
echo 'whatRUfooling$' | sudo -S systemctl restart great-hole-fec-test-ali

# 构建 (tokyo)
cd /home/ggcuser/great-hole && git pull && cd obj2 && make -j2 great-hole
sudo systemctl stop great-hole-fec-test-tokyo; sudo cp obj2/src/great-hole /usr/bin/great-hole-fec-test; sudo systemctl start great-hole-fec-test-tokyo
# 部署到 ali
scp obj2/src/great-hole → ali:/tmp/ → echo pw | sudo -S cp /tmp/... /usr/bin/great-hole-fec-test

# iperf3 测试 (tokyo, ali 需起 server: nohup iperf3 -s -p 5201 &)
iperf3 -c 172.31.40.1 -t 10
iperf3 -c 172.31.40.1 -u -b 80M -t 8
```

## GOAL (未完成)

1. ✅ RS 完整实现 (fbaffad 含全部修复)
2. 🔄 P2P 链路跑通 — ping 通, **iperf3 大流量卡 (进行中)**
3. ❌ dyn_mux 链路跑通 — 协商修复已提交 (de9c1d4) 但未部署验证 (切回 dyn_mux 配置验证收敛 + ping)
4. ❌ 部署测试链接 + 性能测试 — 待链路跑通后: TCP/UDP 吞吐 vs lcrq (预期 UDP ~90M / TCP 接近 nofec)

## 性能参考 (已实测)

- RS 标量基准 (tokyo, K=17): 280Mbps; ali 同库: 216M (AVX2==AVX512 无差异)
- lcrq (tokyo): 27M UDP / 16M TCP; 直连链路: UDP 95M / TCP 36-92M
- 生产嵌套 (speederv2+nofec): TCP 38.9M / UDP 94.6M

## 关键文件

- `src/core/RS256.hpp/cpp` — GF256 库
- `src/core/FecPipeline.cpp/hpp` — RS 路径 (ProcessRsEncode/Decode 等)
- `src/core/FecConfig.hpp` — `fec_codec` 字段
- `src/LuaLib.cpp` — fec_codec 绑定
- `configs/fec-test-tokyo.lua` / `fec-test-ali.lua` — P2P 测试配置
- `openspec/specs/fec-spec.md` — RS 设计文档
- `openspec/changes/fec-implementation/tasks.md` — Phase 9 任务清单
