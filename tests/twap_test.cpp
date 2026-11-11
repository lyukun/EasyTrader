#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "trading/common/live_event_loop.h"
#include "trading/system/trading_kernel.h"
#include "trading/trading/twap_example_strategy.h"

namespace {

using trading::OrderSide;
using trading::Price;
using trading::Quantity;
using trading::TwapStatus;
using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

class ManualScheduler final : public trading::Scheduler {
 public:
  TimerId scheduleAfter(std::chrono::milliseconds delay, Callback callback) override {
    if (fail_schedule) {
      throw std::runtime_error("scheduler unavailable");
    }
    const auto id = ++next_id_;
    tasks_.emplace(std::make_pair(now_ + delay.count(), id), std::move(callback));
    return id;
  }

  void cancel(TimerId id) noexcept override {
    for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
      if (it->first.second == id) {
        tasks_.erase(it);
        return;
      }
    }
  }

  void advance(std::chrono::milliseconds elapsed) {
    now_ += elapsed.count();
    while (!tasks_.empty() && tasks_.begin()->first.first <= now_) {
      auto callback = std::move(tasks_.begin()->second);
      tasks_.erase(tasks_.begin());
      callback();
    }
  }

  Callback takeNext() {
    require(!tasks_.empty(), "manual scheduler has no pending callback");
    auto callback = std::move(tasks_.begin()->second);
    tasks_.erase(tasks_.begin());
    return callback;
  }

  std::size_t pending() const { return tasks_.size(); }
  bool fail_schedule{false};

 private:
  std::int64_t now_{0};
  TimerId next_id_{0};
  std::map<std::pair<std::int64_t, TimerId>, Callback> tasks_;
};

class RecordingClient final : public trading::ExecutionClient {
 public:
  const trading::ExecutionClientId& clientId() const override { return client_id_; }
  const trading::AccountId& accountId() const override { return account_id_; }
  const trading::Venue& venue() const override { return venue_; }
  void submit(const trading::SubmitOrder& order) override { orders.push_back(order); }
  std::vector<trading::SubmitOrder> orders;

 private:
  trading::ExecutionClientId client_id_{"SIM-EXEC"};
  trading::AccountId account_id_{"SIM-001"};
  trading::Venue venue_{"SIM"};
};

trading::Instrument instrument() {
  return {{"BTC-USD", "SIM"}, "BTC", "USD", 2, 8,
          Price::fromString("0.01"), Quantity::fromString("0.1"),
          Quantity::fromString("0.1"), Quantity::fromString("1000")};
}

trading::Account account() {
  const auto money = trading::Money::fromString("1000000", "USD");
  return {"SIM-001", "SIM", "USD", money, trading::AccountBalance::cash(money),
          trading::Money::zero("USD"), trading::Money::zero("USD")};
}

trading::SubmitOrder request(std::string id = "P", std::string_view quantity = "1",
                             std::chrono::milliseconds horizon = 300ms,
                             std::chrono::milliseconds interval = 100ms) {
  trading::SubmitOrder order{std::move(id), {"BTC-USD", "SIM"}, OrderSide::Buy,
                            Quantity::fromString(quantity), Price::fromString("100"),
                            "twap-strategy"};
  order.time_in_force = trading::TimeInForce::Ioc;
  order.exec_algorithm_id = "TWAP";
  order.twap = trading::TwapConfig{horizon, interval};
  return order;
}

struct Fixture {
  Fixture() : kernel(client, {}, &scheduler) {
    kernel.addInstrument(instrument());
    kernel.addAccount(account());
  }

  const trading::TwapExecutionState& state(std::string_view id = "P") const {
    const auto* result = kernel.twapExecution(std::string(id));
    require(result != nullptr, "TWAP execution state is missing");
    return *result;
  }

  trading::OrderFilled fill(std::string id, std::optional<Quantity> quantity = std::nullopt) {
    const auto* state = kernel.cache().order(id);
    require(state != nullptr, "cannot fill a missing TWAP child");
    const auto order = state->order;
    const auto venue_id = "V-" + id;
    if (state->status == trading::OrderStatus::Submitted) {
      kernel.dispatch(trading::OrderAccepted{id, venue_id});
    }
    trading::OrderFilled fill{id, venue_id, order.instrument_id, order.side,
                             quantity.value_or(order.quantity), order.limit_price,
                             "T-" + std::to_string(++trade_count), std::nullopt, 0};
    kernel.dispatch(fill);
    return fill;
  }

  ManualScheduler scheduler;
  RecordingClient client;
  trading::TradingKernel kernel;
  std::uint64_t trade_count{0};
};

void testTimingAndCompletion() {
  Fixture f;
  f.kernel.submitOrder(request());
  require(f.client.orders.size() == 1 && f.scheduler.pending() == 1,
          "TWAP first slice was not synchronous");
  require(f.client.orders[0].client_order_id == "P-E1" &&
              f.client.orders[0].quantity == Quantity::fromString("0.4") &&
              f.client.orders[0].exec_spawn_id == std::optional<trading::ClientOrderId>{"P"} &&
              !f.client.orders[0].exec_algorithm_id && !f.client.orders[0].twap &&
              f.client.orders[0].limit_price == Price::fromString("100") &&
              f.client.orders[0].strategy_id == "twap-strategy" &&
              f.kernel.cache().order("P") == nullptr,
          "TWAP parent-child identity or fixed limit price was wrong");
  f.scheduler.advance(99ms);
  require(f.client.orders.size() == 1, "TWAP submitted before its interval");
  f.scheduler.advance(1ms);
  require(f.client.orders.size() == 2 &&
              f.client.orders[1].quantity == Quantity::fromString("0.3"),
          "TWAP did not submit its second slice at the interval boundary");
  f.scheduler.advance(100ms);
  require(f.client.orders.size() == 3 && f.scheduler.pending() == 0 &&
              f.state().submitted_quantity == Quantity::fromString("1") &&
              f.state().status == TwapStatus::Running,
          "TWAP confused submission with completed execution");
  const auto first_fill = f.fill("P-E1");
  f.fill("P-E2");
  f.fill("P-E3");
  require(f.state().status == TwapStatus::Completed &&
              f.state().filled_quantity == Quantity::fromString("1") &&
              f.kernel.portfolio().position("SIM-001", {"BTC-USD", "SIM"}) ==
                  trading::Decimal::fromString("1"),
          "TWAP did not complete from actual child fills");
  f.kernel.dispatch(first_fill);
  require(f.state().filled_quantity == Quantity::fromString("1"),
          "fill replay increased TWAP execution progress");
}

void testQuantityConservation() {
  Fixture f;
  for (std::uint64_t lots = 1; lots <= 17; ++lots) {
    for (int intervals = 1; intervals <= 8; ++intervals) {
      const auto id = "GRID-" + std::to_string(lots) + "-" + std::to_string(intervals);
      auto order = request(id, "1", 100ms * intervals, 100ms);
      order.quantity = Quantity::fromRaw(lots * Quantity::fromString("0.1").raw());
      const auto begin = f.client.orders.size();
      f.kernel.submitOrder(order);
      for (int tick = 0; tick < intervals; ++tick) {
        f.scheduler.advance(100ms);
      }
      std::uint64_t total = 0;
      for (auto i = begin; i < f.client.orders.size(); ++i) {
        const auto quantity = f.client.orders[i].quantity;
        require(quantity >= instrument().min_quantity &&
                    quantity.raw() % instrument().lot_size.raw() == 0,
                "TWAP generated a zero, undersized, or off-lot child");
        total += quantity.raw();
      }
      require(total == order.quantity.raw() &&
                  f.state(id).submitted_quantity == order.quantity &&
                  f.scheduler.pending() == 0,
              "TWAP lost or added quantity during exact lot allocation");
    }
  }
}

void testInvalidOrders() {
  Fixture f;
  std::vector<trading::SubmitOrder> orders;
  auto order = request();
  order.twap->interval = 0ms;
  orders.push_back(order);
  order = request();
  order.twap->horizon = 99ms;
  orders.push_back(order);
  order = request();
  order.twap->interval = -1ms;
  orders.push_back(order);
  order = request();
  order.twap.reset();
  orders.push_back(order);
  orders.push_back(request("P", "0"));
  orders.push_back(request("P", "0.15"));
  orders.push_back(request("P", "4000"));
  orders.push_back(request("P", "1", 102500ms, 100ms));
  order = request();
  order.order_type = trading::OrderType::Market;
  orders.push_back(order);
  order = request();
  order.time_in_force = trading::TimeInForce::Gtc;
  orders.push_back(order);
  order = request();
  order.limit_price = Price::fromString("100.005");
  orders.push_back(order);
  order = request();
  order.instrument_id.symbol = "UNKNOWN";
  orders.push_back(order);
  order = request();
  order.twap->interval = std::chrono::milliseconds(
      static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) + 1);
  order.twap->horizon = order.twap->interval;
  orders.push_back(order);
  int denials = 0;
  f.kernel.messageBus().subscribe<trading::OrderDenied>([&](const auto&) { ++denials; });
  for (std::size_t i = 0; i < orders.size(); ++i) {
    orders[i].client_order_id = "INVALID-" + std::to_string(i);
    f.kernel.submitOrder(orders[i]);
  }
  require(denials == static_cast<int>(orders.size()) && f.client.orders.empty() &&
              f.scheduler.pending() == 0,
          "invalid TWAP order produced a child or timer");
}

void testDenialAndRouteFailures() {
  Fixture f;
  f.kernel.setMaxNotionalPerOrder({"BTC-USD", "SIM"}, trading::Decimal::fromString("39"));
  f.kernel.submitOrder(request());
  require(f.state().status == TwapStatus::Failed && f.client.orders.empty() &&
              f.scheduler.pending() == 0 &&
              f.kernel.cache().order("P-E1")->status == trading::OrderStatus::Denied,
          "synchronous first-child risk denial did not stop TWAP");

  Fixture later;
  later.kernel.submitOrder(request());
  later.fill("P-E1");
  later.kernel.setMaxNotionalPerOrder({"BTC-USD", "SIM"},
                                     trading::Decimal::fromString("29"));
  later.scheduler.advance(100ms);
  require(later.state().status == TwapStatus::Failed &&
              later.state().filled_quantity == Quantity::fromString("0.4") &&
              later.client.orders.size() == 1 && later.scheduler.pending() == 0,
          "later child bypassed risk or discarded partial parent progress");

  Fixture routed;
  auto order = request();
  order.execution_client_id = "UNKNOWN";
  routed.kernel.submitOrder(order);
  require(routed.state().status == TwapStatus::Failed && routed.scheduler.pending() == 0 &&
              routed.kernel.cache().order("P-E1") == nullptr,
          "uncached route denial left TWAP scheduling active");
}

void testPartialFillAndTerminalChildren() {
  Fixture f;
  f.kernel.submitOrder(request());
  f.fill("P-E1", Quantity::fromString("0.2"));
  f.kernel.dispatch(trading::OrderCanceled{"P-E1", "unfilled IOC remainder", 0});
  f.scheduler.advance(1s);
  require(f.state().status == TwapStatus::Failed &&
              f.state().filled_quantity == Quantity::fromString("0.2") &&
              f.client.orders.size() == 1 && f.scheduler.pending() == 0,
          "partial child cancellation was reported as a full TWAP completion");

  Fixture rejected;
  rejected.kernel.submitOrder(request());
  rejected.kernel.dispatch(trading::OrderRejected{"P-E1", "venue rejected"});
  require(rejected.state().status == TwapStatus::Failed && rejected.scheduler.pending() == 0,
          "venue rejection did not stop remaining TWAP slices");

  Fixture expired;
  expired.kernel.submitOrder(request());
  expired.kernel.dispatch(trading::OrderAccepted{"P-E1", "V-P-E1"});
  expired.kernel.dispatch(trading::OrderExpired{"P-E1", 0});
  require(expired.state().status == TwapStatus::Failed && expired.scheduler.pending() == 0,
          "child expiry did not stop remaining TWAP slices");
}

void testCancellationAndStop() {
  Fixture f;
  f.kernel.submitOrder(request());
  require(f.kernel.cancelTwap("P"), "TWAP cancellation did not find a running execution");
  f.scheduler.advance(1s);
  require(f.state().status == TwapStatus::Stopped && f.client.orders.size() == 1 &&
              f.kernel.cache().order("P-E1")->status == trading::OrderStatus::Submitted,
          "schedule cancellation falsely canceled a working venue child");
  f.fill("P-E1");
  require(f.state().status == TwapStatus::Stopped &&
              f.state().filled_quantity == Quantity::fromString("0.4"),
          "stopped TWAP discarded a fill from its already submitted child");

  f.kernel.submitOrder(request("OTHER"));
  f.kernel.stopAlgorithms();
  f.scheduler.advance(1s);
  const auto count = f.client.orders.size();
  int denials = 0;
  f.kernel.messageBus().subscribe<trading::OrderDenied>([&](const auto&) { ++denials; });
  f.kernel.submitOrder(request("AFTER-STOP"));
  require(f.state("OTHER").status == TwapStatus::Stopped && f.scheduler.pending() == 0 &&
              f.client.orders.size() == count && denials == 1,
          "stopped TWAP accepted or scheduled a new execution");
}

void testIdProtectionAndChangedChild() {
  Fixture f;
  f.kernel.submitOrder(request());
  bool duplicate = false;
  try {
    f.kernel.submitOrder(request());
  } catch (const std::invalid_argument&) {
    duplicate = true;
  }
  auto colliding = request("P-E2");
  colliding.exec_algorithm_id.reset();
  colliding.twap.reset();
  bool protected_child = false;
  try {
    f.kernel.submitOrder(colliding);
  } catch (const std::invalid_argument&) {
    protected_child = true;
  }
  require(duplicate && protected_child && f.client.orders.size() == 1,
          "duplicate primary or reserved child ID was accepted");
  f.kernel.dispatch(trading::OrderAccepted{"P-E1", "V-P-E1"});
  f.kernel.dispatch(trading::OrderUpdated{
      "P-E1", Quantity::fromString("0.2"), Price::fromString("100"), 0});
  require(f.state().status == TwapStatus::Failed && f.scheduler.pending() == 0,
          "changed child quantity left an unachievable TWAP target running");
}

void testSchedulingFailureAndLifetime() {
  Fixture f;
  f.scheduler.fail_schedule = true;
  bool failed = false;
  try {
    f.kernel.submitOrder(request());
  } catch (const std::runtime_error&) {
    failed = true;
  }
  require(failed && f.state().status == TwapStatus::Failed && f.client.orders.size() == 1,
          "scheduler failure was hidden after first-child submission");

  ManualScheduler scheduler;
  RecordingClient client;
  trading::Scheduler::Callback delayed;
  {
    trading::TradingKernel kernel(client, {}, &scheduler);
    kernel.addInstrument(instrument());
    kernel.addAccount(account());
    kernel.submitOrder(request());
    delayed = scheduler.takeNext();
  }
  delayed();
  require(client.orders.size() == 1, "timer callback used a destroyed TWAP algorithm");
}

void testLateTimerDoesNotBurst() {
  Fixture f;
  f.kernel.submitOrder(request());
  f.scheduler.advance(1s);
  require(f.client.orders.size() == 2 && f.scheduler.pending() == 1,
          "late TWAP timer burst-submitted all remaining slices");
  f.scheduler.advance(99ms);
  require(f.client.orders.size() == 2, "late timer shortened the next slice interval");
  f.scheduler.advance(1ms);
  require(f.client.orders.size() == 3, "late TWAP schedule did not continue");
}

void testStrategyAndMissingAlgorithm() {
  Fixture f;
  f.kernel.trader().addStrategy(std::make_unique<trading::TwapExampleStrategy>(
      trading::TwapExampleConfig{{"BTC-USD", "SIM"}, Quantity::fromString("1"),
                                 {300ms, 100ms}}));
  f.kernel.trader().start();
  const trading::QuoteTick quote{{"BTC-USD", "SIM"}, Price::fromString("99.99"),
                                 Price::fromString("100"), Quantity::fromString("1"),
                                 Quantity::fromString("1"), 1, 1};
  f.kernel.dispatch(quote);
  f.kernel.dispatch(quote);
  require(f.state("TWAP-1").primary_order.strategy_id == "twap-example" &&
              f.client.orders.size() == 1,
          "TWAP example did not synchronously route one primary order");
  f.kernel.trader().stop();

  RecordingClient client;
  trading::TradingKernel kernel(client);
  int denials = 0;
  kernel.messageBus().subscribe<trading::OrderDenied>([&](const auto&) { ++denials; });
  kernel.submitOrder(request());
  require(denials == 1 && client.orders.empty(), "missing TWAP scheduler was ignored");
}

void testLiveScheduler() {
  trading::LiveEventLoop loop;
  loop.bind([](trading::RuntimeMessage) {});
  bool canceled_ran = false;
  bool correct_thread = false;
  const auto owner = std::this_thread::get_id();
  const auto canceled = loop.scheduleAfter(1ms, [&] { canceled_ran = true; });
  loop.cancel(canceled);
  loop.scheduleAfter(1ms, [&] {
    correct_thread = std::this_thread::get_id() == owner;
    loop.stop();
  });
  loop.run();
  require(correct_thread && !canceled_ran, "live timer ownership or cancellation failed");

  trading::LiveEventLoop throwing;
  throwing.bind([](trading::RuntimeMessage) {});
  throwing.scheduleAfter(1ms, [] { throw std::runtime_error("timer failure"); });
  bool propagated = false;
  try {
    throwing.run();
  } catch (const std::runtime_error&) {
    propagated = true;
  }
  require(propagated, "live scheduler swallowed a timer callback failure");
}

}  // namespace

int main() {
  try {
    testTimingAndCompletion();
    testQuantityConservation();
    testInvalidOrders();
    testDenialAndRouteFailures();
    testPartialFillAndTerminalChildren();
    testCancellationAndStop();
    testIdProtectionAndChangedChild();
    testSchedulingFailureAndLifetime();
    testLateTimerDoesNotBurst();
    testStrategyAndMissingAlgorithm();
    testLiveScheduler();
    std::cout << "all TWAP tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& e) {
    std::cerr << "TWAP test failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
}
