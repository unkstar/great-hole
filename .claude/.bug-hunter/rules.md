# Bug Hunter Rules

## General

- Hardcoded passwords, API keys, tokens, or secrets → severity: critical
- `eval()` or equivalent dynamic code execution with external input → severity: high
- SQL queries built with string concatenation/interpolation → severity: high
- Deserialization of untrusted data → severity: high

## Language-Specific

### Python
- No bare `except:` clauses
- No `pickle.loads` on user-provided data
- All file handles must be closed (use `with` statement)
- No `subprocess` calls with `shell=True` and user-controlled input

### JavaScript/Node.js
- All promises must be awaited or explicitly voided
- No `require()` with user-controlled paths
- No `child_process.exec` with user-controlled input

## Framework-Specific

<!-- No specific frameworks detected beyond standard Python/Node.js -->

## Auto-Resolve

- Findings in `*.test.*` / `*.spec.*` / `__tests__/**` / `*_test.go` with severity < high → auto-resolve
- Findings in `vendor/` / `third_party/` / `generated/` / `node_modules/` / `dist/` → auto-resolve
- Findings in `**/migrations/**` → auto-resolve (except security category)
- Findings in `**/*.pb.go` / `**/*_generated.*` → auto-resolve

## Ignore Patterns

- `console.log` / `print` / `fmt.Println` in test files → do not flag
- SQL in migration files → do not flag as injection
- Code marked with `//nolint` / `# noqa` / `@SuppressWarnings` / `// eslint-disable` → do not flag
- `TODO` / `FIXME` / `HACK` comments → do not flag (unless describing an active bug in the code)
