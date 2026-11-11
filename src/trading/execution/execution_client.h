#pragma once

#include "trading/model/events.h"

namespace trading {

class ExecutionClient {
 public:
  virtual ~ExecutionClient() = default;
  virtual const ExecutionClientId& clientId() const = 0;
  virtual const AccountId& accountId() const = 0;
  virtual const Venue& venue() const = 0;
  virtual void submit(const SubmitOrder& order) = 0;
};

}  // namespace trading
