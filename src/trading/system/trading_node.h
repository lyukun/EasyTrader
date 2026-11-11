#pragma once

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

#include "trading/adapters/fake_clients.h"
#include "trading/common/live_event_loop.h"
#include "trading/system/trading_kernel.h"

namespace trading {

struct TradingNodeConfig {
  std::size_t io_threads{3};
  std::size_t market_data_ticks{5};
  std::chrono::milliseconds market_data_interval{50};
  std::chrono::milliseconds run_duration{500};
  Decimal max_notional{Decimal::fromString("10000")};
};

class TradingNode {
 public:
  explicit TradingNode(TradingNodeConfig config = {});

  Strategy& addStrategy(std::unique_ptr<Strategy> strategy) {
    return kernel_.trader().addStrategy(std::move(strategy));
  }

  template <typename T>
  void subscribe(std::function<void(const T&)> handler) {
    kernel_.messageBus().subscribe<T>(std::move(handler));
  }

  const Cache& cache() const { return kernel_.cache(); }
  const Portfolio& portfolio() const { return kernel_.portfolio(); }
  const Trader& trader() const { return kernel_.trader(); }
  const TwapExecutionState* twapExecution(const ClientOrderId& id) const {
    return kernel_.twapExecution(id);
  }

  void run();
  void stop();

 private:
  enum class State { Ready, Running, Stopped };

  void finishRun();

  TradingNodeConfig config_;
  folly::CPUThreadPoolExecutor io_executor_;
  LiveEventLoop event_loop_;
  FakeExecutionClient execution_client_;
  TradingKernel kernel_;
  FakeMarketDataClient market_data_client_;
  State state_{State::Ready};
};

}  // namespace trading
