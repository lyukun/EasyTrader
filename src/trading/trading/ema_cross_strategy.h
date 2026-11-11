#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "trading/model/events.h"
#include "trading/trading/strategy.h"

namespace trading {

struct EmaCrossConfig {
  InstrumentId instrument_id;
  Quantity trade_size;
  std::size_t fast_period{10};
  std::size_t slow_period{50};
};

class EmaCrossStrategy : public Strategy {
 public:
  explicit EmaCrossStrategy(EmaCrossConfig config);

  std::string_view id() const override { return "ema-cross"; }
  void start() override;
  void stop() override { running_ = false; }

  bool isInitialized() const { return fast_initialized_ && slow_initialized_; }
  std::uint64_t quoteCount() const { return quote_count_; }
  std::uint64_t signalCount() const { return signal_count_; }

 private:
  void onQuote(const QuoteTick& quote);

  EmaCrossConfig config_;
  bool running_{false};
  bool fast_initialized_{false};
  bool slow_initialized_{false};
  std::optional<bool> prev_fast_above_;
  std::optional<std::int64_t> fast_ema_raw_;
  std::optional<std::int64_t> slow_ema_raw_;
  std::uint64_t quote_count_{0};
  std::uint64_t signal_count_{0};
};

}  // namespace trading