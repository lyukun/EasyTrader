#pragma once

#include <cstdint>

#include "trading/model/identifiers.h"
#include "trading/model/types.h"

namespace trading {

struct Instrument {
  InstrumentId id;
  Currency base_currency;
  Currency quote_currency;
  std::uint8_t price_precision;
  std::uint8_t size_precision;
  Price tick_size;
  Quantity lot_size;
  Quantity min_quantity;
  Quantity max_quantity;
  Decimal contract_multiplier{Decimal::fromString("1")};
};

}  // namespace trading
