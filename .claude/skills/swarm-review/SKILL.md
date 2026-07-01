---
name: swarm-review
description: >
  Perforce Swarm code review skill for Timi/MHA project. Use when the user provides a Swarm review
  URL (e.g. https://timi-swarm.ces.qq.com/reviews/XXXXXX) or asks to review/analyze/comment on a
  Swarm CR. Supports fetching review metadata, fetching all file diffs, performing AI code review
  (UE5/C++/shader aware), and posting review comments back to Swarm.
  Auth via SWARM_USER/SWARM_TICKET env vars or p4 login ticket.
---

# Swarm Code Review Skill

## Auth

p4 ticket required. Check env vars first:
```bash
echo $SWARM_USER $SWARM_TICKET
```

If missing, ask user to run on their Windows machine:
```
p4 login -a -p    # outputs ticket like XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
```

Set for session:
```bash
export SWARM_USER="<your-rtx-id>"
export SWARM_TICKET="<ticket>"
```

**Ticket expires** (days to weeks). On 401 → ask user to re-run `p4 login -a -p`.

Default Swarm: `https://timi-swarm.ces.qq.com`

**⚠️ Windows 环境注意**：
- Python 命令是 `python`，不是 `python3`
- Windows 终端默认 GBK 编码，脚本输出含 emoji 时会 `UnicodeEncodeError`，所有 python 调用须加 `PYTHONIOENCODING=utf-8`

---

## Workflow

### Step 1: Fetch Review Metadata

```bash
PYTHONIOENCODING=utf-8 python /projects/.openclaw/skills/swarm-review/scripts/swarm_review.py \
  <review_id_or_url> --user $SWARM_USER --ticket $SWARM_TICKET
```

Outputs: author, state, description, file count, existing comments, votes.

### Step 2: Fetch All File Diffs

```bash
PYTHONIOENCODING=utf-8 python /projects/.openclaw/skills/swarm-review/scripts/fetch_diffs.py \
  <review_id> --user $SWARM_USER --ticket $SWARM_TICKET \
  --output /tmp/review_<id>_diffs.txt
```

- Script first fetches review metadata to determine the review structure:
  - **Committed changes** (`versions[].pending=False`): diffed as `rev(cur - N) → cur`
  - **Pending shelved change** (`versions[].pending=True`): diffed as `rev(cur) → @=<change_number>`
  - Both parts are shown per file when a review has both committed and pending-shelved layers
- Output sections: `=== filename.cpp (rev N-k → N + pending @CHANGE) ===` with labeled sub-sections
- For large reviews (>20 files), focus first on .cpp/.h changes; .usf/.ush shaders second

**⚠️ Key insight — review structure types:**
| Type | `versions[].pending` | Diff strategy |
|------|----------------------|---------------|
| Committed only | all False | `rev(cur-N) → cur` |
| Pending shelved only | all True | `rev(cur) → @=pending_change` |
| Both | mixed True/False | committed part + shelved part |

**⚠️ Critical — do NOT use `review.changes[]` to count committed rounds**

`review.changes[]` lists all historically associated changes (including prior commits that may predate this review's versions). It is **not** a reliable count of committed versions. The script uses `versions[]` exclusively:
- `versions[i].pending=False` → committed version
- `versions[i].pending=True` → pending shelved version

Example trap: review #2029430 had `changes=[2027980, 2029431]` (historical) but `versions=[{pending:True, change:2029430}]` — only 1 pending version, no committed diff. Using `len(changes)` would have incorrectly diffed `rev(cur-2) → cur` instead of `shelved@2029430 vs head`.

**⚠️ Critical — always take the LAST pending version, not the first**

When an author updates their shelve (reshelves after addressing comments), `versions[]` accumulates multiple pending entries. The script must use `versions[-1]` (last = latest) for the pending change number.

Example trap: review #2029112 had `versions=[{change:2029113, pending:True}, {change:2029112, pending:True}]`. The author's V1 shelve was `2029113` (included `[HTTP]` config unrelated to the story); V2 shelve was `2029112` (cleaned up). Taking `versions[0]` would fetch stale V1 diff and report a false finding.

**Single file:**
```bash
PYTHONIOENCODING=utf-8 python /projects/.openclaw/skills/swarm-review/scripts/fetch_diffs.py \
  <review_id> --user $SWARM_USER --ticket $SWARM_TICKET \
  --file DaySequenceModifierComponent.cpp
```

### Step 2.5: Local Codebase (Optional)

When reviewing diffs, some questions can't be answered from the diff alone — e.g. whether a variable is assigned elsewhere, whether a deleted branch was the only place handling an edge case, or whether callers of a modified function are affected. A local codebase checkout lets you look these up on demand.

**Ask the user once per session** (if not already known):

> 本地有代码库 checkout 吗？如果有，请提供路径（如 `D:\P4\MHA\engine-ue5`），遇到需要确认的问题时可以按需查阅。

**If local path is provided**, use it **on demand during Step 3** when you need to verify a specific question — for example:
- Grep for a function name to check if callers are affected by a signature change
- Read a specific section of a file to confirm a variable's assignment path
- Look up an enum/type definition referenced in the diff
- Check if a deleted condition is guarded elsewhere

Do **not** read files wholesale — query only what's needed to confirm or dismiss a specific concern.

**If no local path**, proceed with diff-only review. Where a conclusion depends on context not visible in the diff, note it explicitly rather than speculating.

---

### Step 3: AI Code Review

Read the diffs and perform review covering:
- **Correctness**: logic errors, null pointer, array bounds, divide-by-zero
- **Interface/Implementation sync**: header changes must be reflected in .cpp
- **Regressions**: removed conditions, disabled code blocks (commented-out)
- **UE5 standards**: `int32` not `int`, UPROPERTY meta, naming conventions
- **Shader-specific** (.usf/.ush): precision, branching, hardcoded magic numbers

See **references/ue5-review-guide.md** for rating criteria and DaySequence-specific knowledge.

Format output as:
```
## 🤖 AI Code Review — #<id> (1/N)

### 一、修改目的与背景
...

### 二、逐文件分析
⚠️ **filename.cpp** ...

### 三、风险汇总
| 优先级 | 问题 | 建议 |
...

### 四、总体评价
结论：[Approve / Conditional Approve / Request Changes]
```

### Step 4: Post Comments to Swarm

> ⚠️ **重要工作流约束（必须遵守）**
> 1. 完成 AI review 报告后，**先发给用户（WeCom/chat）审核**
> 2. **等待用户明确同意后**，才能将评论发到 Swarm
> 3. 禁止自动/立即发布 Swarm 评论，无论报告结论如何

Post review-level comment (works for all depots):
```bash
PYTHONIOENCODING=utf-8 python /projects/.openclaw/skills/swarm-review/scripts/post_comment.py \
  <review_id> "comment text" --user $SWARM_USER --ticket $SWARM_TICKET
```

Or from file:
```bash
PYTHONIOENCODING=utf-8 python /projects/.openclaw/skills/swarm-review/scripts/post_comment.py \
  <review_id> --file /tmp/comment.txt --user $SWARM_USER --ticket $SWARM_TICKET
```

**For long reports**, split into 2 parts (1/2, 2/2) to avoid truncation.

**Inline (line-level) comments**: Two methods available:

**Method A — Browser session (recommended for `//MHA-Protected/` depot)**

The Basic Auth ticket cannot post inline comments to `//MHA-Protected/` because Swarm's server runs `p4 print` using the ticket, which lacks read permission. Use the browser session instead:

1. Ask user to open Swarm in browser, go to review page, open DevTools → Network tab
2. Post any comment in the browser (type something and submit)
3. Right-click the `POST /comments/add` request → **Copy → Copy as cURL (bash)**
4. Paste the cURL to you — it contains `SWARM=...` cookie and `x-csrf-token`

Then post inline comment using the session:
```bash
curl -s 'https://timi-swarm.ces.qq.com/comments/add' \
  -H 'content-type: application/x-www-form-urlencoded; charset=UTF-8' \
  -b 'SWARM=<session_cookie>; review_ui=classic' \
  -H 'origin: https://timi-swarm.ces.qq.com' \
  -H 'referer: https://timi-swarm.ces.qq.com/reviews/<review_id>' \
  -H 'x-csrf-token: <csrf_token>' \
  -H 'x-requested-with: XMLHttpRequest' \
  --data-raw 'topic=reviews%2F<review_id>&user=<username>&context=<url_encoded_json>&body=<comment>&delayNotification=true&_csrf=<csrf_token>'
```

The `context` field is a URL-encoded JSON object:
```json
{
  "file": "//MHA-Protected/engine-ue5/Engine/Shaders/Private/SomeFile.usf",
  "leftLine": null,
  "rightLine": 27,
  "content": ["+line content here"],
  "type": "text",
  "review": 2024593,
  "version": 1
}
```
Key fields: `rightLine` for added lines (shelved), `leftLine` for deleted lines; `content` array = surrounding lines for context display.

**Method B — Basic Auth (only for depots where ticket has read permission)**

Use the `/api/v9/comments` endpoint with the same `context` field. Will fail with `"You don't have permission"` for `//MHA-Protected/`.

**Session cookie expiry**: The `SWARM=...` cookie expires when the browser closes or after a period of inactivity. Expiry symptoms: API returns `302` redirect to login page, or returns error HTML instead of JSON. Fix: ask user to post another comment in the browser and provide a fresh **Copy as cURL**.

**Python helper for inline comments** (use after extracting cookie/token from user's cURL):
```python
# Usage: call post_inline_comment() for each line-level comment
import requests, json

def post_inline_comment(
    swarm_url, review_id, swarm_cookie, csrf_token, username,
    file_path, right_line, content_lines, body, version=1
):
    """
    swarm_url: 'https://timi-swarm.ces.qq.com'
    file_path: '//MHA-Protected/engine-ue5/Engine/Shaders/Private/SomeFile.usf'
    right_line: int (line number of added line in new file; use leftLine for deletions)
    content_lines: list of strings, e.g. ["+    float SurfaceThickness = ..."]
    body: comment text
    """
    context = {
        "file": file_path,
        "leftLine": None,
        "rightLine": right_line,
        "content": content_lines,
        "type": "text",
        "review": review_id,
        "version": version
    }
    resp = requests.post(
        f"{swarm_url}/comments/add",
        data={
            "topic": f"reviews/{review_id}",
            "user": username,
            "context": json.dumps(context),
            "body": body,
            "delayNotification": "true",
            "_csrf": csrf_token
        },
        headers={
            "Content-Type": "application/x-www-form-urlencoded; charset=UTF-8",
            "Cookie": f"SWARM={swarm_cookie}; review_ui=classic",
            "Origin": swarm_url,
            "Referer": f"{swarm_url}/reviews/{review_id}",
            "X-Csrf-Token": csrf_token,
            "X-Requested-With": "XMLHttpRequest"
        }
    )
    # Success: 200 with HTML snippet containing comment id
    # Failure: 302 (session expired) or error HTML
    return resp.status_code, resp.text[:300]
```

**Workaround if no session available**: Reference `filename.cpp:L409` in the review-level comment body, or ask user to post inline comments manually in the browser.

---

## API Notes

See **references/api.md** for full endpoint reference, auth details, and the inline comment limitation.

Key gotcha: **POST comment must use `/api/v9/comments` with `application/x-www-form-urlencoded`** — v10 POST returns 404, JSON body returns 400.

---

## Bug Hunter Integration

Bug Hunter (`https://git.woa.com/jinlong/bug-hunter`) is a multi-agent bug detection framework built for Claude Code. It uses parallel detection passes + consensus filtering + adversarial verification to find real bugs. Bug Hunter assets are bundled at `bug-hunter/.bug-hunter/` within this skill directory.

### How Bug Hunter Works

```
Swarm diff text
    │
    ├── Detection Agent 1 (parallel) ──┐
    └── Detection Agent 2 (parallel) ──┘
                │
        Consensus Aggregation (≥2/2 = HIGH, 1/2 = MEDIUM)
                │
        Verification Agents (batches of 2, one finding per agent)
                │
        Rules Filter → Report
```

### Using Bug Hunter on a Swarm Review

Bug Hunter's `/bug-hunt-diff` command normally operates on `git diff`. For Swarm reviews, we **pipe the Swarm diff text directly** to a claude-internal agent running Bug Hunter as the orchestrator.

#### Step 1: Prepare the diff as a unified diff file

```bash
# Fetch Swarm diff to a file (already done by fetch_diffs.py)
PYTHONIOENCODING=utf-8 python /projects/.openclaw/skills/swarm-review/scripts/fetch_diffs.py \
  <review_id> --user $SWARM_USER --ticket $SWARM_TICKET \
  --output /tmp/review_<id>_diffs.txt
```

The diff file uses `+`/`-` prefixed lines — compatible with Bug Hunter's diff parser.

#### Step 2: Initialize Bug Hunter in a temp workspace

```bash
WORK_DIR=$(mktemp -d)
cp -r /projects/.openclaw/skills/swarm-review/bug-hunter/.bug-hunter $WORK_DIR/
# Copy diff file as the "staged" input
cp /tmp/review_<id>_diffs.txt $WORK_DIR/swarm_diff.txt
```

#### Step 3: Run claude-internal with the bug-hunt-diff command

⚠️ **Must use `pty=true` and sufficient `yieldMs`** (see Claude内网版 skill for details)

```bash
# Read the bug-hunt-diff command template
COMMAND=$(cat /projects/.openclaw/skills/swarm-review/bug-hunter/.bug-hunter/commands/bug-hunt-diff.md)

# Run claude-internal as orchestrator
# Pass the Swarm diff as context instead of running git diff
claude-internal -p --token "$GF_TOKEN" \
  --cwd "$WORK_DIR" \
  "You are performing a bug-hunt-diff review on a Swarm code review diff.
   
   The diff is provided in /tmp/review_<id>_diffs.txt (not from git).
   Treat all lines starting with + as additions, - as deletions, in the review.
   
   Follow these instructions exactly:
   $COMMAND
   
   Use the diff file at swarm_diff.txt as your diff source (skip the git diff step).
   Language: C++/HLSL (UE5 shader/engine code).
   Read the C/C++ pattern reference at .bug-hunter/agents/references/c-cpp-patterns.md"
```

#### Step 4: Collect report

Bug Hunter writes reports to `.bug-hunter/reports/diff-{timestamp}.md` in the working directory.

```bash
cat $WORK_DIR/.bug-hunter/reports/diff-*.md
```

### Workflow Integration

Add Bug Hunter as an **optional enhanced pass** after Step 3 (AI Code Review):

1. Run standard AI review (Step 3) — fast, inline with current session
2. Optionally run Bug Hunter for deeper multi-agent analysis — especially useful for:
   - Large diffs (>20 files)
   - Security-sensitive changes
   - Complex concurrency/shader logic
3. Merge findings from both passes into the final report
4. Follow normal Step 4 flow: send report to user → wait for approval → post to Swarm

### Bug Hunter Assets Location

```
/projects/.openclaw/skills/swarm-review/bug-hunter/
└── .bug-hunter/
    ├── agents/
    │   ├── bug-detector.md       # Detection agent instructions
    │   ├── bug-verifier.md       # Verification agent instructions
    │   └── references/           # Language-specific bug patterns
    │       ├── c-cpp-patterns.md
    │       ├── go-patterns.md
    │       ├── python-patterns.md
    │       └── ...
    ├── commands/
    │   ├── bug-hunt-diff.md      # Diff review orchestration
    │   ├── bug-hunt-file.md      # Single file analysis
    │   ├── bug-hunt.md           # Full repo scan
    │   └── bug-hunt-rules.md     # Rules management
    ├── rules.md                  # Default detection rules (customize per project)
    ├── scheduling.md             # Concurrency rules for sub-agents
    └── scripts/
        └── shuffle.sh            # Input order randomization for bias reduction
```

**Updating Bug Hunter assets:**
```bash
cd /tmp && git clone "https://oauth2:$GF_TOKEN@git.woa.com/jinlong/bug-hunter.git" --depth=1
cp -r /tmp/bug-hunter/.bug-hunter /projects/.openclaw/skills/swarm-review/bug-hunter/
```

---

## Scripts Reference

| Script | Purpose |
|--------|---------|
| `scripts/swarm_review.py` | Fetch review metadata, file list, comments; post comment (v10, JSON) |
| `scripts/fetch_diffs.py` | Fetch all file diffs via Swarm HTML diff endpoint, output line-numbered text |
| `scripts/post_comment.py` | Post review-level comment via v9 form-encoded POST (the working method) |
| `bug-hunter/.bug-hunter/` | Bug Hunter multi-agent detection assets (commands, agents, patterns) |
