#pragma once

#include <optional>
#include <unordered_map>

#include "trading/cache/cache.h"

namespace trading {

class Portfolio {
 public:
  explicit Portfolio(Cache& cache) : cache_(cache) {}

  bool apply(const OrderFilled& fill);
  void update(const QuoteTick& quote);
  void update(const TradeTick& trade);
  void update(const Bar& bar);

  Decimal position(const AccountId& account_id,
                   const InstrumentId& instrument_id) const;
  std::optional<Money> unrealizedPnl(
      const AccountId& account_id,
      const InstrumentId& instrument_id) const;
  std::optional<Money> realizedPnl(
      const AccountId& account_id,
      const InstrumentId& instrument_id) const;
  std::optional<Money> totalPnl(
      const AccountId& account_id,
      const InstrumentId& instrument_id) const;
  std::optional<Money> netExposure(
      const AccountId& account_id,
      const InstrumentId& instrument_id) const;
  std::optional<Money> markValue(
      const AccountId& account_id,
      const InstrumentId& instrument_id) const;
  std::optional<Money> equity(const AccountId& account_id) const;

 private:
  struct ValuationPrices {
    std::optional<Price> bid;
    std::optional<Price> ask;
    std::optional<Price> last;
    std::optional<Price> bar_close;
  };

  std::optional<Price> valuationPrice(const Position& position) const;
  std::optional<Money> signedMarkValue(const Position& position) const;

  Cache& cache_;
  std::unordered_map<InstrumentId, ValuationPrices> valuation_prices_;
};

}  // namespace trading
