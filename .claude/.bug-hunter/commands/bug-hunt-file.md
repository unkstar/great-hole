# /bug-hunt-file — Single File Deep Analysis

Deep defect analysis of a single file with multi-pass detection and verification.

**Target file**: `$ARGUMENTS`

If `$ARGUMENTS` is empty, ask the user to provide a file path and stop.

---

## Scheduling

Read `.bug-hunter/scheduling.md` for complete concurrency rules. Key constraints:

- **Detection agents (Step 2)**: All 3 agents launched in a **single message** (parallel) — within rate limits
- **Verification agents (Step 4)**: Launched in **batches of 2** — do NOT launch all at once
- **On agent failure**: Retry up to 2 times; if still fails, degrade gracefully (see details in each step)

## Step 1: Gather Context

1. Read the target file completely.
2. Identify the language from the file extension.
3. Find and read related files:
   - **Imports/dependencies**: Read files that the target file imports (first-level only)
   - **Importers**: Use Grep to find files that import/require the target file, read them (first-level only)
   - **Test files**: Search for corresponding test files (`*_test.go`, `*.test.ts`, `*.spec.ts`, `test_*.py`, `*Test.java`, etc.)
   - **Type definitions**: If TypeScript/Java, find related type/interface definition files
4. Read `.bug-hunter/rules.md` if it exists. Also read any `.bug-hunter/rules.d/*.md` files.

## Step 2: Multi-Pass Detection

Launch **3 detection agents in parallel** — send all 3 Task tool calls **in a single message** (use `subagent_type: "general-purpose"` and `model: "sonnet"`). 3 concurrent agents is within acceptable API rate limits.

**Detection failure handling**: If any detection agent fails due to a rate limit or timeout error:

1. Retry the failed agent once in a new message (do not re-run successful agents)
2. If retry succeeds, proceed normally with 3-pass consensus
3. If retry fails, proceed with the remaining 2 passes — 2-pass consensus still works (2/2 = HIGH, 1/2 + critical/high = MEDIUM)
4. If 2 agents fail (only 1 pass remaining), **abort the analysis** and report an error to the user — single-pass results cannot provide consensus

Each agent receives:

- The bug-detector agent instructions (read from `.claude/agents/bug-detector.md`)
- The target file content and all gathered context files
- Project rules (if any)
- Instruction to read the appropriate language pattern reference from `.claude/agents/references/`

**Input order shuffling** (critical for reducing LLM position bias):
Before passing context to each agent, shuffle the order of:

1. The **context files** (imports, importers, test files, type definitions) — each pass sees them in a different order
2. The **functions/sections** within the prompt description of what to analyze — present the target file's functions in different order if listing them

The **target file itself** is always provided in full and in original order (since line numbers matter), but the surrounding context that frames the analysis must be shuffled to prevent the model from consistently giving more attention to context that appears first.

```bash
# Shuffle context file order for each pass (replace $PASS_NUMBER with 1, 2, or 3)
echo "$context_files" | bash .bug-hunter/scripts/shuffle.sh lines -s $PASS_NUMBER
```

### Pass 1 — Line-by-Line Logic Analysis

Focus: Trace every function line by line. Check each condition, each variable assignment, each return path. Look for: logic errors, null safety, type errors, boundary conditions, off-by-one errors.

### Pass 2 — Integration Analysis

Focus: Analyze how this file interacts with its callers and dependencies. Check: API contract violations, incorrect assumptions about imported functions, state management across calls, error propagation between modules.

### Pass 3 — Adversarial Edge Case Analysis

Focus: Think like a fuzzer. For each function, consider: What if arguments are null/nil/undefined? What if collections are empty? What if strings are empty or very long? What if numbers are 0, negative, MAX_INT? What if concurrent access occurs? What if the file system/network/database fails?

## Step 3: Consensus Aggregation

Collect all findings from the 3 passes and merge:

1. **Match findings**: Two findings are "the same" if they reference the same file AND overlapping line ranges (±5 lines) AND the same category.

2. **Assign consensus confidence**:
   - Found by **3/3 passes** → confidence: **VERY HIGH**
   - Found by **2/3 passes** → confidence: **HIGH**
   - Found by **1/3 passes** AND severity is `critical` or `high` → confidence: **MEDIUM** (keep)
   - Found by **1/3 passes** AND severity is `medium` or `low` → **DISCARD**

3. When merging duplicate findings, keep the most detailed description and evidence.

## Step 4: Verification

**One finding, one agent.** For each surviving finding, launch a **dedicated verification agent** (use Task tool with `subagent_type: "general-purpose"` and `model: "sonnet"`). **Do NOT use haiku model for verification** — verifiers need strong reasoning to avoid false negatives. **Do NOT combine multiple findings into a single agent** — each agent must focus exclusively on verifying one finding to ensure thorough analysis.

Each verifier receives:

- The bug-verifier agent instructions (read from `.claude/agents/bug-verifier.md`)
- **Only one** specific finding to verify
- The target file and relevant context files
- Instruction to read the code, check reachability, check for guards/defensive code, check tests

**Filter results**:

- `CONFIRMED` or `LIKELY` → **KEEP**
- `UNLIKELY` or `FALSE_POSITIVE` → **REMOVE**

**Concurrency control** (to avoid API rate limits — see `.bug-hunter/scheduling.md`):

Do **NOT** launch all verification agents at once. Follow this procedure:

1. Count total surviving findings: N
2. Divide into batches of **2** (MAX_PARALLEL_VERIFIERS). Each batch contains N agents, each handling exactly 1 finding.
3. For each batch:
   a. Launch the batch's agents **in a single message** (parallel within batch, but each agent still only verifies 1 finding)
   b. **Wait** for all agents in this batch to complete and collect results
   c. Report progress to user: `"Verification progress: X/N findings verified"`
   d. Only then proceed to the next batch
4. After all batches complete, combine all verification results

**Verification failure handling**:

- If a verifier fails, collect successful results from the same batch first
- Add the failed finding to the **next batch** for retry (do not retry immediately)
- Max 2 retry attempts per finding
- If all retries fail, mark as `VERIFICATION_SKIPPED` — keep the finding in the report with a note: "Verification skipped due to agent failure — manual review recommended"

## Step 5: Apply Rules Filters

If project rules exist:

- Apply **Auto-Resolve** rules: remove findings matching auto-resolve patterns
- Apply **Ignore Patterns**: remove findings matching ignore patterns
- Remove pure style/documentation/naming findings (unless rules explicitly request them)

## Step 6: Output Report

Output the final report directly in the conversation (no file needed for single-file analysis).

Format:

````
# Bug Hunt Report: [filename]

**Scanned**: [file path]
**Language**: [detected language]
**Detection passes**: 3
**Findings after consensus**: N
**Findings after verification**: M

---

## Findings

### 1. [Title] — [SEVERITY]
**Category**: [category]
**Location**: [file:line(s)]
**Consensus**: [X/3 passes]
**Verification**: [CONFIRMED|LIKELY] ([confidence]%)

**Description**: [what's wrong]

**Evidence**:
```[lang]
[code snippet with line numbers]
````

**Suggested Fix**:

```[lang]
[fix code or description]
```

---

[repeat for each finding, ordered by severity: critical → high → medium → low]

## Summary

| Severity  | Count |
| --------- | ----- |
| Critical  | X     |
| High      | X     |
| Medium    | X     |
| Low       | X     |
| **Total** | **X** |

```

If no findings survive, output:

```

# Bug Hunt Report: [filename]

**Scanned**: [file path]
**Language**: [detected language]
**Detection passes**: 3

✓ No defects found. The code passed all detection and verification checks.

```

```
