# C/C++ Common Bug Patterns

Use this reference when analyzing C or C++ code. These are high-frequency defect patterns.

## Memory Safety

- **Buffer overflow**: writing beyond allocated buffer bounds (`strcpy`, `sprintf`, array index out of bounds)
- **Stack buffer overflow**: local array overflow; attacker can overwrite return address
- **Heap buffer overflow**: writing past `malloc`/`new` allocated size
- **Off-by-one in buffer**: `for (i = 0; i <= size; i++)` writing to `buf[size]` which is one past the end
- **Use-after-free**: accessing memory after `free()`/`delete`; pointer still holds old address
- **Double free**: calling `free()`/`delete` twice on the same pointer
- **Dangling pointer**: pointer to local variable returned from function; pointer to freed memory stored
- **Memory leak**: `malloc`/`new` without corresponding `free`/`delete` on all paths
- **Null pointer dereference**: using pointer without null check, especially from `malloc` (can return NULL)
- **Uninitialized variable**: reading from variable before assignment; undefined behavior in C/C++
- **Uninitialized struct members**: `struct s; s.field` — fields not zeroed unless `= {0}` or `memset`

## Integer Issues

- **Integer overflow**: `int a = INT_MAX; a + 1` — undefined behavior in signed; wraps in unsigned
- **Signed/unsigned comparison**: `if (signed_val < unsigned_val)` — signed is implicitly converted to unsigned; `-1 < 0u` is false
- **Integer truncation**: assigning `int64_t` to `int32_t` silently truncates
- **Size_t underflow**: `size_t` is unsigned; `size - 1` when `size == 0` wraps to `SIZE_MAX`
- **Shift overflow**: shifting by >= bit-width of type is undefined behavior
- **Division by zero**: undefined behavior; must check divisor before dividing
- **Implicit conversion in arithmetic**: `int a = 1; int b = 2; double c = a/b;` gives 0.0, not 0.5

## String Handling

- **Missing null terminator**: `strncpy` doesn't always null-terminate; manual termination needed
- **`gets()` usage**: unbounded input; always use `fgets()` instead
- **Format string vulnerability**: `printf(user_input)` — attacker can read/write memory; use `printf("%s", user_input)`
- **`strlen` on non-terminated string**: undefined behavior; reads until finds null byte
- **Buffer size mismatch with strlen**: `strlen` doesn't count null terminator; need `strlen(s) + 1` for allocation
- **`strtok` is not reentrant**: modifies input string and uses static state; not thread-safe; use `strtok_r`

## Pointer Issues

- **Pointer arithmetic overflow**: pointer arithmetic beyond allocated region is undefined behavior
- **Void pointer arithmetic**: arithmetic on `void*` is undefined in C (GNU extension allows it)
- **Array decay**: passing array to function loses size information; must pass size separately
- **Pointer/integer confusion**: comparing pointer to integer without cast; `if (ptr == 0)` works but `if (ptr == 1)` is wrong
- **Aliasing violation**: accessing same memory through incompatible pointer types violates strict aliasing rule
- **Returning pointer to local**: `int* f() { int x = 5; return &x; }` — dangling pointer

## Concurrency (C11/C++11+)

- **Data race**: unsynchronized access to shared variable from multiple threads; undefined behavior
- **Lock ordering deadlock**: threads acquiring multiple locks in different orders
- **Missing volatile for signal handlers**: variables accessed from signal handlers should be `volatile sig_atomic_t`
- **Spurious wakeup not handled**: `pthread_cond_wait` / `std::condition_variable::wait` can wake spuriously; must use while loop
- **Thread-unsafe static initialization** (C++03): `static Foo* instance` in function is not thread-safe before C++11
- **Atomic operations misuse**: using `memory_order_relaxed` when sequential consistency is needed

## Resource Management

- **File descriptor leak**: `open()`/`fopen()` without `close()`/`fclose()` on all paths
- **Socket not closed on error**: early return after `socket()` without `close(sock)`
- **`mmap` without `munmap`**: memory-mapped region not unmapped
- **Mixed `malloc`/`new`/`new[]`**: `free` on `new` memory or `delete` on `new[]` is undefined behavior
- **`realloc` losing original pointer**: `ptr = realloc(ptr, size)` — if `realloc` fails, original memory is leaked

## C++ Specific

- **Rule of Three/Five violation**: defining destructor but not copy constructor/assignment operator
- **Slicing**: passing derived class by value to base class parameter; derived members are lost
- **Virtual destructor missing**: deleting derived class through base pointer without virtual destructor — undefined behavior
- **Exception in destructor**: throwing from destructor during stack unwinding calls `std::terminate`
- **Moving from then using**: using object after `std::move`; object is in valid but unspecified state
- **Shared_ptr circular reference**: two `shared_ptr` pointing to each other; memory never freed; use `weak_ptr`
- **auto_ptr pitfalls**: `auto_ptr` (deprecated) has broken copy semantics; use `unique_ptr`
- **Iterator invalidation**: using iterator after container modification (`vector::push_back` can invalidate all iterators)
- **Implicit conversion through single-arg constructor**: `Foo(int)` allows `Foo f = 42`; use `explicit`
- **Static initialization order fiasco**: globals in different translation units; initialization order is undefined
- **Temporary lifetime extension**: `const T& ref = getTemp()` extends lifetime, but `const T& ref = getTemp().member` may not

## Preprocessor and Compilation

- **Macro side effects**: `#define MAX(a,b) ((a)>(b)?(a):(b))` evaluates arguments twice; `MAX(i++, j++)` double-increments
- **Missing include guards**: `#ifndef` / `#pragma once` missing; double inclusion causes redefinition errors
- **ODR violation**: same symbol defined differently in multiple translation units; undefined behavior
- **ABI incompatibility**: linking objects compiled with different compiler versions or flags

## Security

- **Buffer overflow exploits**: stack smashing, return-to-libc, ROP chains via buffer overflow
- **Format string attack**: `printf(user_input)` allows reading stack and writing arbitrary memory
- **Command injection**: `system(user_input)` — arbitrary command execution
- **Path traversal**: unchecked `../` in file paths
- **TOCTOU race**: `if (access(file)) { open(file) }` — file may change between check and use
- **Insecure randomness**: `rand()`/`srand()` are predictable; use `/dev/urandom` or platform CSPRNG
