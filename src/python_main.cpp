#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>

#include "trading/python/python_runtime.h"
#include "trading/python/python_strategy.h"
#include "trading/system/trading_node.h"

int main(int argc, char** argv) {
  spdlog::cfg::load_env_levels();
  try {
    auto runtime = std::make_shared<trading::PythonRuntime>(
        argc > 1 ? argv[1] : "python/strategies");
    trading::TradingNode node;
    node.addStrategy(std::make_unique<trading::PythonStrategy>(
        runtime, "python-example", "example_strategy", "ExampleStrategy"));
    node.run();

    const auto* order = node.cache().order("PY-1");
    const auto position = node.portfolio().position(
        "SIM-001", trading::InstrumentId{"BTC-USD", "SIM"});
    spdlog::info("[main] final position={}", position.toString());
    return order != nullptr && order->status == trading::OrderStatus::Filled &&
                   order->order.strategy_id == "python-example" &&
                   position == trading::Decimal::fromString("1")
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
  } catch (const std::exception& e) {
    spdlog::error("[main] {}", e.what());
    return EXIT_FAILURE;
  }
}
