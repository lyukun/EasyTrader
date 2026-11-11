#pragma once

#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "trading/python/python_runtime.h"
#include "trading/trading/strategy.h"

namespace trading {

// Python receives copied snapshots rather than references into the cache
class PythonStrategyContext final {
 public:
  explicit PythonStrategyContext(StrategyId strategy_id);

  const StrategyId& strategyId() const { return strategy_id_; }
  void submitOrder(const SubmitOrder& order) const;
  std::optional<QuoteTick> quote(const InstrumentId& instrument_id) const;
  std::optional<OrderState> order(const ClientOrderId& client_order_id) const;
  std::optional<Position> position(const AccountId& account_id,
                                   const InstrumentId& instrument_id) const;

 private:
  friend class PythonStrategy;

  StrategyContext& checkedContext() const;

  const StrategyId strategy_id_;
  const std::thread::id owner_{std::this_thread::get_id()};
  StrategyContext* context_{nullptr};
  bool submit_enabled_{false};
};

class PythonStrategy final : public Strategy {
 public:
  PythonStrategy(std::shared_ptr<PythonRuntime> runtime, StrategyId strategy_id,
                 const std::string& module_name, const std::string& class_name);
  ~PythonStrategy() override;

  std::string_view id() const override { return strategy_id_; }
  void start() override;
  void stop() override;

 private:
  struct Impl;
  enum class State { Ready, Running, Stopped };

  std::shared_ptr<PythonRuntime> runtime_;
  StrategyId strategy_id_;
  std::unique_ptr<Impl> impl_;
  State state_{State::Ready};
};

}  // namespace trading
