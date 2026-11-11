#include "trading/trading/ema_cross_strategy.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace trading {

EmaCrossStrategy::EmaCrossStrategy(EmaCrossConfig config)
    : config_(std::move(config)) {
  if (config_.instrument_id.empty()) {
    throw std::invalid_argument("EMA cross instrument ID must not be empty");
  }
  if (config_.trade_size.isZero()) {
    throw std::invalid_argument("trade size must be positive");
  }
  if (config_.fast_period == 0 ||
      config_.fast_period >= config_.slow_period) {
    throw std::invalid_argument(
        "fast period must be positive and less than slow period");
  }
}

void EmaCrossStrategy::start() {
  running_ = true;
  subscribe<QuoteTick>([this](const auto& quote) { onQuote(quote); });
}

void EmaCrossStrategy::onQuote(const QuoteTick& quote) {
  if (!running_ || quote.instrument_id != config_.instrument_id) {
    return;
  }
  ++quote_count_;

  // Use mid price: (bid + ask) / 2
  const auto mid_raw = (quote.bid.raw() + quote.ask.raw()) / 2;

  // EMA: ema = price * alpha + prev_ema * (1 - alpha)
  // alpha = 2 / (period + 1)
  // ema_raw = (price_raw * 2 + prev_ema_raw * (period - 1)) / (period + 1)

  // Fast EMA
  if (fast_ema_raw_) {
    *fast_ema_raw_ =
        (mid_raw * 2 +
         *fast_ema_raw_ * static_cast<std::int64_t>(config_.fast_period - 1)) /
        static_cast<std::int64_t>(config_.fast_period + 1);
  } else {
    fast_ema_raw_ = mid_raw;
    fast_initialized_ = true;
  }

  // Slow EMA
  if (slow_ema_raw_) {
    *slow_ema_raw_ =
        (mid_raw * 2 +
         *slow_ema_raw_ * static_cast<std::int64_t>(config_.slow_period - 1)) /
        static_cast<std::int64_t>(config_.slow_period + 1);
  } else {
    slow_ema_raw_ = mid_raw;
    slow_initialized_ = true;
  }

  if (!isInitialized()) {
    return;
  }

  const bool fast_above = *fast_ema_raw_ > *slow_ema_raw_;

  if (prev_fast_above_) {
    if (fast_above && !*prev_fast_above_) {
      // Fast crossed above slow → buy at the ask
      context().submitOrder(SubmitOrder{
          "EMA-B-" + std::to_string(quote_count_), config_.instrument_id,
          OrderSide::Buy, config_.trade_size, quote.ask, StrategyId(id()),
          std::nullopt, OrderType::Market});
      ++signal_count_;
    } else if (!fast_above && *prev_fast_above_) {
      // Fast crossed below slow → sell at the bid
      context().submitOrder(SubmitOrder{
          "EMA-S-" + std::to_string(quote_count_), config_.instrument_id,
          OrderSide::Sell, config_.trade_size, quote.bid, StrategyId(id()),
          std::nullopt, OrderType::Market});
      ++signal_count_;
    }
  }

  prev_fast_above_ = fast_above;
}

}  // namespace trading