#include "trading/python/python_strategy.h"

#include <pybind11/embed.h>

#include <array>
#include <stdexcept>
#include <utility>

namespace trading {
namespace py = pybind11;

PythonStrategyContext::PythonStrategyContext(StrategyId strategy_id)
    : strategy_id_(std::move(strategy_id)) {}

StrategyContext& PythonStrategyContext::checkedContext() const {
  if (owner_ != std::this_thread::get_id()) {
    throw std::logic_error(
        "Python strategy context accessed outside the core thread");
  }
  if (context_ == nullptr) {
    throw std::logic_error("Python strategy context is no longer active");
  }
  return *context_;
}

void PythonStrategyContext::submitOrder(const SubmitOrder& order) const {
  auto& context = checkedContext();
  if (!submit_enabled_) {
    throw std::logic_error(
        "Python strategy cannot submit orders while stopping");
  }
  auto request = order;
  request.strategy_id = strategy_id_;
  context.submitOrder(request);
}

std::optional<QuoteTick> PythonStrategyContext::quote(
    const InstrumentId& instrument_id) const {
  const auto* value = checkedContext().cache().quote(instrument_id);
  return value ? std::optional<QuoteTick>(*value) : std::nullopt;
}

std::optional<OrderState> PythonStrategyContext::order(
    const ClientOrderId& client_order_id) const {
  const auto* value = checkedContext().cache().order(client_order_id);
  return value ? std::optional<OrderState>(*value) : std::nullopt;
}

std::optional<Position> PythonStrategyContext::position(
    const AccountId& account_id, const InstrumentId& instrument_id) const {
  const auto* value =
      checkedContext().cache().positionForInstrument(account_id, instrument_id);
  return value ? std::optional<Position>(*value) : std::nullopt;
}

struct PythonStrategy::Impl {
  enum Hook { Start, Stop, Quote, Accepted, Filled, Denied, Rejected, Count };
  static constexpr std::array<const char*, Count> kNames{
      "on_start",          "on_stop",         "on_quote",
      "on_order_accepted", "on_order_filled", "on_order_denied",
      "on_order_rejected"};

  py::object instance;
  std::array<py::object, Count> callbacks;
  std::shared_ptr<PythonStrategyContext> context;

  template <typename... Args>
  void invoke(Hook hook, Args&&... args) {
    py::gil_scoped_acquire acquire;
    if (callbacks[hook].is_none()) {
      return;
    }
    try {
      callbacks[hook](std::forward<Args>(args)...);
    } catch (const py::error_already_set& e) {
      throw std::runtime_error("Python strategy " + context->strategyId() +
                               " " + kNames[hook] + " failed:\n" + e.what());
    }
  }

  template <typename Event>
  void event(Hook hook, const Event& event) {
    // Each Python event owns a copy and may outlive the dispatch call
    invoke(hook, Event(event));
  }
};

PythonStrategy::PythonStrategy(std::shared_ptr<PythonRuntime> runtime,
                               StrategyId strategy_id,
                               const std::string& module_name,
                               const std::string& class_name)
    : runtime_(std::move(runtime)), strategy_id_(std::move(strategy_id)) {
  if (!runtime_ || strategy_id_.empty()) {
    throw std::invalid_argument(
        "Python strategy needs a runtime and a nonempty ID");
  }
  runtime_->assertOwner();
  py::gil_scoped_acquire acquire;
  // Keep failed construction cleanup inside the GIL scope
  auto impl = std::make_unique<Impl>();
  impl->context = std::make_shared<PythonStrategyContext>(strategy_id_);
  try {
    impl->instance =
        py::module_::import(module_name.c_str()).attr(class_name.c_str())();
    for (std::size_t i = 0; i < Impl::Count; ++i) {
      impl->callbacks[i] =
          py::getattr(impl->instance, Impl::kNames[i], py::none());
      if (!impl->callbacks[i].is_none() &&
          !PyCallable_Check(impl->callbacks[i].ptr())) {
        throw std::invalid_argument(std::string(Impl::kNames[i]) +
                                    " must be callable");
      }
    }
  } catch (const py::error_already_set& e) {
    throw std::runtime_error("Python strategy " + strategy_id_ +
                             " load failed:\n" + e.what());
  }
  impl_ = std::move(impl);
}

PythonStrategy::~PythonStrategy() {
  runtime_->assertOwner();
  // Contexts retained by Python become unusable before the kernel is destroyed
  impl_->context->context_ = nullptr;
  impl_->context->submit_enabled_ = false;
  py::gil_scoped_acquire acquire;
  impl_.reset();
}

void PythonStrategy::start() {
  runtime_->assertOwner();
  if (state_ != State::Ready) {
    throw std::logic_error("Python strategy can only be started once");
  }
  impl_->context->context_ = &context();
  impl_->context->submit_enabled_ = true;
  state_ = State::Running;

  subscribe<QuoteTick>([this](const auto& event) {
    if (state_ == State::Running) impl_->event(Impl::Quote, event);
  });
  subscribe<OrderAccepted>([this](const auto& event) {
    if (state_ == State::Running) impl_->event(Impl::Accepted, event);
  });
  subscribe<OrderFilled>([this](const auto& event) {
    if (state_ == State::Running) impl_->event(Impl::Filled, event);
  });
  subscribe<OrderDenied>([this](const auto& event) {
    if (state_ == State::Running) impl_->event(Impl::Denied, event);
  });
  subscribe<OrderRejected>([this](const auto& event) {
    if (state_ == State::Running) impl_->event(Impl::Rejected, event);
  });
  impl_->invoke(Impl::Start, impl_->context);
}

void PythonStrategy::stop() {
  runtime_->assertOwner();
  if (state_ != State::Running) {
    return;
  }
  state_ = State::Stopped;
  impl_->context->submit_enabled_ = false;
  try {
    impl_->invoke(Impl::Stop);
  } catch (...) {
    impl_->context->context_ = nullptr;
    throw;
  }
  impl_->context->context_ = nullptr;
}

}  // namespace trading
