# Bug Hunter — Agent Scheduling

Sub-agent 调度规则。所有命令（`/bug-hunt`、`/bug-hunt-diff`、`/bug-hunt-file`）的 orchestrator 都 **必须** 遵循本文档中的并发控制规则。

## 并发控制原理

Claude Code 的 Task tool 并发行为：

- **同一条消息** 中的多个 Task 调用 → 并行执行
- **不同消息** 中的 Task 调用 → 串行执行

因此，控制并发的唯一方式是 **控制每条消息中包含的 Task 调用数量**。

## 模型选择

| 阶段               | 模型          | 说明                                              |
| ------------------ | ------------- | ------------------------------------------------- |
| Detection agent    | `sonnet/opus` | 检测阶段需要强推理能力发现真实缺陷                |
| Verification agent | `sonnet`      | 验证阶段同样需要强推理能力，准确判断 finding 真伪 |

**禁止使用 haiku 模型作为 verifier。** Haiku 推理能力不足，容易误判 — 将真实 bug 标记为 false positive（假阴性），或将 false positive 标记为 confirmed（假阳性）。验证阶段的准确性直接决定最终报告质量，必须使用 sonnet 或以上模型。

## 参数

| 参数                     | 默认值 | 说明                                                       |
| ------------------------ | ------ | ---------------------------------------------------------- |
| `MAX_PARALLEL_DETECTORS` | 3      | 检测阶段：单条消息中最多同时启动的 detection agent 数量    |
| `MAX_PARALLEL_VERIFIERS` | 2      | 验证阶段：单条消息中最多同时启动的 verification agent 数量 |
| `RETRY_MAX_ATTEMPTS`     | 2      | 单个 agent 失败后最大重试次数                              |

## 检测阶段调度

检测 agent（2-3 个）**可以在同一条消息中全部并行启动**。

理由：

- 数量少（2-3 个），在典型 API 并发限制内
- 共识机制要求所有 detection pass 都完成后才能进入聚合阶段
- 串行执行会使检测时间翻倍/翻三倍，代价过高

### 检测失败降级

如果某个 detection agent 因 rate limit 或其他原因失败：

1. **重试一次**：在下一条消息中单独重试失败的 agent（不影响已成功的 agent）
2. **重试仍失败时的降级策略**：
   - 3 pass 命令（`/bug-hunt`、`/bug-hunt-file`）：剩余 2 个 pass 仍可进行共识聚合（2/2 = HIGH，1/2 = MEDIUM + critical 保留）
   - 2 pass 命令（`/bug-hunt-diff`）：剩余 1 个 pass 无法进行共识，在报告中添加警告说明共识不可用，所有 findings 直接进入验证阶段
   - 如果 3 pass 命令中 2 个 agent 都失败（仅剩 1 个 pass），中止扫描并报告错误

## 验证阶段调度 — 一个 finding 一个 agent

**严格要求：每个 finding 必须由独立的 verification agent 验证。禁止将多个 findings 合并到同一个 agent 中处理。**

理由：

- 每个 finding 需要 agent 深入阅读上下文、追踪代码路径、检查防御代码，这需要完整的注意力
- 多个 findings 塞进同一个 agent 会导致上下文过载，降低验证准确性
- 独立 agent 确保每个 finding 获得充分、专注的验证

### 并发控制

验证 agent 通过分批方式控制并发，每批最多 `MAX_PARALLEL_VERIFIERS`（默认 2）个 agent，每个 agent 只处理 1 个 finding：

```
给定 N 个待验证 findings：

1. 将 findings 分为若干批次，每批最多 MAX_PARALLEL_VERIFIERS 个
2. 对每个批次：
   a. 为该批次的每个 finding 各启动 1 个 verification agent，在 **同一条消息** 中发出所有 Task 调用（并行）
   b. 等待该批次 **所有 agent 完成**，收集结果
   c. 向用户报告进度
   d. 开始下一批次
3. 所有批次完成后，汇总全部验证结果
```

### 示例（5 个 findings，MAX_PARALLEL_VERIFIERS=2）

```
批次 1: Finding 1 → Agent A, Finding 2 → Agent B  （同一消息 2 个 Task）→ 等待 → 收集
批次 2: Finding 3 → Agent C, Finding 4 → Agent D  （同一消息 2 个 Task）→ 等待 → 收集
批次 3: Finding 5 → Agent E                        （1 个 Task）→ 等待 → 收集
```

## 失败重试策略

如果 verification agent 因 rate limit、超时或其他原因失败：

1. **记录失败**：记下哪个 agent 失败了、对应哪个 finding
2. **先处理成功结果**：同批次中成功的 agent 结果正常收集
3. **延迟重试**：将失败的 finding 安排到 **下一批次** 重试（而非立即重试，避免连续触发限制）
4. **重试上限**：每个 finding 最多重试 `RETRY_MAX_ATTEMPTS`（默认 2）次
5. **最终失败处理**：如果所有重试都失败，将该 finding 标记为 `VERIFICATION_SKIPPED`：
   - finding 仍保留在报告中，显示其共识置信度
   - 添加标注："Verification skipped due to agent failure — manual review recommended"
   - **不得** 静默丢弃该 finding

## 进度报告

每完成一个批次后，向用户报告进度：

```
Verification progress: X/N findings verified
```

全部验证完成后报告汇总：

```
Verification complete: X confirmed, Y likely, Z rejected out of N candidates
```
