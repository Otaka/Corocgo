# corocgo

A Go-like concurrency library for C++ built on top of [minicoro](https://github.com/edubart/minicoro) — a minimal, cross-platform stackful coroutine library.

corocgo implements lightweight coroutines, buffered channels, `select`, sleep, and I/O waiting with a cooperative scheduling model. The API is intentionally similar to Go's concurrency primitives. All public symbols live in the `corocgo` namespace.

---

## Architecture Overview

The system is composed of two layers:

1. **minicoro** (`minicoro.h`) — a single-file C library providing asymmetric stackful coroutines with platform-specific backends (assembly for x86_64/ARM/ARM64/RISC-V, `ucontext` on Linux/Unix, Windows Fibers, or Binaryen asyncify for WebAssembly).
2. **corocgo** (`corocgo.h` / `corocgo.cpp`) — a C++ scheduler and concurrency primitives built on top of minicoro.

---

## Scheduling Model

corocgo uses a **single-threaded cooperative scheduler**. All coroutines run on the main thread and must voluntarily yield control. The scheduler never preempts a running coroutine.

Two scheduling modes are available:

- **`scheduler_start()`** — blocking mode. Takes full control of the calling thread and runs an event loop until all coroutines complete. Suitable for desktop applications.
- **`scheduler_init()` / `scheduler_step()` / `scheduler_stop()`** — step mode. Each call to `scheduler_step()` performs one non-blocking tick of work and returns, giving the caller control over the main loop. Suitable for embedded systems, microcontrollers, game loops, or any environment where an external loop must retain control.

### Background Threads

Two background threads assist the scheduler:

- **ThreadPool** — a bounded worker pool (capacity 256) for executing blocking operations off the main thread. When a coroutine calls `exec_thread`, it yields and a worker runs the given function. On completion, the worker wakes the coroutine.
- **PollThread** — a dedicated thread that calls `poll()` to monitor file descriptors. Coroutines register a fd via `wait_file`, suspend, and the poll thread notifies the scheduler when the fd becomes ready.

These threads communicate back to the scheduler via a thread-safe **pending wake queue**. The scheduler drains this queue in Phase 1 to avoid lock contention.

---

## Channels

Channels are the primary communication mechanism between coroutines, analogous to Go channels.

### Internal Buffering

Channels use a **circular ring buffer** of fixed capacity. All sizes are set at creation time and do not change.

- `send(value)` — writes a value to the buffer. Blocks (yields) if the buffer is full, waiting until a receiver drains a slot. You can execute send() only from coroutine, because it is not thread-safe.
- `receive()` — reads a value from the buffer. Blocks (yields) if the buffer is empty, waiting until a sender adds a value.
- `tryReceive()` — non-blocking receive; returns immediately with an empty optional if no value is available.
- `close()` — marks the channel closed and wakes all waiting receivers. Sending to a closed channel has undefined behavior.

### External (Thread-Safe) Sending

Channels can be created with an optional **external buffer** for sending from non-coroutine threads (e.g., thread pool workers, OS threads):

- `sendExternalNoBlock(value)` — thread-safe, non-blocking send from any thread. Values accumulate in a separate external buffer protected by a mutex.
- When a receiver calls `receive()`, it drains the external buffer first before reading from the internal buffer.
- The external buffer capacity is set independently of the internal buffer capacity.

### Synchronization

Channel blocking is implemented via internal wait-queue objects:

- A blocked sender is placed in a send-wait queue and yields.
- A blocked receiver is placed in a recv-wait queue and yields.
- On each successful send/receive, the opposite wait-queue wakes one waiter.
- `close()` calls `wakeAll()` on both wait-queues.

For channels that support external sends, a thread-safe variant is used on the receive side, which can be safely woken from outside the scheduler thread.

---

## Select

`select` waits on multiple channels simultaneously, similar to Go's `select` statement.

**Execution order:**

1. All provided receive cases are tried non-blocking first. If any channel has a value ready, the corresponding handler is called immediately.
2. If no channel is ready, the coroutine registers itself on all channel recv-monitors at once, then yields.
3. When any monitor fires, the coroutine is moved back to the ready queue.

**Return values:**

| Value | Meaning |
|---|---|
| `>= 0` | Index of the `RecvCase` that received a value |
| `SELECT_CLOSED` | All channels in the select are closed |
| `SELECT_DEFAULT` | Default case was executed (no channel was ready) |

A `DefaultCase` can be provided as the last argument to make `select` non-blocking: if no channel is immediately ready, the default handler runs and `select` returns `SELECT_DEFAULT`.

---

## Sleep

`sleep(milliseconds)` suspends the current coroutine for at least the specified number of milliseconds.

Sleep precision depends on scheduler iteration frequency; in practice it wakes within one scheduler loop iteration after the deadline passes.

---

## I/O Waiting

`wait_file(fd, modeBitFlag)` suspends the current coroutine until a file descriptor is ready for the requested operation (read, write, or error).

Returns a `pair<int, int>` containing the result flags and any error from `poll()`.

---

## Deadlock Detection

If the scheduler finds no coroutines in any queue (run queue, sleep queue, or thread/fd waiters), it treats this as a deadlock and exits. This mirrors Go's "all goroutines are asleep" panic.

---

## Platform Support 

**Minicoro library**

| Platform | Backend |
|---|---|
| x86_64 | Assembly |
| ARM / ARM64 | Assembly |
| RISC-V | Assembly |
| Linux / Unix | `ucontext` (fallback) |
| Windows | Fibers |
| WebAssembly | Binaryen asyncify |

**Corocgo library**
- Linux
- Raspberry Pi(Raspbian)
- Osx(Arm)

---

## Examples

All examples require `#include "corocgo.h"` and `using namespace corocgo;`. The entry point always spawns at least one coroutine with `coro()` and then runs the scheduler — either via `scheduler_start()` or the step-based API.

---

### Simple

The minimal usage pattern: spawn one coroutine and run the scheduler.

```cpp
#include "corocgo.h"
#include <iostream>
using namespace std;
using namespace corocgo;

int main() {
    cout << "start\n";
    coro([]() {
        cout << "coroutine executed\n";
    });
    scheduler_start();
    cout << "end\n";
}
```

Output:
```
start
coroutine executed
end
```

Other way to make main function also a coroutine:
```cpp
#include "corocgo.h"
#include <iostream>
using namespace std;
using namespace corocgo;

void _main() {
    cout << "start\n";
    coro([]() {
        cout << "coroutine executed\n";
    });
}

int main(int argc, const char * argv[]) {
    coro(_main);
    scheduler_start();
    cout << "end\n";
}
```

`scheduler_start()` blocks until all coroutines have finished. Code after it runs on the main thread once the scheduler exits.

---

### Step-Based Scheduling (Embedded-Friendly)

Use `scheduler_init()`, `scheduler_step()`, and `scheduler_stop()` when you need the external system to retain control of the main loop. `scheduler_step()` performs one non-blocking tick and returns `true` while coroutines are still alive.

```cpp
#include "corocgo.h"
#include <iostream>
using namespace std;
using namespace corocgo;

void _main() {
    cout << "start\n";
    coro([]() {
        cout << "coroutine executed\n";
    });
}

int main() {
    coro(_main);

    scheduler_init();
    while (scheduler_step()) {
        // external system can do work here:
        // read sensors, handle interrupts, run other subsystems, etc.
    }
    scheduler_stop();

    cout << "end\n";
}
```

Output:
```
start
coroutine executed
end
```

This pattern is suitable for Arduino-style `setup()`/`loop()` environments:

```cpp
void setup() {
    coro(my_main);
    scheduler_init();
}

void loop() {
    if (!scheduler_step()) {
        scheduler_stop();
    }
    // handle other hardware tasks
}
```

---

### Sleep in a Coroutine

`sleep(ms)` suspends the current coroutine for the given number of milliseconds. Other ready coroutines run while it sleeps.

```cpp
coro([]() {
    cout << "A: before sleep\n";
    sleep(300);
    cout << "A: after sleep\n";
});

coro([]() {
    cout << "B: running while A sleeps\n";
});

scheduler_start();
```

Output:
```
A: before sleep
B: running while A sleeps
A: after sleep
```

---

### Coroutine Spawning Another Coroutine

A coroutine can call `coro()` to spawn a new coroutine. The new coroutine is added to the run queue and will execute after the spawning coroutine yields or finishes.

```cpp
coro([]() {
    cout << "outer: start\n";
    coro([]() {
        cout << "inner: executed\n";
    });
    cout << "outer: end\n";
});

scheduler_start();
```

Output:
```
outer: start
outer: end
inner: executed
```

---

### Channel

Channels transfer values between coroutines. `send()` blocks if the buffer is full; `receive()` blocks if it is empty. `ChannelResult<T>` has a `.value` field and an `.error` field that is `true` when the channel is closed and empty.

```cpp
auto* ch = makeChannel<int>(4);

coro([ch]() {
    for (int i = 1; i <= 3; i++) {
        ch->send(i);
        cout << "sent: " << i << "\n";
    }
    ch->close();
});

coro([ch]() {
    while (true) {
        auto [value, result] = ch->receive();
        if (error) break;          // channel closed and drained
        cout << "received: " << value << "\n";
    }
    delete ch;
});

scheduler_start();
```

Output:
```
sent: 1
sent: 2
sent: 3
received: 1
received: 2
received: 3
```

(Actual interleaving depends on scheduling order.)

---

### Select from Several Channels

`select` blocks until any of the listed channels has a value. The `Recv` helper pairs a channel with a handler lambda. The loop exits when `SELECT_CLOSED` is returned, which happens once all channels are closed and drained.

```cpp
auto* chInts = makeChannel<int>(2);
auto* chStrs = makeChannel<string>(2);

coro([chInts]() {
    for (int i = 1; i <= 3; i++) {
        sleep(100);
        chInts->send(i);
    }
    chInts->close();
});

coro([chStrs]() {
    for (auto s : {"hello", "world", "done"}) {
        sleep(150);
        chStrs->send(string(s));
    }
    chStrs->close();
});

coro([chInts, chStrs]() {
    while (true) {
        int which = select(
            Recv(chInts, [](int v)    { cout << "int:  " << v << "\n"; }),
            Recv(chStrs, [](string v) { cout << "str:  " << v << "\n"; })
        );
        if (which == SELECT_CLOSED) break;
    }
    delete chInts;
    delete chStrs;
});

scheduler_start();
```

A `Default` case can be added as the last argument to make `select` non-blocking — if no channel is immediately ready it executes the default handler and returns `SELECT_DEFAULT`.

---

### Sending from an External Thread

When a value originates outside the scheduler (e.g. from a worker thread or OS callback), create the channel with a non-zero `extSize` and use `sendExternalNoBlock()`. The channel must have `extSize > 0`; otherwise the call returns `false`.

`exec_thread` runs a lambda on the thread pool and suspends the calling coroutine until the provided `wake` callback is called from inside the lambda.

```cpp
auto* ch = makeChannel<int>(4, 4);   // 4-slot internal buffer, 4-slot external buffer

coro([ch]() {
    // Run blocking work on a thread pool worker; suspend until wake() is called
    exec_thread([ch](auto wake) {
        for (int i = 1; i <= 3; i++) {
            this_thread::sleep_for(chrono::milliseconds(100));
            ch->sendExternalNoBlock(i);
        }
        wake();
    });
    ch->close();
});

coro([ch]() {
    while (true) {
        auto [value, error] = ch->receive();
        if (error) break;
        cout << "received from thread: " << value << "\n";
    }
    delete ch;
});

scheduler_start();
```

Output:
```
received from thread: 1
received from thread: 2
received from thread: 3
```

`sendExternalNoBlock` is non-blocking and returns `false` if the external buffer is full. The caller is responsible for handling backpressure (retry, drop, etc.).

---

### Waking a Coroutine from an External System

`coro_sleep_wake()` returns a `CoroSleepHandler` that owns both sides of the suspend/resume pair. Call `sleep()` on it to suspend the coroutine and `wake()` from any thread to resume it.

```cpp
coro([]() {
    CoroSleepHandler h = coro_sleep_wake();

    // Pass the handle to an external system.
    // CoroSleepHandler is trivially copyable — safe to capture by value in a lambda.
    some_async_operation([h]() {
        // called from any thread when the operation completes
        h.wake();
    });

    h.sleep();   // suspend here; resumes when h.wake() is called
    // execution continues here after the external callback fires
});
```

Qt example — update a widget from a coroutine:

```cpp
coro([]() {
    CoroSleepHandler h = coro_sleep_wake();

    QMetaObject::invokeMethod(qApp, [h]() {
        label->setText("Hello from coroutine");
        h.wake();   // resumes the coroutine on the scheduler thread
    }, Qt::QueuedConnection);

    h.sleep();
    // label has been updated by the time execution reaches here
});
```

**Thread safety.** `wake()` is safe to call from any OS thread. Multiple calls are idempotent — only the first one that finds the coroutine parked will reschedule it.

**Wake before sleep.** Calling `wake()` before `sleep()` is safe. The pending wake is queued and the coroutine resumes on the very next scheduler tick after `sleep()` is called.
