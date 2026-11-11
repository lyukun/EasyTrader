#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "trading/model/identifiers.h"
#include "trading/model/market_data.h"
#include "trading/model/types.h"

namespace trading {

enum class OrderSide { Buy, Sell };
enum class OrderType { Market, Limit };
enum class TimeInForce { Gtc, Ioc, Fok };
enum class TradingState { Active, Reducing, Halted };
enum class OrderStatus {
  Initialized,
  Submitted,
  Accepted,
  PartiallyFilled,
  Filled,
  PendingUpdate,
  PendingCancel,
  Canceled,
  Denied,
  Rejected,
  Expired,
};
enum class BookType { L1Mbp, L2Mbp, L3Mbo };
enum class BookAction { Add, Update, Delete, Clear };

inline constexpr std::uint8_t kBookFlagLast = 1U << 0U;
inline constexpr std::uint8_t kBookFlagSnapshot = 1U << 1U;

struct QuoteTick {
  InstrumentId instrument_id;
  Price bid;
  Price ask;
  Quantity bid_size;
  Quantity ask_size;
  std::uint64_t sequence;
  std::uint64_t ts_event;
};

struct BookOrder {
  OrderSide side;
  Price price;
  Quantity size;
  BookOrderId order_id;
};

struct OrderBookDelta {
  InstrumentId instrument_id;
  BookAction action;
  BookOrder order;
  std::uint8_t flags;
  std::uint64_t sequence;
  std::uint64_t ts_event;
  std::uint64_t ts_init;
};

struct OrderBookDeltas {
  InstrumentId instrument_id;
  std::vector<OrderBookDelta> deltas;
  std::uint64_t sequence;
  std::uint64_t ts_event;
  std::uint64_t ts_init;
};

struct OrderBookDepth {
  InstrumentId instrument_id;
  std::vector<BookOrder> bids;
  std::vector<BookOrder> asks;
  std::uint64_t sequence;
  std::uint64_t ts_event;
  std::uint64_t ts_init;
};

struct TwapConfig {
  std::chrono::milliseconds horizon{0};
  std::chrono::milliseconds interval{0};
};

struct SubmitOrder {
  ClientOrderId client_order_id;
  InstrumentId instrument_id;
  OrderSide side;
  Quantity quantity;
  Price limit_price;
  StrategyId strategy_id{"default-strategy"};
  std::optional<ExecutionClientId> execution_client_id{std::nullopt};
  OrderType order_type{OrderType::Limit};
  TimeInForce time_in_force{TimeInForce::Gtc};
  std::uint64_t ts_init{0};
  bool reduce_only{false};
  std::optional<ExecAlgorithmId> exec_algorithm_id{std::nullopt};
  std::optional<TwapConfig> twap{std::nullopt};
  std::optional<ClientOrderId> exec_spawn_id{std::nullopt};
};

struct OrderAccepted {
  ClientOrderId client_order_id;
  VenueOrderId venue_order_id;
  std::uint64_t ts_event{0};
};

struct OrderFilled {
  ClientOrderId client_order_id;
  VenueOrderId venue_order_id;
  InstrumentId instrument_id;
  OrderSide side;
  Quantity quantity;
  Price price;
  TradeId trade_id;
  std::optional<Money> fee;
  std::uint64_t ts_event{0};

  friend bool operator==(const OrderFilled&, const OrderFilled&) = default;
};

struct OrderDenied {
  ClientOrderId client_order_id;
  std::string reason;
  std::uint64_t ts_event{0};
};

struct OrderRejected {
  ClientOrderId client_order_id;
  std::string reason;
  std::uint64_t ts_event{0};
};

struct OrderUpdated {
  ClientOrderId client_order_id;
  Quantity quantity;
  Price price;
  std::uint64_t ts_event;
};

struct OrderCanceled {
  ClientOrderId client_order_id;
  std::string reason;
  std::uint64_t ts_event;
};

struct OrderExpired {
  ClientOrderId client_order_id;
  std::uint64_t ts_event;
};

struct TradingStateChanged {
  TradingState state;
  std::uint64_t ts_event;
};

struct OrderBookImbalanceSignal {
  InstrumentId instrument_id;
  OrderSide side;
  Price price;
  Quantity quantity;
  std::uint64_t sequence;
  std::uint64_t ts_event;
};

using RuntimeMessage =
    std::variant<QuoteTick, TradeTick, Bar, OrderBookDeltas, OrderBookDepth,
                 OrderAccepted, OrderUpdated, OrderFilled, OrderCanceled,
                 OrderDenied, OrderRejected, OrderExpired,
                 TradingStateChanged>;

}  // namespace trading
