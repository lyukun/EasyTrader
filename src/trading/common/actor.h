#pragma once

#include <string_view>

namespace trading {

class Actor {
 public:
  virtual ~Actor() = default;
  virtual std::string_view id() const = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
};

}  // namespace trading
