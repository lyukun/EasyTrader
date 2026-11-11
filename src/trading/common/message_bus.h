#pragma once

#include <functional>
#include <stdexcept>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace trading {

class MessageBus {
 public:
  MessageBus() : owner_(std::this_thread::get_id()) {}

  template <typename T>
  void subscribe(std::function<void(const T&)> handler) {
    assertOwner();
    handlers_[std::type_index(typeid(T))].push_back(
        [handler = std::move(handler)](const void* message) {
          handler(*static_cast<const T*>(message));
        });
  }

  template <typename T>
  void publish(const T& message) const {
    assertOwner();
    const auto it = handlers_.find(std::type_index(typeid(T)));
    if (it == handlers_.end()) {
      return;
    }
    for (const auto& handler : it->second) {
      handler(&message);
    }
  }

  void assertOwner() const {
    if (std::this_thread::get_id() != owner_) {
      throw std::logic_error("MessageBus accessed outside the core thread");
    }
  }

 private:
  using ErasedHandler = std::function<void(const void*)>;

  std::thread::id owner_;
  std::unordered_map<std::type_index, std::vector<ErasedHandler>> handlers_;
};

}  // namespace trading
