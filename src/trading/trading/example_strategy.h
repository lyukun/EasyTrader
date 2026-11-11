#pragma once

#include <string_view>

#include "trading/model/events.h"
#include "trading/trading/strategy.h"

namespace trading {

class ExampleStrategy : public Strategy {
 public:
  std::string_view id() const override { return "example-strategy"; }
  void start() override;
  void stop() override { running_ = false; }

  bool filled() const { return filled_; }

 private:
  void onQuote(const QuoteTick& quote);
  void onAccepted(const OrderAccepted& event);
  void onFilled(const OrderFilled& event);
  void onDenied(const OrderDenied& event);
  void onRejected(const OrderRejected& event);

  bool running_{false};
  bool submitted_{false};
  bool filled_{false};
};

}  // namespace trading
