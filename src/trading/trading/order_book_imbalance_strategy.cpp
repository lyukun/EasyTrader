#include "trading/trading/order_book_imbalance_strategy.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include <boost/multiprecision/cpp_int.hpp>

namespace trading {

OrderBookImbalanceStrategy::OrderBookImbalanceStrategy(
    OrderBookImbalanceConfig config)
    : config_(std::move(config)) {
  if (config_.instrument_id.empty()) {
    throw std::invalid_argument("imbalance instrument ID must not be empty");
  }
  if (config_.max_signal_size.isZero()) {
    throw std::invalid_argument("max signal size must be positive");
  }
  if (config_.trigger_ratio_ppm == 0 ||
      config_.trigger_ratio_ppm >= 1'000'000) {
    throw std::invalid_argument("trigger ratio must be in (0, 1)");
  }
}

void OrderBookImbalanceStrategy::start() {
  context().ensureOrderBook(config_.instrument_id, config_.book_type);
  running_ = true;
  subscribe<OrderBookDeltas>(
      [this](const auto& deltas) { onDeltas(deltas); });
}

void OrderBookImbalanceStrategy::onDeltas(const OrderBookDeltas& deltas) {
  if (!running_ || deltas.instrument_id != config_.instrument_id) {
    return;
  }
  ++update_count_;

  const auto* book = context().cache().orderBook(config_.instrument_id);
  if (book == nullptr) {
    return;
  }
  const auto bid_price = book->bestBidPrice();
  const auto ask_price = book->bestAskPrice();
  const auto bid_size = book->bestBidSize();
  const auto ask_size = book->bestAskSize();
  if (!bid_price || !ask_price || !bid_size || !ask_size ||
      *bid_price >= *ask_price || bid_size->isZero() || ask_size->isZero()) {
    return;
  }

  const auto smaller = std::min(*bid_size, *ask_size);
  const auto larger = std::max(*bid_size, *ask_size);
  if (larger <= config_.trigger_min_size) {
    return;
  }
  const boost::multiprecision::cpp_int ratio_left =
      boost::multiprecision::cpp_int(smaller.raw()) * 1'000'000;
  const boost::multiprecision::cpp_int ratio_right =
      boost::multiprecision::cpp_int(larger.raw()) *
      config_.trigger_ratio_ppm;
  if (ratio_left >= ratio_right) {
    return;
  }
  if (last_signal_ns_) {
    if (deltas.ts_event <= *last_signal_ns_ ||
        deltas.ts_event - *last_signal_ns_ < config_.cooldown_ns) {
      return;
    }
  }

  // Skip when an in-flight order from this strategy is still unacknowledged,
  // matching the parent NautilusTrader OrderBookImbalance guard.
  {
    const auto inflight = context().cache().ordersInflight();
    const bool has_inflight =
        std::any_of(inflight.begin(), inflight.end(),
                    [this](const auto* o) {
                      return o->order.strategy_id == StrategyId(id());
                    });
    if (has_inflight) {
      return;
    }
  }

  const auto buy = *bid_size > *ask_size;
  const auto quantity = std::min(smaller, config_.max_signal_size);
  const OrderBookImbalanceSignal signal{
      config_.instrument_id,
      buy ? OrderSide::Buy : OrderSide::Sell,
      buy ? *ask_price : *bid_price,
      quantity,
      deltas.sequence,
      deltas.ts_event};
  last_signal_ns_ = deltas.ts_event;
  ++signal_count_;
  publish(signal);

  const auto order_id = "IMB-" + std::to_string(signal_count_);

  // Commit the request count before synchronous denial callbacks can run
  ++submit_count_;
  context().submitOrder(SubmitOrder{order_id, config_.instrument_id,
                                   signal.side, signal.quantity,
                                   signal.price, StrategyId(id()),
                                   std::nullopt, OrderType::Limit,
                                   TimeInForce::Fok});
}

}  // namespace trading
