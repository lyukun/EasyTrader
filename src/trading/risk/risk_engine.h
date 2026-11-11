#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "trading/cache/cache.h"
#include "trading/model/events.h"

namespace trading {

struct RiskEngineConfig {
  std::unordered_map<InstrumentId, Decimal> max_notional_per_order;
};

class RiskEngine {
 public:
  explicit RiskEngine(const Cache& cache, RiskEngineConfig config = {});

  std::optional<OrderDenied> execute(const SubmitOrder& order,
                                     const AccountId& account_id);
  void refreshReservation(const ClientOrderId& client_order_id);
  void setTradingState(TradingState state) { trading_state_ = state; }
  TradingState tradingState() const { return trading_state_; }
  void setMaxNotionalPerOrder(const InstrumentId& instrument_id,
                              Decimal max_notional);

 private:
  // reservation 是用来存冻结金额的
  struct Reservation {
    AccountId account_id;
    InstrumentId instrument_id;
    OrderSide side;
    std::uint64_t quantity_raw;
    std::int64_t notional_raw;
  };

  std::optional<std::string> validate(const SubmitOrder& order,
                                      const Account& account,
                                      const Instrument& instrument,
                                      std::int64_t notional_raw) const;
  std::optional<std::int64_t> calculateNotionalRaw(
      const SubmitOrder& order, const Instrument& instrument) const;
  std::int64_t reservedBuyNotional(const AccountId& account_id) const;
  std::uint64_t reservedQuantity(const AccountId& account_id,
                                 const InstrumentId& instrument_id,
                                 OrderSide side) const;

  const Cache& cache_;
  RiskEngineConfig config_;
  TradingState trading_state_{TradingState::Active};
  std::unordered_map<ClientOrderId, Reservation> reservations_;
};

}  // namespace trading
