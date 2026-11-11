#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "trading/cache/cache.h"
#include "trading/common/message_bus.h"
#include "trading/common/scheduler.h"

namespace trading {

enum class TwapStatus { Running, Completed, Stopped, Failed };

struct TwapExecutionState {
  SubmitOrder primary_order;
  TwapStatus status{TwapStatus::Running};
  std::vector<ClientOrderId> child_order_ids;
  Quantity submitted_quantity;
  Quantity filled_quantity;
  std::string last_reason;
};

class TwapAlgorithm {
 public:
  using SubmitOrderHandler = std::function<void(const SubmitOrder&)>;
  static constexpr std::size_t kMaxSlices = 1024;
  static constexpr std::size_t kMaxExecutions = 1024;

  TwapAlgorithm(const Cache& cache, MessageBus& message_bus,
                Scheduler& scheduler, SubmitOrderHandler submit_order);
  ~TwapAlgorithm();
  TwapAlgorithm(const TwapAlgorithm&) = delete;
  TwapAlgorithm& operator=(const TwapAlgorithm&) = delete;

  void submit(const SubmitOrder& order);
  void process(const ClientOrderId& child_order_id, bool terminal_failure = false);
  bool cancel(const ClientOrderId& primary_order_id);
  void stop() noexcept;
  const TwapExecutionState* execution(const ClientOrderId& primary_order_id) const;
  bool ownsOrderId(const ClientOrderId& order_id) const;

 private:
  struct Schedule {
    TwapExecutionState state;
    std::vector<Quantity> quantities;
    std::size_t next_slice{0};
    Scheduler::TimerId timer_id{0};
  };

  void submitSlice(const std::shared_ptr<Schedule>& schedule);
  void fail(const std::shared_ptr<Schedule>& schedule, std::string reason);
  void deny(const SubmitOrder& order, const std::string& reason);

  const Cache& cache_;
  MessageBus& message_bus_;
  Scheduler& scheduler_;
  SubmitOrderHandler submit_order_;
  bool stopped_{false};
  std::shared_ptr<int> lifetime_{std::make_shared<int>(0)};
  std::unordered_map<ClientOrderId, std::shared_ptr<Schedule>> schedules_;
  std::unordered_map<ClientOrderId, ClientOrderId> child_to_primary_;
};

}  // namespace trading
