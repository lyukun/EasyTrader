#include "trading/trading/example_strategy.h"

#include <spdlog/spdlog.h>

namespace trading {

void ExampleStrategy::start() {
  running_ = true;
  subscribe<QuoteTick>([this](const auto& event) { onQuote(event); });
  subscribe<OrderAccepted>(
      [this](const auto& event) { onAccepted(event); });
  subscribe<OrderFilled>([this](const auto& event) { onFilled(event); });
  subscribe<OrderDenied>([this](const auto& event) { onDenied(event); });
  subscribe<OrderRejected>(
      [this](const auto& event) { onRejected(event); });
}

void ExampleStrategy::onQuote(const QuoteTick& quote) {
  spdlog::debug("[strategy] quote seq={} ask={}", quote.sequence,
               quote.ask.toString());
  if (running_ && !submitted_) {
    submitted_ = true;
    context().submitOrder(
        SubmitOrder{"ORDER-1", quote.instrument_id, OrderSide::Buy,
                    Quantity::fromString("1"), quote.ask});
  }
}

void ExampleStrategy::onAccepted(const OrderAccepted& event) {
  spdlog::info("[strategy] accepted {} as {}", event.client_order_id,
              event.venue_order_id);
}

void ExampleStrategy::onFilled(const OrderFilled& event) {
  filled_ = true;
  spdlog::info("[strategy] filled {} qty={} price={}", event.client_order_id,
              event.quantity.toString(), event.price.toString());
}

void ExampleStrategy::onDenied(const OrderDenied& event) {
  spdlog::warn("[strategy] denied {}: {}", event.client_order_id, event.reason);
}

void ExampleStrategy::onRejected(const OrderRejected& event) {
  spdlog::warn("[strategy] rejected {}: {}", event.client_order_id, event.reason);
}

}  // namespace trading
