#include "trading/system/trader.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace trading {

Strategy& Trader::addStrategy(std::unique_ptr<Strategy> strategy) {
  if (state_ != State::Ready) {
    throw std::logic_error("strategies can only be added before trader start");
  }
  if (strategy == nullptr) {
    throw std::invalid_argument("strategy must not be null");
  }

  const StrategyId strategy_id(strategy->id());
  if (strategy_id.empty()) {
    throw std::invalid_argument("strategy ID must not be empty");
  }
  const auto duplicate = std::find_if(
      strategies_.begin(), strategies_.end(),
      [&strategy_id](const auto& registered) {
        return registered->id() == strategy_id;
      });
  if (duplicate != strategies_.end()) {
    throw std::invalid_argument("duplicate strategy ID: " + strategy_id);
  }

  strategy->registerContext(context_);
  Strategy& result = *strategy;
  strategies_.push_back(std::move(strategy));
  return result;
}

void Trader::start() {
  if (state_ != State::Ready) {
    throw std::logic_error("trader can only be started once");
  }

  std::size_t started = 0;
  try {
    for (; started < strategies_.size(); ++started) {
      strategies_[started]->start();
    }
    state_ = State::Running;
  } catch (...) {
    state_ = State::Stopped;
    try {
      strategies_[started]->stop();
    } catch (...) {
    }
    while (started > 0) {
      --started;
      try {
        strategies_[started]->stop();
      } catch (...) {
      }
    }
    throw;
  }
}

void Trader::stop() {
  if (state_ != State::Running) {
    return;
  }
  state_ = State::Stopped;

  std::exception_ptr first_error;
  for (auto it = strategies_.rbegin(); it != strategies_.rend(); ++it) {
    try {
      (*it)->stop();
    } catch (...) {
      if (first_error == nullptr) {
        first_error = std::current_exception();
      }
    }
  }
  if (first_error != nullptr) {
    std::rethrow_exception(first_error);
  }
}

}  // namespace trading
