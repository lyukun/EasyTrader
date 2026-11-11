#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "trading/model/events.h"

namespace trading {

enum class BookError {
  BookNotFound,
  InstrumentMismatch,
  InvalidBatch,
  InvalidOrderId,
  InvalidQuantity,
  InvalidSide,
  CrossedBook,
  ArithmeticOverflow,
};

struct BookLevel {
  Price price;
  std::vector<BookOrder> orders;

  std::optional<Quantity> totalSize() const;
};

struct SimulatedFill {
  Price price;
  Quantity quantity;
};

class OrderBook {
 public:
  OrderBook(InstrumentId instrument_id, BookType book_type);

  const InstrumentId& instrumentId() const { return instrument_id_; }
  BookType bookType() const { return book_type_; }
  std::uint64_t sequence() const { return sequence_; }
  std::uint64_t tsLast() const { return ts_last_; }
  std::uint64_t updateCount() const { return update_count_; }

  std::optional<BookError> applyDelta(const OrderBookDelta& delta);
  std::optional<BookError> applyDeltas(const OrderBookDeltas& deltas);
  std::optional<BookError> applyDepth(const OrderBookDepth& depth);
  std::optional<BookError> updateQuote(const QuoteTick& quote);

  std::optional<Price> bestBidPrice() const;
  std::optional<Price> bestAskPrice() const;
  std::optional<Quantity> bestBidSize() const;
  std::optional<Quantity> bestAskSize() const;
  std::optional<Price> spread() const;
  std::optional<Price> midpoint() const;
  std::vector<BookLevel> bids(std::optional<std::size_t> depth = std::nullopt) const;
  std::vector<BookLevel> asks(std::optional<std::size_t> depth = std::nullopt) const;
  std::optional<Quantity> quantityAtPrice(OrderSide side, Price price) const;
  std::vector<SimulatedFill> simulateFills(OrderSide side,
                                           Quantity quantity) const;
  std::optional<Price> averagePriceForQuantity(OrderSide side,
                                                Quantity quantity) const;
  std::optional<BookError> checkIntegrity() const;

 private:
  using BidLevels = std::map<Price, BookLevel, std::greater<Price>>;
  using AskLevels = std::map<Price, BookLevel>;

  struct OrderLocation {
    OrderSide side;
    Price price;
  };

  std::optional<BookError> applyDeltaUnchecked(const OrderBookDelta& delta);
  std::optional<BookError> applyOrder(const BookOrder& order, BookAction action);
  void removeL3(BookOrderId order_id);
  void updateMetadata(std::uint64_t sequence, std::uint64_t ts_event,
                      bool reset_sequence);
  void clearLevels();

  InstrumentId instrument_id_;
  BookType book_type_;
  std::uint64_t sequence_{0};
  std::uint64_t ts_last_{0};
  std::uint64_t update_count_{0};
  BidLevels bids_;
  AskLevels asks_;
  std::unordered_map<BookOrderId, OrderLocation> order_locations_;
};

}  // namespace trading
