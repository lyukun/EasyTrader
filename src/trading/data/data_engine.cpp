#include "trading/data/data_engine.h"

#include <stdexcept>

namespace trading {
namespace {

std::optional<BookError> validateBatch(const OrderBookDeltas& deltas,
                                      std::size_t& group_count) {
  if (deltas.deltas.empty() ||
      (deltas.deltas.back().flags & kBookFlagLast) == 0) {
    return BookError::InvalidBatch;
  }
  bool first = true;
  bool snapshot = false;
  for (const auto& delta : deltas.deltas) {
    if (delta.instrument_id != deltas.instrument_id) {
      return BookError::InstrumentMismatch;
    }
    const bool snapshot_flag = (delta.flags & kBookFlagSnapshot) != 0;
    if (first) {
      snapshot = snapshot_flag;
      if (snapshot && delta.action != BookAction::Clear) {
        return BookError::InvalidBatch;
      }
    } else if (snapshot && delta.action != BookAction::Add) {
      return BookError::InvalidBatch;
    }
    if (snapshot_flag != snapshot) {
      return BookError::InvalidBatch;
    }
    first = (delta.flags & kBookFlagLast) != 0;
    if (first) {
      ++group_count;
    }
  }
  return std::nullopt;
}

}  // namespace

void DataEngine::addOrderBook(InstrumentId instrument_id, BookType book_type) {
  if (cache_.instrument(instrument_id) == nullptr) {
    throw std::invalid_argument("order book instrument is not cached");
  }
  cache_.addOrderBook(OrderBook(std::move(instrument_id), book_type));
}

void DataEngine::process(const QuoteTick& quote) {
  if (cache_.instrument(quote.instrument_id) == nullptr) {
    throw std::invalid_argument("quote instrument is not cached");
  }
  if (quote.bid.raw() <= 0 || quote.ask.raw() <= 0 || quote.bid > quote.ask ||
      quote.bid_size.isZero() || quote.ask_size.isZero()) {
    throw std::invalid_argument("quote is invalid");
  }
  if (auto* book = cache_.orderBook(quote.instrument_id);
      book != nullptr && book->bookType() == BookType::L1Mbp) {
    last_book_error_ = book->updateQuote(quote);
    if (last_book_error_) {
      return;
    }
  }
  cache_.storeQuote(quote);
  message_bus_.publish(quote);
}

void DataEngine::process(const TradeTick& trade) {
  if (cache_.instrument(trade.instrument_id) == nullptr) {
    throw std::invalid_argument("trade instrument is not cached");
  }
  if (trade.price.raw() <= 0 || trade.size.isZero() || trade.trade_id.empty()) {
    throw std::invalid_argument("trade is invalid");
  }
  cache_.storeTrade(trade);
  message_bus_.publish(trade);
}

bool DataEngine::process(const Bar& bar) {
  if (cache_.instrument(bar.bar_type.instrument_id) == nullptr) {
    throw std::invalid_argument("bar instrument is not cached");
  }
  if (bar.bar_type.interval_ns == 0 || bar.open.raw() <= 0 ||
      bar.high.raw() <= 0 || bar.low.raw() <= 0 || bar.close.raw() <= 0 ||
      bar.high < bar.low || bar.open > bar.high || bar.open < bar.low ||
      bar.close > bar.high || bar.close < bar.low) {
    throw std::invalid_argument("bar is invalid");
  }
  if (cache_.storeBar(bar)) {
    message_bus_.publish(bar);
    return true;
  }
  return false;
}

std::optional<BookError> DataEngine::process(const OrderBookDeltas& deltas) {
  auto* book = cache_.orderBook(deltas.instrument_id);
  if (book == nullptr) {
    last_book_error_ = BookError::BookNotFound;
    return last_book_error_;
  }
  // The adapter owns sequence linkage and recovery, DataEngine only accepts
  // complete event groups and applies each group before strategy callbacks.
  std::size_t group_count = 0;
  last_book_error_ = validateBatch(deltas, group_count);
  if (last_book_error_) {
    return last_book_error_;
  }
  if (group_count == 1) {
    last_book_error_ = book->applyDeltas(deltas);
    if (!last_book_error_) {
      message_bus_.publish(deltas);
    }
    return last_book_error_;
  }

  OrderBookDeltas batch{deltas.instrument_id, {}, 0, 0, 0};
  for (const auto& delta : deltas.deltas) {
    batch.deltas.push_back(delta);
    if ((delta.flags & kBookFlagLast) == 0) {
      continue;
    }
    batch.sequence = delta.sequence;
    batch.ts_event = delta.ts_event;
    batch.ts_init = delta.ts_init;
    last_book_error_ = book->applyDeltas(batch);
    if (last_book_error_) {
      return last_book_error_;
    }
    message_bus_.publish(batch);
    batch.deltas.clear();
  }
  return last_book_error_;
}

std::optional<BookError> DataEngine::process(const OrderBookDepth& depth) {
  auto* book = cache_.orderBook(depth.instrument_id);
  if (book == nullptr) {
    last_book_error_ = BookError::BookNotFound;
    return last_book_error_;
  }
  last_book_error_ = book->applyDepth(depth);
  if (!last_book_error_) {
    message_bus_.publish(depth);
  }
  return last_book_error_;
}

}  // namespace trading
