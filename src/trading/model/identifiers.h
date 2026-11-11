#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace trading {

using Symbol = std::string;
using Venue = std::string;

struct InstrumentId {
  Symbol symbol;
  Venue venue;

  InstrumentId() = default;
  InstrumentId(Symbol symbol_value, Venue venue_value)
      : symbol(std::move(symbol_value)), venue(std::move(venue_value)) {}

  bool empty() const { return symbol.empty() || venue.empty(); }
  std::string toString() const { return symbol + "." + venue; }

  friend bool operator==(const InstrumentId&, const InstrumentId&) = default;
};

using Currency = std::string;
using AccountId = std::string;
using PositionId = std::string;
using ExecutionClientId = std::string;
using ExecAlgorithmId = std::string;
using ClientOrderId = std::string;
using VenueOrderId = std::string;
using StrategyId = std::string;
using BookOrderId = std::uint64_t;
using TradeId = std::string;

}  // namespace trading

template <>
struct std::hash<trading::InstrumentId> {
  std::size_t operator()(const trading::InstrumentId& value) const noexcept {
    const auto symbol_hash = std::hash<trading::Symbol>{}(value.symbol);
    const auto venue_hash = std::hash<trading::Venue>{}(value.venue);
    return symbol_hash ^
           (venue_hash + 0x9e3779b9U + (symbol_hash << 6U) +
            (symbol_hash >> 2U));
  }
};
