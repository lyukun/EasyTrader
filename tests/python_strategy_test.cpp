#include "trading/python/python_strategy.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "trading/common/live_event_loop.h"
#include "trading/python/python_runtime.h"
#include "trading/system/trading_kernel.h"
#include "trading/system/trading_node.h"

namespace {
namespace py = pybind11;
using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

template <typename F>
void expectFailure(F&& action, std::string_view expected) {
  try {
    action();
  } catch (const std::exception& e) {
    require(std::string(e.what()).find(expected) != std::string::npos,
            std::string("unexpected error: ") + e.what());
    return;
  }
  throw std::runtime_error("expected failure did not occur");
}

template <typename T>
T read(const char* class_name, const char* attribute) {
  py::gil_scoped_acquire acquire;
  try {
    return py::module_::import("strategy_fixtures")
        .attr("instances")[class_name]
        .attr(attribute)
        .cast<T>();
  } catch (const py::error_already_set& e) {
    throw std::runtime_error(e.what());
  }
}

void check(const char* class_name, const char* method) {
  py::gil_scoped_acquire acquire;
  try {
    const auto module = py::module_::import("strategy_fixtures");
    if (class_name)
      module.attr("instances")[class_name].attr(method)();
    else
      module.attr(method)();
  } catch (const py::error_already_set& e) {
    throw std::runtime_error(e.what());
  }
}

class RecordingClient final : public trading::ExecutionClient {
 public:
  const trading::ExecutionClientId& clientId() const override {
    return client_id_;
  }
  const trading::AccountId& accountId() const override { return account_id_; }
  const trading::Venue& venue() const override { return venue_; }
  void submit(const trading::SubmitOrder& order) override {
    orders.push_back(order);
  }
  std::vector<trading::SubmitOrder> orders;

 private:
  trading::ExecutionClientId client_id_{"SIM-EXEC"};
  trading::AccountId account_id_{"SIM-001"};
  trading::Venue venue_{"SIM"};
};

trading::QuoteTick quote(std::uint64_t sequence = 1,
                         std::string_view ask = "100.05") {
  return {{"BTC-USD", "SIM"},
          trading::Price::fromString("100"),
          trading::Price::fromString(ask),
          trading::Quantity::fromString("5"),
          trading::Quantity::fromString("5"),
          sequence,
          sequence};
}

struct Fixture {
  Fixture(const std::shared_ptr<trading::PythonRuntime>& runtime,
          const char* class_name)
      : kernel(client) {
    kernel.addInstrument({{"BTC-USD", "SIM"},
                          "BTC",
                          "USD",
                          2,
                          8,
                          trading::Price::fromString("0.01"),
                          trading::Quantity::fromString("0.00000001"),
                          trading::Quantity::fromString("0.00000001"),
                          trading::Quantity::fromString("1000")});
    const auto money = trading::Money::fromString("1000", "USD");
    kernel.addAccount(
        {"SIM-001", "SIM", "USD", money, trading::AccountBalance::cash(money),
         trading::Money::zero("USD"), trading::Money::zero("USD")});
    kernel.trader().addStrategy(std::make_unique<trading::PythonStrategy>(
        runtime, "python-test", "strategy_fixtures", class_name));
  }

  RecordingClient client;
  trading::TradingKernel kernel;
};

void testSubmissionAndSnapshots(
    const std::shared_ptr<trading::PythonRuntime>& runtime) {
  {
    Fixture fixture(runtime, "SubmitOnQuote");
    fixture.kernel.trader().start();
    fixture.kernel.dispatch(quote());
    require(fixture.client.orders.size() == 1,
            "Python did not submit synchronously");
    const auto& order = fixture.client.orders.front();
    require(order.strategy_id == "python-test",
            "Python order lost its strategy identity");
    require(order.limit_price == trading::Price::fromString("100.05"),
            "Python order price lost precision");
    fixture.kernel.dispatch(
        trading::OrderAccepted{order.client_order_id, "V-1"});
    fixture.kernel.dispatch(trading::OrderFilled{
        order.client_order_id, "V-1", order.instrument_id, order.side,
        order.quantity, order.limit_price, "T-1", std::nullopt});
    fixture.kernel.dispatch(quote(2, "101.05"));
    require(
        read<std::int64_t>("SubmitOnQuote", "position_raw") == 1'000'000'000,
        "Python fill callback observed stale portfolio state");
    require(read<std::vector<std::string>>("SubmitOnQuote", "calls") ==
                std::vector<std::string>{"start", "quote", "returned",
                                         "accepted", "filled", "quote"},
            "Python callback ordering changed");
    fixture.kernel.trader().stop();
    fixture.kernel.dispatch(quote(3));
    require(read<int>("SubmitOnQuote", "stop_count") == 1,
            "Python stop hook did not run");
    require(read<bool>("SubmitOnQuote", "stopping_submit_rejected"),
            "Python submitted during shutdown");
    require(read<std::vector<std::string>>("SubmitOnQuote", "calls").back() ==
                "stop",
            "stopped Python strategy still received events");
  }
  check("SubmitOnQuote", "check_snapshots");
  check("SubmitOnQuote", "check_inactive");
}

void testSynchronousDenials(
    const std::shared_ptr<trading::PythonRuntime>& runtime) {
  for (const auto* name : {"SubmitOnQuote", "MissingRoute", "SubmitOnStart"}) {
    Fixture fixture(runtime, name);
    fixture.kernel.setMaxNotionalPerOrder({"BTC-USD", "SIM"},
                                          trading::Decimal::fromString("1"));
    fixture.kernel.trader().start();
    if (std::string_view(name) != "SubmitOnStart")
      fixture.kernel.dispatch(quote());
    require(fixture.client.orders.empty(),
            "denied Python order reached the client");
    require(read<bool>(name, "denied_during_submit"),
            "Python denial did not reenter the submission callback");
    const auto calls = read<std::vector<std::string>>(name, "calls");
    require(calls[calls.size() - 2] == "denied" && calls.back() == "returned",
            "Python submission returned before local denial");
    const auto* order = fixture.kernel.cache().order("python-test-1");
    if (std::string_view(name) == "MissingRoute") {
      require(order == nullptr, "route denial unexpectedly cached an order");
    } else {
      require(order && order->status == trading::OrderStatus::Denied,
              "risk denial did not update the cache");
    }
    fixture.kernel.trader().stop();
  }
}

void testExternalRejection(
    const std::shared_ptr<trading::PythonRuntime>& runtime) {
  Fixture fixture(runtime, "SubmitOnQuote");
  fixture.kernel.trader().start();
  fixture.kernel.dispatch(quote());
  fixture.kernel.dispatch(
      trading::OrderRejected{"python-test-1", "venue refused", 0});
  require(read<std::vector<std::string>>("SubmitOnQuote", "calls").back() ==
              "rejected",
          "Python did not receive venue rejection");
  fixture.kernel.trader().stop();
}

void testThreadGuard(const std::shared_ptr<trading::PythonRuntime>& runtime) {
  Fixture fixture(runtime, "ThreadProbe");
  fixture.kernel.trader().start();
  fixture.kernel.dispatch(quote());
  require(
      read<std::vector<std::string>>("ThreadProbe", "thread_errors").size() ==
          4,
      "Python background thread reached core state");
  require(fixture.client.orders.empty(),
          "Python background thread submitted an order");
  fixture.kernel.trader().stop();
}

void testCallbackFailure(
    const std::shared_ptr<trading::PythonRuntime>& runtime) {
  Fixture fixture(runtime, "QuoteFailure");
  fixture.kernel.trader().start();
  trading::LiveEventLoop loop;
  loop.bind([&](auto event) { fixture.kernel.dispatch(std::move(event)); });
  loop.scheduleAfter(50ms, [&] { loop.stop(); });
  loop.send(quote());
  expectFailure([&] { loop.run(); },
                "ValueError: intentional Python quote failure");
  fixture.kernel.trader().stop();
  require(read<int>("QuoteFailure", "stop_count") == 1,
          "Python cleanup did not run after event-loop failure");
  check("QuoteFailure", "check_inactive");
}

void testLifecycleFailures(
    const std::shared_ptr<trading::PythonRuntime>& runtime) {
  {
    Fixture fixture(runtime, "StartFailure");
    expectFailure([&] { fixture.kernel.trader().start(); }, "on_start failed:");
    require(read<int>("StartFailure", "stop_count") == 1,
            "partial Python start was not rolled back");
    check("StartFailure", "check_inactive");
  }
  {
    Fixture fixture(runtime, "StopFailure");
    fixture.kernel.trader().start();
    expectFailure([&] { fixture.kernel.trader().stop(); }, "on_stop failed:");
    fixture.kernel.trader().stop();
    require(read<int>("StopFailure", "stop_count") == 1,
            "Python stop was repeated");
    check("StopFailure", "check_inactive");
  }
}

void testInvalidStrategies(
    const std::shared_ptr<trading::PythonRuntime>& runtime) {
  expectFailure(
      [&] {
        trading::PythonStrategy strategy(runtime, "bad",
                                         "missing_strategy_module", "Probe");
      },
      "ModuleNotFoundError");
  expectFailure(
      [&] {
        trading::PythonStrategy strategy(runtime, "bad", "strategy_fixtures",
                                         "InvalidHook");
      },
      "on_quote must be callable");
  expectFailure(
      [&] {
        trading::PythonStrategy strategy(runtime, "bad", "strategy_fixtures",
                                         "ConstructorFailure");
      },
      "intentional Python constructor failure");
  expectFailure([&] { trading::PythonRuntime duplicate("tests/python"); },
                "already running");
  check(nullptr, "check_types");
}

void testLiveNode(const std::shared_ptr<trading::PythonRuntime>& runtime) {
  trading::TradingNodeConfig config;
  config.market_data_ticks = 1;
  config.market_data_interval = 5ms;
  config.run_duration = 200ms;
  trading::TradingNode node(config);
  node.addStrategy(std::make_unique<trading::PythonStrategy>(
      runtime, "python-test", "strategy_fixtures", "SubmitOnQuote"));
  node.run();
  const auto* order = node.cache().order("python-test-1");
  require(order && order->status == trading::OrderStatus::Filled,
          "embedded Python strategy did not complete a live fill");
  require(read<std::int64_t>("SubmitOnQuote", "position_raw") == 1'000'000'000,
          "live Python callback did not observe the filled position");
  check("SubmitOnQuote", "check_inactive");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto directory = argc > 1 ? argv[1] : "tests/python";
    expectFailure(
        [&] { trading::PythonRuntime invalid("missing_python_directory"); },
        "does not exist");
    {
      auto runtime = std::make_shared<trading::PythonRuntime>(directory);
      testInvalidStrategies(runtime);
      testSubmissionAndSnapshots(runtime);
      testSynchronousDenials(runtime);
      testExternalRejection(runtime);
      testThreadGuard(runtime);
      testCallbackFailure(runtime);
      testLifecycleFailures(runtime);
      testLiveNode(runtime);
    }
    require(!Py_IsInitialized(),
            "interpreter was not finalized after strategies were destroyed");
    {
      auto runtime = std::make_shared<trading::PythonRuntime>(directory);
      std::weak_ptr<trading::PythonRuntime> weak = runtime;
      {
        Fixture fixture(runtime, "Probe");
        runtime.reset();
        require(!weak.expired(), "strategy did not retain its interpreter");
        fixture.kernel.trader().start();
        fixture.kernel.trader().stop();
      }
      require(weak.expired() && !Py_IsInitialized(),
              "retained interpreter did not finalize");
    }
    std::cout << "all embedded Python strategy tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& e) {
    std::cerr << "Python strategy test failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
}
