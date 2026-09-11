/*
    SimpleScheduler.hpp
    Copyright (c) 2026, Amit Gefen

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#ifndef AMITGDEV_SIMPLE_SCHEDULER_HPP_
#define AMITGDEV_SIMPLE_SCHEDULER_HPP_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

namespace amitgdev {

// Checked against the decayed types because the stored task invokes its own
// copy of the callback as an rvalue, with rvalue copies of the arguments, not
// the caller's lvalues. Checking the deduced types (F&, T&) would wrongly
// reject callbacks taking rvalue references and wrongly accept ones that are
// only invocable as lvalues.
template <typename Callback, typename... Args>
concept NothrowTimerCallback =
    std::is_nothrow_invocable_v<std::decay_t<Callback>, std::uint64_t,
                                std::decay_t<Args>...>;

// One-shot timer scheduler backed by a single worker thread and no external
// dependencies. Timer callbacks run on that worker thread, which keeps them
// off the caller's thread and serializes them with each other.
//
// Contract: all callbacks must be noexcept. The task type encodes this, so a
// callback that throws anyway calls std::terminate() instead of unwinding
// into the worker loop and leaving the scheduler in an undefined state.
//
// Lifetime: the caller must ensure no other thread is inside ScheduleTimer()
// when the scheduler is destroyed (the mutex does not outlive the object).
// Destroying it from another thread while a callback is running is fine; the
// destructor waits for that callback. Destroying it from inside one of its own
// callbacks is not, because the worker would try to join itself.
class SimpleScheduler final {
 public:
  SimpleScheduler()
      : worker_([this](const std::stop_token& stop_token) {
          Service(stop_token);
        }) {}

  // The jthread requests stop and joins on destruction. Pending timers are
  // dropped without running, matching "pending handlers are not guaranteed
  // to execute" semantics: shutdown must not block on far-future timers.
  ~SimpleScheduler() = default;

  SimpleScheduler(const SimpleScheduler&) = delete;
  SimpleScheduler& operator=(const SimpleScheduler&) = delete;
  SimpleScheduler(SimpleScheduler&&) = delete;
  SimpleScheduler& operator=(SimpleScheduler&&) = delete;

  // Schedules a one-shot timer. The callback receives the timer ID followed by
  // the arguments supplied to this function. The timer ID is only passed
  // through; it is not a handle and there is no cancellation.
  //
  // Safe to call from inside a callback (re-arming a timer), because the
  // worker releases the lock before running a task.
  //
  // Important: The callback must be noexcept.
  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  template <typename Callback, typename... Args>
    requires NothrowTimerCallback<Callback, Args...>
  void ScheduleTimer(const std::uint64_t timer_id, const std::uint32_t duration,
                     Callback&& callback, Args&&... callback_args) {
    // NOLINTEND(bugprone-easily-swappable-parameters)

    // Arguments are moved into the task now, so the caller's objects need not
    // outlive this call.
    Task task = [timer_id, callback = std::forward<Callback>(callback),
                 ... callback_args =
                     std::forward<Args>(callback_args)] mutable noexcept {
      std::move(callback)(timer_id, std::move(callback_args)...);
    };

    {
      const std::scoped_lock lock(mutex_);
      tasks_.emplace(Clock::now() + std::chrono::milliseconds(duration),
                     std::move(task));
    }

    // The new timer may expire before the one the worker is sleeping on, so
    // the worker must re-evaluate its deadline.
    wake_.notify_one();
  }

 private:
  using Clock = std::chrono::steady_clock;

  // Encodes the noexcept contract in the type so it cannot be bypassed.
  using Task = std::move_only_function<void() noexcept>;

  // Runs on the dedicated worker thread. Sleeps until the earliest deadline,
  // a sooner timer arrives, or stop is requested, whichever comes first.
  void Service(const std::stop_token& stop_token) {
    std::unique_lock lock(mutex_);

    while (!stop_token.stop_requested()) {
      if (tasks_.empty()) {
        // Nothing to time; sleep until work arrives or stop is requested.
        wake_.wait(lock, stop_token, [this] { return !tasks_.empty(); });
        continue;
      }

      const Clock::time_point deadline = tasks_.begin()->first;

      // Wake early only if a timer earlier than the current front arrived.
      // A false result means the deadline passed or stop was requested.
      const bool earlier_arrived =
          wake_.wait_until(lock, stop_token, deadline, [this, deadline] {
            return !tasks_.empty() && tasks_.begin()->first < deadline;
          });
      if (earlier_arrived) {
        continue;
      }
      if (stop_token.stop_requested()) {
        break;
      }

      // Extract before unlocking so the task is owned by this thread and
      // cannot be touched by producers while it runs.
      const auto node = tasks_.extract(tasks_.begin());

      // Unlocked while running so a callback can reschedule itself without
      // deadlocking, and so slow callbacks do not block ScheduleTimer().
      lock.unlock();
      node.mapped()();
      lock.lock();
    }
  }

  std::mutex mutex_;
  std::condition_variable_any wake_;

  // Ordered by deadline; multimap keeps insertion order for equal deadlines,
  // so timers with the same expiry fire in the order they were scheduled.
  std::multimap<Clock::time_point, Task> tasks_;

  // Must be declared last: the thread starts in the constructor's member
  // initialization and uses every member above, and on destruction it must be
  // joined before they are destroyed.
  std::jthread worker_;
};

}  // namespace amitgdev

#endif  // AMITGDEV_SIMPLE_SCHEDULER_HPP_
