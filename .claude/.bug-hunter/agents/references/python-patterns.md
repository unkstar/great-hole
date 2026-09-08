# Python Common Bug Patterns

Use this reference when analyzing Python code. These are high-frequency defect patterns specific to Python.

## Mutable Default Arguments

- **Mutable default parameter**: `def f(items=[])` — the list is shared across calls; use `None` + create inside function
- **Default dict/set argument**: same issue as list; `def f(cache={})` accumulates state between calls
- **Class-level mutable attribute**: `class C: items = []` — shared across all instances; define in `__init__` instead

## Exception Handling

- **Bare except**: `except:` catches `SystemExit`, `KeyboardInterrupt`, `GeneratorExit` — use `except Exception:`
- **Broad except swallowing errors**: `except Exception: pass` silently ignores all errors
- **Except clause variable scope**: in Python 3, the variable bound in `except SomeError as e:` is deleted after the block
- **Re-raising without chain**: `raise NewError()` instead of `raise NewError() from e` loses original traceback
- **Catching and logging but not re-raising**: error is logged but execution continues in an invalid state
- **StopIteration leaking from generators**: in Python 3.7+, `StopIteration` raised inside generator becomes `RuntimeError`

## Type and Comparison

- **`is` vs `==`**: `is` checks identity, not equality; `x is "string"` may work for interned strings but is unreliable
- **Mutable object as dict key**: using list/dict/set as dictionary key raises `TypeError`; using mutable object with `__hash__` causes silent bugs
- **Integer cache boundary**: `a is b` works for small integers (-5 to 256) but fails for larger ones
- **`None` comparison**: use `x is None` not `x == None`; classes can override `__eq__`
- **Boolean vs integer**: `True == 1` and `False == 0` — accidental integer/bool mixing in conditions
- **`not a in b` vs `a not in b`**: both work but `not a in b` is parsed as `not (a in b)`, which can confuse readers

## Concurrency and Async

- **GIL misconception**: GIL doesn't make data structures thread-safe; list.append is atomic but compound operations aren't
- **Thread-unsafe shared state**: modifying shared dict/list from multiple threads without lock
- **Forgotten `await`**: calling async function without `await` returns a coroutine object, doesn't execute it
- **Async generator not finalized**: async generators need `async for` or manual `aclose()` to clean up
- **Mixing asyncio and threading incorrectly**: calling `asyncio.run()` from a thread that already has an event loop
- **Deadlock from nested locks**: acquiring the same `threading.Lock()` twice in the same thread (use `RLock`)

## Resource Management

- **File not closed**: `f = open("file")` without `with` statement or explicit `close()`
- **Database connection leak**: not closing connections/cursors in finally/with block
- **Temp file not cleaned**: `tempfile.mktemp()` is insecure; use `tempfile.NamedTemporaryFile` with context manager
- **Socket not closed on error**: early return/exception before `socket.close()`
- **`__del__` for cleanup**: `__del__` is not guaranteed to be called; use context managers or `atexit`

## TOCTOU and File Operations

- **Check-then-act race**: `if os.path.exists(f): open(f)` — file may be removed between check and open
- **Insecure temp file**: `tempfile.mktemp()` creates a race condition; use `tempfile.mkstemp()`
- **Symlink following**: `os.path.exists` follows symlinks; may access unintended targets
- **Non-atomic file write**: writing directly to target file instead of write-to-temp-then-rename

## Iteration

- **Iterator exhaustion**: iterating over a generator/iterator twice — second iteration silently yields nothing
- **Modifying list during iteration**: `for x in items: items.remove(x)` skips elements
- **Dict modification during iteration**: `for k in d: del d[k]` raises `RuntimeError` in Python 3
- **Forgetting `list()` on map/filter**: `map()` / `filter()` return iterators in Python 3; consuming once then reusing fails

## Import and Scope

- **Circular import**: module A imports B, B imports A — causes `ImportError` or `AttributeError`
- **Late binding closure**: `[lambda: i for i in range(5)]` — all lambdas return 4; use default arg `lambda i=i: i`
- **Global variable modification without `global`**: assignment in function creates local variable, doesn't modify global
- **Wildcard import collision**: `from module import *` can silently override local names

## Security

- **`eval()` / `exec()` on untrusted input**: arbitrary code execution
- **`pickle.loads` on untrusted data**: arbitrary code execution during deserialization
- **SQL injection via string formatting**: `f"SELECT * FROM t WHERE id={user_input}"` — use parameterized queries
- **YAML unsafe load**: `yaml.load(data)` without `Loader=SafeLoader` allows code execution
- **`subprocess.shell=True` with user input**: command injection risk
- **`os.system()` with user input**: command injection; use `subprocess.run` with list args
