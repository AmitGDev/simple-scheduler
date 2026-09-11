# SimpleScheduler

A lightweight, dependency-free, header-only timer scheduler built on `std::jthread`. Timers execute callbacks on a single dedicated thread, keeping scheduling independent from the caller's thread.

## Purpose

`SimpleScheduler` provides a small abstraction for scheduling one-shot callbacks after a specified delay.

It is intended for applications that need delayed or background timer-based work without pulling in an event-loop library or managing worker threads themselves.

The scheduler supports:

- Lambda callbacks
- Function objects
- Free-function callbacks
- Member-function callbacks
- Additional callback arguments
- Self-rescheduling timers

## Interface

```cpp
template <typename Callback, typename... Args>
  requires NothrowTimerCallback<Callback, Args...>
void ScheduleTimer(std::uint64_t timer_id, std::uint32_t duration,
                   Callback&& callback, Args&&... callback_args);
```

- `timer_id`: Application-defined identifier, passed to the callback as its first argument
- `duration`: Delay in milliseconds (`std::uint32_t`, up to about 49.7 days)
- `callback`: Any callable that is `noexcept` when invoked as an rvalue with `(std::uint64_t, Args...)`
- `callback_args`: Extra arguments, stored by value in the timer and passed to the callback as rvalues

`SimpleScheduler` itself is neither copyable nor movable.

## Design Rationale

### Dedicated scheduler thread

The scheduler owns a single `std::jthread` that waits for the earliest deadline and runs expired callbacks.

This keeps timer processing independent from the thread that creates or schedules the timers:

```text
Application thread
       |
       | ScheduleTimer(...)
       |
       | continues working
       |
       v

Scheduler thread
       |
       | waits for earliest deadline
       |
       | invokes callback
       v
```

The application therefore does not need to run or integrate an event loop itself.

### Standard library only

The scheduler uses only standard components:

- `std::jthread` and `std::stop_token`
- `std::mutex`
- `std::condition_variable_any`
- `std::multimap`
- `std::move_only_function`

`std::chrono::steady_clock` is used because timer durations should be based on a monotonic clock rather than changes to the system wall clock.

`std::condition_variable_any` is used instead of `std::condition_variable` because only the former has `wait` and `wait_until` overloads that accept a `std::stop_token`. This lets a stop request wake the worker directly, with no separate shutdown flag or extra notification.

### Deadline-ordered queue

Pending timers are stored in a `std::multimap` keyed by expiry time, so the earliest timer is always at the front. A multimap is used (rather than a map) so that timers with identical deadlines are kept and fire in the order they were scheduled.

The worker sleeps until the front deadline. When `ScheduleTimer()` inserts a timer, it notifies the worker, because the new timer may expire before the one the worker is currently waiting for. The worker then re-evaluates its deadline.

### Callback ownership

Each callback and its arguments are moved into a type-erased task when the timer is scheduled. The caller's objects therefore do not need to outlive the `ScheduleTimer()` call.

The task type is `std::move_only_function<void() noexcept>`, which allows move-only callbacks and arguments.

### Callback exceptions

Timer callbacks execute on the scheduler thread and must be `noexcept`. This requirement is enforced at compile time by the `NothrowTimerCallback` concept, so a callable that is not `noexcept` is rejected by `ScheduleTimer()`. The concept checks the callable the way the scheduler will invoke it: as an rvalue, with rvalue copies of the arguments.

The task type also carries `noexcept`, so the contract cannot be bypassed. If an exception escapes a callback despite the contract, `std::terminate()` is called and the process ends, instead of unwinding into the worker loop and leaving the scheduler in an undefined state.

Callbacks are responsible for handling their own errors. This also applies to work done inside a callback: `ScheduleTimer()` is not `noexcept` (for example, it can throw on allocation failure), so a callback that reschedules itself must catch exceptions from it.

## Usage

Include the scheduler header:

```cpp
#include "SimpleScheduler.hpp"
```

Create a scheduler and schedule a timer:

```cpp
amitgdev::SimpleScheduler scheduler;

scheduler.ScheduleTimer(
    1,
    2000,
    [](std::uint64_t timer_id, int value) noexcept {
        std::cout << "Timer " << timer_id
                  << " expired: " << value << '\n';
    },
    42);
```

The callback receives the timer ID as its first argument, followed by any additional arguments supplied to `ScheduleTimer()`. The delay is in milliseconds, so the timer above fires after two seconds.

The examples in this document keep callback bodies short. A callback that performs operations which can throw (such as stream output) should catch exceptions itself; `main.cpp` wraps each callback body in `try`/`catch` and reports failures through a small helper built on `std::fputs`, which cannot throw.

### Function callback

A regular function can be used directly:

```cpp
void OnTimer(std::uint64_t timer_id) noexcept {
    std::cout << "Timer " << timer_id << " expired\n";
}

amitgdev::SimpleScheduler scheduler;

scheduler.ScheduleTimer(1, 2000, OnTimer);
```

### Functor callback

Function objects are also supported:

```cpp
struct TimerHandler {
    void operator()(std::uint64_t timer_id, int value) const noexcept {
        std::cout << "Timer " << timer_id
                  << " expired: " << value << '\n';
    }
};

TimerHandler handler;

scheduler.ScheduleTimer(1, 2000, handler, 42);
```

### Member-function callback

A member function can be scheduled together with its object using `std::bind_front` (from `<functional>`). The scheduler requires a `noexcept` callable, so this relies on `std::bind_front` propagating `noexcept` from the bound member function. libstdc++ does this only from GCC 15; older versions return a callable that is not `noexcept`, and `ScheduleTimer()` rejects it (see Requirements for the workaround):

```cpp
class Model final {
 public:
    void Start() {
        scheduler_.ScheduleTimer(
            1, 1000, std::bind_front(&Model::OnTimer, this), 42);
    }

 private:
    void OnTimer(std::uint64_t timer_id, int value) noexcept {
        // Handle timer expiration.
    }

    // Declared last: members are destroyed in reverse order, so the
    // scheduler (and its thread) is shut down before the other members.
    amitgdev::SimpleScheduler scheduler_;
};
```

The scheduler does not take ownership of the object. The caller must ensure that the object remains alive until the timer callback has completed. Owning the `SimpleScheduler` as the last data member of the target object, as above, ties the two lifetimes together.

## Rescheduling

Timers are one-shot. Recurring behavior can be implemented by scheduling another timer from the callback:

```cpp
void OnTimer(std::uint64_t timer_id, std::uint8_t count_down) noexcept {
    if (count_down == 0) {
        return;
    }

    try {
        std::osyncstream(std::cout) << "timer " << timer_id << " expired\n";

        scheduler_.ScheduleTimer(
            1, 1000, std::bind_front(&Model::OnTimer, this),
            static_cast<std::uint8_t>(count_down - 1));
    } catch (...) {
        // ScheduleTimer() can throw; the callback is noexcept, so the
        // exception must not escape.
    }
}
```

The first timer is scheduled with an initial count (for example, `static_cast<std::uint8_t>(5)`), which is passed as the extra argument.

Calling `ScheduleTimer()` from inside a callback is safe: the worker releases its lock before running a task, so the call does not deadlock.

This keeps the scheduler itself simple while allowing the caller to control the recurrence policy.

For example, the callback can implement:

```text
Timer
  |
  +-- 1 second --> callback(5)
                       |
                       +-- 1 second --> callback(4)
                                          |
                                          +-- 1 second --> callback(3)
                                                             |
                                                             +-- ...
```

## Timer IDs

The `timer_id` is an application-defined identifier passed to the callback.

`SimpleScheduler` does not maintain a timer registry and does not enforce uniqueness. The ID is not a handle and cannot be used to cancel a timer.

For example:

```cpp
scheduler.ScheduleTimer(1, 1000, callback);
scheduler.ScheduleTimer(2, 2000, callback);
```

The callback can use the ID to determine which logical timer expired.

If timer IDs must be unique, that policy is the responsibility of the application.

## Threading Model

All timer callbacks are executed by the scheduler's single worker thread, one at a time, in deadline order. Timers with the same deadline run in the order they were scheduled. A long-running callback delays every timer that expires while it is running.

Timers can be scheduled from the application thread and from inside callbacks. `ScheduleTimer()` is thread-safe; the queue is protected by an internal mutex that is never held while a callback runs.

Callbacks should still synchronize access to application state that is also accessed by other threads. The scheduler does not provide synchronization for application-owned data.

## Shutdown

The scheduler stops its worker thread when it is destroyed:

```cpp
{
    amitgdev::SimpleScheduler scheduler;

    scheduler.ScheduleTimer(1, 2000, callback);
}
```

Destruction:

1. Requests stop on the worker's `std::stop_token` (done by `std::jthread`).
2. Wakes the worker if it is sleeping, whether it is waiting for a deadline or for new work.
3. Joins the worker thread.
4. Destroys any pending timers, together with their stored arguments, without running them.

A callback that is already running completes before the destructor returns. Pending timers are never run during shutdown, so destruction does not block on far-future timers.

Do not destroy the scheduler from inside one of its own callbacks: the worker would try to join itself.

Likewise, the caller must ensure no other thread is inside `ScheduleTimer()` when the scheduler is destroyed: the internal mutex does not outlive the object. Destroying the scheduler from another thread while a callback is running is fine; the destructor waits for that callback.

## Examples

The repository includes a complete example demonstrating:

- Lambda callbacks
- Functor callbacks
- Free-function callbacks
- Member-function callbacks (`std::bind_front`)
- Additional callback arguments
- Timer self-rescheduling
- Exception-safe error reporting from `noexcept` callbacks
- Edge cases, checked at runtime: zero delay, deadline and same-delay ordering, waking a sleeping worker for an earlier timer, and shutdown with pending timers

See [`main.cpp`](https://github.com/AmitGDev/simple-scheduler/blob/main/src/main.cpp).

## Scope

`SimpleScheduler` deliberately provides only timer scheduling. It does not attempt to be a general-purpose task scheduler.

It does not provide:

- Timer cancellation
- Timer ID registration or uniqueness enforcement
- Recurring timers (rescheduling is done by the callback)
- Cron-style scheduling
- Calendar-based scheduling
- A thread pool (all callbacks share one worker thread)
- Asynchronous I/O
- Ownership of callback target objects
- Synchronization of application-owned state

These responsibilities remain with the application. If you need cancellation, multiple worker threads, or I/O on the same event loop, a Boost.Asio-based design is a better fit.

## Requirements

- A C++23-compatible compiler (GCC 16 or later on Linux) and standard library
- A standard library that provides `std::jthread`, `std::condition_variable_any` stop-token overloads, and `std::move_only_function` (for example, Visual Studio 2022 17.2 or later on Windows)
- No third-party dependencies

Note: The member-function examples use `std::bind_front`, which must propagate `noexcept` to satisfy `NothrowTimerCallback`. Before GCC 15, libstdc++ does not do this. On an older toolchain, schedule a `noexcept` lambda instead:

```cpp
scheduler_.ScheduleTimer(
    1, 1000,
    [this](std::uint64_t timer_id, int value) noexcept {
        OnTimer(timer_id, value);
    },
    42);
```

## Build and Run

- [Build on Windows](https://github.com/AmitGDev/cpp-ci-template#build-on-windows)
- [Build on Linux](https://github.com/AmitGDev/cpp-ci-template#build-on-linux)

## Code Style

- [C++](https://github.com/AmitGDev/cpp-coding-standard)
- [Python](https://github.com/AmitGDev/cpp-ci-template#python)
- [CMake](https://github.com/AmitGDev/cpp-ci-template#cmake)

## License

MIT License. Copyright © 2026 Amit Gefen.

---

**Last Updated:** 2026-10-25  
**Maintainer:** AmitGDev
