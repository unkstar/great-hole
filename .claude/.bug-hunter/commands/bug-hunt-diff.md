# /bug-hunt-diff — Diff Review

Review git diff for defects introduced by code changes. Uses multi-pass detection with consensus filtering and verification.

**Arguments**: `$ARGUMENTS`

Supported formats:

- (empty) or `--staged` → review staged changes (`git diff --cached`)
- `--pr=NUMBER` → review a pull request (`gh pr diff NUMBER`)
- `<git-ref>` → review changes from ref to HEAD (`git diff <ref>..HEAD`)
- `<ref1>..<ref2>` → review changes between two refs (`git diff <ref1>..<ref2>`)

---

## Scheduling

Read `.bug-hunter/scheduling.md` for complete concurrency rules. Key constraints:

- **Detection agents (Step 3)**: Both agents launched in a **single message** (parallel) — within rate limits
- **Verification agents (Step 5)**: Launched in **batches of 2** — do NOT launch all at once
- **On agent failure**: Retry up to 2 times; if still fails, degrade gracefully (see details in each step)

## Step 1: Parse Arguments and Get Diff

Determine the diff source from `$ARGUMENTS`:

1. **No arguments or `--staged`**:
   - Run: `git diff --cached`
   - If empty, try `git diff` (unstaged changes)
   - If still empty, tell user "No changes to review" and stop

2. **`--pr=NUMBER`** (extract NUMBER):
   - Run: `gh pr diff NUMBER`
   - If fails, report error and stop

3. **Contains `..`** (e.g., `main..feature`):
   - Run: `git diff $ARGUMENTS`

4. **Single ref** (e.g., `main`, `HEAD~3`, commit hash):
   - Run: `git diff $ARGUMENTS..HEAD`

Capture the diff output. If the diff is empty, tell the user and stop.

Parse the diff to identify:

- List of changed files (added, modified, deleted)
- For each file: the specific changed line ranges

## Step 2: Gather Context

1. **Read rules**: Load `.bug-hunter/rules.md` and `.bug-hunter/rules.d/*.md` if they exist.

2. **Local codebase**: If the user has provided a local codebase path (e.g. `D:\P4\MHA\engine-ue5`), it is available for **on-demand lookup** during analysis — use Grep or targeted reads to verify specific concerns (caller impact, type definitions, variable assignment paths, etc.) rather than reading files wholesale. If not known, ask once:
   > 本地有代码库 checkout 吗？提供路径后遇到需要确认的问题可以按需查阅。

3. **For each changed file** (skip deleted files):
   - Read the complete current version of the file
   - Identify the language from file extension

4. **Gather dependency context** (for each changed file):
   - Read files that the changed file imports (first-level)
   - Use Grep to find files that import the changed file (first-level)
   - Read related type definitions if applicable

5. **Identify languages and frameworks** from the changed files.

## Step 3: Multi-Pass Detection

Launch **2 detection agents in parallel** — send both Task tool calls **in a single message** (use `subagent_type: "general-purpose"` and `model: "sonnet"`). 2 concurrent agents is within acceptable API rate limits.

**Detection failure handling**: If either detection agent fails due to a rate limit or timeout error:

1. Retry the failed agent once in a new message (do not re-run the successful agent)
2. If retry succeeds, proceed normally with 2-pass consensus
3. If retry fails, proceed with single-pass results but add a **warning** to the report: "Consensus not available — only 1 detection pass completed. Results may contain more false positives."

Each agent receives:

- The bug-detector agent instructions (from `.claude/agents/bug-detector.md`)
- The full diff text — **with diff hunks and changed files in different random order per pass** (see below)
- The complete current version of each changed file
- Dependency context files
- Project rules text
- Instruction to read the appropriate language pattern references
- **CRITICAL instruction**: "Only report bugs INTRODUCED or EXPOSED by the diff changes. Do NOT report pre-existing issues in unchanged code. Focus on the changed lines and their immediate effects."

**Input order shuffling** (critical for reducing LLM position bias):
Before passing the diff to each agent, shuffle:

1. The order of **per-file blocks** in the diff (file-level blocks are shuffled; hunks within each file stay in original order since hunk headers contain sequential line numbers)
2. The order of **context files** (imports, importers)

Each pass MUST receive a different random ordering. This prevents the model from consistently giving more attention to files that appear first in the input, which is a known LLM bias.

```bash
# Shuffle diff per-file block order (replace $PASS_NUMBER with 1 or 2)
echo "$diff_text" | bash .bug-hunter/scripts/shuffle.sh diff-files -s $PASS_NUMBER
# Shuffle context file order
echo "$context_files" | bash .bug-hunter/scripts/shuffle.sh lines -s $((PASS_NUMBER + 100))
```

### Pass 1 — Change Correctness

**Focus**: Are the changes logically correct? Check: new conditions, new variable assignments, new return paths, null safety of new code, type correctness, boundary conditions in new code, error handling of new code paths.

### Pass 2 — Integration Impact

**Focus**: Do the changes break existing contracts or assumptions? Check: API contract violations, changed function signatures vs callers, state management changes, security implications of changes, new resource usage without cleanup, new concurrency concerns.

## Step 4: Consensus Aggregation

Collect findings from both passes and merge:

1. **Match findings**: Two findings are "the same" if they reference the same file AND overlapping line ranges (±5 lines) AND the same category.

2. **Assign consensus confidence** (diff mode is stricter since only 2 passes):
   - Found by **2/2 passes** → confidence: **HIGH**
   - Found by **1/2 passes** → confidence: **MEDIUM** (keep — diff scope is small, single-pass findings are still valuable)

3. When merging duplicates, keep the most detailed description.

## Step 5: Verification

**One finding, one agent.** For each surviving finding, launch a **dedicated verification agent** (use Task tool with `subagent_type: "general-purpose"` and `model: "sonnet"`). **Do NOT use haiku model for verification** — verifiers need strong reasoning to avoid false negatives. **Do NOT combine multiple findings into a single agent** — each agent must focus exclusively on verifying one finding to ensure thorough analysis.

Each verifier receives:

- The bug-verifier agent instructions (from `.claude/agents/bug-verifier.md`)
- **Only one** specific finding to verify
- The source file and context
- **Extra instruction**: "This finding is from a diff review. Verify that the bug is actually introduced by recent changes, not a pre-existing issue. If the same pattern exists in unchanged code, it's likely pre-existing and should be marked FALSE_POSITIVE."

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

## Step 6: Apply Rules Filters

- Apply **Auto-Resolve** rules
- Apply **Ignore Patterns**
- Remove pure style/documentation/naming findings

## Step 7: Generate Report

Create report file: `.bug-hunter/reports/diff-{YYYY-MM-DD-HHmmss}.md`

### Report Format:

````markdown
# Bug Hunt Report — Diff Review

**Date**: [timestamp]
**Diff source**: [staged / PR #N / ref1..ref2]
**Changed files**: [count]
**Languages**: [detected languages]
**Detection passes**: 2

## Changes Reviewed

| File             | Status   | Lines Changed |
| ---------------- | -------- | ------------- |
| path/to/file.ext | modified | +X / -Y       |
| ...              | added    | +X            |

## Summary

| Severity  | Count |
| --------- | ----- |
| Critical  | X     |
| High      | X     |
| Medium    | X     |
| Low       | X     |
| **Total** | **X** |

## Findings

### 1. [Title] — [SEVERITY]

**Category**: [category]
**Location**: [file:line(s)]
**In diff**: Yes (lines X-Y of the diff)
**Consensus**: [X/2 passes]
**Verification**: [CONFIRMED|LIKELY] ([confidence]%)

**Description**: [what's wrong]

**Evidence**:

```[lang]
[code snippet showing the problematic change, with + prefix for added lines]
```
````

**Suggested Fix**:

```[lang]
[fix code]
```

---

[repeat for each finding]

```

### Also output to conversation:

```

Bug Hunt Complete — Diff Review ([source])

Reviewed X changed files.
Found Y defects (C critical, H high, M medium, L low)

Findings:

1. [CRITICAL] file:line — title
2. [HIGH] file:line — title
   ...

Full report: .bug-hunter/reports/diff-{timestamp}.md

```

If no findings:

```

Bug Hunt Complete — Diff Review ([source])

✓ No defects found in X changed files.
All changes passed detection and verification checks.

```

```
