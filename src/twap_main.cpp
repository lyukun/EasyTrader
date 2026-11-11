#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <memory>

#include "trading/system/trading_node.h"
#include "trading/trading/twap_example_strategy.h"

int main() {
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v");
  spdlog::cfg::load_env_levels();
  trading::TradingNodeConfig config;
  config.run_duration = std::chrono::seconds(1);
  trading::TradingNode node(config);
  const trading::InstrumentId instrument{"BTC-USD", "SIM"};
  node.addStrategy(std::make_unique<trading::TwapExampleStrategy>(
      trading::TwapExampleConfig{
          instrument, trading::Quantity::fromString("3"),
          {std::chrono::milliseconds(300), std::chrono::milliseconds(100)}}));
  node.subscribe<trading::OrderFilled>([](const auto& fill) {
    spdlog::info("[twap] filled {} qty={}", fill.client_order_id,
                fill.quantity.toString());
  });
  node.run();

  const auto* execution = node.twapExecution("TWAP-1");
  const auto position = node.portfolio().position("SIM-001", instrument);
  spdlog::info("[twap] final position={}", position.toString());
  return execution && execution->status == trading::TwapStatus::Completed &&
                 execution->child_order_ids.size() == 3 &&
                 execution->filled_quantity == trading::Quantity::fromString("3") &&
                 position == trading::Decimal::fromString("3")
             ? 0
             : 1;
}
