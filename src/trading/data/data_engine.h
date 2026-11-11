#pragma once

#include "trading/cache/cache.h"
#include "trading/common/message_bus.h"

#include <optional>

namespace trading {

class DataEngine {
 public:
  DataEngine(Cache& cache, MessageBus& message_bus)
      : cache_(cache), message_bus_(message_bus) {}
  void addOrderBook(InstrumentId instrument_id, BookType book_type);
  void process(const QuoteTick& quote);
  void process(const TradeTick& trade);
  bool process(const Bar& bar);

  // Sequence rules stay in the adapter, F_LAST closes a publishable event group
  std::optional<BookError> process(const OrderBookDeltas& deltas);
  std::optional<BookError> process(const OrderBookDepth& depth);
  std::optional<BookError> lastBookError() const { return last_book_error_; }

 private:
  Cache& cache_;
  MessageBus& message_bus_;
  std::optional<BookError> last_book_error_;
};

}  // namespace trading
