#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "trading/cache/cache.h"
#include "trading/common/message_bus.h"
#include "trading/data/data_engine.h"
#include "trading/execution/execution_engine.h"
#include "trading/execution/twap_algorithm.h"
#include "trading/portfolio/portfolio.h"
#include "trading/risk/risk_engine.h"
#include "trading/system/trader.h"

namespace trading {

class TradingKernel {
 public:
  explicit TradingKernel(ExecutionClient& default_client,
                         RiskEngineConfig risk_config = {},
                         Scheduler* scheduler = nullptr);

  MessageBus& messageBus() { return message_bus_; }
  const Cache& cache() const { return cache_; }
  const Portfolio& portfolio() const { return portfolio_; }
  Trader& trader() { return trader_; }
  const Trader& trader() const { return trader_; }

  void addInstrument(Instrument instrument) {
    cache_.addInstrument(std::move(instrument));
  }
  void addAccount(Account account) { cache_.addAccount(std::move(account)); }
  void addExecutionClient(ExecutionClient& client,
                          bool route_for_venue = false) {
    execution_engine_.registerClient(client, route_for_venue);
  }
  void setMaxNotionalPerOrder(const InstrumentId& instrument_id,
                              Decimal max_notional) {
    risk_engine_.setMaxNotionalPerOrder(instrument_id, max_notional);
  }
  void setTradingState(TradingState state, std::uint64_t ts_event = 0);
  TradingState tradingState() const {
    return risk_engine_.tradingState();
  }
  void addOrderBook(InstrumentId instrument_id, BookType book_type) {
    data_engine_.addOrderBook(std::move(instrument_id), book_type);
  }
  std::optional<BookError> lastBookError() const {
    return data_engine_.lastBookError();
  }

  // Runs routing, risk checks, and client submission on the core thread
  void submitOrder(const SubmitOrder& order);
  bool cancelTwap(const ClientOrderId& primary_order_id);
  void stopAlgorithms();
  const TwapExecutionState* twapExecution(const ClientOrderId& id) const;
  void dispatch(RuntimeMessage message);

 private:
  void wireComponentEvents();
  void submitDirect(const SubmitOrder& order);
  void refreshOrderState(const ClientOrderId& id, bool terminal_failure = false);
  void process(const QuoteTick& message);
  void process(const TradeTick& message);
  void process(const Bar& message);
  void process(const OrderBookDeltas& message);
  void process(const OrderBookDepth& message);
  void process(const OrderAccepted& event);
  void process(const OrderUpdated& event);
  void process(const OrderFilled& event);
  void process(const OrderCanceled& event);
  void process(const OrderDenied& event);
  void process(const OrderRejected& event);
  void process(const OrderExpired& event);
  void process(const TradingStateChanged& event);

  MessageBus message_bus_;
  Cache cache_;
  Portfolio portfolio_;
  RiskEngine risk_engine_;
  DataEngine data_engine_;
  ExecutionEngine execution_engine_;
  std::unique_ptr<TwapAlgorithm> twap_;
  StrategyContext strategy_context_;
  Trader trader_;
};

}  // namespace trading
