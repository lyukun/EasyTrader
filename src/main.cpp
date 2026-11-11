#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>

#include <memory>

#include "trading/system/trading_node.h"
#include "trading/trading/ema_cross_strategy.h"
#include "trading/trading/example_strategy.h"
#include "trading/trading/order_book_imbalance_strategy.h"

int main() {
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v");
  spdlog::cfg::load_env_levels();
  trading::TradingNode node;
  node.addStrategy(std::make_unique<trading::ExampleStrategy>());

  auto imbalance = std::make_unique<trading::OrderBookImbalanceStrategy>(
      trading::OrderBookImbalanceConfig{
          trading::InstrumentId{"BTC-USD", "SIM"},
          trading::BookType::L2Mbp,
                                        trading::Quantity::fromString("1"),
                                        trading::Quantity::fromString("5"),
                                        200'000, 1'000'000'000});
  const auto* imbalance_result = imbalance.get();
  node.addStrategy(std::move(imbalance));

  auto ema_cross = std::make_unique<trading::EmaCrossStrategy>(
      trading::EmaCrossConfig{
          trading::InstrumentId{"BTC-USD", "SIM"},
          trading::Quantity::fromString("1"), 3, 10});
  const auto* ema_result = ema_cross.get();
  node.addStrategy(std::move(ema_cross));

  node.subscribe<trading::OrderBookImbalanceSignal>(
      [](const auto& signal) {
        spdlog::info("[imbalance] {} qty={} price={}",
                    signal.side == trading::OrderSide::Buy ? "BUY" : "SELL",
                    signal.quantity.toString(), signal.price.toString());
      });

  spdlog::info("[main] core loop starts on the calling thread");
  node.run();

  const auto position = node.portfolio().position(
      "SIM-001", trading::InstrumentId{"BTC-USD", "SIM"});
  spdlog::info("[main] final position={}", position.toString());
  const auto* example_order = node.cache().order("ORDER-1");
  const auto* imb_order = node.cache().order("IMB-1");
  return example_order != nullptr &&
                 example_order->status == trading::OrderStatus::Filled &&
                 imb_order != nullptr &&
                 imb_order->status == trading::OrderStatus::Filled &&
                 position == trading::Decimal::fromString("2") &&
                 imbalance_result->signalCount() > 0 &&
                 ema_result->quoteCount() > 0
             ? 0
             : 1;
}
