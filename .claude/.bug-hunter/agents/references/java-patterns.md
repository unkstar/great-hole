# Java Common Bug Patterns

Use this reference when analyzing Java code. These are high-frequency defect patterns specific to Java.

## Null Pointer

- **Unboxing null**: `Integer x = null; int y = x;` throws `NullPointerException`
- **Chained method calls without null check**: `getA().getB().getC()` — any intermediate null causes NPE
- **Map.get without null check**: `map.get(key).method()` — key may not exist, returns null
- **Optional misuse**: `optional.get()` without `isPresent()` check; defeats the purpose of Optional
- **String.equals on possibly null**: `str.equals("value")` — use `"value".equals(str)` or `Objects.equals`
- **Null in collections**: `List.of(null)` throws NPE; `ArrayList` allows null but `contains`/`indexOf` may surprise
- **@NonNull annotation without enforcement**: annotations alone don't prevent null at runtime

## Resource Management

- **Resource not closed**: `InputStream`, `Connection`, `ResultSet` etc. not closed in finally/try-with-resources
- **Close order matters**: closing resources in wrong order (e.g., closing connection before statement)
- **Try-with-resources suppressed exceptions**: exception in `close()` suppresses the original exception; use `addSuppressed`
- **BufferedWriter not flushed**: data lost if not flushed before close (close should flush, but verify)
- **JDBC ResultSet/Statement leak**: not closing ResultSet when reusing Statement
- **Thread pool not shut down**: `ExecutorService` not shut down on application exit; threads prevent JVM exit

## Concurrency

- **ConcurrentModificationException**: modifying collection while iterating without `ConcurrentHashMap` or `CopyOnWriteArrayList`
- **Non-atomic compound operation**: `if (!map.containsKey(k)) map.put(k, v)` — use `putIfAbsent` or `computeIfAbsent`
- **Double-checked locking without volatile**: classic singleton pattern broken without `volatile` on the field
- **Synchronizing on wrong object**: synchronizing on local variable, mutable field, or `String` literal (interned)
- **Synchronizing on boxed primitive**: `synchronized(Integer.valueOf(1))` — cached instances shared JVM-wide
- **Thread-unsafe SimpleDateFormat**: `SimpleDateFormat` is not thread-safe; use `DateTimeFormatter` (Java 8+)
- **Visibility without volatile/synchronized**: field written in one thread may not be visible to another
- **Lock not released in finally**: `lock.lock()` without `try { ... } finally { lock.unlock() }`
- **CompletableFuture exception swallowed**: exception in `thenApply` chain not handled; use `exceptionally` or `handle`

## Equals / HashCode / Comparable

- **equals without hashCode**: violates contract; objects equal by `equals` must have same `hashCode`
- **hashCode without equals**: less dangerous but inconsistent
- **equals with wrong signature**: `equals(MyClass other)` instead of `equals(Object other)` — doesn't override
- **Mutable fields in hashCode**: if fields used in hashCode change after insertion in HashSet/HashMap, object becomes unreachable
- **compareTo inconsistent with equals**: `TreeSet`/`TreeMap` use `compareTo`, not `equals`; inconsistency causes subtle bugs
- **Array equality**: `array1.equals(array2)` uses reference equality; use `Arrays.equals` or `Arrays.deepEquals`

## String and Data

- **String comparison with ==**: `str1 == str2` compares references; use `.equals()` (except for interned/constant strings)
- **BigDecimal equality**: `new BigDecimal("1.0").equals(new BigDecimal("1.00"))` is `false`; use `compareTo`
- **Floating point comparison**: `0.1 + 0.2 == 0.3` is `false`; use `BigDecimal` or epsilon comparison
- **Integer cache**: `Integer.valueOf(127) == Integer.valueOf(127)` is `true`, but `Integer.valueOf(128) == Integer.valueOf(128)` is `false`
- **String concatenation in loop**: `str += "text"` in loop creates many intermediate objects; use `StringBuilder`
- **Locale-sensitive String operations**: `"TITLE".toLowerCase()` gives different results in Turkish locale; use `toLowerCase(Locale.ROOT)`

## Exception Handling

- **Catching Throwable/Error**: `catch (Throwable t)` catches `OutOfMemoryError`, `StackOverflowError` — usually wrong
- **Empty catch block**: `catch (Exception e) {}` silently swallows errors
- **Catch and lose stacktrace**: `catch (Exception e) { throw new RuntimeException(e.getMessage()); }` loses cause
- **Returning in finally**: `return` in `finally` overrides return/throw from `try`/`catch`
- **Exception in static initializer**: wraps in `ExceptionInInitializerError`; class becomes unusable
- **Checked exception from lambda**: lambdas in functional interfaces can't throw checked exceptions; wrap or use unchecked

## Collections

- **Unmodifiable collection modifications**: `Collections.unmodifiableList(list)` — modifying original list still changes the "unmodifiable" view
- **Arrays.asList fixed-size**: `Arrays.asList(arr)` returns fixed-size list; `add`/`remove` throws `UnsupportedOperationException`
- **ConcurrentHashMap null key/value**: `ConcurrentHashMap` doesn't allow null keys or values (unlike `HashMap`)
- **Iterator invalidation**: using iterator after structural modification of collection
- **Sublist view modification**: `list.subList(a, b)` returns a view; modifying original list invalidates the sublist
- **TreeMap/TreeSet with incompatible comparator**: custom comparator must be consistent; null handling needed if nulls present

## Security

- **SQL injection**: string concatenation in SQL queries; use `PreparedStatement`
- **XXE**: XML parsing without disabling external entities
- **Deserialization of untrusted data**: `ObjectInputStream.readObject()` on untrusted input — arbitrary code execution
- **Insecure random**: `java.util.Random` is predictable; use `java.security.SecureRandom` for security contexts
- **Hardcoded credentials**: passwords/keys in source code
- **Path traversal**: `new File(base + userInput)` — user can use `../` to escape; use `Path.normalize` and validate

## Generics and Types

- **Raw type usage**: `List list` instead of `List<String>` — loses type safety at compile time
- **Type erasure surprise**: `list instanceof List<String>` doesn't work; generic type not available at runtime
- **Unchecked cast**: `(List<String>) obj` — no runtime check; ClassCastException may occur later
- **Generic array creation**: `new T[10]` is not allowed; `new List<String>[10]` is not type-safe
- **Varargs with generics**: `<T> void f(T... args)` creates `Object[]` at runtime; possible heap pollution
