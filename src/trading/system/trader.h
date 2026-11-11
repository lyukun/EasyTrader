#pragma once

#include <memory>
#include <vector>

#include "trading/trading/strategy.h"

namespace trading {

class Trader {
 public:
  explicit Trader(StrategyContext& context) : context_(context) {}

  Strategy& addStrategy(std::unique_ptr<Strategy> strategy);
  void start();
  void stop();

  bool isRunning() const { return state_ == State::Running; }

 private:
  enum class State { Ready, Running, Stopped };

  StrategyContext& context_;
  std::vector<std::unique_ptr<Strategy>> strategies_;
  State state_{State::Ready};
};

}  // namespace trading
