// SPDX-License-Identifier: MIT
// Copyright (c) 2026, Amit Gefen

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <syncstream>
#include <thread>

#include "SimpleScheduler.hpp"

using amitgdev::SimpleScheduler;

// fputs instead of std::println: the error path must not throw again.
static void ReportError(const char* what) noexcept {
  (void)std::fputs(what, stderr);
  (void)std::fputs("\n", stderr);
}

static void TestGenericCallback() {
  std::cout << "* test lambda & functor callbacks\n";

  // Lambda callback
  const auto lambda_callback = [](std::uint64_t timer_id, int value) noexcept {
    try {
      std::osyncstream sync_stream(std::cout);
      sync_stream << "lambda callback for timer " << timer_id
                  << " expired. int value: " << value << '\n';
    } catch (const std::exception& e) {
      ReportError(e.what());
    }
  };

  // Functor callback
  struct MyFunctor {
    void operator()(std::uint64_t timer_id, const std::string& str,
                    int value) const noexcept {
      try {
        std::osyncstream sync_stream(std::cout);
        sync_stream << "functor callback for timer " << timer_id
                    << " expired. string data: " << str
                    << " int value: " << value << '\n';
      } catch (const std::exception& e) {
        ReportError(e.what());
      }
    }
  };

  SimpleScheduler scheduler{};

  // Using lambda callback
  scheduler.ScheduleTimer(1, 2000, lambda_callback, 42);

  // Using functor callback
  const auto functor = MyFunctor{};
  scheduler.ScheduleTimer(2, 4000, functor, std::string("test functor string"),
                          2024);

  // Sleep for a while to let the timers expire
  std::this_thread::sleep_for(std::chrono::seconds(6));
}

// Function Callback

static void OnTimer(const std::uint64_t timer_id) noexcept {
  try {
    std::osyncstream sync_stream(std::cout);
    sync_stream << "timer " << timer_id << " expired\n";
  } catch (const std::exception& e) {
    ReportError(e.what());
  }
}

static void TestFunctionCallback2Timers() {
  std::cout << "* test \"traditional\" function callback\n";

  SimpleScheduler scheduler{};

  scheduler.ScheduleTimer(1, 2000, OnTimer);
  scheduler.ScheduleTimer(2, 4000, OnTimer);

  // Sleep for a while to let the timers expire
  std::this_thread::sleep_for(std::chrono::seconds(6));
}

// Member function callback, extra parameter, and rescheduling

static void TestMemberFunctionCallbackPlusExtraParameterPlusReschedule() {
  std::cout << "* test member function & instance callback\n";

  class Model final {
   public:
    void Test() {
      std::cout << "* test re-activate the timer 5 times\n";

      scheduler_.ScheduleTimer(1, 1000, std::bind_front(&Model::OnTimer, this),
                               static_cast<std::uint8_t>(5));

      std::this_thread::sleep_for(std::chrono::seconds(6));
    }

   private:
    SimpleScheduler scheduler_;

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void OnTimer(const std::uint64_t timer_id,
                 const std::uint8_t count_down) noexcept {
      if (count_down == 0) {
        return;
      }

      try {
        std::osyncstream sync_stream(std::cout);
        sync_stream << "timer " << timer_id << " expired\n";

        const auto next = static_cast<std::uint8_t>(count_down - 1);
        scheduler_.ScheduleTimer(1, 1000,
                                 std::bind_front(&Model::OnTimer, this), next);
      } catch (const std::exception& e) {
        ReportError(e.what());
      }
    }
  };

  Model model{};
  model.Test();
}

namespace {

// Records the order in which timers fire as the decimal digits of one integer
// (timer IDs 1..9), so a callback needs no container, lock or allocation and
// stays trivially noexcept.
class FireRecorder final {
 public:
  void Record(const std::uint64_t timer_id) noexcept {
    // Callbacks are serialized on the scheduler's single worker thread, so
    // this read-modify-write has one writer and cannot lose an update.
    sequence_.store((sequence_.load() * 10) + timer_id);
    // Incremented last so a reader that sees the new count also sees the
    // matching sequence.
    count_.fetch_add(1);
  }

  [[nodiscard]] std::uint64_t Sequence() const noexcept {
    return sequence_.load();
  }

  [[nodiscard]] std::size_t Count() const noexcept { return count_.load(); }

 private:
  std::atomic<std::uint64_t> sequence_{0};
  std::atomic<std::size_t> count_{0};
};

}  // namespace

// Builds a noexcept callback that reports into the given recorder.
static auto MakeRecordingCallback(FireRecorder& recorder) {
  return [&recorder](std::uint64_t timer_id) noexcept {
    recorder.Record(timer_id);
  };
}

// Throws instead of asserting so a failure flows through main()'s existing
// error path and produces EXIT_FAILURE, which CI can detect.
static void Expect(const bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

// Polls instead of blocking on a signalling primitive, which keeps the
// noexcept callbacks down to plain atomic operations.
template <typename Predicate>
static bool WaitUntil(const Predicate& predicate,
                      const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

// In the tests below the recorder is declared before the scheduler so the
// worker is joined before the recorder it writes to is destroyed.

static void TestZeroDelay() {
  std::cout << "* test end case: zero delay\n";

  FireRecorder recorder;
  SimpleScheduler scheduler{};

  scheduler.ScheduleTimer(1, 0, MakeRecordingCallback(recorder));

  Expect(WaitUntil([&recorder] { return recorder.Count() == 1; },
                   std::chrono::seconds(1)),
         "zero-delay timer did not fire");
}

static void TestDeadlineOrdering() {
  std::cout << "* test end case: fires in deadline order\n";

  FireRecorder recorder;
  SimpleScheduler scheduler{};

  // Scheduled out of deadline order to prove the queue sorts by expiry, not
  // by insertion.
  scheduler.ScheduleTimer(3, 300, MakeRecordingCallback(recorder));
  scheduler.ScheduleTimer(1, 100, MakeRecordingCallback(recorder));
  scheduler.ScheduleTimer(2, 200, MakeRecordingCallback(recorder));

  Expect(WaitUntil([&recorder] { return recorder.Count() == 3; },
                   std::chrono::seconds(2)),
         "not all timers fired");
  Expect(recorder.Sequence() == 123, "timers did not fire in deadline order");
}

static void TestSameDelayOrdering() {
  std::cout << "* test end case: same delay keeps scheduling order\n";

  FireRecorder recorder;
  SimpleScheduler scheduler{};

  // The API takes a delay, not a deadline, so identical deadlines cannot be
  // forced. steady_clock is monotonic, so back-to-back calls with the same
  // delay yield non-decreasing deadlines, and the multimap keeps equal ones
  // in insertion order.
  scheduler.ScheduleTimer(1, 100, MakeRecordingCallback(recorder));
  scheduler.ScheduleTimer(2, 100, MakeRecordingCallback(recorder));
  scheduler.ScheduleTimer(3, 100, MakeRecordingCallback(recorder));

  Expect(WaitUntil([&recorder] { return recorder.Count() == 3; },
                   std::chrono::seconds(2)),
         "not all timers fired");
  Expect(recorder.Sequence() == 123,
         "timers with the same delay did not fire in scheduling order");
}

static void TestEarlierTimerPreemptsSleep() {
  std::cout << "* test end case: earlier timer wakes a sleeping worker\n";

  FireRecorder recorder;
  SimpleScheduler scheduler{};

  scheduler.ScheduleTimer(1, 60000, MakeRecordingCallback(recorder));

  // Let the worker settle into its sleep on the 60 s deadline first;
  // otherwise the test would pass without exercising the wake-up path.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  scheduler.ScheduleTimer(2, 100, MakeRecordingCallback(recorder));

  Expect(WaitUntil([&recorder] { return recorder.Count() == 1; },
                   std::chrono::seconds(2)),
         "earlier timer did not wake the worker");
  Expect(recorder.Sequence() == 2, "the wrong timer fired");
}

static void TestShutdownWithPendingTimer() {
  std::cout << "* test end case: shutdown drops pending timers\n";

  FireRecorder recorder;
  const auto start = std::chrono::steady_clock::now();

  {
    SimpleScheduler scheduler{};
    scheduler.ScheduleTimer(1, 60000, MakeRecordingCallback(recorder));
  }

  // Shutdown must not wait for far-future timers, and must not run them.
  const auto elapsed = std::chrono::steady_clock::now() - start;
  Expect(elapsed < std::chrono::seconds(5),
         "destruction blocked on a pending timer");
  Expect(recorder.Count() == 0, "a pending timer ran during shutdown");
}

static void TestEndCases() {
  std::cout << "* test end cases\n";

  TestZeroDelay();
  TestDeadlineOrdering();
  TestSameDelayOrdering();
  TestEarlierTimerPreemptsSleep();
  TestShutdownWithPendingTimer();

  std::cout << "* end cases passed\n";
}

int main() {
  try {
    TestGenericCallback();
    TestFunctionCallback2Timers();
    TestMemberFunctionCallbackPlusExtraParameterPlusReschedule();
    TestEndCases();
    return EXIT_SUCCESS;
  } catch (const std::exception& e) {
    ReportError(e.what());
    return EXIT_FAILURE;
  }
}
