#pragma once

#include "trading/model/events.h"

namespace trading {

class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual bool send(RuntimeMessage message) = 0;
};

}  // namespace trading
