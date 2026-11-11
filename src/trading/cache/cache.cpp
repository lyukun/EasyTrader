#include "trading/cache/cache.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace trading {
namespace {

constexpr std::size_t kMaximumCapacity = 1'000'000;

template <typename T>
void pushBounded(std::deque<T>& values, T value, std::size_t capacity) {
  values.push_front(std::move(value));
  if (values.size() > capacity) {
    values.pop_back();
  }
}

}  // namespace

Cache::Cache(CacheConfig config) : config_(config) {
  if (config_.tick_capacity == 0 || config_.tick_capacity > kMaximumCapacity ||
      config_.bar_capacity == 0 || config_.bar_capacity > kMaximumCapacity) {
    throw std::invalid_argument("cache capacities must be in [1, 1000000]");
  }
}

void Cache::addInstrument(Instrument instrument) {
  if (instrument.id.empty() || instrument.base_currency.empty() ||
      instrument.quote_currency.empty() ||
      instrument.price_precision > Price::kMaxPrecision ||
      instrument.size_precision > Quantity::kMaxPrecision ||
      instrument.tick_size.precision() > instrument.price_precision ||
      instrument.lot_size.precision() > instrument.size_precision ||
      instrument.tick_size.raw() <= 0 || instrument.lot_size.isZero() ||
      instrument.min_quantity.isZero() ||
      instrument.min_quantity > instrument.max_quantity ||
      instrument.contract_multiplier.raw() <= 0) {
    throw std::invalid_argument("instrument definition is invalid");
  }
  instruments_.insert_or_assign(instrument.id, std::move(instrument));
}

void Cache::addAccount(Account account) {
  if (account.id.empty() || account.venue.empty() ||
      account.base_currency.empty() || !account.starting_balance.isValid() ||
      account.starting_balance.currency() != account.base_currency ||
      account.starting_balance.raw() < 0 || !account.balance.isValid() ||
      account.balance.total.currency() != account.base_currency ||
      !account.realized_pnl.isValid() ||
      account.realized_pnl.currency() != account.base_currency ||
      !account.fees.isValid() || account.fees.currency() != account.base_currency) {
    throw std::invalid_argument("account definition is invalid");
  }
  accounts_.insert_or_assign(account.id, std::move(account));
}

void Cache::storeQuote(const QuoteTick& quote) {
  pushBounded(quotes_[quote.instrument_id], quote, config_.tick_capacity);
}

void Cache::storeTrade(const TradeTick& trade) {
  pushBounded(trades_[trade.instrument_id], trade, config_.tick_capacity);
}

bool Cache::storeBar(const Bar& bar) {
  auto& values = bars_[bar.bar_type];
  if (!values.empty() && values.front().ts_event == bar.ts_event) {
    values.front() = bar;
    return true;
  }
  if (!values.empty() && values.front().ts_event > bar.ts_event) {
    return false;
  }
  pushBounded(values, bar, config_.bar_capacity);
  return true;
}

void Cache::addOrder(const SubmitOrder& order, const AccountId& account_id,
                     const ExecutionClientId& execution_client_id) {
  if (orders_.contains(order.client_order_id)) {
    throw std::invalid_argument("client order ID already exists");
  }
  if (!instruments_.contains(order.instrument_id)) {
    throw std::invalid_argument("order instrument is not cached");
  }
  if (!accounts_.contains(account_id)) {
    throw std::invalid_argument("order account is not cached");
  }
  if (execution_client_id.empty()) {
    throw std::invalid_argument("execution client ID must not be empty");
  }
  orders_.emplace(
      order.client_order_id,
      OrderState{order,
                 account_id,
                 execution_client_id,
                 OrderStatus::Initialized,
                 {},
                 Quantity::fromRaw(0),
                 order.quantity,
                 std::nullopt,
                 Money::zero(accounts_.at(account_id).base_currency),
                 {},
                 0,
                 0,
                 order.ts_init});
  instrument_orders_[order.instrument_id].insert(order.client_order_id);
  strategy_orders_[order.strategy_id].insert(order.client_order_id);
  account_orders_[account_id].insert(order.client_order_id);
}

void Cache::submitOrder(const ClientOrderId& client_order_id,
                        std::uint64_t ts_event) {
  auto& state = orders_.at(client_order_id);
  if (state.status != OrderStatus::Initialized) {
    throw std::logic_error("only initialized orders can be submitted");
  }
  state.status = OrderStatus::Submitted;
  state.ts_submitted = ts_event;
  state.ts_last = ts_event;
  inflight_orders_.insert(client_order_id);
}

void Cache::acceptOrder(const OrderAccepted& event) {
  auto& state = orders_.at(event.client_order_id);
  if (state.status != OrderStatus::Submitted || event.venue_order_id.empty() ||
      event.ts_event < state.ts_last) {
    throw std::logic_error("only submitted orders can be accepted");
  }
  if (const auto existing = venue_to_client_order_ids_.find(event.venue_order_id);
      existing != venue_to_client_order_ids_.end() &&
      existing->second != event.client_order_id) {
    throw std::logic_error("venue order ID is already mapped");
  }
  state.status = OrderStatus::Accepted;
  state.venue_order_id = event.venue_order_id;
  state.ts_accepted = event.ts_event;
  state.ts_last = event.ts_event;
  venue_to_client_order_ids_.insert_or_assign(event.venue_order_id,
                                               event.client_order_id);
  client_to_venue_order_ids_.insert_or_assign(event.client_order_id,
                                               event.venue_order_id);
  inflight_orders_.erase(event.client_order_id);
  markOrderOpen(event.client_order_id);
}

void Cache::updateOrder(const OrderUpdated& event) {
  auto& state = orders_.at(event.client_order_id);
  if (state.status != OrderStatus::Accepted &&
      state.status != OrderStatus::PartiallyFilled) {
    throw std::logic_error("only open orders can be updated");
  }
  if (event.quantity.isZero() || event.quantity < state.filled_quantity ||
      event.price.raw() <= 0 || event.ts_event < state.ts_last) {
    throw std::invalid_argument("order update is invalid");
  }
  state.status = OrderStatus::Accepted;
  state.order.quantity = event.quantity;
  state.order.limit_price = event.price;
  state.leaves_quantity = Quantity::fromRaw(
      event.quantity.raw() - state.filled_quantity.raw());
  state.ts_last = event.ts_event;
}

void Cache::cancelOrder(const OrderCanceled& event) {
  auto& state = orders_.at(event.client_order_id);
  if (state.status != OrderStatus::Accepted &&
      state.status != OrderStatus::PartiallyFilled &&
      state.status != OrderStatus::PendingCancel) {
    throw std::logic_error("only open orders can be canceled");
  }
  if (event.ts_event < state.ts_last) {
    throw std::invalid_argument("cancel timestamp predates order state");
  }
  state.status = OrderStatus::Canceled;
  state.last_reason = event.reason;
  state.ts_last = event.ts_event;
  markOrderClosed(event.client_order_id);
}

void Cache::denyOrder(const OrderDenied& event) {
  auto& state = orders_.at(event.client_order_id);
  if (state.status != OrderStatus::Initialized &&
      state.status != OrderStatus::Submitted) {
    throw std::logic_error("only local orders can be denied");
  }
  state.status = OrderStatus::Denied;
  state.last_reason = event.reason;
  state.ts_last = event.ts_event;
  markOrderClosed(event.client_order_id);
}

void Cache::rejectOrder(const OrderRejected& event) {
  auto& state = orders_.at(event.client_order_id);
  if (state.status == OrderStatus::Filled || state.status == OrderStatus::Canceled ||
      state.status == OrderStatus::Expired) {
    throw std::logic_error("closed order cannot be rejected");
  }
  if (event.ts_event < state.ts_last) {
    throw std::invalid_argument("rejection timestamp predates order state");
  }
  state.status = OrderStatus::Rejected;
  state.last_reason = event.reason;
  state.ts_last = event.ts_event;
  markOrderClosed(event.client_order_id);
}

void Cache::expireOrder(const OrderExpired& event) {
  auto& state = orders_.at(event.client_order_id);
  if (state.status != OrderStatus::Accepted &&
      state.status != OrderStatus::PartiallyFilled) {
    throw std::logic_error("only open orders can expire");
  }
  if (event.ts_event < state.ts_last) {
    throw std::invalid_argument("expiry timestamp predates order state");
  }
  state.status = OrderStatus::Expired;
  state.ts_last = event.ts_event;
  markOrderClosed(event.client_order_id);
}

void Cache::addOrderBook(OrderBook book) {
  const auto instrument_id = book.instrumentId();
  if (!instruments_.contains(instrument_id)) {
    throw std::invalid_argument("order book instrument is not cached");
  }
  order_books_.insert_or_assign(instrument_id, std::move(book));
}

const Instrument* Cache::instrument(const InstrumentId& instrument_id) const {
  const auto it = instruments_.find(instrument_id);
  return it == instruments_.end() ? nullptr : &it->second;
}

const Account* Cache::account(const AccountId& account_id) const {
  const auto it = accounts_.find(account_id);
  return it == accounts_.end() ? nullptr : &it->second;
}

const Position* Cache::position(const PositionId& position_id) const {
  const auto it = positions_.find(position_id);
  return it == positions_.end() ? nullptr : &it->second;
}

const Position* Cache::positionForInstrument(
    const AccountId& account_id, const InstrumentId& instrument_id) const {
  return position(makePositionId(account_id, instrument_id));
}

std::vector<const Position*> Cache::positionsForAccount(
    const AccountId& account_id) const {
  std::vector<const Position*> result;
  for (const auto& [position_id, position] : positions_) {
    static_cast<void>(position_id);
    if (position.account_id == account_id) {
      result.push_back(&position);
    }
  }
  return result;
}

const OrderFilled* Cache::fill(const TradeId& trade_id) const {
  const auto it = fills_.find(trade_id);
  return it == fills_.end() ? nullptr : &it->second;
}

const QuoteTick* Cache::quote(const InstrumentId& instrument_id,
                              std::size_t index) const {
  const auto it = quotes_.find(instrument_id);
  return it == quotes_.end() || index >= it->second.size()
             ? nullptr
             : &it->second[index];
}

std::vector<QuoteTick> Cache::quotes(const InstrumentId& instrument_id) const {
  const auto it = quotes_.find(instrument_id);
  return it == quotes_.end()
             ? std::vector<QuoteTick>{}
             : std::vector<QuoteTick>(it->second.begin(), it->second.end());
}

std::size_t Cache::quoteCount(const InstrumentId& instrument_id) const {
  const auto it = quotes_.find(instrument_id);
  return it == quotes_.end() ? 0 : it->second.size();
}

const TradeTick* Cache::trade(const InstrumentId& instrument_id,
                              std::size_t index) const {
  const auto it = trades_.find(instrument_id);
  return it == trades_.end() || index >= it->second.size()
             ? nullptr
             : &it->second[index];
}

std::vector<TradeTick> Cache::trades(const InstrumentId& instrument_id) const {
  const auto it = trades_.find(instrument_id);
  return it == trades_.end()
             ? std::vector<TradeTick>{}
             : std::vector<TradeTick>(it->second.begin(), it->second.end());
}

std::size_t Cache::tradeCount(const InstrumentId& instrument_id) const {
  const auto it = trades_.find(instrument_id);
  return it == trades_.end() ? 0 : it->second.size();
}

const Bar* Cache::bar(const BarType& bar_type, std::size_t index) const {
  const auto it = bars_.find(bar_type);
  return it == bars_.end() || index >= it->second.size()
             ? nullptr
             : &it->second[index];
}

std::vector<Bar> Cache::bars(const BarType& bar_type) const {
  const auto it = bars_.find(bar_type);
  return it == bars_.end() ? std::vector<Bar>{}
                           : std::vector<Bar>(it->second.begin(), it->second.end());
}

std::size_t Cache::barCount(const BarType& bar_type) const {
  const auto it = bars_.find(bar_type);
  return it == bars_.end() ? 0 : it->second.size();
}

const OrderState* Cache::order(const ClientOrderId& client_order_id) const {
  const auto it = orders_.find(client_order_id);
  return it == orders_.end() ? nullptr : &it->second;
}

std::optional<ClientOrderId> Cache::clientOrderId(
    const VenueOrderId& venue_order_id) const {
  const auto it = venue_to_client_order_ids_.find(venue_order_id);
  return it == venue_to_client_order_ids_.end()
             ? std::nullopt
             : std::optional<ClientOrderId>(it->second);
}

std::optional<VenueOrderId> Cache::venueOrderId(
    const ClientOrderId& client_order_id) const {
  const auto it = client_to_venue_order_ids_.find(client_order_id);
  return it == client_to_venue_order_ids_.end()
             ? std::nullopt
             : std::optional<VenueOrderId>(it->second);
}

std::vector<const OrderState*> Cache::ordersOpen() const {
  return ordersForIds(open_orders_);
}

std::vector<const OrderState*> Cache::ordersClosed() const {
  return ordersForIds(closed_orders_);
}

std::vector<const OrderState*> Cache::ordersInflight() const {
  return ordersForIds(inflight_orders_);
}

std::vector<const OrderState*> Cache::ordersForInstrument(
    const InstrumentId& instrument_id) const {
  const auto it = instrument_orders_.find(instrument_id);
  return it == instrument_orders_.end() ? std::vector<const OrderState*>{}
                                        : ordersForIds(it->second);
}

std::vector<const OrderState*> Cache::ordersForStrategy(
    const StrategyId& strategy_id) const {
  const auto it = strategy_orders_.find(strategy_id);
  return it == strategy_orders_.end() ? std::vector<const OrderState*>{}
                                      : ordersForIds(it->second);
}

std::vector<const OrderState*> Cache::ordersForAccount(
    const AccountId& account_id) const {
  const auto it = account_orders_.find(account_id);
  return it == account_orders_.end() ? std::vector<const OrderState*>{}
                                     : ordersForIds(it->second);
}

const OrderBook* Cache::orderBook(const InstrumentId& instrument_id) const {
  const auto it = order_books_.find(instrument_id);
  return it == order_books_.end() ? nullptr : &it->second;
}

OrderBook* Cache::orderBook(const InstrumentId& instrument_id) {
  const auto it = order_books_.find(instrument_id);
  return it == order_books_.end() ? nullptr : &it->second;
}

bool Cache::hasOrderBook(const InstrumentId& instrument_id) const {
  return order_books_.contains(instrument_id);
}

std::uint64_t Cache::bookUpdateCount(
    const InstrumentId& instrument_id) const {
  const auto* book = orderBook(instrument_id);
  return book == nullptr ? 0 : book->updateCount();
}

PositionId Cache::makePositionId(const AccountId& account_id,
                                 const InstrumentId& instrument_id) {
  return account_id + ":" + instrument_id.toString();
}

void Cache::commitFill(OrderState order, Account account, Position position,
                       OrderFilled fill) {
  if (!orders_.contains(fill.client_order_id) || !accounts_.contains(account.id) ||
      fills_.contains(fill.trade_id) ||
      position.id != makePositionId(account.id, fill.instrument_id)) {
    throw std::logic_error("fill state change is inconsistent with cache");
  }

  const auto client_order_id = fill.client_order_id;
  const auto account_id = account.id;
  const auto position_id = position.id;
  const auto trade_id = fill.trade_id;
  const auto order_status = order.status;
  const bool position_exists = positions_.contains(position_id);

  // Complete every allocation before changing domain state
  fills_.reserve(fills_.size() + 1);
  positions_.reserve(positions_.size() + (position_exists ? 0 : 1));
  open_orders_.reserve(open_orders_.size() + 1);
  closed_orders_.reserve(closed_orders_.size() + 1);

  std::unordered_map<TradeId, OrderFilled> pending_fills;
  pending_fills.emplace(trade_id, std::move(fill));
  auto fill_node = pending_fills.extract(trade_id);

  std::unordered_map<PositionId, Position> pending_positions;
  if (!position_exists) {
    pending_positions.emplace(position_id, std::move(position));
  }

  // Node insertion and swaps are non-allocating after the reserves above
  fills_.insert(std::move(fill_node));
  if (position_exists) {
    using std::swap;
    swap(positions_.at(position_id), position);
  } else {
    positions_.insert(pending_positions.extract(position_id));
  }
  using std::swap;
  swap(orders_.at(client_order_id), order);
  swap(accounts_.at(account_id), account);
  inflight_orders_.erase(client_order_id);
  if (order_status == OrderStatus::Filled) {
    open_orders_.erase(client_order_id);
    closed_orders_.insert(client_order_id);
  } else {
    closed_orders_.erase(client_order_id);
    open_orders_.insert(client_order_id);
  }
}

std::vector<const OrderState*> Cache::ordersForIds(
    const OrderIdSet& ids) const {
  std::vector<const OrderState*> result;
  result.reserve(ids.size());
  for (const auto& id : ids) {
    result.push_back(&orders_.at(id));
  }
  return result;
}

void Cache::markOrderOpen(const ClientOrderId& client_order_id) {
  closed_orders_.erase(client_order_id);
  open_orders_.insert(client_order_id);
}

void Cache::markOrderClosed(const ClientOrderId& client_order_id) {
  inflight_orders_.erase(client_order_id);
  open_orders_.erase(client_order_id);
  closed_orders_.insert(client_order_id);
}

}  // namespace trading
