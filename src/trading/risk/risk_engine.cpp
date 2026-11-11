#include "trading/risk/risk_engine.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include <boost/multiprecision/cpp_int.hpp>

namespace trading {
namespace {

using boost::multiprecision::cpp_int;

std::uint64_t magnitude(std::int64_t value) {
  if (value >= 0) {
    return static_cast<std::uint64_t>(value);
  }
  return static_cast<std::uint64_t>(-(value + 1)) + 1;
}

bool isClosed(OrderStatus status) {
  return status == OrderStatus::Filled || status == OrderStatus::Canceled ||
         status == OrderStatus::Denied || status == OrderStatus::Rejected ||
         status == OrderStatus::Expired;
}

}  // namespace

RiskEngine::RiskEngine(const Cache& cache, RiskEngineConfig config)
    : cache_(cache), config_(std::move(config)) {
  for (const auto& [instrument_id, value] :
       config_.max_notional_per_order) {
    static_cast<void>(instrument_id);
    if (value.raw() <= 0) {
      throw std::invalid_argument("max notional per order must be positive");
    }
  }
}

std::optional<OrderDenied> RiskEngine::execute(
    const SubmitOrder& order, const AccountId& account_id) {
  const auto* instrument = cache_.instrument(order.instrument_id);
  const auto* account = cache_.account(account_id);
  if (instrument == nullptr) {
    return OrderDenied{order.client_order_id, "instrument is not cached",
                       order.ts_init};
  }
  if (account == nullptr) {
    return OrderDenied{order.client_order_id, "account is not cached",
                       order.ts_init};
  }
  if (order.order_type != OrderType::Limit) {
    return OrderDenied{order.client_order_id,
                       "market order risk pricing is not supported",
                       order.ts_init};
  }
  if (order.quantity.isZero() || order.limit_price.raw() <= 0) {
    return OrderDenied{order.client_order_id,
                       "quantity and price must be positive", order.ts_init};
  }

  const auto notional_raw = calculateNotionalRaw(order, *instrument);
  if (!notional_raw) {
    return OrderDenied{order.client_order_id,
                       "order notional exceeds representable range",
                       order.ts_init};
  }
  if (const auto reason = validate(order, *account, *instrument,
                                   *notional_raw)) {
    return OrderDenied{order.client_order_id, *reason, order.ts_init};
  }

  reservations_.insert_or_assign(
      order.client_order_id,
      Reservation{account_id, order.instrument_id, order.side,
                  order.quantity.raw(), *notional_raw});
  return std::nullopt;
}

void RiskEngine::refreshReservation(
    const ClientOrderId& client_order_id) {
  const auto* state = cache_.order(client_order_id);
  if (state == nullptr || isClosed(state->status)) {
    reservations_.erase(client_order_id);
    return;
  }
  const auto reservation = reservations_.find(client_order_id);
  if (reservation == reservations_.end()) {
    return;
  }
  const auto* instrument = cache_.instrument(state->order.instrument_id);
  if (instrument == nullptr) {
    reservations_.erase(reservation);
    return;
  }
  auto remaining = state->order;
  remaining.quantity = state->leaves_quantity;
  const auto notional_raw = calculateNotionalRaw(remaining, *instrument);
  if (!notional_raw) {
    throw std::overflow_error("reserved notional exceeds representable range");
  }
  reservation->second.quantity_raw = state->leaves_quantity.raw();
  reservation->second.notional_raw = *notional_raw;
}

void RiskEngine::setMaxNotionalPerOrder(
    const InstrumentId& instrument_id, Decimal max_notional) {
  if (max_notional.raw() <= 0) {
    throw std::invalid_argument("max notional per order must be positive");
  }
  config_.max_notional_per_order.insert_or_assign(instrument_id,
                                                   max_notional);
}

std::optional<std::string> RiskEngine::validate(
    const SubmitOrder& order, const Account& account,
    const Instrument& instrument, std::int64_t notional_raw) const {

  // Reject every new order while the global trading kill switch is active
  if (trading_state_ == TradingState::Halted) {
    return "trading is halted";
  }

  // Permit only explicitly reduce-only orders while reducing exposure
  if (trading_state_ == TradingState::Reducing && !order.reduce_only) {
    return "only reduce-only orders are allowed while reducing";
  }

  // Prevent an account from trading an instrument on another venue
  if (account.venue != instrument.id.venue) {
    return "account venue does not match instrument venue";
  }

  // Enforce the instrument minimum and maximum order quantity
  if (order.quantity < instrument.min_quantity ||
      order.quantity > instrument.max_quantity) {
    return "quantity is outside instrument limits";
  }

  // Require quantity to be an exact multiple of the instrument lot size
  if (order.quantity.raw() % instrument.lot_size.raw() != 0) {
    return "quantity is not aligned to lot size";
  }

  // Require limit price to be an exact multiple of the instrument tick size
  if (order.limit_price.raw() % instrument.tick_size.raw() != 0) {
    return "price is not aligned to tick size";
  }

  // Enforce the configured quote-currency notional limit for this instrument
  if (const auto limit =
          config_.max_notional_per_order.find(order.instrument_id);
      limit != config_.max_notional_per_order.end() &&
      notional_raw > limit->second.raw()) {
    return "max notional per order exceeded";
  }

  // Resolve settled position and same-side inflight quantity for exposure checks
  const auto* position =
      cache_.positionForInstrument(account.id, order.instrument_id);
  const auto position_raw = position == nullptr ? 0 : position->quantity_raw;
  const auto reserved_quantity =
      reservedQuantity(account.id, order.instrument_id, order.side);

  // Ensure reduce-only direction and quantity cannot increase or reverse exposure
  if (order.reduce_only) {
    const bool reduces_long = order.side == OrderSide::Sell && position_raw > 0;
    const bool reduces_short = order.side == OrderSide::Buy && position_raw < 0;

    // Require the order side to oppose the current position direction
    if (!reduces_long && !reduces_short) {
      return "reduce-only order would not reduce the position";
    }
    const auto closable = magnitude(position_raw);

    // Include earlier close orders when calculating remaining closable quantity
    if (reserved_quantity > closable ||
        order.quantity.raw() > closable - reserved_quantity) {
      return "reduce-only quantity exceeds the available position";
    }
  }

  // Prevent cash accounts from selling more than unreserved long inventory
  if (account.type == AccountType::Cash && order.side == OrderSide::Sell) {
    const auto long_quantity =
        position_raw > 0 ? static_cast<std::uint64_t>(position_raw) : 0;
    if (reserved_quantity > long_quantity ||
        order.quantity.raw() > long_quantity - reserved_quantity) {
      return "insufficient available position";
    }
  }

  // Prevent cash-account buys from exceeding unreserved quote-currency cash
  if (account.type == AccountType::Cash && order.side == OrderSide::Buy) {

    // Reject cross-currency funding until an exchange-rate model is available
    if (account.base_currency != instrument.quote_currency) {
      return "account currency conversion is not supported";
    }

    // Include earlier inflight buys when calculating available account cash
    const auto reserved = reservedBuyNotional(account.id);
    if (reserved > account.balance.free.raw() ||
        notional_raw > account.balance.free.raw() - reserved) {
      return "insufficient available cash";
    }
  }
  return std::nullopt;
}

std::optional<std::int64_t> RiskEngine::calculateNotionalRaw(
    const SubmitOrder& order, const Instrument& instrument) const {
  const cpp_int notional =
      cpp_int(order.limit_price.raw()) * order.quantity.raw() *
      instrument.contract_multiplier.raw() /
      (cpp_int(Price::kScale) * Price::kScale);
  if (notional < 0 || notional > std::numeric_limits<std::int64_t>::max()) {
    return std::nullopt;
  }
  return notional.convert_to<std::int64_t>();
}

std::int64_t RiskEngine::reservedBuyNotional(
    const AccountId& account_id) const {
  cpp_int total = 0;
  for (const auto& [client_order_id, reservation] : reservations_) {
    static_cast<void>(client_order_id);
    if (reservation.account_id == account_id &&
        reservation.side == OrderSide::Buy) {
      total += reservation.notional_raw;
    }
  }
  if (total > std::numeric_limits<std::int64_t>::max()) {
    throw std::overflow_error("reserved buy notional exceeds representable range");
  }
  return total.convert_to<std::int64_t>();
}

std::uint64_t RiskEngine::reservedQuantity(
    const AccountId& account_id, const InstrumentId& instrument_id,
    OrderSide side) const {
  cpp_int total = 0;
  for (const auto& [client_order_id, reservation] : reservations_) {
    static_cast<void>(client_order_id);
    if (reservation.account_id == account_id &&
        reservation.instrument_id == instrument_id &&
        reservation.side == side) {
      total += reservation.quantity_raw;
    }
  }
  if (total > std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("reserved quantity exceeds representable range");
  }
  return total.convert_to<std::uint64_t>();
}

}  // namespace trading
