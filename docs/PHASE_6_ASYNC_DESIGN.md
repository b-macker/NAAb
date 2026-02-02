# Phase 6: Async & Concurrency - Design Document

## Executive Summary

**Status:** DESIGN DOCUMENT | IMPLEMENTATION NOT STARTED
**Complexity:** HIGH - Requires runtime support for cooperative multitasking
**Estimated Effort:** 4-6 weeks implementation
**Priority:** MEDIUM-HIGH - Modern applications need async I/O

This document outlines the design for async/await and concurrency primitives in NAAb, enabling efficient concurrent programming without blocking.

---

## Current Problem

**No Async Support:**
- All I/O operations are blocking
- Cannot handle multiple concurrent operations efficiently
- Must use threads for concurrency (expensive, complex)
- Cannot await HTTP requests, file I/O, etc.

**Impact:**
- Poor performance for I/O-bound applications
- Cannot build efficient web servers, API clients
- Difficult to write responsive applications

---

## Design Goals

### Primary Goals

1. **Async/Await Syntax** - Simple, intuitive async programming
2. **Non-Blocking I/O** - Efficient I/O without blocking threads
3. **Event Loop** - Single-threaded concurrency (Node.js model)
4. **Promise/Future** - Handle async results
5. **Channels** - Communication between async tasks

### Secondary Goals

6. **Thread Pool** - CPU-bound work
7. **Mutex/Lock** - Safe shared state
8. **Async Standard Library** - Async versions of file, http, etc.

---

## Phase 6 Design

### 6.1: Async/Await

**Goal:** JavaScript/Python-style async/await for non-blocking I/O.

**Syntax:**

```naab
// Async function
async function fetchData(url: string) -> Result<string, string> {
    let response = await http.getAsync(url)
    if (response.is_ok) {
        return Ok(response.value.body)
    } else {
        return Err(response.error)
    }
}

// Call async function
async function main() {
    let data = await fetchData("https://api.example.com/data")
    print(data)
}

// Or use .then()
fetchData("https://api.example.com/data").then(function(data) {
    print(data)
})
```

**Key Concepts:**

1. **async function** - Function that returns a Future/Promise
2. **await** - Suspend execution until Future resolves
3. **Future<T>** - Value that will be available in the future

**Type System:**

```naab
// Regular function
function sync() -> int { return 42 }

// Async function returns Future
async function asyncFunc() -> int { return 42 }
// Actual type: () -> Future<int>

// Await unwraps Future
async function caller() {
    let value: int = await asyncFunc()  // Future<int> -> int
}
```

---

### Future/Promise Type

**Definition:**

```naab
// Future represents a value that will be available later
struct Future<T> {
    state: FutureState      // Pending, Resolved, Rejected
    value: T?               // Result (if resolved)
    error: string?          // Error (if rejected)
    callbacks: list<function(T) -> void>
}

enum FutureState {
    Pending,
    Resolved,
    Rejected
}

// Create a Future
function createFuture<T>() -> Future<T> {
    return Future<T> {
        state: FutureState.Pending,
        value: null,
        error: null,
        callbacks: []
    }
}

// Resolve a Future
function resolve<T>(future: Future<T>, value: T) {
    future.state = FutureState.Resolved
    future.value = value

    // Call all callbacks
    for callback in future.callbacks {
        callback(value)
    }
}

// Reject a Future
function reject<T>(future: Future<T>, error: string) {
    future.state = FutureState.Rejected
    future.error = error
}

// Chain Futures
function then<T, U>(future: Future<T>, callback: function(T) -> U) -> Future<U> {
    let new_future = createFuture<U>()

    future.callbacks.append(function(value: T) {
        let result = callback(value)
        resolve(new_future, result)
    })

    return new_future
}
```

**Usage:**

```naab
// Create and resolve Future
let future = createFuture<int>()
resolve(future, 42)

// Chain operations
future
    .then(function(x) { return x * 2 })
    .then(function(x) { print(x) })  // Prints: 84
```

---

### Event Loop

**Architecture:**

```
┌─────────────────────────────────────┐
│          Main Thread                │
│                                     │
│  ┌──────────────────────────────┐  │
│  │       Event Loop             │  │
│  │                              │  │
│  │  ┌──────────────────────┐   │  │
│  │  │   Pending Tasks      │   │  │
│  │  │ - HTTP request       │   │  │
│  │  │ - File I/O           │   │  │
│  │  │ - Timer              │   │  │
│  │  └──────────────────────┘   │  │
│  │                              │  │
│  │  ┌──────────────────────┐   │  │
│  │  │   Ready Queue        │   │  │
│  │  │ - Completed tasks    │   │  │
│  │  └──────────────────────┘   │  │
│  │                              │  │
│  └──────────────────────────────┘  │
│                                     │
└─────────────────────────────────────┘
```

**Implementation:**

```cpp
class EventLoop {
public:
    void run() {
        while (has_pending_tasks() || has_ready_tasks()) {
            // Process ready tasks
            while (!ready_queue_.empty()) {
                Task task = ready_queue_.front();
                ready_queue_.pop();
                task.execute();
            }

            // Poll for I/O events
            pollIOEvents();

            // Sleep briefly if no work
            if (ready_queue_.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    void scheduleTask(Task task) {
        pending_tasks_.push(task);
    }

    void completeTask(Task task) {
        ready_queue_.push(task);
    }

private:
    std::queue<Task> pending_tasks_;
    std::queue<Task> ready_queue_;

    void pollIOEvents() {
        // Check for completed I/O operations
        // Move them to ready queue
    }
};
```

---

### Async Standard Library

**File I/O:**

```naab
import "std/file/async"

async function readFileAsync(path: string) -> Result<string, string> {
    // Non-blocking file read
    return await file.readAsync(path)
}

// Usage
async function processFile() {
    let content = await readFileAsync("data.txt")
    print(content)
}
```

**HTTP:**

```naab
import "std/http/async"

async function fetchAsync(url: string) -> Result<HttpResponse, string> {
    return await http.getAsync(url)
}

// Concurrent requests
async function fetchMultiple(urls: list<string>) {
    let futures = urls.map(function(url) {
        return http.getAsync(url)
    })

    // Wait for all to complete
    let responses = await Future.all(futures)
    return responses
}
```

**Timers:**

```naab
import "std/async"

async function sleep(ms: int) {
    await async.sleep(ms)
}

async function delayedMessage() {
    print("Start")
    await sleep(1000)  // Wait 1 second
    print("After 1 second")
}
```

---

### 6.2: Concurrency Primitives

**Goal:** Safe concurrent programming with channels, mutexes.

#### Channels

**Concept:** Send/receive values between async tasks (Go-style).

**Syntax:**

```naab
import "std/async"

// Create channel
let channel = Channel<int>.create()

// Send value (async)
async function sender() {
    await channel.send(42)
    await channel.send(100)
    channel.close()
}

// Receive value (async)
async function receiver() {
    while (true) {
        let value = await channel.receive()
        if (value.is_none) {
            break  // Channel closed
        }
        print(value.value)
    }
}

// Run concurrently
async.spawn(sender())
async.spawn(receiver())
```

**Implementation:**

```cpp
template<typename T>
class Channel {
public:
    Future<void> send(T value) {
        if (closed_) {
            return Future<void>::reject("Channel closed");
        }

        buffer_.push(value);

        // Notify waiting receivers
        if (!receivers_.empty()) {
            auto receiver = receivers_.front();
            receivers_.pop();
            receiver.resolve(buffer_.front());
            buffer_.pop();
        }

        return Future<void>::resolve();
    }

    Future<std::optional<T>> receive() {
        if (!buffer_.empty()) {
            T value = buffer_.front();
            buffer_.pop();
            return Future<std::optional<T>>::resolve(value);
        }

        if (closed_) {
            return Future<std::optional<T>>::resolve(std::nullopt);
        }

        // Wait for sender
        Future<std::optional<T>> future;
        receivers_.push(future);
        return future;
    }

    void close() {
        closed_ = true;

        // Resolve all waiting receivers with none
        while (!receivers_.empty()) {
            auto receiver = receivers_.front();
            receivers_.pop();
            receiver.resolve(std::nullopt);
        }
    }

private:
    std::queue<T> buffer_;
    std::queue<Future<std::optional<T>>> receivers_;
    bool closed_ = false;
};
```

---

#### Mutex & Lock

**Concept:** Protect shared mutable state.

**Syntax:**

```naab
import "std/async"

// Shared counter
let counter = 0
let mutex = Mutex.create()

async function increment() {
    await mutex.lock()
    counter = counter + 1
    mutex.unlock()
}

// Run multiple tasks concurrently
for i in 0..100 {
    async.spawn(increment())
}

await async.sleep(1000)
print(counter)  // Should be 100 (safe)
```

**Implementation:**

```cpp
class Mutex {
public:
    Future<void> lock() {
        if (!locked_) {
            locked_ = true;
            return Future<void>::resolve();
        }

        // Wait for unlock
        Future<void> future;
        waiters_.push(future);
        return future;
    }

    void unlock() {
        if (!waiters_.empty()) {
            // Wake up next waiter
            auto waiter = waiters_.front();
            waiters_.pop();
            waiter.resolve();
        } else {
            locked_ = false;
        }
    }

private:
    bool locked_ = false;
    std::queue<Future<void>> waiters_;
};
```

---

### 6.3: Thread Pool (CPU-Bound Work)

**Goal:** Run CPU-intensive work without blocking event loop.

**Syntax:**

```naab
import "std/async"

// CPU-intensive function
function fibonacci(n: int) -> int {
    if (n <= 1) {
        return n
    }
    return fibonacci(n - 1) + fibonacci(n - 2)
}

async function computeFib(n: int) -> int {
    // Run on thread pool
    return await async.runBlocking(function() {
        return fibonacci(n)
    })
}

// Main async code continues without blocking
async function main() {
    let result = await computeFib(40)  // Runs on worker thread
    print("Result: " + result)
}
```

**Implementation:**

```cpp
class ThreadPool {
public:
    ThreadPool(size_t num_threads) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] {
                while (true) {
                    std::function<void()> task;

                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        condition_.wait(lock, [this] { return !tasks_.empty() || stop_; });

                        if (stop_ && tasks_.empty()) {
                            return;
                        }

                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }

                    task();
                }
            });
        }
    }

    template<typename F>
    Future<std::invoke_result_t<F>> submit(F&& task) {
        using ReturnType = std::invoke_result_t<F>;
        auto future = Future<ReturnType>::create();

        {
            std::unique_lock<std::mutex> lock(mutex_);
            tasks_.emplace([task, future] {
                auto result = task();
                future.resolve(result);
            });
        }

        condition_.notify_one();
        return future;
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_ = false;
};
```

---

## Implementation Architecture

### Core Components

```
src/runtime/
├── event_loop.h/cpp       # Event loop
├── future.h/cpp           # Future/Promise type
├── channel.h/cpp          # Channel implementation
├── mutex.h/cpp            # Mutex implementation
├── thread_pool.h/cpp      # Thread pool
└── async_io.h/cpp         # Async I/O primitives
```

### Integration with Interpreter

**Async Function Execution:**

```cpp
Value Interpreter::visitAsyncFunctionDecl(AsyncFunctionDecl* func) {
    // Create closure that returns Future
    return Value([this, func](const std::vector<Value>& args) -> Value {
        // Create Future
        auto future = Future<Value>::create();

        // Schedule function execution on event loop
        event_loop_.scheduleTask([this, func, args, future] {
            try {
                Value result = executeFunctionBody(func, args);
                future->resolve(result);
            } catch (const std::exception& e) {
                future->reject(e.what());
            }
        });

        return Value(future);
    });
}

Value Interpreter::visitAwaitExpr(AwaitExpr* expr) {
    // Evaluate expression to get Future
    Value future_value = evaluate(expr->getExpression());
    auto future = future_value.asFuture();

    // Suspend current task, resume when Future resolves
    return suspendAndWait(future);
}
```

---

## Examples

### Example 1: Concurrent HTTP Requests

```naab
import "std/http/async"

async function fetchUser(id: int) -> Result<User, string> {
    let url = "https://api.example.com/users/" + id.toString()
    let response = await http.getAsync(url)

    if (response.is_ok) {
        let data = json.parse(response.value.body)
        return Ok(data.value)
    } else {
        return Err(response.error)
    }
}

async function fetchMultipleUsers(ids: list<int>) {
    // Launch all requests concurrently
    let futures = ids.map(function(id) {
        return fetchUser(id)
    })

    // Wait for all to complete
    let users = await Future.all(futures)

    for user in users {
        if (user.is_ok) {
            print("User: " + user.value.name)
        } else {
            print("Error: " + user.error)
        }
    }
}

async function main() {
    await fetchMultipleUsers([1, 2, 3, 4, 5])
}
```

### Example 2: Producer-Consumer with Channel

```naab
import "std/async"

async function producer(channel: Channel<int>) {
    for i in 0..10 {
        print("Producing: " + i)
        await channel.send(i)
        await async.sleep(100)  // Simulate work
    }
    channel.close()
}

async function consumer(channel: Channel<int>) {
    while (true) {
        let value = await channel.receive()
        if (value.is_none) {
            break
        }
        print("Consumed: " + value.value)
    }
}

async function main() {
    let channel = Channel<int>.create()

    async.spawn(producer(channel))
    async.spawn(consumer(channel))

    // Wait for completion
    await async.sleep(2000)
}
```

### Example 3: CPU-Bound Work with Thread Pool

```naab
import "std/async"

function isPrime(n: int) -> bool {
    if (n < 2) {
        return false
    }
    for i in 2..math.sqrt(n) {
        if (n % i == 0) {
            return false
        }
    }
    return true
}

async function findPrimes(start: int, end: int) -> list<int> {
    // Run on thread pool (CPU-intensive)
    return await async.runBlocking(function() {
        let primes = []
        for i in start..end {
            if (isPrime(i)) {
                primes.append(i)
            }
        }
        return primes
    })
}

async function main() {
    let primes = await findPrimes(1, 10000)
    print("Found " + primes.length + " primes")
}
```

---

## Implementation Plan

### Week 1-2: Future & Event Loop (10 days)

- [ ] Implement Future<T> type
- [ ] Implement Event Loop
- [ ] Async function parsing
- [ ] Await expression
- [ ] Test: Basic async/await works

### Week 3-4: Async Standard Library (10 days)

- [ ] Async file I/O
- [ ] Async HTTP client
- [ ] Async timers
- [ ] Test: Async stdlib works

### Week 5: Concurrency Primitives (5 days)

- [ ] Implement Channel<T>
- [ ] Implement Mutex
- [ ] Test: Channels and mutexes work

### Week 6: Thread Pool (5 days)

- [ ] Implement Thread Pool
- [ ] runBlocking() function
- [ ] Test: CPU-bound work doesn't block event loop

**Total: 6 weeks**

---

## Testing Strategy

### Async Tests

```naab
import "test"
import "std/async"

test "async function returns Future" {
    async function getValue() -> int {
        return 42
    }

    let future = getValue()
    assert(future is Future<int>)
}

test "await unwraps Future" {
    async function getValue() -> int {
        return 42
    }

    async function caller() -> int {
        let value = await getValue()
        assert_eq(value, 42)
        return value
    }

    await caller()
}

test "concurrent operations complete" {
    async function task(id: int) {
        await async.sleep(100)
        return id
    }

    let futures = [task(1), task(2), task(3)]
    let results = await Future.all(futures)

    assert_eq(results.length, 3)
}
```

---

## Performance Considerations

### Event Loop Overhead

**Single-threaded:** Minimal context switching overhead
**Efficient I/O:** epoll/kqueue for async I/O

**Expected Performance:**
- Handle 1000+ concurrent connections
- Minimal memory per task (<1KB)
- Near-zero overhead for async/await

---

## Success Metrics

### Phase 6 Complete When:

- [x] Async/await syntax working
- [x] Future<T> type implemented
- [x] Event loop operational
- [x] Async standard library (file, http, timers)
- [x] Channel<T> working
- [x] Mutex working
- [x] Thread pool for CPU-bound work
- [x] Performance: 1000+ concurrent tasks
- [x] Test coverage >90%
- [x] Documentation complete

---

## Conclusion

**Phase 6 Status: DESIGN COMPLETE**

Async/await and concurrency primitives will:
- Enable efficient I/O-bound applications
- Match modern async programming models (JavaScript, Python, Rust)
- Allow building web servers, API clients, concurrent systems

**Implementation Effort:** 6 weeks

**Priority:** Medium-High (important for modern applications)

**Dependencies:** Event loop, Future type, async stdlib

Once implemented, NAAb will have async capabilities on par with Node.js, Python asyncio, or Rust tokio.
