#pragma once

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <atomic>
#include <chrono>
#include <utility>

#include "trading/common/event_sink.h"
#include "trading/execution/execution_client.h"

namespace trading {

class FakeMarketDataClient {
 public:
  FakeMarketDataClient(EventSink& event_sink,
                       folly::CPUThreadPoolExecutor& executor)
      : event_sink_(event_sink), executor_(executor) {}

  void start(std::size_t ticks, std::chrono::milliseconds interval =
                                    std::chrono::milliseconds(50));
  void stop() { running_.store(false); }

 private:
  EventSink& event_sink_;
  folly::CPUThreadPoolExecutor& executor_;
  std::atomic<bool> running_{false};
};

class FakeExecutionClient : public ExecutionClient {
 public:
  FakeExecutionClient(EventSink& event_sink,
                      folly::CPUThreadPoolExecutor& executor,
                      ExecutionClientId client_id = "SIM-EXEC",
                      AccountId account_id = "SIM-001", Venue venue = "SIM")
      : event_sink_(event_sink),
        executor_(executor),
        client_id_(std::move(client_id)),
        account_id_(std::move(account_id)),
        venue_(std::move(venue)) {}

  const ExecutionClientId& clientId() const override { return client_id_; }
  const AccountId& accountId() const override { return account_id_; }
  const Venue& venue() const override { return venue_; }
  void submit(const SubmitOrder& order) override;

 private:
  EventSink& event_sink_;
  folly::CPUThreadPoolExecutor& executor_;
  ExecutionClientId client_id_;
  AccountId account_id_;
  Venue venue_;
};

}  // namespace trading
