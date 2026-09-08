# /bug-hunt-rules — Rule Management

Manage Bug Hunter detection rules for this project.

**Usage**: `/bug-hunt-rules $ARGUMENTS`

**Arguments**: `init` | `show` | `suggest` | `add <rule>`

---

## Execute the requested operation based on `$ARGUMENTS`:

### If `$ARGUMENTS` is `init`:

1. Analyze the project to identify languages and frameworks:
   - Use Glob to find source files: `**/*.go`, `**/*.py`, `**/*.ts`, `**/*.tsx`, `**/*.js`, `**/*.jsx`, `**/*.java`, `**/*.c`, `**/*.cpp`, `**/*.h`
   - Look for configuration files: `go.mod`, `package.json`, `requirements.txt`, `Pipfile`, `pyproject.toml`, `pom.xml`, `build.gradle`, `CMakeLists.txt`, `Makefile`, `Cargo.toml`
   - Identify frameworks from imports/dependencies

2. Generate `.bug-hunter/rules.md` with:
   - The General section (keep defaults)
   - Uncomment and customize Language-Specific sections for detected languages
   - Uncomment and customize Framework-Specific sections for detected frameworks
   - Keep Auto-Resolve and Ignore Patterns sections

3. Report what was detected and configured.

### If `$ARGUMENTS` is `show`:

1. Read and display `.bug-hunter/rules.md`
2. Check for files in `.bug-hunter/rules.d/`:
   - If any `*.md` files exist, read and display each one
3. Summarize the active rules count by section

### If `$ARGUMENTS` is `suggest`:

1. Analyze the project codebase:
   - Read linter configurations (`.eslintrc*`, `.golangci.yml`, `pylintrc`, `.flake8`, `checkstyle.xml`, etc.)
   - Read CI configurations (`.github/workflows/*.yml`, `.gitlab-ci.yml`, `Jenkinsfile`, etc.)
   - Read existing code patterns and common issues

2. Suggest rules that would complement existing tooling:
   - Rules that linters don't cover (semantic bugs, logic errors)
   - Rules specific to the project's domain
   - Rules based on patterns seen in the codebase

3. For each suggestion, explain WHY it would be valuable for this project.

4. Ask the user which suggestions to add, then update `rules.md`.

### If `$ARGUMENTS` starts with `add`:

1. Parse the rule text after `add`
2. Determine which section the rule belongs to (General, Language-Specific, Framework-Specific, Auto-Resolve, or Ignore Patterns)
3. Append the rule to the appropriate section in `.bug-hunter/rules.md`
4. Confirm the addition and show the updated section

### If `$ARGUMENTS` is empty or unrecognized:

Display usage help:

```
Bug Hunter Rules Management

Usage:
  /bug-hunt-rules init              Analyze project and initialize rules
  /bug-hunt-rules show              Display current rules
  /bug-hunt-rules suggest           Analyze project and suggest rules
  /bug-hunt-rules add <rule>        Add a rule to the configuration

Rules file: .bug-hunter/rules.md
Extra rules: .bug-hunter/rules.d/*.md
```
