#include "trading/trading/twap_example_strategy.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace trading {

TwapExampleStrategy::TwapExampleStrategy(TwapExampleConfig config)
    : config_(std::move(config)) {
  if (config_.instrument_id.empty() || config_.trade_size.isZero()) {
    throw std::invalid_argument("TWAP example requires an instrument and positive trade size");
  }
}

void TwapExampleStrategy::start() {
  running_ = true;
  subscribe<QuoteTick>([this](const auto& quote) {
    if (!running_ || submitted_ || quote.instrument_id != config_.instrument_id) {
      return;
    }
    submitted_ = true;
    SubmitOrder order{"TWAP-1", config_.instrument_id, OrderSide::Buy,
                      config_.trade_size, quote.ask, StrategyId(id())};
    order.time_in_force = TimeInForce::Ioc;
    order.exec_algorithm_id = "TWAP";
    order.twap = config_.twap;
    context().submitOrder(order);
  });
}

}  // namespace trading
