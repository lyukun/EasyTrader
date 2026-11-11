#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

namespace trading {

// Scheduling and cancellation are owned by the core thread
class Scheduler {
 public:
  using TimerId = std::uint64_t;
  using Callback = std::function<void()>;

  virtual ~Scheduler() = default;
  virtual TimerId scheduleAfter(std::chrono::milliseconds delay,
                                Callback callback) = 0;
  virtual void cancel(TimerId timer_id) noexcept = 0;
};

}  // namespace trading
