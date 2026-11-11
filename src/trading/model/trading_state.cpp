#include "trading/model/trading_state.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include <boost/multiprecision/cpp_int.hpp>

#include "trading/model/events.h"
#include "trading/model/instrument.h"

namespace trading {
namespace {

std::int64_t checkedAdd(std::int64_t lhs, std::int64_t rhs,
                        const char* message) {
  if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
      (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs)) {
    throw std::overflow_error(message);
  }
  return lhs + rhs;
}

std::int64_t checkedScaledProduct(std::int64_t price_raw,
                                  std::uint64_t quantity_raw,
                                  std::int64_t multiplier_raw) {
  const boost::multiprecision::cpp_int product =
      boost::multiprecision::cpp_int(price_raw) * quantity_raw *
      multiplier_raw /
      (boost::multiprecision::cpp_int(Price::kScale) * Price::kScale);
  if (product > std::numeric_limits<std::int64_t>::max() ||
      product < std::numeric_limits<std::int64_t>::min()) {
    throw std::overflow_error("position PnL exceeds representable range");
  }
  return product.convert_to<std::int64_t>();
}

Price weightedPrice(Price lhs, std::uint64_t lhs_quantity, Price rhs,
                    std::uint64_t rhs_quantity) {
  const boost::multiprecision::cpp_int total =
      boost::multiprecision::cpp_int(lhs.raw()) * lhs_quantity +
      boost::multiprecision::cpp_int(rhs.raw()) * rhs_quantity;
  const auto quantity = lhs_quantity + rhs_quantity;
  const boost::multiprecision::cpp_int average = total / quantity;
  if (average > std::numeric_limits<std::int64_t>::max() ||
      average < std::numeric_limits<std::int64_t>::min()) {
    throw std::overflow_error("average price exceeds representable range");
  }
  return Price::fromRaw(average.convert_to<std::int64_t>());
}

std::uint64_t magnitude(std::int64_t value) {
  if (value >= 0) {
    return static_cast<std::uint64_t>(value);
  }
  return static_cast<std::uint64_t>(-(value + 1)) + 1;
}

}  // namespace

Money Position::apply(const OrderFilled& fill, const Instrument& instrument) {
  if (fill.instrument_id != instrument_id || fill.instrument_id != instrument.id) {
    throw std::invalid_argument("fill instrument does not match position");
  }
  if (fill.fee && fill.fee->currency() != fees.currency()) {
    throw std::invalid_argument("fill fee currency does not match position");
  }

  const auto fill_quantity = static_cast<std::int64_t>(fill.quantity.raw());
  const auto signed_fill =
      fill.side == OrderSide::Buy ? fill_quantity : -fill_quantity;
  const auto old_quantity = quantity_raw;
  const auto old_magnitude = magnitude(old_quantity);
  const bool same_direction =
      old_quantity == 0 || (old_quantity > 0) == (signed_fill > 0);
  Money realized_delta = Money::zero(realized_pnl.currency());

  if (same_direction) {
    if (old_quantity == 0) {
      average_open_price = fill.price;
      ts_opened = fill.ts_event;
    } else {
      average_open_price = weightedPrice(
          *average_open_price, old_magnitude, fill.price, fill.quantity.raw());
    }
  } else {
    const auto closed_quantity = std::min(old_magnitude, fill.quantity.raw());
    const auto price_difference =
        old_quantity > 0 ? fill.price.raw() - average_open_price->raw()
                         : average_open_price->raw() - fill.price.raw();
    realized_delta = Money::fromRaw(
        checkedScaledProduct(price_difference, closed_quantity,
                             instrument.contract_multiplier.raw()),
        realized_pnl.currency());
    if (fill.quantity.raw() > old_magnitude) {
      average_open_price = fill.price;
      ts_opened = fill.ts_event;
    }
  }

  quantity_raw = checkedAdd(old_quantity, signed_fill,
                            "position quantity overflow");
  if (fill.fee) {
    fees = fees.checkedAdd(*fill.fee);
    realized_delta = realized_delta.checkedSubtract(*fill.fee);
  }
  realized_pnl = realized_pnl.checkedAdd(realized_delta);
  ts_last = fill.ts_event;
  if (quantity_raw == 0) {
    average_open_price.reset();
    ts_closed = fill.ts_event;
  } else {
    ts_closed = 0;
  }
  return realized_delta;
}

AccountBalance AccountBalance::cash(Money total) {
  const auto currency = total.currency();
  return AccountBalance{total, total, Money::zero(currency)};
}

bool AccountBalance::isValid() const {
  if (!total.isValid() || !free.isValid() || !locked.isValid() ||
      total.currency() != free.currency() ||
      total.currency() != locked.currency() || total.raw() < 0 ||
      free.raw() < 0 || locked.raw() < 0 || free.raw() > total.raw()) {
    return false;
  }
  return locked.raw() == total.raw() - free.raw();
}

}  // namespace trading
