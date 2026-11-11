#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

#include "trading/cache/cache.h"
#include "trading/common/actor.h"
#include "trading/common/message_bus.h"

namespace trading {

class StrategyContext {
 public:
  using EnsureOrderBook = std::function<void(InstrumentId, BookType)>;
  using SubmitOrderHandler = std::function<void(const SubmitOrder&)>;

  StrategyContext(const Cache& cache, MessageBus& message_bus,
                  SubmitOrderHandler submit_order,
                  EnsureOrderBook ensure_order_book)
      : cache_(cache),
        message_bus_(message_bus),
        submit_order_(std::move(submit_order)),
        ensure_order_book_(std::move(ensure_order_book)) {}

  const Cache& cache() const { return cache_; }
  void submitOrder(const SubmitOrder& order) const {
    submit_order_(order);
  }
  void ensureOrderBook(InstrumentId instrument_id, BookType book_type) const {
    ensure_order_book_(std::move(instrument_id), book_type);
  }

 private:
  friend class Strategy;

  template <typename T>
  void subscribe(std::function<void(const T&)> handler) {
    message_bus_.subscribe<T>(std::move(handler));
  }

  template <typename T>
  void publish(const T& event) const {
    message_bus_.publish(event);
  }

  const Cache& cache_;
  MessageBus& message_bus_;
  SubmitOrderHandler submit_order_;
  EnsureOrderBook ensure_order_book_;
};

class Strategy : public Actor {
 public:
  void registerContext(StrategyContext& context) {
    if (context_ != nullptr) {
      throw std::logic_error("strategy context is already registered");
    }
    context_ = &context;
  }

 protected:
  template <typename T>
  void subscribe(std::function<void(const T&)> handler) {
    context().subscribe<T>(std::move(handler));
  }

  template <typename T>
  void publish(const T& event) const {
    context().publish(event);
  }

  StrategyContext& context() const {
    if (context_ == nullptr) {
      throw std::logic_error("strategy context is not registered");
    }
    return *context_;
  }

 private:
  StrategyContext* context_{nullptr};
};

}  // namespace trading
