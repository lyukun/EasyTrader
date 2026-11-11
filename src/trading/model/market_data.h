#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "trading/model/identifiers.h"
#include "trading/model/types.h"

namespace trading {

enum class AggressorSide { None, Buyer, Seller };

struct TradeTick {
  InstrumentId instrument_id;
  Price price;
  Quantity size;
  AggressorSide aggressor_side;
  TradeId trade_id;
  std::uint64_t sequence;
  std::uint64_t ts_event;
  std::uint64_t ts_init;
};

struct BarType {
  InstrumentId instrument_id;
  std::uint64_t interval_ns;

  friend bool operator==(const BarType&, const BarType&) = default;
};

struct BarTypeHash {
  std::size_t operator()(const BarType& value) const {
    const auto instrument_hash = std::hash<InstrumentId>{}(value.instrument_id);
    const auto interval_hash = std::hash<std::uint64_t>{}(value.interval_ns);
    return instrument_hash ^ (interval_hash + 0x9e3779b9U +
                              (instrument_hash << 6U) +
                              (instrument_hash >> 2U));
  }
};

struct Bar {
  BarType bar_type;
  Price open;
  Price high;
  Price low;
  Price close;
  Quantity volume;
  std::uint64_t ts_event;
  std::uint64_t ts_init;
};

}  // namespace trading
