# Handoff: RS FEC 最终状态 (2026-08-08)

**状态**: 主线全部解决。TCP 停滞根因(乱序投递)已修复并验证;补偿率浪费(PI 积分漂移)已定位并切换到 EWMA。
**分支**: `fec-writebatch` (**HEAD = 7ea548c**)

---

## 一、最终测试结果(测试隧道 tokyo↔ali,多次复现)

| 测试 | 结果 |
|---|---|
| TCP dl | **88.5 / 86.8M**(重传 ~1105-1953,乱序 dup ACK,数据全到,无害) |
| TCP ul | **65.2 / 64.3M** |
| UDP dl/ul 50M | **0% / 0%** |
| 实际补偿率(线缆实测) | **5.0%**(repair/shard,algo=1 EWMA) |

生产隧道(tokyo↔ali,无 FEC great-hole + 外层 UDPspeeder `-f1:1,20:2,30:2`):TCP dl 85.3M / ul 52.5M,UDP 50M 0% / 0.0044%。

## 二、完整修复链(自 544d446 起,按时间)

| 提交 | 内容 | 验证 |
|---|---|---|
| `f082528` | repair 24 位 bid 字节序(bbf1455 回归,恢复永不触发的根因)+ initiate 乒乓风暴(无条件回复自维持循环)+ repair mask/重试 + FecPipeline drain EPOLLET | 恢复开始触发;风暴 34/s→0 |
| `87648ba` | 回滚 repair ×2(错误证据)+ 批次口径丢包测量(RS 缺 PI 反馈) | fail_rate ≈ 真实 |
| `ae9cbd9` | AlgoPI overhead floor(冷启动 m=0:公式零丢包时负值→clamp 0) | — |
| `5a201ca` | **乱序投递 + repair 摊开(TCP 停滞根因)** | TCP 10-14M → 92M |
| `7ea548c` | 配置 algo=1 EWMA + overhead=0.03(补偿率 15.5%→5%) | 吞吐不降反升 |

## 三、TCP 停滞根因链(乱序投递修复前)

```
线路偶发乱序/延迟(线路本身 0% 丢,裸连 UDP 100M 双向 0%)
→ watermark 保序:缺口卡 200ms → guard "跳过" = 暂时缺口变永久丢失
→ dup ACK 风暴(306 ACK/s = 13 倍)→ 重传 burst(800-1000 shard/s)
→ 中间设备丢 burst(裸连/UDP 稳态无 burst 0% 丢;burst 时 31%)
→ 循环自维持 → TCP 0.2-14M
```

修复 = 乱序投递(数据不丢,TCP 重排)+ repair 摊开(1ms 间隔,避免与批次尾叠加 burst)。借鉴 UDPspeeder(mode 1 + -t)。

**关键教训**: 曾把 15% 端到端差异归因线路(ER-X)——被用户纠正:裸连 100M 0% 证明线路不丢,15% 是两端 tcpdump burst 漏抓的测量假象。放大器在解码端自身。

## 四、补偿率调查结论

- **PI(algo=3)在零丢包线路上必浪费**:积分漂移(原始设计,非最近改)把 overhead 推到 15.5% 固定高位,配置 floor 只挡下限挡不住向上漂移
- **algo=1(EWMA)** `oh = loss/(1-loss) + safety`:零丢包 ~5%(可衰减更低),15% 丢包 ~19% —— 与 UDPspeeder 多档按需同思路
- 实测:补偿率 15.5%→5%,TCP ul 62→65M

## 五、服务器状态

| 机器 | 服务 | 配置要点 |
|---|---|---|
| tokyo | `great-hole-fec-test-tokyo` | **algo=1, overhead=0.03**,decode_timeout=200,dynmux,运行 7ea548c 构建 |
| ali | `great-hole-fec-test-ali` | 同上 |

部署注意:
1. **scp 保留源 mtime → 部署前必须 `touch` 源文件再 make**(否则 make 误判不重编)
2. 部署链命令分步验证(pid 变化 + 二进制 mtime)
3. ali sudo: `echo 'whatRUfooling$' | sudo -S ...`;tokyo 免密
4. 测试前预热(或依赖 floor/EWMA 种子,冷启动已缓解)

## 六、遗留/可选

1. TCP 重传仍高(dl ~1105-1953):乱序投递的 dup ACK 快速重传,数据全到无害;若要压低:repair 摊开间隔/`--timeout` 调优
2. 24h soak 未做
3. 反向 80M UDP 未重测(50M 已 0%)
4. openspec Phase 7/9 状态更新未做
5. 生产 great-hole 为无 FEC 老代码 + UDPspeeder 外层(验证过 85M/52M)——若要升级到最终构建需单独评估
6. 测试配置已同步仓库(configs/fec-test-*.lua: algo=1, overhead=0.03)

## 七、关键文件

- `src/core/RsCodec.cpp/hpp` — 乱序投递 + repair 摊开 + 批次测量
- `src/core/AdaptiveOverhead.cpp/hpp` — PI floor + EWMA
- `src/core/EndpointUdpDynMux.cpp` — initiate 修复 + pacing 钩子
- `src/core/FecPipeline.cpp` — drain EPOLLET
- `configs/fec-test-*.lua` — algo=1, overhead=0.03
