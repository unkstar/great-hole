# /bug-hunt — Full Repository Scan

Scan the entire repository (or a specified directory) for code defects using multi-pass detection with consensus filtering and verification.

**Target**: `$ARGUMENTS` (optional directory path; defaults to entire repository)

---

## Scheduling

Read `.bug-hunter/scheduling.md` for complete concurrency rules. Key constraints:

- **Detection agents (Step 3)**: All 3 agents launched in a **single message** (parallel) — within rate limits
- **Verification agents (Step 5)**: Launched in **batches of 2** — do NOT launch all at once
- **On agent failure**: Retry up to 2 times; if still fails, degrade gracefully (see details in each step)

## Step 1: Load Rules

1. Check if `.bug-hunter/rules.md` exists. If yes, read it.
2. Check if `.bug-hunter/rules.d/` contains any `*.md` files. If yes, read them all.
3. Combine all rules into a single rules context for passing to agents.

## Step 2: Collect Context and Identify Languages

1. Use Glob to collect all source code files in the target directory:
   - `**/*.go`, `**/*.py`, `**/*.ts`, `**/*.tsx`, `**/*.js`, `**/*.jsx`, `**/*.java`, `**/*.c`, `**/*.cpp`, `**/*.h`, `**/*.hpp`, `**/*.rs`, `**/*.rb`, `**/*.php`, `**/*.swift`, `**/*.kt`, `**/*.scala`, `**/*.cs`

2. **Exclude** files matching Auto-Resolve ignore patterns from rules:
   - `vendor/**`, `third_party/**`, `node_modules/**`, `dist/**`, `build/**`, `generated/**`
   - `**/*.pb.go`, `**/*_generated.*`, `**/*.min.js`, `**/*.min.css`

3. Identify the primary languages by file count and total lines.

4. Identify frameworks by checking configuration files:
   - `go.mod` → Go modules
   - `package.json` → Node.js (check for React, Vue, Angular, Express, etc.)
   - `requirements.txt` / `pyproject.toml` → Python (check for Django, Flask, FastAPI, etc.)
   - `pom.xml` / `build.gradle` → Java (check for Spring, etc.)
   - `CMakeLists.txt` / `Makefile` → C/C++

5. Report to the user what was found: X files across Y languages, Z frameworks detected.

## Step 3: Multi-Pass Detection

Launch **3 detection agents in parallel** — send all 3 Task tool calls **in a single message** (use `subagent_type: "general-purpose"` and `model: "sonnet"`). 3 concurrent agents is within acceptable API rate limits.

**Detection failure handling**: If any detection agent fails due to a rate limit or timeout error:

1. Retry the failed agent once in a new message (do not re-run successful agents)
2. If retry succeeds, proceed normally with 3-pass consensus
3. If retry fails, proceed with the remaining 2 passes — 2-pass consensus still works (2/2 = HIGH, 1/2 + critical = MEDIUM)
4. If 2 agents fail (only 1 pass remaining), **abort the scan** and report an error to the user — single-pass results cannot provide consensus

Each agent receives:

- The bug-detector agent instructions (from `.claude/agents/bug-detector.md`)
- The list of files to analyze (same files, different order per pass)
- Project rules text
- Instruction to read the appropriate language pattern references from `.claude/agents/references/`
- Instruction to read each file using the Read tool before analyzing

**Input order shuffling** (critical for reducing LLM position bias):
Before passing the file list to each agent, shuffle the file order using the shuffle script so each pass sees files in a completely different sequence. This ensures the model doesn't consistently give more attention to files that happen to appear first.

```bash
# Generate shuffled file list for each pass (replace $PASS_NUMBER with 1, 2, or 3)
cat "$file_list" | bash .bug-hunter/scripts/shuffle.sh lines -s $PASS_NUMBER
```

### Pass 1 — Logic and Safety

**File order**: Shuffled with `bash .bug-hunter/scripts/shuffle.sh lines -s 1`.
**Focus**: Logic errors, null/nil safety, type errors, boundary conditions. For each file, trace function logic, check conditions, verify return values.

### Pass 2 — Concurrency and Resources

**File order**: Shuffled with `bash .bug-hunter/scripts/shuffle.sh lines -s 2`.
**Focus**: Concurrency issues (race conditions, deadlocks, goroutine/thread leaks), resource management (unclosed handles, leaked connections), error handling defects (swallowed errors, incorrect propagation).

### Pass 3 — Security

**File order**: Shuffled with `bash .bug-hunter/scripts/shuffle.sh lines -s 3`.
**Focus**: Security vulnerabilities — injection (SQL, command, XSS), authentication/authorization bypass, path traversal, insecure deserialization, hardcoded secrets, data exposure, SSRF. Also check for dangerous API usage.

**Important for agents**: Files may be too many to read all at once. Agents should:

- Prioritize files likely to contain bugs (entry points, handlers, middleware, data access layers)
- Skip obviously safe files (constants, pure type definitions, generated code)
- For large repos, focus on the most critical paths

## Step 4: Consensus Aggregation

Collect all findings from the 3 passes and merge:

1. **Match findings**: Two findings are "the same" if they reference the same file AND overlapping line ranges (±5 lines) AND the same or closely related category.

2. **Assign consensus confidence**:
   - Found by **≥2/3 passes** → confidence: **HIGH** (keep)
   - Found by **1/3 passes** AND severity is `critical` → confidence: **MEDIUM** (keep)
   - Found by **1/3 passes** AND severity is not `critical` → **DISCARD**

3. When merging duplicate findings, keep the most detailed description and evidence.

4. Report to user: "Consensus phase: X unique findings from Y total across 3 passes"

## Step 5: Verification

**One finding, one agent.** For each surviving finding, launch a **dedicated verification agent** (use Task tool with `subagent_type: "general-purpose"` and `model: "sonnet"`). **Do NOT use haiku model for verification** — verifiers need strong reasoning to avoid false negatives. **Do NOT combine multiple findings into a single agent** — each agent must focus exclusively on verifying one finding to ensure thorough analysis.

Each verifier receives:

- The bug-verifier agent instructions (from `.claude/agents/bug-verifier.md`)
- **Only one** specific finding to verify
- Instruction to read the actual source file and surrounding context
- Instruction to check reachability, defensive code, type system, tests, intentional patterns

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

Report to user: "Verification phase: X findings confirmed out of Y candidates"

## Step 6: Classification Filter

Apply final filters:

- Remove pure style/documentation/naming findings (unless rules explicitly request them)
- Apply **Auto-Resolve** rules from project rules
- Apply **Ignore Patterns** from project rules
- De-duplicate any remaining duplicates

## Step 7: Generate Report

Create the report directory if needed: `.bug-hunter/reports/`

Generate a timestamped report file: `.bug-hunter/reports/scan-{YYYY-MM-DD-HHmmss}.md`

### Report Format:

````markdown
# Bug Hunt Report — Full Scan

**Date**: [timestamp]
**Target**: [directory or "full repository"]
**Languages**: [detected languages]
**Files scanned**: [count]
**Detection passes**: 3

## Summary

| Severity  | Count |
| --------- | ----- |
| Critical  | X     |
| High      | X     |
| Medium    | X     |
| Low       | X     |
| **Total** | **X** |

| Category       | Count |
| -------------- | ----- |
| security       | X     |
| logic-error    | X     |
| null-safety    | X     |
| concurrency    | X     |
| resource-leak  | X     |
| error-handling | X     |
| type-error     | X     |
| boundary       | X     |
| api-misuse     | X     |

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
```
````

**Suggested Fix**:

```[lang]
[fix code or description]
```

---

[repeat for each finding, ordered by severity then by file path]

```

### Also output to conversation:

Print a summary table to the user and the path to the full report file:

```

Bug Hunt Complete — Full Scan

Found X defects (C critical, H high, M medium, L low)

Top findings:

1. [CRITICAL] file:line — title
2. [HIGH] file:line — title
   ...

Full report: .bug-hunter/reports/scan-{timestamp}.md

```

If no findings survive, report:

```

Bug Hunt Complete — Full Scan

✓ No defects found across X files in Y languages.
All detection passes and verification checks passed.

```

```
