#include "trading/portfolio/portfolio.h"

#include <limits>
#include <stdexcept>
#include <utility>

#include <boost/multiprecision/cpp_int.hpp>

namespace trading {
namespace {

std::int64_t checkedScaledProduct(std::int64_t price_raw,
                                  std::int64_t quantity_raw,
                                  std::int64_t multiplier_raw,
                                  const char* message) {
  const boost::multiprecision::cpp_int product =
      boost::multiprecision::cpp_int(price_raw) * quantity_raw *
      multiplier_raw /
      (boost::multiprecision::cpp_int(Price::kScale) * Price::kScale);
  if (product > std::numeric_limits<std::int64_t>::max() ||
      product < std::numeric_limits<std::int64_t>::min()) {
    throw std::overflow_error(message);
  }
  return product.convert_to<std::int64_t>();
}

Price weightedPrice(Price lhs, std::uint64_t lhs_quantity, Price rhs,
                    std::uint64_t rhs_quantity) {
  const boost::multiprecision::cpp_int total =
      boost::multiprecision::cpp_int(lhs.raw()) * lhs_quantity +
      boost::multiprecision::cpp_int(rhs.raw()) * rhs_quantity;
  const boost::multiprecision::cpp_int average =
      total / (lhs_quantity + rhs_quantity);
  if (average > std::numeric_limits<std::int64_t>::max() ||
      average < std::numeric_limits<std::int64_t>::min()) {
    throw std::overflow_error("average price exceeds representable range");
  }
  return Price::fromRaw(average.convert_to<std::int64_t>());
}

OrderState applyFillToOrder(const OrderState& current,
                            const OrderFilled& fill) {
  if (current.status != OrderStatus::Accepted &&
      current.status != OrderStatus::PartiallyFilled) {
    throw std::logic_error("only open orders can be filled");
  }
  if (fill.quantity.isZero() || fill.quantity > current.leaves_quantity ||
      fill.instrument_id != current.order.instrument_id ||
      fill.side != current.order.side ||
      fill.venue_order_id != current.venue_order_id || fill.trade_id.empty() ||
      (fill.fee && fill.fee->raw() < 0) || fill.price.raw() <= 0 ||
      fill.ts_event < current.ts_last) {
    throw std::invalid_argument("fill does not match the open order");
  }
  if (fill.fee && fill.fee->currency() != current.fees.currency()) {
    throw std::invalid_argument("fill fee currency does not match order account");
  }

  auto next = current;
  const auto previous_filled = next.filled_quantity.raw();
  const auto new_filled = previous_filled + fill.quantity.raw();
  next.average_price = next.average_price
                           ? weightedPrice(*next.average_price, previous_filled,
                                           fill.price, fill.quantity.raw())
                           : std::optional<Price>(fill.price);
  next.filled_quantity = Quantity::fromRaw(new_filled);
  next.leaves_quantity =
      Quantity::fromRaw(next.order.quantity.raw() - new_filled);
  if (fill.fee) {
    next.fees = next.fees.checkedAdd(*fill.fee);
  }
  next.ts_last = fill.ts_event;
  next.status = next.leaves_quantity.isZero() ? OrderStatus::Filled
                                               : OrderStatus::PartiallyFilled;
  return next;
}

Account applyFillToAccount(const Account& current, const OrderFilled& fill,
                           const Instrument& instrument,
                           const Money& realized_delta) {
  if (current.base_currency != instrument.quote_currency ||
      realized_delta.currency() != current.base_currency) {
    throw std::invalid_argument(
        "account currency conversion is not supported");
  }
  if (fill.fee && fill.fee->currency() != current.base_currency) {
    throw std::invalid_argument("fill fee currency does not match account");
  }

  auto next = current;
  const auto notional = checkedScaledProduct(
      fill.price.raw(), static_cast<std::int64_t>(fill.quantity.raw()),
      instrument.contract_multiplier.raw(),
      "fill notional exceeds representable range");
  auto cash_change = Money::fromRaw(
      fill.side == OrderSide::Buy ? -notional : notional,
      current.base_currency);
  if (fill.fee) {
    cash_change = cash_change.checkedSubtract(*fill.fee);
    next.fees = next.fees.checkedAdd(*fill.fee);
  }
  next.balance.total = next.balance.total.checkedAdd(cash_change);
  next.balance.free = next.balance.free.checkedAdd(cash_change);
  if (!next.balance.isValid()) {
    throw std::invalid_argument("fill would make account balance invalid");
  }
  next.realized_pnl = next.realized_pnl.checkedAdd(realized_delta);
  return next;
}

}  // namespace

bool Portfolio::apply(const OrderFilled& fill) {
  if (const auto* applied = cache_.fill(fill.trade_id); applied != nullptr) {
    if (*applied == fill) {
      return false;
    }
    throw std::invalid_argument("trade ID is already assigned to another fill");
  }

  const auto* current_order = cache_.order(fill.client_order_id);
  if (current_order == nullptr) {
    throw std::invalid_argument("fill order is not cached");
  }
  const auto* instrument = cache_.instrument(fill.instrument_id);
  if (instrument == nullptr) {
    throw std::logic_error("fill instrument is not cached");
  }
  const auto* current_account = cache_.account(current_order->account_id);
  if (current_account == nullptr) {
    throw std::logic_error("fill account is not cached");
  }

  auto next_order = applyFillToOrder(*current_order, fill);
  const auto position_id =
      Cache::makePositionId(current_account->id, fill.instrument_id);
  const auto* current_position = cache_.position(position_id);
  auto next_position =
      current_position == nullptr
          ? Position{position_id,
                     current_account->id,
                     fill.instrument_id,
                     0,
                     std::nullopt,
                     Money::zero(instrument->quote_currency),
                     Money::zero(instrument->quote_currency),
                     0,
                     0,
                     0}
          : *current_position;
  const auto realized_delta = next_position.apply(fill, *instrument);
  auto next_account = applyFillToAccount(
      *current_account, fill, *instrument, realized_delta);

  cache_.commitFill(std::move(next_order), std::move(next_account),
                    std::move(next_position), fill);
  return true;
}

void Portfolio::update(const QuoteTick& quote) {
  auto& prices = valuation_prices_[quote.instrument_id];
  prices.bid = quote.bid;
  prices.ask = quote.ask;
}

void Portfolio::update(const TradeTick& trade) {
  valuation_prices_[trade.instrument_id].last = trade.price;
}

void Portfolio::update(const Bar& bar) {
  valuation_prices_[bar.bar_type.instrument_id].bar_close = bar.close;
}

Decimal Portfolio::position(const AccountId& account_id,
                            const InstrumentId& instrument_id) const {
  const auto* value = cache_.positionForInstrument(account_id, instrument_id);
  return value == nullptr ? Decimal{} : value->signedQuantity();
}

std::optional<Money> Portfolio::unrealizedPnl(
    const AccountId& account_id,
    const InstrumentId& instrument_id) const {
  const auto* account = cache_.account(account_id);
  const auto* instrument = cache_.instrument(instrument_id);
  if (account == nullptr || instrument == nullptr ||
      account->base_currency != instrument->quote_currency) {
    return std::nullopt;
  }
  const auto* position =
      cache_.positionForInstrument(account_id, instrument_id);
  if (position == nullptr || position->quantity_raw == 0) {
    return Money::zero(account->base_currency);
  }
  const auto price = valuationPrice(*position);
  if (!price || !position->average_open_price) {
    return std::nullopt;
  }
  return Money::fromRaw(
      checkedScaledProduct(
          price->raw() - position->average_open_price->raw(),
          position->quantity_raw, instrument->contract_multiplier.raw(),
          "unrealized PnL exceeds representable range"),
      account->base_currency);
}

std::optional<Money> Portfolio::realizedPnl(
    const AccountId& account_id,
    const InstrumentId& instrument_id) const {
  const auto* account = cache_.account(account_id);
  const auto* instrument = cache_.instrument(instrument_id);
  if (account == nullptr || instrument == nullptr ||
      account->base_currency != instrument->quote_currency) {
    return std::nullopt;
  }
  const auto* position =
      cache_.positionForInstrument(account_id, instrument_id);
  return position == nullptr ? Money::zero(account->base_currency)
                             : std::optional<Money>(position->realized_pnl);
}

std::optional<Money> Portfolio::totalPnl(
    const AccountId& account_id,
    const InstrumentId& instrument_id) const {
  const auto realized = realizedPnl(account_id, instrument_id);
  const auto unrealized = unrealizedPnl(account_id, instrument_id);
  if (!realized || !unrealized) {
    return std::nullopt;
  }
  return realized->checkedAdd(*unrealized);
}

std::optional<Money> Portfolio::netExposure(
    const AccountId& account_id,
    const InstrumentId& instrument_id) const {
  return markValue(account_id, instrument_id);
}

std::optional<Money> Portfolio::markValue(
    const AccountId& account_id,
    const InstrumentId& instrument_id) const {
  const auto* account = cache_.account(account_id);
  const auto* instrument = cache_.instrument(instrument_id);
  if (account == nullptr || instrument == nullptr ||
      account->base_currency != instrument->quote_currency) {
    return std::nullopt;
  }
  const auto* position =
      cache_.positionForInstrument(account_id, instrument_id);
  if (position == nullptr || position->quantity_raw == 0) {
    return Money::zero(account->base_currency);
  }
  return signedMarkValue(*position);
}

std::optional<Money> Portfolio::equity(const AccountId& account_id) const {
  const auto* account = cache_.account(account_id);
  if (account == nullptr) {
    return std::nullopt;
  }
  auto result = account->balance.total;
  for (const auto* position : cache_.positionsForAccount(account_id)) {
    if (position->quantity_raw == 0) {
      continue;
    }
    const auto value = signedMarkValue(*position);
    if (!value || value->currency() != result.currency()) {
      return std::nullopt;
    }
    result = result.checkedAdd(*value);
  }
  return result;
}

std::optional<Price> Portfolio::valuationPrice(
    const Position& position) const {
  const auto it = valuation_prices_.find(position.instrument_id);
  if (it == valuation_prices_.end()) {
    return std::nullopt;
  }
  const auto& prices = it->second;
  const auto quote = position.quantity_raw > 0 ? prices.bid : prices.ask;
  if (quote) {
    return quote;
  }
  if (prices.last) {
    return prices.last;
  }
  return prices.bar_close;
}

std::optional<Money> Portfolio::signedMarkValue(
    const Position& position) const {
  const auto* account = cache_.account(position.account_id);
  const auto* instrument = cache_.instrument(position.instrument_id);
  const auto price = valuationPrice(position);
  if (account == nullptr || instrument == nullptr || !price ||
      account->base_currency != instrument->quote_currency) {
    return std::nullopt;
  }
  return Money::fromRaw(
      checkedScaledProduct(price->raw(), position.quantity_raw,
                           instrument->contract_multiplier.raw(),
                           "mark value exceeds representable range"),
      account->base_currency);
}

}  // namespace trading
