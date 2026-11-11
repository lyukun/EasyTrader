#include "trading/model/order_book.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include <boost/multiprecision/cpp_int.hpp>

namespace trading {
namespace {

template <typename Levels>
std::vector<BookLevel> copyLevels(const Levels& levels,
                                  std::optional<std::size_t> depth) {
  std::vector<BookLevel> result;
  const auto limit = depth.value_or(levels.size());
  result.reserve(std::min(limit, levels.size()));
  for (const auto& [price, level] : levels) {
    static_cast<void>(price);
    if (result.size() == limit) {
      break;
    }
    result.push_back(level);
  }
  return result;
}

std::optional<Quantity> addQuantities(Quantity lhs, Quantity rhs) {
  if (rhs.raw() > std::numeric_limits<std::uint64_t>::max() - lhs.raw()) {
    return std::nullopt;
  }
  return Quantity::fromRaw(lhs.raw() + rhs.raw(),
                           std::max(lhs.precision(), rhs.precision()));
}

}  // namespace

std::optional<Quantity> BookLevel::totalSize() const {
  auto total = Quantity::fromRaw(0);
  for (const auto& order : orders) {
    const auto next = addQuantities(total, order.size);
    if (!next) {
      return std::nullopt;
    }
    total = *next;
  }
  return total;
}

OrderBook::OrderBook(InstrumentId instrument_id, BookType book_type)
    : instrument_id_(std::move(instrument_id)), book_type_(book_type) {
  if (instrument_id_.empty()) {
    throw std::invalid_argument("order book instrument ID must not be empty");
  }
}

std::optional<BookError> OrderBook::applyDelta(const OrderBookDelta& delta) {
  if (delta.instrument_id != instrument_id_) {
    return BookError::InstrumentMismatch;
  }
  return applyDeltaUnchecked(delta);
}

std::optional<BookError> OrderBook::applyDeltas(const OrderBookDeltas& deltas) {
  if (deltas.instrument_id != instrument_id_) {
    return BookError::InstrumentMismatch;
  }
  for (const auto& delta : deltas.deltas) {
    if (delta.instrument_id != instrument_id_) {
      return BookError::InstrumentMismatch;
    }
  }

  const bool snapshot = !deltas.deltas.empty() &&
                        (deltas.deltas.front().flags & kBookFlagSnapshot) != 0;
  if (snapshot) {
    OrderBook replacement(instrument_id_, book_type_);
    replacement.sequence_ = sequence_;
    replacement.ts_last_ = ts_last_;
    replacement.update_count_ = update_count_;
    for (const auto& delta : deltas.deltas) {
      if (const auto error = replacement.applyDeltaUnchecked(delta)) {
        return error;
      }
    }
    *this = std::move(replacement);
    return std::nullopt;
  }

  for (const auto& delta : deltas.deltas) {
    if (const auto error = applyDeltaUnchecked(delta)) {
      return error;
    }
  }
  return std::nullopt;
}

std::optional<BookError> OrderBook::applyDepth(const OrderBookDepth& depth) {
  if (depth.instrument_id != instrument_id_) {
    return BookError::InstrumentMismatch;
  }

  OrderBook replacement(instrument_id_, book_type_);
  auto add_side = [&replacement](const std::vector<BookOrder>& orders,
                                 OrderSide expected) -> std::optional<BookError> {
    if (replacement.book_type_ == BookType::L1Mbp && !orders.empty()) {
      const auto compare = [expected](const BookOrder& lhs, const BookOrder& rhs) {
        return expected == OrderSide::Buy ? lhs.price < rhs.price
                                          : lhs.price > rhs.price;
      };
      const auto best = std::max_element(orders.begin(), orders.end(), compare);
      if (best->side != expected) {
        return BookError::InvalidSide;
      }
      return replacement.applyOrder(*best, BookAction::Add);
    }
    for (const auto& order : orders) {
      if (order.side != expected) {
        return BookError::InvalidSide;
      }
      if (const auto error = replacement.applyOrder(order, BookAction::Add)) {
        return error;
      }
    }
    return std::nullopt;
  };
  if (const auto error = add_side(depth.bids, OrderSide::Buy)) {
    return error;
  }
  if (const auto error = add_side(depth.asks, OrderSide::Sell)) {
    return error;
  }

  replacement.sequence_ = std::max(sequence_, depth.sequence);
  replacement.ts_last_ = std::max(ts_last_, depth.ts_event);
  replacement.update_count_ = update_count_ + 1;
  *this = std::move(replacement);
  return std::nullopt;
}

std::optional<BookError> OrderBook::updateQuote(const QuoteTick& quote) {
  if (quote.instrument_id != instrument_id_) {
    return BookError::InstrumentMismatch;
  }
  if (book_type_ != BookType::L1Mbp) {
    return BookError::InvalidSide;
  }
  if (quote.bid_size.isZero()) {
    bids_.clear();
  } else {
    bids_.clear();
    bids_.emplace(quote.bid,
                  BookLevel{quote.bid,
                            {BookOrder{OrderSide::Buy, quote.bid, quote.bid_size, 1}}});
  }
  if (quote.ask_size.isZero()) {
    asks_.clear();
  } else {
    asks_.clear();
    asks_.emplace(quote.ask,
                  BookLevel{quote.ask,
                            {BookOrder{OrderSide::Sell, quote.ask, quote.ask_size, 2}}});
  }
  order_locations_.clear();
  updateMetadata(quote.sequence, quote.ts_event, false);
  return std::nullopt;
}

std::optional<BookError> OrderBook::applyDeltaUnchecked(
    const OrderBookDelta& delta) {
  if (delta.action == BookAction::Clear) {
    clearLevels();
    const bool snapshot = (delta.flags & kBookFlagSnapshot) != 0;
    updateMetadata(delta.sequence, delta.ts_event, !snapshot);
    return std::nullopt;
  }
  if (delta.order.size.isZero() &&
      (delta.action == BookAction::Add || delta.action == BookAction::Update)) {
    return BookError::InvalidQuantity;
  }
  if (book_type_ == BookType::L3Mbo && delta.order.order_id == 0) {
    return BookError::InvalidOrderId;
  }
  if (const auto error = applyOrder(delta.order, delta.action)) {
    return error;
  }
  updateMetadata(delta.sequence, delta.ts_event, false);
  return std::nullopt;
}

std::optional<BookError> OrderBook::applyOrder(const BookOrder& order,
                                                BookAction action) {
  if (book_type_ == BookType::L1Mbp) {
    if (order.side == OrderSide::Buy) {
      if (action == BookAction::Delete) {
        bids_.clear();
      } else {
        bids_.clear();
        bids_.emplace(order.price, BookLevel{order.price, {order}});
      }
    } else {
      if (action == BookAction::Delete) {
        asks_.clear();
      } else {
        asks_.clear();
        asks_.emplace(order.price, BookLevel{order.price, {order}});
      }
    }
    return std::nullopt;
  }

  if (book_type_ == BookType::L2Mbp) {
    if (order.side == OrderSide::Buy) {
      if (action == BookAction::Delete) {
        bids_.erase(order.price);
      } else {
        bids_.insert_or_assign(order.price, BookLevel{order.price, {order}});
      }
    } else {
      if (action == BookAction::Delete) {
        asks_.erase(order.price);
      } else {
        asks_.insert_or_assign(order.price, BookLevel{order.price, {order}});
      }
    }
    return std::nullopt;
  }

  if (action == BookAction::Delete) {
    removeL3(order.order_id);
    return std::nullopt;
  }

  const auto location = order_locations_.find(order.order_id);
  if (location == order_locations_.end() && action == BookAction::Update) {
    return std::nullopt;
  }
  if (location != order_locations_.end() && action == BookAction::Update &&
      location->second.side == order.side && location->second.price == order.price) {
    if (order.side == OrderSide::Buy) {
      auto& orders = bids_.at(order.price).orders;
      const auto it = std::find_if(orders.begin(), orders.end(), [&order](const auto& existing) {
        return existing.order_id == order.order_id;
      });
      if (it != orders.end()) {
        *it = order;
      }
    } else {
      auto& orders = asks_.at(order.price).orders;
      const auto it = std::find_if(orders.begin(), orders.end(), [&order](const auto& existing) {
        return existing.order_id == order.order_id;
      });
      if (it != orders.end()) {
        *it = order;
      }
    }
    return std::nullopt;
  }

  removeL3(order.order_id);
  if (order.side == OrderSide::Buy) {
    auto [it, inserted] = bids_.try_emplace(order.price, BookLevel{order.price, {}});
    static_cast<void>(inserted);
    it->second.orders.push_back(order);
  } else {
    auto [it, inserted] = asks_.try_emplace(order.price, BookLevel{order.price, {}});
    static_cast<void>(inserted);
    it->second.orders.push_back(order);
  }
  order_locations_.insert_or_assign(order.order_id,
                                    OrderLocation{order.side, order.price});
  return std::nullopt;
}

void OrderBook::removeL3(BookOrderId order_id) {
  const auto location = order_locations_.find(order_id);
  if (location == order_locations_.end()) {
    return;
  }
  if (location->second.side == OrderSide::Buy) {
    auto level = bids_.find(location->second.price);
    if (level != bids_.end()) {
      std::erase_if(level->second.orders,
                    [order_id](const auto& order) { return order.order_id == order_id; });
      if (level->second.orders.empty()) {
        bids_.erase(level);
      }
    }
  } else {
    auto level = asks_.find(location->second.price);
    if (level != asks_.end()) {
      std::erase_if(level->second.orders,
                    [order_id](const auto& order) { return order.order_id == order_id; });
      if (level->second.orders.empty()) {
        asks_.erase(level);
      }
    }
  }
  order_locations_.erase(location);
}

void OrderBook::updateMetadata(std::uint64_t sequence, std::uint64_t ts_event,
                               bool reset_sequence) {
  sequence_ = reset_sequence ? sequence : std::max(sequence_, sequence);
  ts_last_ = std::max(ts_last_, ts_event);
  ++update_count_;
}

void OrderBook::clearLevels() {
  bids_.clear();
  asks_.clear();
  order_locations_.clear();
}

std::optional<Price> OrderBook::bestBidPrice() const {
  return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
}

std::optional<Price> OrderBook::bestAskPrice() const {
  return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
}

std::optional<Quantity> OrderBook::bestBidSize() const {
  return bids_.empty() || bids_.begin()->second.orders.empty()
             ? std::nullopt
             : std::optional<Quantity>(bids_.begin()->second.orders.front().size);
}

std::optional<Quantity> OrderBook::bestAskSize() const {
  return asks_.empty() || asks_.begin()->second.orders.empty()
             ? std::nullopt
             : std::optional<Quantity>(asks_.begin()->second.orders.front().size);
}

std::optional<Price> OrderBook::spread() const {
  const auto bid = bestBidPrice();
  const auto ask = bestAskPrice();
  if (!bid || !ask) {
    return std::nullopt;
  }
  if (ask->raw() < bid->raw()) {
    return std::nullopt;
  }
  return Price::fromRaw(ask->raw() - bid->raw(),
                        std::max(ask->precision(), bid->precision()));
}

std::optional<Price> OrderBook::midpoint() const {
  const auto bid = bestBidPrice();
  const auto ask = bestAskPrice();
  if (!bid || !ask) {
    return std::nullopt;
  }
  const auto half = bid->raw() / 2 + ask->raw() / 2;
  const auto remainder = bid->raw() % 2 + ask->raw() % 2;
  return Price::fromRaw(half + remainder / 2, Price::kMaxPrecision);
}

std::vector<BookLevel> OrderBook::bids(std::optional<std::size_t> depth) const {
  return copyLevels(bids_, depth);
}

std::vector<BookLevel> OrderBook::asks(std::optional<std::size_t> depth) const {
  return copyLevels(asks_, depth);
}

std::optional<Quantity> OrderBook::quantityAtPrice(OrderSide side,
                                                   Price price) const {
  if (side == OrderSide::Buy) {
    const auto level = bids_.find(price);
    return level == bids_.end() ? std::nullopt : level->second.totalSize();
  }
  const auto level = asks_.find(price);
  return level == asks_.end() ? std::nullopt : level->second.totalSize();
}

std::vector<SimulatedFill> OrderBook::simulateFills(OrderSide side,
                                                    Quantity quantity) const {
  std::vector<SimulatedFill> fills;
  auto remaining = quantity.raw();
  const auto consume = [&fills, &remaining](const auto& levels) {
    for (const auto& [price, level] : levels) {
      const auto available = level.totalSize();
      if (!available || remaining == 0) {
        break;
      }
      const auto amount = std::min(remaining, available->raw());
      fills.push_back(
          SimulatedFill{price, Quantity::fromRaw(amount, Quantity::kMaxPrecision)});
      remaining -= amount;
    }
  };
  if (side == OrderSide::Buy) {
    consume(asks_);
  } else {
    consume(bids_);
  }
  return fills;
}

std::optional<Price> OrderBook::averagePriceForQuantity(
    OrderSide side, Quantity quantity) const {
  if (quantity.isZero()) {
    return std::nullopt;
  }
  const auto fills = simulateFills(side, quantity);
  boost::multiprecision::cpp_int cost = 0;
  std::uint64_t filled = 0;
  for (const auto& fill : fills) {
    cost += boost::multiprecision::cpp_int(fill.price.raw()) * fill.quantity.raw();
    filled += fill.quantity.raw();
  }
  if (filled != quantity.raw()) {
    return std::nullopt;
  }
  const boost::multiprecision::cpp_int average = cost / filled;
  if (average > std::numeric_limits<std::int64_t>::max() ||
      average < std::numeric_limits<std::int64_t>::min()) {
    return std::nullopt;
  }
  return Price::fromRaw(average.convert_to<std::int64_t>(), Price::kMaxPrecision);
}

std::optional<BookError> OrderBook::checkIntegrity() const {
  if (book_type_ == BookType::L1Mbp && (bids_.size() > 1 || asks_.size() > 1)) {
    return BookError::InvalidOrderId;
  }
  if (book_type_ == BookType::L2Mbp) {
    const auto multiple_orders = [](const auto& levels) {
      return std::any_of(levels.begin(), levels.end(), [](const auto& entry) {
        return entry.second.orders.size() != 1;
      });
    };
    if (multiple_orders(bids_) || multiple_orders(asks_)) {
      return BookError::InvalidOrderId;
    }
  }
  const auto bid = bestBidPrice();
  const auto ask = bestAskPrice();
  if (bid && ask && *bid > *ask) {
    return BookError::CrossedBook;
  }
  return std::nullopt;
}

}  // namespace trading
