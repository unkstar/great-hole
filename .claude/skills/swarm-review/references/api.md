# Swarm API Reference (实战总结)

## 认证

- 方式：HTTP Basic Auth，用户名 + p4 ticket
- ticket 通过 `p4 login -a -p` 获取（Windows 上运行）
- ticket 会过期（通常几天到几周），401 时重新获取

```python
import base64
AUTH = base64.b64encode(f"{user}:{ticket}".encode()).decode()
headers = {"Authorization": f"Basic {AUTH}"}
```

## 版本差异

| 操作 | 可用版本 | 备注 |
|------|----------|------|
| GET review info | v9, v10 | v10 返回格式在 `data.reviews[0]` |
| GET file list | v10 | `/api/v10/reviews/<id>/files` → `data.files` |
| GET comments | v9, v10 | v10: `data.comments`; v9: `comments` |
| POST comment | **v9 only** | v10 POST /comments 返回 404 |
| GET diff HTML | 无版本 | `/diff?left=...&right=...` |

## 关键端点

### GET review info
```
GET /api/v10/reviews/<id>
→ data.reviews[0]: {id, author, state, description, versions, changes, participantsData}
```

### GET file list
```
GET /api/v10/reviews/<id>/files
→ data.files[]: {depotFile, rev, action, type, fileSize, digest}
```

### GET comments
```
GET /api/v9/comments?topic=reviews/<id>&max=100
→ comments[]: {id, user, body, context, time, taskState}
```

context 字段（inline comment）:
```json
{
  "file": "//depot/path/file.cpp",
  "leftLine": null,
  "rightLine": 42,
  "content": [" ctx line", "-old line", "+new line"],
  "review": 2024428,
  "version": "2",
  "type": "text"
}
```

### POST comment（v9 form-encoded）
```
POST /api/v9/comments
Content-Type: application/x-www-form-urlencoded

topic=reviews/<id>&body=<text>
→ comment.id
```

### GET diff HTML
```
GET /diff?left=<depot_file>#<prev_rev>&right=<depot_file>#<cur_rev>&ignoreWs=0&type=file&reviewId=<id>&action=edit
→ HTML page containing <tr class="diff ..."> rows
```

HTML diff 行解析：
```python
import re, html as html_mod

for row in re.findall(r'<tr[^>]*class="diff[^"]*"[^>]*>(.*?)</tr>', html, re.DOTALL):
    left_n  = re.search(r'line-num-left"[^>]*data-num="(\d*)"', row)
    right_n = re.search(r'line-num-right"[^>]*data-num="(\d*)"', row)
    val     = re.search(r'class="line-value">(.*?)</td>', row, re.DOTALL)
    if val:
        text = re.sub(r'<[^>]+>', '', val.group(1))
        text = html_mod.unescape(text)
        # text 以 + 开头 → add, - 开头 → delete, 否则 → same
```

## Inline Comment 限制

**受保护 depot（如 `//MHA-Protected/`）下无法通过 API 发 inline comment**。

Swarm 在接受 inline comment 时会调用 `p4 print` 验证文件内容，而 API token 用户通常没有 MHA-Protected depot 的直接 p4 读权限。错误信息：
```json
{"error": "Provided context could not be filtered.",
 "details": {"context": "Command failed: You don't have permission for this operation."}}
```

**绕过方案**：在 review 级别评论的正文里注明文件和行号，例如：
```
`DaySequenceModifierComponent.cpp:409` 🟠 [High] xxx
```

浏览器登录态（session cookie）可以发 inline comment，但 Basic Auth 不行。

## review.versions 字段

获取 review 的多个版本（change）信息：
```json
versions: [
  {"change": 2024429, "user": "hughhu", "time": ..., "difference": 1, "pending": true},
  {"change": 2024717, "user": "hughhu", "time": ..., "difference": 2, "pending": false}
]
```

`difference` 是版本序号（1-indexed）。最终 revision = `file.rev`，基础 revision = `file.rev - 1`（单 change review），多 change review 可能需要跨多个版本。

## 完整 review 流程的 revision 计算

```python
# 从 file list 获取 cur_rev
cur_rev = int(file["rev"])
# 通常 prev_rev = cur_rev - 1 即可覆盖所有 review changes
prev_rev = cur_rev - 1
# 若需要跨全部版本，找最早的 change 对应的 rev
# 通过搜索不同 prev_rev 直到 diff 包含所有改动
```
