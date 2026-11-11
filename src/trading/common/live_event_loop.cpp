#include "trading/common/live_event_loop.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace trading {
namespace {

constexpr std::size_t kMaxDrainBatchSize = 128;

int priority(const RuntimeMessage& message) {
  return std::visit([](const auto& event) {
    using T = std::decay_t<decltype(event)>;
    if constexpr (std::is_same_v<T, TradingStateChanged>) {
      return 2;
    } else if constexpr (std::is_same_v<T, OrderAccepted> ||
                         std::is_same_v<T, OrderUpdated> ||
                         std::is_same_v<T, OrderFilled> ||
                         std::is_same_v<T, OrderCanceled> ||
                         std::is_same_v<T, OrderDenied> ||
                         std::is_same_v<T, OrderRejected> ||
                         std::is_same_v<T, OrderExpired>) {
      return 1;
    } else {
      static_assert(std::is_same_v<T, QuoteTick> ||
                    std::is_same_v<T, TradeTick> ||
                    std::is_same_v<T, Bar> ||
                    std::is_same_v<T, OrderBookDeltas> ||
                    std::is_same_v<T, OrderBookDepth>);
      return 0;
    }
  }, message);
}

}  // namespace

void LiveEventLoop::bind(Handler handler) { handler_ = std::move(handler); }

bool LiveEventLoop::send(RuntimeMessage message) {
  auto queued = std::make_unique<RuntimeMessage>(std::move(message));
  queue_.enqueue(std::move(queued));
  scheduleDrain();
  return true;
}

void LiveEventLoop::scheduleDrain() {
  if (!drain_scheduled_.exchange(true, std::memory_order_acq_rel)) {
    event_base_.runInEventBaseThreadAlwaysEnqueue([this] { drain(); });
  }
}

Scheduler::TimerId LiveEventLoop::scheduleAfter(std::chrono::milliseconds delay,
                                              Callback callback) {
  if (std::this_thread::get_id() != owner_) {
    throw std::logic_error("scheduler accessed outside the core thread");
  }
  if (timers_stopped_ || !callback || delay.count() <= 0 ||
      delay.count() > std::numeric_limits<std::uint32_t>::max() ||
      timers_.size() >= 1024 || next_timer_id_ == std::numeric_limits<TimerId>::max()) {
    throw std::invalid_argument("invalid timer or scheduler capacity exceeded");
  }
  const auto id = ++next_timer_id_;
  const auto task = std::make_shared<Callback>(std::move(callback));
  timers_.emplace(id, task);
  const std::weak_ptr<Callback> weak = task;
  try {
    event_base_.runAfterDelay([this, id, weak] {
      if (const auto task = weak.lock()) {
        timers_.erase(id);
        try {
          (*task)();
        } catch (...) {
          callback_error_ = std::current_exception();
          stop();
        }
      }
    }, static_cast<std::uint32_t>(delay.count()));
  } catch (...) {
    timers_.erase(id);
    throw;
  }
  return id;
}

void LiveEventLoop::cancel(TimerId timer_id) noexcept {
  timers_.erase(timer_id);
}

void LiveEventLoop::drain() noexcept {
  if (callback_error_) {
    return;
  }

  try {
    std::vector<std::unique_ptr<RuntimeMessage>> batch;
    batch.reserve(kMaxDrainBatchSize);
    std::unique_ptr<RuntimeMessage> message;
    while (batch.size() < kMaxDrainBatchSize && queue_.try_dequeue(message)) {
      batch.push_back(std::move(message));
    }

    std::stable_sort(batch.begin(), batch.end(), [](const auto& lhs, const auto& rhs) {
      return priority(*lhs) > priority(*rhs);
    });

    for (auto& event : batch) {
      handler_(std::move(*event));
    }

    if (batch.size() == kMaxDrainBatchSize) {

      // Keep the wakeup reservation while yielding to the event loop
      event_base_.runInLoop([this] { drain(); });
      return;
    }

    drain_scheduled_.exchange(false, std::memory_order_acq_rel);
    if (!queue_.empty()) {
      scheduleDrain();
    }
  } catch (...) {
    callback_error_ = std::current_exception();
    stop();
  }
}

void LiveEventLoop::run() {
  if (!handler_) {
    throw std::logic_error("LiveEventLoop must be bound before run()");
  }
  event_base_.loopForever();
  if (callback_error_) {
    std::rethrow_exception(callback_error_);
  }
}

void LiveEventLoop::stop() {
  event_base_.runInEventBaseThread([this] {
    timers_stopped_ = true;
    timers_.clear();
    event_base_.terminateLoopSoon();
  });
}

}  // namespace trading
