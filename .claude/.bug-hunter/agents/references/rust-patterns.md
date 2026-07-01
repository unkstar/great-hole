# Rust Common Bug Patterns

Use this reference when analyzing Rust code. These are high-frequency defect patterns specific to Rust.

## Ownership and Interior Mutability

- **Use-after-move assumptions**: value moved into another owner and later logic still assumes original binding is valid (often hidden by `clone()` added in the wrong place)
- **`Rc<RefCell<T>>` runtime borrow panic**: nested `borrow_mut()` / `borrow()` on `RefCell` causes panic at runtime despite compiling
- **Reference cycle leak with `Rc`**: parent/child both hold `Rc` strong refs; memory never drops; use `Weak` for back-references
- **Returning reference to temporary data**: API design tries to return borrowed data derived from short-lived locals; often "fixed" unsafely with leaked allocations
- **Hidden aliasing through interior mutability**: `Cell`/`RefCell` used to mutate shared state without clear invariants, causing state corruption across call paths
- **`mem::replace` / `take` misuse**: replacing critical fields with defaults without restoring invariants leaves object in invalid logical state

## Error Handling

- **`unwrap()` / `expect()` in recoverable paths**: panics on normal error conditions instead of returning `Result`
- **Dropped `Result`**: `let _ = fallible_call()` or ignored async join result discards failures
- **Error context loss**: converting rich errors to strings (`err.to_string()`) too early breaks structured handling and root-cause tracing
- **Over-broad `map_err(|_| ...)`**: replacing specific errors with generic ones hides actionable failure modes
- **`panic!` in library boundary**: library code panics where callers expect explicit error handling
- **Retry loop that swallows terminal error**: repeated attempts end with success-like return while last error is discarded

## Concurrency and Synchronization

- **Deadlock from inconsistent lock ordering**: different paths acquire `Mutex`/`RwLock` in opposite order
- **Lock held across blocking call**: holding a lock while doing I/O or waiting on channel causes contention and deadlock risk
- **Poisoned lock blindly unwrapped**: `mutex.lock().unwrap()` after panic cascades into process-level failures
- **Channel protocol mismatch**: sender/receiver wait on each other forever due to missing close signal or wrong send/recv order
- **Detached thread/task without lifecycle control**: spawned worker is never joined/cancelled, leaking work or hiding panics
- **Non-atomic shared flag in unsafe/FFI code**: cross-thread boolean/state updated without atomics or lock
- **`Arc<Mutex<T>>` logical race**: invariants rely on multi-step updates across separate locks, leading to inconsistent observable state

## Async and Futures

- **Forgotten `.await`**: async call result is created but never awaited; intended side effects never run
- **Blocking APIs inside async runtime**: `std::thread::sleep`, blocking I/O, or heavy CPU work on async executor thread stalls the runtime
- **Holding sync lock across `.await`**: `std::sync::MutexGuard` kept over await point can deadlock/starve executors
- **Unbounded task spawning**: `tokio::spawn` in hot loop without backpressure exhausts memory/scheduler
- **Missing cancellation/timeout**: network or channel await without timeout hangs request path indefinitely
- **Dropped `JoinHandle` result**: task panic/cancellation is ignored because handle result is never awaited/checked

## Unsafe and FFI

- **Invalid raw pointer dereference**: `*ptr` used without proving validity, alignment, and lifetime
- **`slice::from_raw_parts` length mismatch**: wrong element count creates out-of-bounds reads/writes
- **`MaybeUninit` misuse**: calling `assume_init()` before full initialization causes UB
- **Incorrect `transmute` assumptions**: size/layout/lifetime mismatch when converting between types
- **Missing `repr(C)` for FFI struct**: Rust layout differs from C expectation, corrupting data at boundary
- **FFI ownership mismatch**: both sides free the same allocation (double free) or neither side frees (leak)
- **Invalid C string handling**: untrusted bytes without NUL validation passed to `CStr`/`CString`

## Collections and Boundaries

- **Unchecked indexing panic**: `vec[i]` / `slice[i]` on untrusted index panics; use `get()` for fallible access
- **UTF-8 boundary slicing bug**: string slicing by byte index splits code point and panics
- **Release-mode integer overflow**: arithmetic silently wraps in release builds; use `checked_*` / `saturating_*` when required
- **`usize` underflow in length math**: `len() - 1` when empty wraps to huge value before bounds checks
- **Division/modulo by zero**: denominator derived from input/state without validation
- **Assuming map lookup exists**: `map.get(key).unwrap()` panics on missing key under edge cases

## Resource Management

- **Buffered writer not flushed**: `BufWriter` dropped on error path without explicit `flush()` loses trailing data
- **Child process handle leak**: spawned process not `wait()`ed/terminated, leaving zombies or leaked resources
- **Temporary resource cleanup skipped**: temp files/dirs/sockets created but not removed on early return
- **`Drop` implementation can panic**: panic during unwinding aborts process (`double panic`)
- **Intentional leaks left in production**: `Box::leak` / `mem::forget` used as shortcut and never reclaimed

## Security

- **Command injection via shell**: building `sh -c` command with user input enables arbitrary command execution
- **Path traversal**: joining untrusted path segments without canonicalization allows escaping intended directory
- **SQL injection by string formatting**: `format!("... {} ...", user_input)` used instead of parameterized queries
- **SSRF through unvalidated outbound URL**: server fetches attacker-controlled internal endpoints
- **Weak randomness for secrets**: non-cryptographic RNG used for tokens, reset codes, or keys
- **TLS verification disabled**: accepting invalid certs/hostnames in HTTP clients exposes MITM risk
- **Sensitive data in logs/panics**: tokens/passwords/PII included in `debug!`, `error!`, or panic messages
