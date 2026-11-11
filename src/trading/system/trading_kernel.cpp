#include "trading/system/trading_kernel.h"

#include <stdexcept>
#include <utility>

namespace trading {

TradingKernel::TradingKernel(ExecutionClient& default_client,
                             RiskEngineConfig risk_config, Scheduler* scheduler)
    : portfolio_(cache_),
      risk_engine_(cache_, std::move(risk_config)),
      data_engine_(cache_, message_bus_),
      execution_engine_(cache_, portfolio_, message_bus_, default_client),
      strategy_context_(
          cache_, message_bus_,
          [this](const SubmitOrder& order) { submitOrder(order); },
          [this](InstrumentId instrument_id, BookType book_type) {
            addOrderBook(std::move(instrument_id), book_type);
          }),
      trader_(strategy_context_) {
  if (scheduler) {
    twap_ = std::make_unique<TwapAlgorithm>(
        cache_, message_bus_, *scheduler,
        [this](const SubmitOrder& order) { submitDirect(order); });
  }
  wireComponentEvents();
}

void TradingKernel::wireComponentEvents() {
  message_bus_.subscribe<QuoteTick>(
      [this](const auto& quote) { portfolio_.update(quote); });
  message_bus_.subscribe<TradeTick>(
      [this](const auto& trade) { portfolio_.update(trade); });
  message_bus_.subscribe<Bar>(
      [this](const auto& bar) { portfolio_.update(bar); });

  message_bus_.subscribe<OrderAccepted>(
      [this](const auto& event) { refreshOrderState(event.client_order_id); });
  message_bus_.subscribe<OrderUpdated>(
      [this](const auto& event) { refreshOrderState(event.client_order_id); });
  message_bus_.subscribe<OrderFilled>(
      [this](const auto& event) { refreshOrderState(event.client_order_id); });
  message_bus_.subscribe<OrderCanceled>([this](const auto& event) {
    refreshOrderState(event.client_order_id, true);
  });
  message_bus_.subscribe<OrderDenied>([this](const auto& event) {
    refreshOrderState(event.client_order_id, true);
  });
  message_bus_.subscribe<OrderRejected>([this](const auto& event) {
    refreshOrderState(event.client_order_id, true);
  });
  message_bus_.subscribe<OrderExpired>([this](const auto& event) {
    refreshOrderState(event.client_order_id, true);
  });
}

void TradingKernel::setTradingState(TradingState state,
                                    std::uint64_t ts_event) {
  process(TradingStateChanged{state, ts_event});
}

void TradingKernel::dispatch(RuntimeMessage message) {
  std::visit([this](const auto& value) { process(value); }, message);
}

// 行情数据统一交给 data engine 处理
void TradingKernel::process(const QuoteTick& message) {
  data_engine_.process(message);
}

void TradingKernel::process(const TradeTick& message) {
  data_engine_.process(message);
}

void TradingKernel::process(const Bar& message) {
  static_cast<void>(data_engine_.process(message));
}

void TradingKernel::process(const OrderBookDeltas& message) {
  static_cast<void>(data_engine_.process(message));
}

void TradingKernel::process(const OrderBookDepth& message) {
  static_cast<void>(data_engine_.process(message));
}

void TradingKernel::submitOrder(const SubmitOrder& order) {
  message_bus_.assertOwner();
  if (order.exec_algorithm_id) {
    if (*order.exec_algorithm_id != "TWAP" || !twap_) {
      message_bus_.publish(OrderDenied{order.client_order_id,
                                       "execution algorithm is not available",
                                       order.ts_init});
      return;
    }
    twap_->submit(order);
    return;
  }
  if (twap_ && twap_->ownsOrderId(order.client_order_id)) {
    throw std::invalid_argument("client order ID belongs to a TWAP execution");
  }
  if (order.twap) {
    message_bus_.publish(OrderDenied{
        order.client_order_id, "TWAP parameters require exec_algorithm_id",
        order.ts_init});
    return;
  }
  submitDirect(order);
}

void TradingKernel::submitDirect(const SubmitOrder& command) {
  const auto route = execution_engine_.route(command);
  if (!route) {
    message_bus_.publish(OrderDenied{
        command.client_order_id, "no execution client route", command.ts_init});
    return;
  }

  cache_.addOrder(command, route->account_id, route->client_id);
  if (const auto denied = risk_engine_.execute(command, route->account_id)) {
    execution_engine_.process(*denied);
    return;
  }
  execution_engine_.submit(command, *route);
}

void TradingKernel::refreshOrderState(const ClientOrderId& id,
                                      bool terminal_failure) {
  // 风控需要更新
  risk_engine_.refreshReservation(id);
  if (twap_) {
    twap_->process(id, terminal_failure);
  }
}

bool TradingKernel::cancelTwap(const ClientOrderId& id) {
  message_bus_.assertOwner();
  return twap_ && twap_->cancel(id);
}

void TradingKernel::stopAlgorithms() {
  message_bus_.assertOwner();
  if (twap_) {
    twap_->stop();
  }
}

const TwapExecutionState* TradingKernel::twapExecution(
    const ClientOrderId& id) const {
  return twap_ ? twap_->execution(id) : nullptr;
}

void TradingKernel::process(const OrderAccepted& event) {
  execution_engine_.process(event);
}

void TradingKernel::process(const OrderUpdated& event) {
  execution_engine_.process(event);
}

void TradingKernel::process(const OrderFilled& event) {
  execution_engine_.process(event);
}

void TradingKernel::process(const OrderCanceled& event) {
  execution_engine_.process(event);
}

void TradingKernel::process(const OrderDenied& event) {
  execution_engine_.process(event);
}

void TradingKernel::process(const OrderRejected& event) {
  execution_engine_.process(event);
}

void TradingKernel::process(const OrderExpired& event) {
  execution_engine_.process(event);
}

void TradingKernel::process(const TradingStateChanged& event) {
  risk_engine_.setTradingState(event.state);
  message_bus_.publish(event);
}

}  // namespace trading
