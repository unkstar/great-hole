# TypeScript / JavaScript Common Bug Patterns

Use this reference when analyzing TypeScript or JavaScript code. These are high-frequency defect patterns.

## Promise and Async

- **Forgotten `await`**: calling async function without `await` — returns Promise, doesn't execute to completion; result checks pass on the truthy Promise object
- **`await` in loop without parallelization**: `for (const x of items) { await fetch(x) }` when items are independent — use `Promise.all`
- **Unhandled promise rejection**: promise chain without `.catch()` or missing try/catch around `await`
- **`async` callback in non-async-aware API**: `array.forEach(async (item) => { await ... })` — forEach doesn't await; use `for...of` or `Promise.all(array.map(...))`
- **Promise.all short-circuit**: if one promise rejects, others are abandoned; use `Promise.allSettled` when all results needed
- **Mixing callbacks and promises**: calling both `resolve()` and `callback()` in promisified function
- **Returning inside `.then()` without return**: `.then(result => { doSomething(result) })` — missing `return` breaks the chain
- **`try/catch` not catching async errors**: `try { asyncFunc() } catch(e) {}` — must `await asyncFunc()` for catch to work

## Type Safety (TypeScript)

- **Unsafe type assertion**: `value as SomeType` without runtime validation — bypasses type checker
- **Non-null assertion `!`**: `obj!.prop` silences null check but doesn't prevent runtime null
- **`any` type propagation**: `any` disables all type checking downstream; silently corrupts type safety
- **Optional chaining with side effects**: `obj?.method()` — if obj is null, method doesn't execute but code continues as if it did
- **Type narrowing lost after `await`**: TypeScript narrowing doesn't persist across `await` boundaries; re-check after await
- **Enum numeric comparison**: `if (status === 0)` when `enum Status { OK = 0 }` — use `Status.OK`
- **Index signature returns `undefined`**: `Record<string, T>` access returns `T`, not `T | undefined` by default; enable `noUncheckedIndexedAccess`

## Equality and Comparison

- **`==` coercion**: `"0" == false` is `true`, `[] == false` is `true`; always use `===`
- **`NaN !== NaN`**: `NaN === NaN` is `false`; use `Number.isNaN()` not `=== NaN`
- **Object/array equality by reference**: `{a:1} === {a:1}` is `false`; need deep comparison
- **Falsy value confusion**: `0`, `""`, `null`, `undefined`, `NaN`, `false` are all falsy; `if (value)` rejects valid `0` or `""`
- **`typeof null === "object"`**: null check requires explicit `=== null` or `== null`

## Closure and Scope

- **`var` in loop**: `for (var i = 0; ...) { setTimeout(() => console.log(i)) }` — all callbacks share same `i`; use `let`
- **Closure over mutable variable**: similar to var issue but with any mutable outer variable
- **`this` binding loss**: `const handler = obj.method; handler()` — `this` is `undefined` in strict mode; use arrow function or `.bind()`
- **Arrow function `this` in class**: arrow functions in object literals capture outer `this`, not the object
- **Implicit global**: assignment to undeclared variable creates global in non-strict mode

## Null / Undefined

- **Optional chaining default**: `obj?.prop ?? default` — nullish coalescing catches `null`/`undefined` but not `false`/`0`/`""`
- **Destructuring default vs nullish**: `const { x = 5 } = obj` — default only applies for `undefined`, not `null`
- **`Array.find` returns `undefined`**: unchecked result of `.find()` used as if always defined
- **`Map.get` returns `undefined`**: accessing non-existent key returns `undefined`, not throwing
- **`JSON.parse` null propagation**: `JSON.parse("null")` returns `null`, not an error

## Array / Object

- **Array sparse holes**: `new Array(5)` creates sparse array; `.map()` skips holes but `.length` is 5
- **Object.keys ordering**: not guaranteed in older engines; don't depend on insertion order for non-string keys
- **Shallow copy gotcha**: `{...obj}` / `[...arr]` is shallow; nested objects are still shared references
- **`Array.sort` mutates in place**: `arr.sort()` modifies the original array; use `[...arr].sort()`
- **`Array.sort` default comparison**: `[10, 2, 1].sort()` gives `[1, 10, 2]` — sorts as strings; provide comparator
- **`delete obj[key]` leaves undefined**: doesn't shift array indices; use `splice` for arrays
- **`for...in` on arrays**: iterates over all enumerable properties including prototype; use `for...of` or `.forEach`

## Error Handling

- **Catching and ignoring**: `catch(e) {}` or `catch(e) { console.log(e) }` — error lost, execution continues in bad state
- **Throwing non-Error objects**: `throw "error"` or `throw 404` — no stack trace; always `throw new Error()`
- **Error in constructor**: object partially initialized if constructor throws after some assignments
- **`finally` return overrides**: `return` in `finally` block overrides `return` in `try`/`catch`

## Security

- **Prototype pollution**: `obj[userInput] = value` can set `__proto__` properties; affects all objects
- **XSS via innerHTML**: `element.innerHTML = userInput` — use `textContent` or sanitize
- **`eval()` with user input**: arbitrary code execution
- **ReDoS**: regex with catastrophic backtracking on user input (e.g., `(a+)+$`)
- **Insecure randomness**: `Math.random()` is not cryptographically secure; use `crypto.getRandomValues()`
- **Open redirect**: `window.location = userInput` without validation

## React Specific (when applicable)

- **Missing dependency in useEffect/useMemo/useCallback**: stale closure captures outdated state/props
- **Setting state in render**: causes infinite re-render loop
- **Mutating state directly**: `state.items.push(x)` instead of `setState([...state.items, x])` — React won't re-render
- **Key prop using array index**: `{items.map((item, i) => <C key={i} />)}` — causes issues on reorder/delete
- **useEffect cleanup missing**: subscriptions/timers not cleaned up on unmount
- **Conditional hooks**: hooks called inside conditions or loops violate Rules of Hooks
