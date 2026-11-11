#pragma once

#include <string_view>

#include "trading/trading/strategy.h"

namespace trading {

struct TwapExampleConfig {
  InstrumentId instrument_id;
  Quantity trade_size;
  TwapConfig twap;
};

// Submits one TWAP buy target on the first quote for its instrument
class TwapExampleStrategy : public Strategy {
 public:
  explicit TwapExampleStrategy(TwapExampleConfig config);
  std::string_view id() const override { return "twap-example"; }
  void start() override;
  void stop() override { running_ = false; }

 private:
  TwapExampleConfig config_;
  bool running_{false};
  bool submitted_{false};
};

}  // namespace trading
