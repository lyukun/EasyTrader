#pragma once

#include <folly/concurrency/UnboundedQueue.h>
#include <folly/io/async/EventBase.h>

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>

#include "trading/common/event_sink.h"
#include "trading/common/scheduler.h"

namespace trading {

class LiveEventLoop final : public EventSink, public Scheduler {
 public:
  using Handler = std::function<void(RuntimeMessage)>;

  LiveEventLoop() = default;

  void bind(Handler handler);
  bool send(RuntimeMessage message) override;
  TimerId scheduleAfter(std::chrono::milliseconds delay, Callback callback) override;
  void cancel(TimerId timer_id) noexcept override;
  void run();
  void stop();

 private:
  void scheduleDrain();
  void drain() noexcept;

  folly::EventBase event_base_;
  folly::UMPSCQueue<std::unique_ptr<RuntimeMessage>, false> queue_;
  std::atomic<bool> drain_scheduled_{false};
  Handler handler_;
  std::thread::id owner_{std::this_thread::get_id()};
  TimerId next_timer_id_{0};
  std::unordered_map<TimerId, std::shared_ptr<Callback>> timers_;
  bool timers_stopped_{false};
  std::exception_ptr callback_error_;
};

}  // namespace trading
