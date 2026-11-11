#include "trading/execution/twap_algorithm.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace trading {

TwapAlgorithm::TwapAlgorithm(const Cache& cache, MessageBus& message_bus,
                             Scheduler& scheduler,
                             SubmitOrderHandler submit_order)
    : cache_(cache),
      message_bus_(message_bus),
      scheduler_(scheduler),
      submit_order_(std::move(submit_order)) {
  if (!submit_order_) {
    throw std::invalid_argument("TWAP submit order handler must be configured");
  }
}

TwapAlgorithm::~TwapAlgorithm() {
  lifetime_.reset();
  stop();
}

void TwapAlgorithm::deny(const SubmitOrder& order, const std::string& reason) {
  message_bus_.publish(
      OrderDenied{order.client_order_id, reason, order.ts_init});
}

void TwapAlgorithm::submit(const SubmitOrder& order) {
  message_bus_.assertOwner();
  if (ownsOrderId(order.client_order_id) ||
      cache_.order(order.client_order_id)) {
    throw std::invalid_argument("TWAP client order ID already exists");
  }
  if (stopped_) {
    deny(order, "TWAP is stopped");
    return;
  }
  if (order.client_order_id.empty() || order.strategy_id.empty() ||
      order.exec_algorithm_id != std::optional<ExecAlgorithmId>{"TWAP"} ||
      order.exec_spawn_id) {
    deny(order, "invalid TWAP primary order identity");
    return;
  }
  if (!order.twap || order.twap->interval.count() <= 0 ||
      order.twap->horizon < order.twap->interval ||
      order.twap->interval.count() >
          std::numeric_limits<std::uint32_t>::max()) {
    deny(order, "invalid TWAP horizon or interval");
    return;
  }
  if (order.order_type != OrderType::Limit ||
      (order.time_in_force != TimeInForce::Ioc &&
       order.time_in_force != TimeInForce::Fok)) {
    deny(order, "TWAP requires a limit order with IOC or FOK");
    return;
  }
  const auto* instrument = cache_.instrument(order.instrument_id);
  if (!instrument || order.quantity.isZero() || order.limit_price.raw() <= 0) {
    deny(order,
         "TWAP requires a cached instrument and positive quantity and price");
    return;
  }
  const auto lot = instrument->lot_size.raw();
  if (order.quantity.raw() % lot != 0 ||
      order.limit_price.raw() % instrument->tick_size.raw() != 0) {
    deny(order,
         "TWAP quantity or price is not aligned to instrument increments");
    return;
  }
  const auto requested =
      order.twap->horizon.count() / order.twap->interval.count();
  if (requested > static_cast<std::int64_t>(kMaxSlices) ||
      schedules_.size() >= kMaxExecutions) {
    deny(order, "TWAP execution or slice limit exceeded");
    return;
  }
  const auto lots = order.quantity.raw() / lot;
  const auto minimum_lots = instrument->min_quantity.raw() / lot +
                            (instrument->min_quantity.raw() % lot != 0);
  const auto count =
      std::min(static_cast<std::uint64_t>(requested), lots / minimum_lots);
  if (count == 0) {
    deny(order, "TWAP quantity is below the instrument minimum");
    return;
  }
  const auto per_slice = lots / count;
  const auto remainder = lots % count;
  if ((per_slice + (remainder != 0)) * lot > instrument->max_quantity.raw()) {
    deny(order, "TWAP child quantity exceeds the instrument maximum");
    return;
  }

  auto schedule = std::make_shared<Schedule>();
  schedule->state.primary_order = order;
  schedule->state.child_order_ids.reserve(count);
  schedule->quantities.reserve(count);
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto id = order.client_order_id + "-E" + std::to_string(i + 1);
    if (ownsOrderId(id) || cache_.order(id)) {
      deny(order, "TWAP child order ID already exists");
      return;
    }
    schedule->state.child_order_ids.push_back(id);
    schedule->quantities.push_back(
        Quantity::fromRaw((per_slice + (i < remainder)) * lot));
  }
  schedules_.emplace(order.client_order_id, schedule);
  for (const auto& id : schedule->state.child_order_ids) {
    child_to_primary_.emplace(id, order.client_order_id);
  }
  submitSlice(schedule);
}

void TwapAlgorithm::fail(const std::shared_ptr<Schedule>& schedule,
                         std::string reason) {
  if (schedule->state.status != TwapStatus::Running) {
    return;
  }
  // 调度失败，不会进行下一次调度
  schedule->state.status = TwapStatus::Failed;
  schedule->state.last_reason = std::move(reason);
  scheduler_.cancel(schedule->timer_id);
  schedule->timer_id = 0;
}

void TwapAlgorithm::submitSlice(const std::shared_ptr<Schedule>& schedule) {
  if (schedule->state.status != TwapStatus::Running ||
      schedule->next_slice == schedule->quantities.size()) {
    // 只有在 schedule 还在进行时，才能继续提单
    return;
  }
  schedule->timer_id = 0;
  const auto index = schedule->next_slice++;
  auto child = schedule->state.primary_order;
  child.client_order_id = schedule->state.child_order_ids[index];
  child.quantity = schedule->quantities[index];
  child.exec_spawn_id = schedule->state.primary_order.client_order_id;
  child.exec_algorithm_id.reset();
  child.twap.reset();
  schedule->state.submitted_quantity = Quantity::fromRaw(
      schedule->state.submitted_quantity.raw() + child.quantity.raw());

  try {
    submit_order_(child);
    if (schedule->state.status == TwapStatus::Running &&
        schedule->next_slice < schedule->quantities.size()) {
      const std::weak_ptr<int> lifetime = lifetime_;
      schedule->timer_id =
          scheduler_.scheduleAfter(schedule->state.primary_order.twap->interval,
                                   [this, lifetime, schedule] {
                                     if (!lifetime.expired()) {
                                       // 递归进行下一次
                                       submitSlice(schedule);
                                     }
                                   });
    }
  } catch (...) {
    fail(schedule, "TWAP child submission or scheduling failed");
    throw;
  }
}

void TwapAlgorithm::process(const ClientOrderId& child_order_id,
                            bool terminal_failure) {
  message_bus_.assertOwner();
  const auto child = child_to_primary_.find(child_order_id);
  if (child == child_to_primary_.end()) {
    return;
  }
  const auto schedule = schedules_.at(child->second);
  if (terminal_failure) {
    fail(schedule, "TWAP child ended without a full fill: " + child_order_id);
  }
  std::uint64_t filled = 0;
  for (std::size_t i = 0; i < schedule->state.child_order_ids.size(); ++i) {
    const auto& id = schedule->state.child_order_ids[i];
    if (const auto* order = cache_.order(id)) {
      if (order->filled_quantity.raw() >
          std::numeric_limits<std::uint64_t>::max() - filled) {
        throw std::overflow_error(
            "TWAP filled quantity exceeds representable range");
      }
      filled += order->filled_quantity.raw();
      if (order->order.quantity != schedule->quantities[i] ||
          order->order.limit_price !=
              schedule->state.primary_order.limit_price) {
        fail(schedule, "TWAP child quantity or limit price changed: " + id);
      }
      if (order->status == OrderStatus::Canceled ||
          order->status == OrderStatus::Denied ||
          order->status == OrderStatus::Rejected ||
          order->status == OrderStatus::Expired) {
        fail(schedule, "TWAP child ended without a full fill: " + id);
      }
    }
  }
  schedule->state.filled_quantity = Quantity::fromRaw(filled);
  if (filled > schedule->state.primary_order.quantity.raw()) {
    fail(schedule, "TWAP fills exceed the primary quantity");
  } else if (filled == schedule->state.primary_order.quantity.raw() &&
             schedule->state.status == TwapStatus::Running) {
    schedule->state.status = TwapStatus::Completed;
    scheduler_.cancel(schedule->timer_id);
    schedule->timer_id = 0;
  }
}

bool TwapAlgorithm::cancel(const ClientOrderId& primary_order_id) {
  message_bus_.assertOwner();
  const auto it = schedules_.find(primary_order_id);
  if (it == schedules_.end() ||
      it->second->state.status != TwapStatus::Running) {
    return false;
  }
  auto& schedule = *it->second;
  schedule.state.status = TwapStatus::Stopped;
  schedule.state.last_reason =
      "TWAP scheduling stopped; submitted children remain live";
  scheduler_.cancel(schedule.timer_id);
  schedule.timer_id = 0;
  return true;
}

void TwapAlgorithm::stop() noexcept {
  stopped_ = true;
  for (const auto& [id, schedule] : schedules_) {
    static_cast<void>(id);
    if (schedule->state.status == TwapStatus::Running) {
      schedule->state.status = TwapStatus::Stopped;
      scheduler_.cancel(schedule->timer_id);
      schedule->timer_id = 0;
    }
  }
}

const TwapExecutionState* TwapAlgorithm::execution(
    const ClientOrderId& id) const {
  const auto it = schedules_.find(id);
  return it == schedules_.end() ? nullptr : &it->second->state;
}

bool TwapAlgorithm::ownsOrderId(const ClientOrderId& id) const {
  return schedules_.contains(id) || child_to_primary_.contains(id);
}

}  // namespace trading
