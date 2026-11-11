#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "trading/model/events.h"
#include "trading/model/instrument.h"
#include "trading/model/order_book.h"
#include "trading/model/trading_state.h"

namespace trading {

class Portfolio;

struct CacheConfig {
  std::size_t tick_capacity{10'000};
  std::size_t bar_capacity{10'000};
};

struct OrderState {
  SubmitOrder order;
  AccountId account_id;
  ExecutionClientId execution_client_id;
  OrderStatus status{OrderStatus::Initialized};
  VenueOrderId venue_order_id;
  Quantity filled_quantity;
  Quantity leaves_quantity;
  std::optional<Price> average_price;
  Money fees;
  std::string last_reason;
  std::uint64_t ts_submitted{0};
  std::uint64_t ts_accepted{0};
  std::uint64_t ts_last{0};
};

class Cache {
 public:
  explicit Cache(CacheConfig config = {});

  void addInstrument(Instrument instrument);
  void addAccount(Account account);
  void storeQuote(const QuoteTick& quote);
  void storeTrade(const TradeTick& trade);
  bool storeBar(const Bar& bar);
  void addOrder(const SubmitOrder& order, const AccountId& account_id,
                const ExecutionClientId& execution_client_id);
  void submitOrder(const ClientOrderId& client_order_id,
                   std::uint64_t ts_event);
  void acceptOrder(const OrderAccepted& event);
  void updateOrder(const OrderUpdated& event);
  void cancelOrder(const OrderCanceled& event);
  void denyOrder(const OrderDenied& event);
  void rejectOrder(const OrderRejected& event);
  void expireOrder(const OrderExpired& event);
  void addOrderBook(OrderBook book);

  const Instrument* instrument(const InstrumentId& instrument_id) const;
  const Account* account(const AccountId& account_id) const;
  const Position* position(const PositionId& position_id) const;
  const Position* positionForInstrument(
      const AccountId& account_id, const InstrumentId& instrument_id) const;
  std::vector<const Position*> positionsForAccount(
      const AccountId& account_id) const;
  const OrderFilled* fill(const TradeId& trade_id) const;

  const QuoteTick* quote(const InstrumentId& instrument_id,
                         std::size_t index = 0) const;
  std::vector<QuoteTick> quotes(const InstrumentId& instrument_id) const;
  std::size_t quoteCount(const InstrumentId& instrument_id) const;
  const TradeTick* trade(const InstrumentId& instrument_id,
                         std::size_t index = 0) const;
  std::vector<TradeTick> trades(const InstrumentId& instrument_id) const;
  std::size_t tradeCount(const InstrumentId& instrument_id) const;
  const Bar* bar(const BarType& bar_type, std::size_t index = 0) const;
  std::vector<Bar> bars(const BarType& bar_type) const;
  std::size_t barCount(const BarType& bar_type) const;

  const OrderState* order(const ClientOrderId& client_order_id) const;
  std::optional<ClientOrderId> clientOrderId(
      const VenueOrderId& venue_order_id) const;
  std::optional<VenueOrderId> venueOrderId(
      const ClientOrderId& client_order_id) const;
  std::vector<const OrderState*> ordersOpen() const;
  std::vector<const OrderState*> ordersClosed() const;
  std::vector<const OrderState*> ordersInflight() const;
  std::vector<const OrderState*> ordersForInstrument(
      const InstrumentId& instrument_id) const;
  std::vector<const OrderState*> ordersForStrategy(
      const StrategyId& strategy_id) const;
  std::vector<const OrderState*> ordersForAccount(
      const AccountId& account_id) const;

  const OrderBook* orderBook(const InstrumentId& instrument_id) const;
  OrderBook* orderBook(const InstrumentId& instrument_id);
  bool hasOrderBook(const InstrumentId& instrument_id) const;
  std::uint64_t bookUpdateCount(const InstrumentId& instrument_id) const;

 private:
  friend class Portfolio;

  using OrderIdSet = std::unordered_set<ClientOrderId>;

  static PositionId makePositionId(const AccountId& account_id,
                                   const InstrumentId& instrument_id);
  void commitFill(OrderState order, Account account, Position position,
                  OrderFilled fill);
  std::vector<const OrderState*> ordersForIds(const OrderIdSet& ids) const;
  void markOrderOpen(const ClientOrderId& client_order_id);
  void markOrderClosed(const ClientOrderId& client_order_id);

  CacheConfig config_;
  std::unordered_map<InstrumentId, Instrument> instruments_;
  std::unordered_map<AccountId, Account> accounts_;
  std::unordered_map<PositionId, Position> positions_;
  std::unordered_map<InstrumentId, std::deque<QuoteTick>> quotes_;
  std::unordered_map<InstrumentId, std::deque<TradeTick>> trades_;
  std::unordered_map<BarType, std::deque<Bar>, BarTypeHash> bars_;
  std::unordered_map<ClientOrderId, OrderState> orders_;
  std::unordered_map<TradeId, OrderFilled> fills_;
  std::unordered_map<InstrumentId, OrderBook> order_books_;
  std::unordered_map<VenueOrderId, ClientOrderId> venue_to_client_order_ids_;
  std::unordered_map<ClientOrderId, VenueOrderId> client_to_venue_order_ids_;

  // order index
  std::unordered_map<InstrumentId, OrderIdSet> instrument_orders_;
  std::unordered_map<StrategyId, OrderIdSet> strategy_orders_;
  std::unordered_map<AccountId, OrderIdSet> account_orders_;

  OrderIdSet open_orders_;
  OrderIdSet closed_orders_;
  OrderIdSet inflight_orders_;
};

}  // namespace trading
