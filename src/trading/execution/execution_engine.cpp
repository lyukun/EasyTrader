#include "trading/execution/execution_engine.h"

#include <stdexcept>

namespace trading {

ExecutionEngine::ExecutionEngine(Cache& cache, Portfolio& portfolio,
                                 MessageBus& message_bus,
                                 ExecutionClient& default_client)
    : cache_(cache), portfolio_(portfolio), message_bus_(message_bus) {
  registerClient(default_client, true);
  default_client_id_ = default_client.clientId();
}

void ExecutionEngine::registerClient(ExecutionClient& client,
                                     bool route_for_venue) {
  if (client.clientId().empty() || client.accountId().empty() ||
      client.venue().empty()) {
    throw std::invalid_argument("execution client identity is invalid");
  }
  if (clients_.contains(client.clientId())) {
    throw std::invalid_argument("execution client ID already exists");
  }
  clients_.emplace(client.clientId(), &client);
  if (route_for_venue) {
    venue_routes_.insert_or_assign(client.venue(), client.clientId());
  }
}

std::optional<ExecutionRoute> ExecutionEngine::route(
    const SubmitOrder& order) const {
  ExecutionClientId client_id;
  if (order.execution_client_id) {
    client_id = *order.execution_client_id;
  } else if (const auto venue = venue_routes_.find(order.instrument_id.venue);
             venue != venue_routes_.end()) {
    client_id = venue->second;
  } else {
    client_id = default_client_id_;
  }

  const auto client = clients_.find(client_id);
  if (client == clients_.end() ||
      client->second->venue() != order.instrument_id.venue) {
    return std::nullopt;
  }
  return ExecutionRoute{client_id, client->second->accountId()};
}

void ExecutionEngine::submit(const SubmitOrder& order,
                             const ExecutionRoute& route) {
  const auto client = clients_.find(route.client_id);
  if (client == clients_.end() ||
      client->second->accountId() != route.account_id) {
    throw std::logic_error("execution route is no longer valid");
  }
  cache_.submitOrder(order.client_order_id, order.ts_init);
  // 通过 client 的线程池去提交
  client->second->submit(order);
}

// execution engine 是最先收到回执的，需要先更新 cache 然后 publish event
void ExecutionEngine::process(const OrderAccepted& event) {
  cache_.acceptOrder(event);
  message_bus_.publish(event);
}

void ExecutionEngine::process(const OrderUpdated& event) {
  cache_.updateOrder(event);
  message_bus_.publish(event);
}

void ExecutionEngine::process(const OrderFilled& event) {
  if (portfolio_.apply(event)) {
    message_bus_.publish(event);
  }
}

void ExecutionEngine::process(const OrderCanceled& event) {
  cache_.cancelOrder(event);
  message_bus_.publish(event);
}

void ExecutionEngine::process(const OrderDenied& event) {
  cache_.denyOrder(event);
  message_bus_.publish(event);
}

void ExecutionEngine::process(const OrderRejected& event) {
  cache_.rejectOrder(event);
  message_bus_.publish(event);
}

void ExecutionEngine::process(const OrderExpired& event) {
  cache_.expireOrder(event);
  message_bus_.publish(event);
}

}  // namespace trading
