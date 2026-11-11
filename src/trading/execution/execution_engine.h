#pragma once

#include <optional>
#include <unordered_map>

#include "trading/cache/cache.h"
#include "trading/common/message_bus.h"
#include "trading/execution/execution_client.h"
#include "trading/portfolio/portfolio.h"

namespace trading {

struct ExecutionRoute {
  ExecutionClientId client_id;
  AccountId account_id;
};

class ExecutionEngine {
 public:
  ExecutionEngine(Cache& cache, Portfolio& portfolio, MessageBus& message_bus,
                  ExecutionClient& default_client);

  void registerClient(ExecutionClient& client, bool route_for_venue = false);
  std::optional<ExecutionRoute> route(const SubmitOrder& order) const;
  void submit(const SubmitOrder& order, const ExecutionRoute& route);
  void process(const OrderAccepted& event);
  void process(const OrderUpdated& event);
  void process(const OrderFilled& event);
  void process(const OrderCanceled& event);
  void process(const OrderDenied& event);
  void process(const OrderRejected& event);
  void process(const OrderExpired& event);

 private:
  Cache& cache_;
  Portfolio& portfolio_;
  MessageBus& message_bus_;
  std::unordered_map<ExecutionClientId, ExecutionClient*> clients_;
  std::unordered_map<Venue, ExecutionClientId> venue_routes_;
  ExecutionClientId default_client_id_;
};

}  // namespace trading
