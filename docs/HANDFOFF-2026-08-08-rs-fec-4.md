# Handoff: RS FEC 第四阶段 (2026-08-08)

**状态**: TCP 停滞根因已找到并修复 (`5a201ca`);TCP dl 92M / TCP ul 56M / UDP 双方向 0%。**主线问题全部解决**。
**分支**: `fec-writebatch` (HEAD = 5a201ca)

---

## 一、TCP 停滞的完整因果链(全部有证据)

```
1. 线路偶发乱序/延迟(裸连 UDP 100M 双向 0% 丢 —— 线路本身是好的)
2. 解码端 watermark 保序投递:任何暂时性缺口 → 卡住等 200ms → guard "跳过"
   = 把"暂时不在场"变成"永久丢失"     ← 放大器 #1(根因)
3. TCP 收到丢失 → dup ACK 风暴(306 ACK/s = 正常 13 倍)→ 快速重传 burst(800-1000 shard/s 瞬时)
4. burst 触发中间设备丢包(裸连/隧道 UDP 稳态无 burst → 0% 丢;burst 时丢 31%)
5. 更多缺失 → 循环自维持 → TCP 0.2-14M
```

**关键误判历程**:曾把第 4 步归因"线路 15% 丢"(ER-X)—— 被用户纠正:裸连 UDP 100M 双向 0% 证明线路不丢;15% 的"端到端差异"是两端 tcpdump 在 burst 时刻漏抓的测量假象。真正的放大器在解码端自身。

## 二、最终修复 (`5a201ca`,借鉴 UDPspeeder 设计)

| 项 | 内容 | 对应 UDPspeeder |
|---|---|---|
| 1 | **乱序投递**:shard 到达即投递,TCP 自己重排;缺口永不阻塞后续、永不被跳过成永久丢失;ring 保留副本供修复,恢复的 shard 立即投递。删除 watermark/guard/stall 机制 | mode 1 |
| 2 | **repair 摊开**:repairs 入队,Tick 每 1ms 发一个,不与批次尾部 shard 叠加 burst | -t |
| 3 | **发送 pacing 钩子**:WriteBatch 最小包间隔(常量默认 0,按需开启) | -j 精神 |

## 三、验证结果(最终版,多次复现)

| 测试 | 修复前 | 修复后 |
|---|---|---|
| TCP dl | 10-14M | **92.1/90.3M** |
| TCP ul | 0.2-2M | **53.6-56/52.8-55M** |
| UDP dl/ul 50M | 0% | **0% / 0%** |

## 四、本阶段全部提交(自 544d446 起)

| 提交 | 内容 |
|---|---|
| `f082528` | bid 24 位字节序回归修复(恢复从不触发的根因)+ initiate 乒乓风暴 + repair mask/重试 + FecPipeline drain EPOLLET |
| `87648ba` | 回滚 repair ×2(错误证据下的权宜)+ 批次口径丢包测量(补 RS 缺失的 PI 反馈) |
| `ae9cbd9` | AlgoPI overhead floor(冷启动 m=0 bug) |
| `5a201ca` | **乱序投递 + repair 摊开**(TCP 停滞根因修复) |

## 五、遗留/可选

- 重传计数仍非零(dl 591 / ul 177):乱序投递仍会触发少量 dup ACK/快速重传(数据都到,TCP 去重,无害)。若要进一步压低:repair 摊开间隔可调、或 pacing 开启
- 24h soak、反向 80M UDP、openspec Phase 7/9 状态更新未做
- 测量口径:批次过期时"未见 shard"计失败(乱序投递下延迟 shard 会高估丢包 → PI 升 overhead → 良性,可接受)

## 六、服务器状态

两端运行 `5a201ca` 最终构建,dynmux + algo=3,decode_timeout_ms=200,overhead=0.15(与 lcrq 默认一致)。
部署注意:scp 保留源 mtime,部署前 `touch` 源文件再 make(否则 make 误判不重编)。
