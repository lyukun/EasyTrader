#include <folly/executors/CPUThreadPoolExecutor.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "trading/adapters/fake_clients.h"
#include "trading/common/live_event_loop.h"
#include "trading/model/order_book.h"
#include "trading/system/trader.h"
#include "trading/system/trading_kernel.h"
#include "trading/trading/ema_cross_strategy.h"
#include "trading/trading/order_book_imbalance_strategy.h"

namespace {

using trading::BookAction;
using trading::BookOrder;
using trading::BookType;
using trading::OrderBookDelta;
using trading::OrderSide;
using trading::Price;
using trading::Quantity;

trading::InstrumentId instrumentId(std::string symbol) {
  return {std::move(symbol), "SIM"};
}

trading::Instrument bitcoinInstrument() {
  return trading::Instrument{instrumentId("BTC-USD"),
                             "BTC",
                             "USD",
                             2,
                             8,
                             Price::fromString("0.01"),
                             Quantity::fromString("0.00000001"),
                             Quantity::fromString("0.00000001"),
                             Quantity::fromString("1000")};
}

trading::Account simulationAccount(std::string_view balance = "1000") {
  const auto starting_balance = trading::Money::fromString(balance, "USD");
  return trading::Account{"SIM-001",
                          "SIM",
                          "USD",
                          starting_balance,
                          trading::AccountBalance::cash(starting_balance),
                          trading::Money::zero("USD"),
                          trading::Money::zero("USD")};
}

class RecordingStrategy final : public trading::Strategy {
 public:
  RecordingStrategy(std::string strategy_id, std::vector<std::string>& events,
                    bool fail_on_start = false)
      : strategy_id_(std::move(strategy_id)),
        events_(events),
        fail_on_start_(fail_on_start) {}

  std::string_view id() const override { return strategy_id_; }

  void start() override {
    events_.push_back("start:" + strategy_id_);
    if (fail_on_start_) {
      throw std::runtime_error("strategy start failed");
    }
  }

  void stop() override { events_.push_back("stop:" + strategy_id_); }

 private:
  std::string strategy_id_;
  std::vector<std::string>& events_;
  bool fail_on_start_;
};

class RecordingExecutionClient final : public trading::ExecutionClient {
 public:
  RecordingExecutionClient(trading::ExecutionClientId client_id,
                           trading::AccountId account_id,
                           trading::Venue venue)
      : client_id_(std::move(client_id)),
        account_id_(std::move(account_id)),
        venue_(std::move(venue)) {}

  const trading::ExecutionClientId& clientId() const override {
    return client_id_;
  }
  const trading::AccountId& accountId() const override { return account_id_; }
  const trading::Venue& venue() const override { return venue_; }
  void submit(const trading::SubmitOrder& order) override {
    submitted_orders_.push_back(order);
  }

  const std::vector<trading::SubmitOrder>& submittedOrders() const {
    return submitted_orders_;
  }

 private:
  trading::ExecutionClientId client_id_;
  trading::AccountId account_id_;
  trading::Venue venue_;
  std::vector<trading::SubmitOrder> submitted_orders_;
};

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

OrderBookDelta delta(trading::InstrumentId instrument_id, BookAction action,
                     OrderSide side,
                     std::string_view price, std::string_view size,
                     std::uint64_t order_id, std::uint64_t sequence,
                     std::uint8_t flags = 0) {
  return OrderBookDelta{std::move(instrument_id),
                        action,
                        BookOrder{side, Price::fromString(price),
                                  Quantity::fromString(size), order_id},
                        flags,
                        sequence,
                        sequence,
                        sequence};
}

void testFixedPointTypes() {
  const trading::InstrumentId sim_btc{"BTC-USD", "SIM"};
  const trading::InstrumentId other_btc{"BTC-USD", "OTHER"};
  require(sim_btc.symbol == "BTC-USD" && sim_btc.venue == "SIM" &&
              sim_btc.toString() == "BTC-USD.SIM" && sim_btc != other_btc,
          "instrument ID did not preserve symbol and venue identity");

  require(Price::fromString("0.1").raw() == 100'000'000,
          "price parsing was not exact");
  require(Price::fromString("100.050").toString() == "100.050",
          "price precision was not retained");
  require(Quantity::fromString("1.000000001").raw() == 1'000'000'001,
          "quantity parsing was not exact");

  bool rejected = false;
  try {
    static_cast<void>(Quantity::fromString("-1"));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "negative quantity was accepted");

  const auto usd = trading::Money::fromString("10.25", "USD");
  require(usd.checkedAdd(trading::Money::fromString("1.75", "USD")) ==
              trading::Money::fromString("12", "USD"),
          "same-currency money addition was wrong");
  bool currency_mismatch_rejected = false;
  try {
    static_cast<void>(
        usd.checkedAdd(trading::Money::fromString("1", "EUR")));
  } catch (const std::invalid_argument&) {
    currency_mismatch_rejected = true;
  }
  require(currency_mismatch_rejected,
          "money arithmetic accepted different currencies");
}

void testL1Book() {
  trading::OrderBook book(instrumentId("BTC-USD"), BookType::L1Mbp);
  const trading::QuoteTick quote{instrumentId("BTC-USD"),
                                 Price::fromString("99.9"),
                                 Price::fromString("100.1"),
                                 Quantity::fromString("3"),
                                 Quantity::fromString("4"),
                                 1,
                                 10};
  require(!book.updateQuote(quote), "L1 quote update failed");
  require(book.bestBidPrice() == Price::fromString("99.9"),
          "L1 best bid was wrong");
  require(book.bestAskSize() == Quantity::fromString("4"),
          "L1 best ask size was wrong");
  require(book.spread() == Price::fromString("0.2"), "L1 spread was wrong");

  const trading::OrderBookDepth depth{
      instrumentId("BTC-USD"),
      {{OrderSide::Buy, Price::fromString("99.8"), Quantity::fromString("6"), 10},
       {OrderSide::Buy, Price::fromString("99.95"), Quantity::fromString("2"), 11}},
      {{OrderSide::Sell, Price::fromString("100.2"), Quantity::fromString("8"), 12},
       {OrderSide::Sell, Price::fromString("100.05"), Quantity::fromString("1"), 13}},
      2,
      20,
      20};
  require(!book.applyDepth(depth), "L1 depth update failed");
  require(book.bestBidPrice() == Price::fromString("99.95") &&
              book.bestAskPrice() == Price::fromString("100.05"),
          "L1 depth did not retain the best levels");
}

void testL2BookAndAtomicSnapshot() {
  trading::OrderBook book(instrumentId("BTC-USD"), BookType::L2Mbp);
  const trading::OrderBookDepth snapshot{
      instrumentId("BTC-USD"),
      {{OrderSide::Buy, Price::fromString("99.9"), Quantity::fromString("4"), 1},
       {OrderSide::Buy, Price::fromString("99.8"), Quantity::fromString("6"), 2}},
      {{OrderSide::Sell, Price::fromString("100.1"), Quantity::fromString("2"), 3},
       {OrderSide::Sell, Price::fromString("100.2"), Quantity::fromString("8"), 4}},
      10,
      100,
      100};
  require(!book.applyDepth(snapshot), "L2 snapshot failed");
  require(book.bids().size() == 2 && book.asks().size() == 2,
          "L2 snapshot depth was wrong");
  require(book.bestBidPrice() == Price::fromString("99.9"),
          "L2 bids were not descending");

  const trading::OrderBookDeltas updates{
      instrumentId("BTC-USD"),
      {delta(instrumentId("BTC-USD"), BookAction::Update, OrderSide::Sell,
             "100.1", "7", 3,
             11)},
      11,
      110,
      110};
  require(!book.applyDeltas(updates), "L2 update failed");
  require(book.bestAskSize() == Quantity::fromString("7"),
          "L2 level replacement failed");

  const auto old_bid = book.bestBidPrice();
  const trading::OrderBookDeltas invalid_snapshot{
      instrumentId("BTC-USD"),
      {delta(instrumentId("BTC-USD"), BookAction::Clear, OrderSide::Buy, "0",
             "0", 0, 12,
             trading::kBookFlagSnapshot),
       delta(instrumentId("BTC-USD"), BookAction::Add, OrderSide::Buy, "101",
             "0", 5, 12,
             trading::kBookFlagSnapshot | trading::kBookFlagLast)},
      12,
      120,
      120};
  require(book.applyDeltas(invalid_snapshot) == trading::BookError::InvalidQuantity,
          "invalid snapshot was accepted");
  require(book.bestBidPrice() == old_bid,
          "invalid snapshot partially replaced the book");

  const auto fills = book.simulateFills(OrderSide::Buy, Quantity::fromString("9"));
  require(fills.size() == 2 && fills[0].quantity == Quantity::fromString("7"),
          "fill simulation did not walk ask levels");
  require(book.averagePriceForQuantity(OrderSide::Buy, Quantity::fromString("9")) ==
              Price::fromString("100.122222222"),
          "average fill price was wrong");
}

void testL3PriceTimePriority() {
  trading::OrderBook book(instrumentId("ETH-USD"), BookType::L3Mbo);
  require(!book.applyDelta(
              delta(instrumentId("ETH-USD"), BookAction::Add, OrderSide::Buy,
                    "99", "2", 1, 1)),
          "L3 first add failed");
  require(!book.applyDelta(
              delta(instrumentId("ETH-USD"), BookAction::Add, OrderSide::Buy,
                    "99", "3", 2, 2)),
          "L3 second add failed");
  require(book.bids(1).front().orders.front().order_id == 1,
          "L3 add did not preserve FIFO");

  require(!book.applyDelta(delta(instrumentId("ETH-USD"), BookAction::Update,
                                 OrderSide::Buy, "99", "5", 1, 3)),
          "L3 same-price update failed");
  require(book.bids(1).front().orders.front().order_id == 1,
          "same-price update lost priority");

  require(!book.applyDelta(delta(instrumentId("ETH-USD"), BookAction::Update,
                                 OrderSide::Buy, "98", "5", 1, 4)),
          "L3 price move failed");
  require(book.bestBidPrice() == Price::fromString("99") &&
              book.bids(1).front().orders.front().order_id == 2,
          "L3 price move left the order at its old level");
  require(!book.applyDelta(delta(instrumentId("ETH-USD"), BookAction::Update,
                                 OrderSide::Sell, "101", "7", 999, 5)),
          "unknown L3 update returned an error");
  require(book.bestAskPrice() == std::nullopt,
          "unknown L3 update incorrectly added an order");
  require(!book.checkIntegrity(), "valid L3 book failed integrity check");
}

void testTraderLifecycle() {
  trading::Cache cache;
  trading::MessageBus message_bus;
  trading::StrategyContext context(
      cache, message_bus, [](const trading::SubmitOrder&) {},
      [](trading::InstrumentId, BookType) {});
  std::vector<std::string> events;

  trading::Trader trader(context);
  trader.addStrategy(
      std::make_unique<RecordingStrategy>("first", events));
  trader.addStrategy(
      std::make_unique<RecordingStrategy>("second", events));

  bool duplicate_rejected = false;
  try {
    trader.addStrategy(
        std::make_unique<RecordingStrategy>("first", events));
  } catch (const std::invalid_argument&) {
    duplicate_rejected = true;
  }
  require(duplicate_rejected, "trader accepted a duplicate strategy ID");

  trader.start();
  require(events == std::vector<std::string>{"start:first", "start:second"},
          "trader did not start strategies in registration order");

  bool late_registration_rejected = false;
  try {
    trader.addStrategy(
        std::make_unique<RecordingStrategy>("third", events));
  } catch (const std::logic_error&) {
    late_registration_rejected = true;
  }
  require(late_registration_rejected,
          "trader accepted strategy registration after start");

  trader.stop();
  require(events ==
              std::vector<std::string>{"start:first", "start:second",
                                       "stop:second", "stop:first"},
          "trader did not stop strategies in reverse order");

  events.clear();
  trading::Trader failing_trader(context);
  failing_trader.addStrategy(
      std::make_unique<RecordingStrategy>("first", events));
  failing_trader.addStrategy(
      std::make_unique<RecordingStrategy>("failing", events, true));
  bool start_failed = false;
  try {
    failing_trader.start();
  } catch (const std::runtime_error&) {
    start_failed = true;
  }
  require(start_failed, "trader ignored a strategy start failure");
  require(events ==
              std::vector<std::string>{"start:first", "start:failing",
                                       "stop:failing", "stop:first"},
          "trader did not roll back a partial start");
}

void testCacheState() {
  trading::Cache cache(trading::CacheConfig{2, 2});
  cache.addInstrument(bitcoinInstrument());
  cache.addAccount(simulationAccount());
  trading::Portfolio portfolio(cache);
  trading::MessageBus message_bus;
  trading::DataEngine data(cache, message_bus);
  message_bus.subscribe<trading::QuoteTick>(
      [&](const auto& quote) { portfolio.update(quote); });
  message_bus.subscribe<trading::TradeTick>(
      [&](const auto& trade) { portfolio.update(trade); });
  message_bus.subscribe<trading::Bar>(
      [&](const auto& bar) { portfolio.update(bar); });

  int trade_events = 0;
  int bar_events = 0;
  message_bus.subscribe<trading::TradeTick>(
      [&](const auto&) { ++trade_events; });
  message_bus.subscribe<trading::Bar>([&](const auto&) { ++bar_events; });
  for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
    data.process(trading::QuoteTick{
        instrumentId("BTC-USD"), Price::fromString("99"),
        Price::fromString("101"),
        Quantity::fromString("1"), Quantity::fromString("1"), sequence,
        sequence});
    data.process(trading::TradeTick{
        instrumentId("BTC-USD"), Price::fromString("100"),
        Quantity::fromString("1"),
        trading::AggressorSide::Buyer, "T-" + std::to_string(sequence),
        sequence, sequence, sequence});
  }
  require(cache.quoteCount(instrumentId("BTC-USD")) == 2 &&
              cache.quote(instrumentId("BTC-USD"), 0)->sequence == 3 &&
              cache.quote(instrumentId("BTC-USD"), 1)->sequence == 2,
          "bounded quote history was not newest-first");
  require(cache.tradeCount(instrumentId("BTC-USD")) == 2 &&
              trade_events == 3,
          "bounded trade history or publication was wrong");

  const trading::BarType bar_type{instrumentId("BTC-USD"), 60'000'000'000};
  data.process(trading::Bar{bar_type,
                            Price::fromString("100"),
                            Price::fromString("105"),
                            Price::fromString("95"),
                            Price::fromString("101"),
                            Quantity::fromString("10"),
                            100,
                            100});
  data.process(trading::Bar{bar_type,
                            Price::fromString("100"),
                            Price::fromString("106"),
                            Price::fromString("95"),
                            Price::fromString("102"),
                            Quantity::fromString("12"),
                            100,
                            101});
  data.process(trading::Bar{bar_type,
                            Price::fromString("99"),
                            Price::fromString("100"),
                            Price::fromString("98"),
                            Price::fromString("99"),
                            Quantity::fromString("1"),
                            99,
                            102});
  require(cache.barCount(bar_type) == 1 &&
              cache.bar(bar_type)->close == Price::fromString("102") &&
              bar_events == 2,
          "bar replacement or stale-bar filtering was wrong");

  const trading::SubmitOrder buy{
      "BUY-1",          instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("3"), Price::fromString("100"),
      "strategy-a",     "SIM-EXEC", trading::OrderType::Limit,
      trading::TimeInForce::Gtc, 10};
  cache.addOrder(buy, "SIM-001", "SIM-EXEC");
  require(cache.order("BUY-1")->status == trading::OrderStatus::Initialized &&
              cache.ordersInflight().empty(),
          "order was not cached before risk submission");
  cache.submitOrder("BUY-1", 10);
  require(cache.ordersInflight().size() == 1,
          "submitted order was not indexed as inflight");
  cache.acceptOrder(trading::OrderAccepted{"BUY-1", "V-BUY-1", 11});
  cache.updateOrder(trading::OrderUpdated{
      "BUY-1", Quantity::fromString("3"), Price::fromString("100"), 12});
  const trading::OrderFilled first_fill{
      "BUY-1", "V-BUY-1", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("1"), Price::fromString("100"), "TF-1",
      trading::Money::fromString("0.1", "USD"), 13};
  require(portfolio.apply(first_fill), "first fill was not applied");
  const bool duplicate_fill_ignored = !portfolio.apply(first_fill);
  require(cache.order("BUY-1")->status == trading::OrderStatus::PartiallyFilled &&
              cache.ordersOpen().size() == 1 && duplicate_fill_ignored,
          "partial fill state or duplicate trade detection was wrong");
  const trading::OrderFilled second_fill{
      "BUY-1", "V-BUY-1", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("2"), Price::fromString("100"), "TF-2",
      trading::Money::fromString("0.2", "USD"), 14};
  portfolio.apply(second_fill);

  const trading::SubmitOrder sell{
      "SELL-1",         instrumentId("BTC-USD"), OrderSide::Sell,
      Quantity::fromString("4"), Price::fromString("110"),
      "strategy-a",     "SIM-EXEC", trading::OrderType::Limit,
      trading::TimeInForce::Gtc, 20};
  cache.addOrder(sell, "SIM-001", "SIM-EXEC");
  cache.submitOrder("SELL-1", 20);
  cache.acceptOrder(trading::OrderAccepted{"SELL-1", "V-SELL-1", 21});
  const trading::OrderFilled sell_fill{
      "SELL-1", "V-SELL-1", instrumentId("BTC-USD"), OrderSide::Sell,
      Quantity::fromString("4"), Price::fromString("110"), "TF-3",
      trading::Money::fromString("0.4", "USD"), 22};
  portfolio.apply(sell_fill);

  const auto* position =
      cache.positionForInstrument("SIM-001", instrumentId("BTC-USD"));
  const auto* account = cache.account("SIM-001");
  require(position != nullptr && position->quantity_raw == -1'000'000'000 &&
              position->isOpen() && !position->isClosed() &&
              position->average_open_price == Price::fromString("110") &&
              position->realized_pnl ==
                  trading::Money::fromString("29.3", "USD"),
          "position reversal or realized PnL was wrong");
  require(account != nullptr &&
              account->balance.total ==
                  trading::Money::fromString("1139.3", "USD") &&
              account->fees == trading::Money::fromString("0.7", "USD") &&
              account->realized_pnl ==
                  trading::Money::fromString("29.3", "USD"),
          "account cash, fees, or realized PnL was wrong");
  require(portfolio.unrealizedPnl("SIM-001", instrumentId("BTC-USD")) ==
              trading::Money::fromString("9", "USD") &&
              portfolio.markValue("SIM-001", instrumentId("BTC-USD")) ==
                  trading::Money::fromString("-101", "USD") &&
              portfolio.equity("SIM-001") ==
                  trading::Money::fromString("1038.3", "USD"),
          "short position was not marked at the quote ask");
  require(cache.clientOrderId("V-BUY-1") == "BUY-1" &&
              cache.venueOrderId("BUY-1") == "V-BUY-1" &&
              cache.ordersForInstrument(instrumentId("BTC-USD")).size() == 2 &&
              cache.ordersForStrategy("strategy-a").size() == 2 &&
              cache.ordersForAccount("SIM-001").size() == 2 &&
              cache.ordersClosed().size() == 2,
          "order secondary indexes were not maintained");

  const trading::SubmitOrder canceled{
      "CANCEL-1",       instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("1"), Price::fromString("100"),
      "strategy-b",     "SIM-EXEC", trading::OrderType::Limit,
      trading::TimeInForce::Gtc, 30};
  cache.addOrder(canceled, "SIM-001", "SIM-EXEC");
  cache.submitOrder("CANCEL-1", 30);
  cache.acceptOrder(trading::OrderAccepted{"CANCEL-1", "V-CANCEL-1", 31});
  cache.cancelOrder(trading::OrderCanceled{"CANCEL-1", "user request", 32});

  auto expired = canceled;
  expired.client_order_id = "EXPIRE-1";
  cache.addOrder(expired, "SIM-001", "SIM-EXEC");
  cache.submitOrder("EXPIRE-1", 30);
  cache.acceptOrder(trading::OrderAccepted{"EXPIRE-1", "V-EXPIRE-1", 33});
  cache.expireOrder(trading::OrderExpired{"EXPIRE-1", 34});

  auto rejected = canceled;
  rejected.client_order_id = "REJECT-1";
  cache.addOrder(rejected, "SIM-001", "SIM-EXEC");
  cache.submitOrder("REJECT-1", 30);
  cache.rejectOrder(
      trading::OrderRejected{"REJECT-1", "venue rejected", 35});
  auto denied = canceled;
  denied.client_order_id = "DENY-1";
  cache.addOrder(denied, "SIM-001", "SIM-EXEC");
  cache.denyOrder(trading::OrderDenied{"DENY-1", "risk denied", 36});
  require(cache.order("CANCEL-1")->status == trading::OrderStatus::Canceled &&
              cache.order("EXPIRE-1")->status == trading::OrderStatus::Expired &&
              cache.order("DENY-1")->status == trading::OrderStatus::Denied &&
              cache.order("REJECT-1")->status == trading::OrderStatus::Rejected &&
              cache.ordersOpen().empty() && cache.ordersInflight().empty() &&
              cache.ordersClosed().size() == 6,
          "deny, cancel, expire, or reject lifecycle state was wrong");
}

void testRiskUsesCachedState() {
  trading::Cache cache;
  cache.addInstrument(bitcoinInstrument());
  cache.addAccount(simulationAccount("100"));
  trading::RiskEngineConfig config;
  config.max_notional_per_order.emplace(
      instrumentId("BTC-USD"), trading::Decimal::fromString("1000"));
  trading::RiskEngine risk(cache, std::move(config));

  const auto off_tick = risk.execute(
      trading::SubmitOrder{"R-1", instrumentId("BTC-USD"), OrderSide::Buy,
                           Quantity::fromString("1"),
                           Price::fromString("100.001")},
      "SIM-001");
  require(off_tick && off_tick->reason == "price is not aligned to tick size",
          "risk accepted a price off tick size");
  const auto insufficient_cash = risk.execute(
      trading::SubmitOrder{"R-2", instrumentId("BTC-USD"), OrderSide::Buy,
                           Quantity::fromString("2"),
                           Price::fromString("100")},
      "SIM-001");
  require(insufficient_cash &&
              insufficient_cash->reason == "insufficient available cash",
          "risk accepted a buy above available cash");
  const auto insufficient_position = risk.execute(
      trading::SubmitOrder{"R-3", instrumentId("BTC-USD"), OrderSide::Sell,
                           Quantity::fromString("2"),
                           Price::fromString("100")},
      "SIM-001");
  require(insufficient_position &&
              insufficient_position->reason ==
                  "insufficient available position",
          "cash account risk accepted an uncovered sell");

  const auto first_buy = risk.execute(
      trading::SubmitOrder{"R-4", instrumentId("BTC-USD"), OrderSide::Buy,
                           Quantity::fromString("0.6"),
                           Price::fromString("100")},
      "SIM-001");
  const auto second_buy = risk.execute(
      trading::SubmitOrder{"R-5", instrumentId("BTC-USD"), OrderSide::Buy,
                           Quantity::fromString("0.5"),
                           Price::fromString("100")},
      "SIM-001");
  require(!first_buy && second_buy &&
              second_buy->reason == "insufficient available cash",
          "inflight buy reservation did not protect account cash");

  risk.setTradingState(trading::TradingState::Halted);
  const auto halted = risk.execute(
      trading::SubmitOrder{"R-6", instrumentId("BTC-USD"), OrderSide::Buy,
                           Quantity::fromString("0.1"),
                           Price::fromString("100")},
      "SIM-001");
  require(halted && halted->reason == "trading is halted",
          "halted trading state accepted an order");
}

void testAtomicFillAndReplay() {
  trading::Cache cache;
  cache.addInstrument(bitcoinInstrument());
  cache.addAccount(simulationAccount("1000"));
  trading::Portfolio portfolio(cache);

  const trading::SubmitOrder order{
      "ATOMIC-1", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("1"), Price::fromString("100")};
  cache.addOrder(order, "SIM-001", "SIM-EXEC");
  cache.submitOrder("ATOMIC-1", 1);
  cache.acceptOrder(trading::OrderAccepted{"ATOMIC-1", "V-ATOMIC-1", 2});

  const trading::OrderFilled invalid_fill{
      "ATOMIC-1", "V-ATOMIC-1", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("1"), Price::fromString("100"), "T-ATOMIC-1",
      trading::Money::fromString("1", "EUR"), 3};
  bool invalid_fill_rejected = false;
  try {
    static_cast<void>(portfolio.apply(invalid_fill));
  } catch (const std::invalid_argument&) {
    invalid_fill_rejected = true;
  }
  const auto* unchanged_order = cache.order("ATOMIC-1");
  const auto* unchanged_account = cache.account("SIM-001");
  require(invalid_fill_rejected && unchanged_order != nullptr &&
              unchanged_order->status == trading::OrderStatus::Accepted &&
              unchanged_order->filled_quantity.isZero() &&
              unchanged_account != nullptr &&
              unchanged_account->balance.total ==
                  trading::Money::fromString("1000", "USD") &&
              cache.positionForInstrument("SIM-001",
                                          instrumentId("BTC-USD")) == nullptr &&
              cache.fill("T-ATOMIC-1") == nullptr,
          "failed fill left a partial order, account, or position update");

  auto valid_fill = invalid_fill;
  valid_fill.fee = trading::Money::fromString("1", "USD");
  require(portfolio.apply(valid_fill) && !portfolio.apply(valid_fill),
          "identical fill replay was not idempotent");
  auto conflicting_fill = valid_fill;
  conflicting_fill.price = Price::fromString("101");
  bool conflicting_replay_rejected = false;
  try {
    static_cast<void>(portfolio.apply(conflicting_fill));
  } catch (const std::invalid_argument&) {
    conflicting_replay_rejected = true;
  }
  require(conflicting_replay_rejected &&
              cache.account("SIM-001")->balance.total ==
                  trading::Money::fromString("899", "USD") &&
              cache.order("ATOMIC-1")->filled_quantity ==
                  Quantity::fromString("1"),
          "conflicting replay changed committed fill state");
}

void testPortfolioMarkToMarket() {
  trading::Cache cache;
  cache.addInstrument(bitcoinInstrument());
  cache.addAccount(simulationAccount("1000"));
  trading::Portfolio portfolio(cache);
  trading::MessageBus message_bus;
  trading::DataEngine data(cache, message_bus);
  message_bus.subscribe<trading::TradeTick>(
      [&](const auto& trade) { portfolio.update(trade); });
  message_bus.subscribe<trading::QuoteTick>(
      [&](const auto& quote) { portfolio.update(quote); });
  message_bus.subscribe<trading::Bar>(
      [&](const auto& bar) { portfolio.update(bar); });

  const trading::SubmitOrder order{
      "MARK-1", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("2"), Price::fromString("100")};
  cache.addOrder(order, "SIM-001", "SIM-EXEC");
  cache.submitOrder("MARK-1", 1);
  cache.acceptOrder(trading::OrderAccepted{"MARK-1", "V-MARK-1", 2});
  portfolio.apply(trading::OrderFilled{
      "MARK-1", "V-MARK-1", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("2"), Price::fromString("100"), "T-MARK-1",
      trading::Money::fromString("1", "USD"), 3});

  require(!portfolio.unrealizedPnl("SIM-001", instrumentId("BTC-USD")) &&
              !portfolio.markValue("SIM-001", instrumentId("BTC-USD")) &&
              !portfolio.equity("SIM-001"),
          "unpriced open position was reported as zero-valued");

  data.process(trading::TradeTick{
      instrumentId("BTC-USD"), Price::fromString("105"),
      Quantity::fromString("1"), trading::AggressorSide::Buyer,
      "T-MARKET-1", 1, 4, 4});
  require(portfolio.unrealizedPnl("SIM-001", instrumentId("BTC-USD")) ==
              trading::Money::fromString("10", "USD") &&
              portfolio.equity("SIM-001") ==
                  trading::Money::fromString("1009", "USD"),
          "latest trade did not provide the valuation fallback");

  bool portfolio_current_on_publish = false;
  message_bus.subscribe<trading::QuoteTick>([&](const auto&) {
    portfolio_current_on_publish =
        portfolio.unrealizedPnl("SIM-001", instrumentId("BTC-USD")) ==
        trading::Money::fromString("18", "USD");
  });
  data.process(trading::QuoteTick{
      instrumentId("BTC-USD"), Price::fromString("109"),
      Price::fromString("111"), Quantity::fromString("1"),
      Quantity::fromString("1"), 1, 4});
  require(portfolio_current_on_publish &&
              portfolio.unrealizedPnl("SIM-001", instrumentId("BTC-USD")) ==
              trading::Money::fromString("18", "USD") &&
              portfolio.realizedPnl("SIM-001", instrumentId("BTC-USD")) ==
                  trading::Money::fromString("-1", "USD") &&
              portfolio.totalPnl("SIM-001", instrumentId("BTC-USD")) ==
                  trading::Money::fromString("17", "USD") &&
              portfolio.markValue("SIM-001", instrumentId("BTC-USD")) ==
                  trading::Money::fromString("218", "USD") &&
              portfolio.netExposure("SIM-001", instrumentId("BTC-USD")) ==
                  trading::Money::fromString("218", "USD") &&
              portfolio.equity("SIM-001") ==
                  trading::Money::fromString("1017", "USD"),
          "quote mark-to-market portfolio values were wrong");
}

void testPositionRiskAndContractMultiplier() {
  trading::Cache cache;
  auto instrument = bitcoinInstrument();
  instrument.contract_multiplier = trading::Decimal::fromString("2");
  cache.addInstrument(instrument);
  cache.addAccount(simulationAccount("10000"));
  trading::Portfolio portfolio(cache);

  const trading::SubmitOrder seed{
      "SEED", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("10"), Price::fromString("10")};
  cache.addOrder(seed, "SIM-001", "SIM-EXEC");
  cache.submitOrder("SEED", 1);
  cache.acceptOrder(trading::OrderAccepted{"SEED", "V-SEED", 2});
  const trading::OrderFilled seed_fill{
      "SEED", "V-SEED", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("10"), Price::fromString("10"), "T-SEED",
      std::nullopt, 3};
  portfolio.apply(seed_fill);

  trading::RiskEngineConfig config;
  config.max_notional_per_order.emplace(
      instrumentId("BTC-USD"), trading::Decimal::fromString("150"));
  trading::RiskEngine risk(cache, std::move(config));
  const auto multiplier_limit = risk.execute(
      trading::SubmitOrder{"M-1", instrumentId("BTC-USD"), OrderSide::Buy,
                           Quantity::fromString("1"),
                           Price::fromString("100")},
      "SIM-001");
  require(multiplier_limit &&
              multiplier_limit->reason == "max notional per order exceeded",
          "contract multiplier was not included in order notional");

  const auto first_sell = risk.execute(
      trading::SubmitOrder{"P-1", instrumentId("BTC-USD"), OrderSide::Sell,
                           Quantity::fromString("6"),
                           Price::fromString("10")},
      "SIM-001");
  const auto second_sell = risk.execute(
      trading::SubmitOrder{"P-2", instrumentId("BTC-USD"), OrderSide::Sell,
                           Quantity::fromString("5"),
                           Price::fromString("10")},
      "SIM-001");
  require(!first_sell && second_sell &&
              second_sell->reason == "insufficient available position",
          "inflight sell reservation did not protect the position");

  risk.setTradingState(trading::TradingState::Reducing);
  auto reduce_order = trading::SubmitOrder{
      "P-3", instrumentId("BTC-USD"), OrderSide::Sell,
      Quantity::fromString("4"), Price::fromString("10")};
  reduce_order.reduce_only = true;
  require(!risk.execute(reduce_order, "SIM-001"),
          "reducing state denied a valid reduce-only order");
  auto increase_order = reduce_order;
  increase_order.client_order_id = "P-4";
  increase_order.side = OrderSide::Buy;
  require(risk.execute(increase_order, "SIM-001")->reason ==
              "reduce-only order would not reduce the position",
          "reduce-only order was allowed to increase the position");
}

void testExecutionClientAccountRouting() {
  RecordingExecutionClient primary("SIM-EXEC-1", "SIM-001", "SIM");
  RecordingExecutionClient secondary("SIM-EXEC-2", "SIM-002", "SIM");
  trading::TradingKernel kernel(primary);
  kernel.addExecutionClient(secondary);
  kernel.addInstrument(bitcoinInstrument());
  kernel.addAccount(simulationAccount("1000"));
  auto second_account = simulationAccount("2000");
  second_account.id = "SIM-002";
  kernel.addAccount(std::move(second_account));

  kernel.submitOrder(trading::SubmitOrder{
      "ROUTE-1", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("1"), Price::fromString("100"), "strategy-route",
      trading::ExecutionClientId{"SIM-EXEC-2"}});
  const auto* routed = kernel.cache().order("ROUTE-1");
  require(primary.submittedOrders().empty() &&
              secondary.submittedOrders().size() == 1 && routed != nullptr &&
              routed->account_id == "SIM-002" &&
              routed->execution_client_id == "SIM-EXEC-2" &&
              kernel.cache().ordersForAccount("SIM-002").size() == 1,
          "explicit execution client did not select its bound account");

  kernel.submitOrder(trading::SubmitOrder{
      "ROUTE-2", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("1"), Price::fromString("100")});
  const auto* default_routed = kernel.cache().order("ROUTE-2");
  require(primary.submittedOrders().size() == 1 &&
              default_routed != nullptr &&
              default_routed->account_id == "SIM-001" &&
              default_routed->execution_client_id == "SIM-EXEC-1",
          "venue routing did not select the default execution client");

  int route_denials = 0;
  kernel.messageBus().subscribe<trading::OrderDenied>([&](const auto& event) {
    if (event.client_order_id == "ROUTE-3" &&
        event.reason == "no execution client route") {
      ++route_denials;
    }
  });
  kernel.submitOrder(trading::SubmitOrder{
      "ROUTE-3", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("1"), Price::fromString("100"), "strategy-route",
      trading::ExecutionClientId{"UNKNOWN"}});
  require(route_denials == 1 && kernel.cache().order("ROUTE-3") == nullptr,
          "unknown execution client route was not rejected");

  int risk_denials = 0;
  kernel.messageBus().subscribe<trading::OrderDenied>([&](const auto& event) {
    if (event.client_order_id == "ROUTE-4" &&
        event.reason == "trading is halted") {
      ++risk_denials;
    }
  });
  kernel.setTradingState(trading::TradingState::Halted, 10);
  kernel.submitOrder(trading::SubmitOrder{
      "ROUTE-4", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("1"), Price::fromString("100")});
  const auto* denied = kernel.cache().order("ROUTE-4");
  require(risk_denials == 1 && denied != nullptr &&
              denied->status == trading::OrderStatus::Denied &&
              primary.submittedOrders().size() == 1,
          "risk denial was not cached before publication");
}

void testSynchronousStrategySubmission() {
  static_assert(
      !std::is_constructible_v<trading::RuntimeMessage, trading::SubmitOrder>);
  RecordingExecutionClient client("SIM-EXEC", "SIM-001", "SIM");
  trading::TradingKernel kernel(client);
  kernel.addInstrument(bitcoinInstrument());
  kernel.addAccount(simulationAccount("1000"));
  auto strategy = std::make_unique<trading::OrderBookImbalanceStrategy>(
      trading::OrderBookImbalanceConfig{
          instrumentId("BTC-USD"), BookType::L2Mbp,
          Quantity::fromString("1"), Quantity::fromString("5"), 200'000, 0});
  const auto* result = strategy.get();
  kernel.trader().addStrategy(std::move(strategy));
  kernel.trader().start();

  std::vector<std::string> callbacks;
  kernel.messageBus().subscribe<trading::OrderBookImbalanceSignal>(
      [&](const auto&) { callbacks.push_back("signal"); });
  kernel.messageBus().subscribe<trading::OrderDenied>([&](const auto& event) {
    callbacks.push_back("denied");
    const auto* order = kernel.cache().order(event.client_order_id);
    require(order != nullptr && order->status == trading::OrderStatus::Denied &&
                result->submitCount() == 2,
            "synchronous denial observed incomplete strategy or order state");
  });
  kernel.messageBus().subscribe<trading::OrderBookDeltas>([&](const auto&) {
    callbacks.push_back("deltas");
    require(kernel.cache().order("IMB-1") != nullptr,
            "strategy order was deferred until after the market callback");
  });

  kernel.dispatch(trading::OrderBookDepth{
      instrumentId("BTC-USD"),
      {{OrderSide::Buy, Price::fromString("99.95"), Quantity::fromString("12"), 1}},
      {{OrderSide::Sell, Price::fromString("100.05"), Quantity::fromString("10"), 2}},
      1, 1, 1});
  const auto update = [&](std::uint64_t sequence) {
    kernel.dispatch(trading::OrderBookDeltas{
        instrumentId("BTC-USD"),
        {delta(instrumentId("BTC-USD"), BookAction::Update, OrderSide::Sell,
               "100.05", "2", 2, sequence, trading::kBookFlagLast)},
        sequence, sequence, sequence});
  };

  update(2);
  require(client.submittedOrders().size() == 1 &&
              kernel.cache().order("IMB-1")->status ==
                  trading::OrderStatus::Submitted,
          "strategy did not submit synchronously without an event loop");
  const auto& order = client.submittedOrders().front();
  require(order.strategy_id == result->id() && order.side == OrderSide::Buy &&
              order.quantity == Quantity::fromString("1") &&
              order.limit_price == Price::fromString("100.05") &&
              order.time_in_force == trading::TimeInForce::Fok,
          "synchronous imbalance order fields were wrong");
  update(3);
  require(client.submittedOrders().size() == 1 && result->signalCount() == 1,
          "strategy submitted a second order while its first was inflight");

  kernel.dispatch(trading::OrderAccepted{"IMB-1", "VENUE-IMB-1"});
  kernel.dispatch(trading::OrderFilled{
      "IMB-1", "VENUE-IMB-1", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("1"), Price::fromString("100.05"),
      "TRADE-IMB-1", std::nullopt, 2});
  kernel.setTradingState(trading::TradingState::Halted);
  callbacks.clear();
  update(4);
  require(callbacks == std::vector<std::string>{"signal", "denied", "deltas"} &&
              client.submittedOrders().size() == 1,
          "risk denial did not run inside the originating strategy callback");

  kernel.setTradingState(trading::TradingState::Active);
  update(5);
  require(client.submittedOrders().size() == 2 &&
              client.submittedOrders().back().client_order_id == "IMB-3" &&
              result->submitCount() == 3,
          "strategy did not resume with a unique order ID after a denial");
  kernel.trader().stop();
}

void testSynchronousDenialCallbackSubmission() {
  RecordingExecutionClient client("SIM-EXEC", "SIM-001", "SIM");
  trading::TradingKernel kernel(client);
  kernel.addInstrument(bitcoinInstrument());
  kernel.addAccount(simulationAccount("1000"));
  std::vector<std::string> callbacks{"before-submit"};
  kernel.messageBus().subscribe<trading::OrderDenied>([&](const auto& event) {
    callbacks.push_back("denied");
    const auto* denied = kernel.cache().order(event.client_order_id);
    require(event.reason == "insufficient available cash" && denied != nullptr &&
                denied->status == trading::OrderStatus::Denied,
            "denial callback observed an incorrect reason or stale cache");
    kernel.submitOrder(trading::SubmitOrder{
        "REPLACEMENT", instrumentId("BTC-USD"), OrderSide::Buy,
        Quantity::fromString("1"), Price::fromString("100")});
    callbacks.push_back("replacement-submitted");
  });
  kernel.submitOrder(trading::SubmitOrder{
      "DENIED", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("20"), Price::fromString("100")});
  callbacks.push_back("after-submit");
  require(callbacks == std::vector<std::string>{
                           "before-submit", "denied", "replacement-submitted",
                           "after-submit"} &&
              client.submittedOrders().size() == 1 &&
              client.submittedOrders().front().client_order_id == "REPLACEMENT",
          "denial callback could not synchronously submit a valid replacement");
}

void testSynchronousSubmissionThreadOwnership() {
  RecordingExecutionClient client("SIM-EXEC", "SIM-001", "SIM");
  trading::TradingKernel kernel(client);
  kernel.addInstrument(bitcoinInstrument());
  kernel.addAccount(simulationAccount("1000"));
  bool rejected = false;
  std::thread worker([&] {
    try {
      kernel.submitOrder(trading::SubmitOrder{
          "WRONG-THREAD", instrumentId("BTC-USD"), OrderSide::Buy,
          Quantity::fromString("1"), Price::fromString("100")});
    } catch (const std::logic_error&) {
      rejected = true;
    }
  });
  worker.join();
  require(rejected && kernel.cache().order("WRONG-THREAD") == nullptr &&
              client.submittedOrders().empty(),
          "cross-thread synchronous submission changed core state");
}

void testLiveEventLoopBatchPriority() {
  trading::LiveEventLoop event_loop;
  std::vector<trading::RuntimeMessage> messages{
      trading::QuoteTick{}, trading::OrderAccepted{}, trading::TradeTick{},
      trading::TradingStateChanged{}, trading::OrderFilled{}, trading::Bar{},
      trading::OrderUpdated{}, trading::OrderBookDeltas{}, trading::OrderCanceled{},
      trading::OrderBookDepth{}, trading::OrderDenied{}, trading::OrderRejected{},
      trading::OrderExpired{}, trading::TradingStateChanged{}, trading::QuoteTick{}};
  std::vector<std::uint64_t> received;
  event_loop.bind([&](trading::RuntimeMessage message) {
    received.push_back(std::visit([](const auto& event) { return event.ts_event; },
                                 message));
    if (received.size() == messages.size()) {
      event_loop.stop();
    }
  });
  for (std::size_t i = 0; i < messages.size(); ++i) {
    std::visit([i](auto& event) { event.ts_event = i + 1; }, messages[i]);
    event_loop.send(std::move(messages[i]));
  }
  event_loop.scheduleAfter(std::chrono::seconds(2), [&] { event_loop.stop(); });
  event_loop.run();
  require(received == std::vector<std::uint64_t>{4, 14, 2, 5, 7, 9, 11, 12, 13,
                                               1, 3, 6, 8, 10, 15},
          "batch priority changed same-priority order or misclassified an event");
}

void testLiveEventLoopBatchBoundary() {
  trading::LiveEventLoop event_loop;
  constexpr std::uint64_t quote_count = 200;
  std::vector<std::uint64_t> received;
  event_loop.bind([&](trading::RuntimeMessage message) {
    received.push_back(std::visit([](const auto& event) { return event.ts_event; },
                                 message));
    if (received.size() == 1) {
      event_loop.send(trading::OrderAccepted{"LATE", "SIM", 0});
    }
    if (received.size() == quote_count + 1) {
      event_loop.stop();
    }
  });
  for (std::uint64_t i = 1; i <= quote_count; ++i) {
    trading::QuoteTick quote{};
    quote.ts_event = i;
    event_loop.send(quote);
  }
  event_loop.scheduleAfter(std::chrono::seconds(2), [&] { event_loop.stop(); });
  event_loop.run();

  std::vector<std::uint64_t> expected;
  for (std::uint64_t i = 1; i <= quote_count; ++i) {
    expected.push_back(i);
    if (i == 128) {
      expected.push_back(0);
    }
  }
  require(received == expected,
          "late execution event did not wait for and lead the next batch");
}

void testLiveEventLoopYieldsToTimers() {
  trading::LiveEventLoop event_loop;
  constexpr std::size_t event_count = 4096;
  std::size_t received = 0;
  std::size_t received_at_timer = event_count;
  event_loop.bind([&](trading::RuntimeMessage) {
    if (++received == 1) {
      const auto timer_due = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(5);
      event_loop.scheduleAfter(std::chrono::milliseconds(1),
                               [&] { received_at_timer = received; });

      // Make the timer due while the first batch is still executing
      std::this_thread::sleep_until(timer_due);
    }
    if (received == event_count) {
      event_loop.stop();
    }
  });
  for (std::size_t i = 0; i < event_count; ++i) {
    event_loop.send(trading::OrderAccepted{"ORDER", "SIM", i});
  }
  event_loop.scheduleAfter(std::chrono::seconds(2), [&] { event_loop.stop(); });
  event_loop.run();
  require(received == event_count && received_at_timer > 0 &&
              received_at_timer < event_count,
          "event backlog prevented a due timer from running between batches");
}

void testLiveEventLoopHandlerFailure() {
  trading::LiveEventLoop event_loop;
  int received = 0;
  event_loop.bind([&](trading::RuntimeMessage) {
    ++received;
    throw std::runtime_error("event handler failed");
  });
  event_loop.send(trading::OrderAccepted{"FIRST", "SIM", 1});
  event_loop.send(trading::OrderAccepted{"SECOND", "SIM", 2});
  event_loop.scheduleAfter(std::chrono::seconds(2), [&] { event_loop.stop(); });
  bool propagated = false;
  try {
    event_loop.run();
  } catch (const std::runtime_error& e) {
    propagated = std::string_view(e.what()) == "event handler failed";
  }
  require(propagated && received == 1,
          "handler failure was not propagated or the failed batch kept running");
}

void testUnboundedLiveEventLoopIngress() {
  constexpr std::size_t producer_count = 4;
  constexpr std::size_t batch_size = 1024;
  constexpr std::size_t first_batch_count = producer_count * batch_size;
  constexpr std::size_t total_count = 2 * first_batch_count;
  trading::LiveEventLoop event_loop;
  const auto owner = std::this_thread::get_id();
  std::vector<std::size_t> next_sequence(producer_count, 0);
  std::size_t received = 0;
  bool valid = true;
  std::promise<void> resume;
  const auto resumed = resume.get_future().share();
  event_loop.bind([&](trading::RuntimeMessage message) {
    const auto& event = std::get<trading::OrderAccepted>(message);
    const auto producer = std::stoul(event.client_order_id);
    valid = valid && std::this_thread::get_id() == owner &&
            producer < producer_count &&
            event.ts_event == next_sequence[producer]++;
    if (++received == first_batch_count) {

      // Release producers from a timer after the first drain has finished
      event_loop.scheduleAfter(std::chrono::milliseconds(1),
                               [&] { resume.set_value(); });
    } else if (received == total_count) {
      event_loop.stop();
    }
  });

  const auto send_batch = [&](std::size_t producer, std::size_t begin) {
    for (std::size_t sequence = begin; sequence < begin + batch_size;
         ++sequence) {
      require(event_loop.send(trading::OrderAccepted{std::to_string(producer),
                                                     "SIM", sequence}),
              "unbounded event loop rejected an incoming event");
    }
  };
  std::latch start(producer_count);
  std::vector<std::future<void>> workers;
  for (std::size_t producer = 0; producer < producer_count; ++producer) {
    workers.push_back(std::async(std::launch::async, [&, producer] {
      start.arrive_and_wait();
      send_batch(producer, 0);
    }));
  }
  for (auto& worker : workers) {
    worker.get();
  }
  workers.clear();

  for (std::size_t producer = 0; producer < producer_count; ++producer) {
    workers.push_back(std::async(std::launch::async, [&, producer] {
      require(resumed.wait_for(std::chrono::seconds(5)) ==
                  std::future_status::ready,
              "event loop did not drain its initial backlog");
      send_batch(producer, batch_size);
    }));
  }
  event_loop.scheduleAfter(std::chrono::seconds(2), [&] { event_loop.stop(); });
  event_loop.run();
  for (auto& worker : workers) {
    worker.get();
  }
  require(
      valid && received == total_count &&
          next_sequence ==
              std::vector<std::size_t>(producer_count, 2 * batch_size),
      "MPSC ingress lost, duplicated, reordered, or failed to wake for events");
}

void testKernelPipelineAndStrategy() {
  folly::CPUThreadPoolExecutor executor(2);
  trading::LiveEventLoop event_loop;
  trading::FakeExecutionClient client(event_loop, executor);
  trading::TradingKernel kernel(client);
  kernel.addInstrument(bitcoinInstrument());
  kernel.addAccount(simulationAccount("1000000"));
  auto strategy = std::make_unique<trading::OrderBookImbalanceStrategy>(
      trading::OrderBookImbalanceConfig{
          instrumentId("BTC-USD"), BookType::L2Mbp,
          Quantity::fromString("1"),
          Quantity::fromString("5"), 200'000, 0});
  const auto* strategy_result = strategy.get();
  kernel.trader().addStrategy(std::move(strategy));
  kernel.trader().start();

  int delta_count = 0;
  int signal_count = 0;
  bool cache_was_current = false;
  kernel.messageBus().subscribe<trading::OrderBookDeltas>([&](const auto&) {
    ++delta_count;
    const auto* book = kernel.cache().orderBook(instrumentId("BTC-USD"));
    cache_was_current = book != nullptr &&
                        book->bestAskSize() == Quantity::fromString("2");
  });
  kernel.messageBus().subscribe<trading::OrderBookImbalanceSignal>(
      [&](const auto& signal) {
        ++signal_count;
        require(signal.side == OrderSide::Buy,
                "imbalance strategy chose the wrong side");
        require(signal.price == Price::fromString("100.05"),
                "imbalance strategy chose the wrong price");
      });

  event_loop.bind([&kernel](trading::RuntimeMessage message) {
    kernel.dispatch(std::move(message));
  });
  event_loop.send(trading::OrderBookDepth{
      instrumentId("BTC-USD"),
      {{OrderSide::Buy, Price::fromString("99.95"), Quantity::fromString("12"), 1}},
      {{OrderSide::Sell, Price::fromString("100.05"), Quantity::fromString("10"), 2}},
      1,
      1,
      1});
  event_loop.send(trading::OrderBookDeltas{
      instrumentId("BTC-USD"),
      {delta(instrumentId("BTC-USD"), BookAction::Update, OrderSide::Sell,
             "100.05", "2", 2, 2, trading::kBookFlagLast)},
      2,
      2,
      2});
  kernel.submitOrder(trading::SubmitOrder{
      "TEST-1", instrumentId("BTC-USD"), OrderSide::Buy,
      Quantity::fromString("2"), Price::fromString("100")});
  std::promise<void> completed;
  const auto completion = completed.get_future().share();
  int fill_count = 0;
  kernel.messageBus().subscribe<trading::OrderFilled>([&](const auto&) {
    if (++fill_count == 2) {
      completed.set_value();
      event_loop.stop();
    }
  });
  executor.add([&event_loop, completion] {
    if (completion.wait_for(std::chrono::seconds(2)) !=
        std::future_status::ready) {
      event_loop.stop();
    }
  });
  event_loop.run();
  executor.join();
  kernel.trader().stop();

  const auto* order = kernel.cache().order("TEST-1");
  const auto* imb_order = kernel.cache().order("IMB-1");
  require(delta_count == 1 && signal_count == 1 && cache_was_current,
          "book pipeline did not update cache before publication");
  require(strategy_result->updateCount() == 1 &&
              strategy_result->signalCount() == 1 &&
              strategy_result->submitCount() == 1,
          "trader-managed strategy did not process the book update");
  require(order != nullptr && order->status == trading::OrderStatus::Filled,
          "exact order flow did not finish");
  require(imb_order != nullptr &&
              imb_order->status == trading::OrderStatus::Filled,
          "imbalance strategy order was not filled");
  require(kernel.portfolio().position("SIM-001", instrumentId("BTC-USD")) ==
              trading::Decimal::fromString("3"),
          "exact portfolio position was wrong");
}

void testEmaCrossCrossover() {
  trading::Cache cache;
  cache.addInstrument(bitcoinInstrument());
  cache.addAccount(simulationAccount("1000000"));
  trading::MessageBus message_bus;
  std::vector<trading::SubmitOrder> submitted;
  trading::StrategyContext context(
      cache, message_bus,
      [&](const trading::SubmitOrder& order) {
        submitted.push_back(order);
      },
      [](trading::InstrumentId, BookType) {});

  auto strategy = std::make_unique<trading::EmaCrossStrategy>(
      trading::EmaCrossConfig{
          instrumentId("BTC-USD"),
          Quantity::fromString("1"), 3, 10});
  const auto* result = strategy.get();
  strategy->registerContext(context);
  strategy->start();

  // Send quotes at mid price 100 to warm up both EMAs.
  for (int i = 0; i < 15; ++i) {
    message_bus.publish(trading::QuoteTick{
        instrumentId("BTC-USD"), Price::fromString("99"),
        Price::fromString("101"), Quantity::fromString("1"),
        Quantity::fromString("1"), static_cast<std::uint64_t>(i + 1),
        static_cast<std::uint64_t>(i + 1)});
  }

  // No crossover yet — mid = 100, both EMAs converged to 100.
  require(submitted.empty(), "EMA cross fired with stable mid price");

  // Send a quote with higher mid price so fast EMA crosses above slow.
  message_bus.publish(trading::QuoteTick{
      instrumentId("BTC-USD"), Price::fromString("109"),
      Price::fromString("111"), Quantity::fromString("1"),
      Quantity::fromString("1"), 16, 16});

  require(submitted.size() == 1,
          "EMA cross did not fire buy on up-cross");
  require(submitted[0].side == OrderSide::Buy &&
              submitted[0].quantity == Quantity::fromString("1"),
          "EMA cross buy order had wrong side or size");
  require(result->quoteCount() == 16 && result->signalCount() == 1,
          "EMA cross state counters were wrong");
}

}  // namespace

int main() {
  try {
    testFixedPointTypes();
    testL1Book();
    testL2BookAndAtomicSnapshot();
    testL3PriceTimePriority();
    testTraderLifecycle();
    testCacheState();
    testRiskUsesCachedState();
    testAtomicFillAndReplay();
    testPortfolioMarkToMarket();
    testPositionRiskAndContractMultiplier();
    testExecutionClientAccountRouting();
    testSynchronousStrategySubmission();
    testSynchronousDenialCallbackSubmission();
    testSynchronousSubmissionThreadOwnership();
    testLiveEventLoopBatchPriority();
    testLiveEventLoopBatchBoundary();
    testLiveEventLoopYieldsToTimers();
    testLiveEventLoopHandlerFailure();
    testUnboundedLiveEventLoopIngress();
    testKernelPipelineAndStrategy();
    testEmaCrossCrossover();
    std::cout << "all core tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& e) {
    std::cerr << "core test failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
}
