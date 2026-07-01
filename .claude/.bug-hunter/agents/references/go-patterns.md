# Go Common Bug Patterns

Use this reference when analyzing Go code. These are high-frequency defect patterns specific to Go.

## Error Handling

- **Ignored error return**: `val, _ := SomeFunc()` — silently discarding errors hides failures
- **Error shadow**: inner `:=` shadows outer `err` variable, causing wrong error to be checked/returned
- **Deferred close without error check**: `defer f.Close()` ignores Close() error; for writes this can lose data
- **Error wrapping loses type**: `fmt.Errorf("failed: %s", err)` instead of `%w` breaks `errors.Is`/`errors.As`
- **Returning nil error with zero-value result**: function returns `(0, nil)` when it should return an error
- **Checking error string instead of type**: `strings.Contains(err.Error(), "not found")` is fragile

## Concurrency

- **Goroutine leak**: goroutine blocked on channel/lock with no cancellation path (missing context/done channel)
- **Loop variable capture in goroutine** (Go < 1.22): `go func() { use(v) }()` in `for _, v := range` captures the mutating variable
- **WaitGroup Add/Done mismatch**: `wg.Add(1)` not paired with `wg.Done()`, or `wg.Add` called inside goroutine (race with `wg.Wait`)
- **Concurrent map read/write**: map accessed from multiple goroutines without `sync.Mutex` or `sync.Map`
- **Data race on shared variable**: non-atomic read/write of shared variable across goroutines
- **Channel direction misuse**: sending on receive-only channel or vice versa (compile error, but wrong direction in design)
- **Mutex copied by value**: passing `sync.Mutex` or struct containing it by value instead of pointer
- **Unlock not deferred**: `mu.Lock()` without `defer mu.Unlock()` risks missing unlock on early return/panic

## Nil / Zero Value

- **Nil map write**: writing to a nil map panics; map must be initialized with `make(map[K]V)`
- **Nil pointer dereference**: using a pointer without nil check, especially from map lookups or interface assertions
- **Unchecked type assertion**: `v := x.(T)` panics if x is not T; use `v, ok := x.(T)` instead
- **Nil slice pitfalls**: `len(nil)` is 0 and `append(nil, ...)` works, but passing nil slice to some APIs may fail
- **Nil function/interface call**: calling a method on a nil interface panics (nil concrete type with method is OK)
- **Zero-value time.Time**: `time.Time{}` is not the same as "no time"; compare with `IsZero()` not `== time.Time{}`

## Resource Management

- **HTTP response body not closed**: `resp.Body.Close()` must be called even on error responses
- **SQL rows not closed**: `rows.Close()` must be called after iteration; missing on early return
- **File opened but not closed**: `os.Open` / `os.Create` without `defer f.Close()` (after error check)
- **Context leak**: `context.WithCancel`/`WithTimeout`/`WithDeadline` returns cancel func that must be called
- **Ticker not stopped**: `time.NewTicker` must be stopped with `ticker.Stop()` to release resources
- **Temp file not cleaned up**: `os.CreateTemp` without deferred `os.Remove`

## API Misuse

- **strings.Builder copied**: copying a `strings.Builder` after first write causes panic on next write
- **time.After in loop**: `time.After` allocates a new timer each iteration; use `time.NewTimer` + `Reset`
- **Append to slice alias**: `append` may or may not create a new backing array; aliased slices may see unexpected mutations
- **Range over string gives runes, not bytes**: `for i, c := range s` gives rune index/value; `s[i]` gives byte
- **json.Unmarshal into non-pointer**: passing non-pointer to Unmarshal silently does nothing
- **sync.Pool Put without Reset**: returning objects to pool without clearing sensitive/stale data
- **regexp.Compile in hot path**: compile regex once, not per-call; use `regexp.MustCompile` at package level
- **Comparing structs containing slices/maps with ==**: results in compile error for slices, but `==` on structs without them can still be wrong if fields are added later

## Control Flow

- **Missing break in type switch**: each case in type switch is independent (no fallthrough by default), but forgetting that `switch` on values DOES fallthrough with `fallthrough` keyword
- **Deferred function argument evaluation**: `defer f(x)` evaluates `x` immediately, not at defer time; use closure for lazy eval
- **Named return value shadowed**: named return `(err error)` shadowed by `:=` in inner scope, returning wrong value
- **Infinite recursion through interface**: method calling itself through embedded interface without explicit delegation
- **recover() only works in deferred function**: `recover()` called outside `defer` returns nil
