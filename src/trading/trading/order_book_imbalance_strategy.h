#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "trading/cache/cache.h"
#include "trading/model/events.h"
#include "trading/trading/strategy.h"

namespace trading {

struct OrderBookImbalanceConfig {
  InstrumentId instrument_id;
  BookType book_type{BookType::L2Mbp};
  Quantity max_signal_size;
  Quantity trigger_min_size;
  std::uint32_t trigger_ratio_ppm{200'000};
  std::uint64_t cooldown_ns{1'000'000'000};
};

class OrderBookImbalanceStrategy : public Strategy {
 public:
  explicit OrderBookImbalanceStrategy(OrderBookImbalanceConfig config);

  std::string_view id() const override { return "order-book-imbalance"; }
  void start() override;
  void stop() override { running_ = false; }

  std::uint64_t updateCount() const { return update_count_; }
  std::uint64_t signalCount() const { return signal_count_; }
  std::uint64_t submitCount() const { return submit_count_; }

 private:
  void onDeltas(const OrderBookDeltas& deltas);

  OrderBookImbalanceConfig config_;
  bool running_{false};
  std::optional<std::uint64_t> last_signal_ns_;
  std::uint64_t update_count_{0};
  std::uint64_t signal_count_{0};
  std::uint64_t submit_count_{0};
};

}  // namespace trading
