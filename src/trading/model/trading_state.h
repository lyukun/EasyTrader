#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "trading/model/identifiers.h"
#include "trading/model/types.h"

namespace trading {

struct Instrument;
struct OrderFilled;

enum class AccountType { Cash, Margin };

struct Position {
  PositionId id;
  AccountId account_id;
  InstrumentId instrument_id;
  std::int64_t quantity_raw{0};
  std::optional<Price> average_open_price;
  Money realized_pnl;
  Money fees;
  std::uint64_t ts_opened{0};
  std::uint64_t ts_closed{0};
  std::uint64_t ts_last{0};

  Decimal signedQuantity() const { return Decimal::fromRaw(quantity_raw); }
  bool isOpen() const { return quantity_raw != 0 && ts_closed == 0; }
  bool isClosed() const { return quantity_raw == 0 && ts_closed != 0; }
  Money apply(const OrderFilled& fill, const Instrument& instrument);
};

struct AccountBalance {
  Money total;
  Money free;
  Money locked;

  static AccountBalance cash(Money total);
  bool isValid() const;
};

struct Account {
  AccountId id;
  Venue venue;
  Currency base_currency;
  Money starting_balance;
  AccountBalance balance;
  Money realized_pnl;
  Money fees;
  AccountType type{AccountType::Cash};
};

}  // namespace trading
