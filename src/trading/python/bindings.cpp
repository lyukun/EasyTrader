#include <pybind11/embed.h>
#include <pybind11/operators.h>
#include <pybind11/stl.h>

#include <utility>

#include "trading/python/python_strategy.h"

namespace trading {
namespace py = pybind11;
namespace {

template <typename T>
void bindFixedPoint(py::module_& module, const char* name) {
  py::class_<T>(module, name)
      .def_static("from_string", &T::fromString)
      .def_property_readonly("raw", &T::raw)
      .def_property_readonly("precision", &T::precision)
      .def("to_string", &T::toString)
      .def("__str__", &T::toString)
      .def(py::self == py::self)
      .def(py::self != py::self)
      .def(py::self < py::self)
      .def(py::self <= py::self)
      .def(py::self > py::self)
      .def(py::self >= py::self);
}

void bindModule(py::module_& module) {
  module.doc() = "Embedded EasyTrader strategy API";
  bindFixedPoint<Price>(module, "Price");
  bindFixedPoint<Quantity>(module, "Quantity");
  bindFixedPoint<Decimal>(module, "Decimal");

  py::class_<Money>(module, "Money")
      .def_static("from_string", &Money::fromString)
      .def_property_readonly("amount",
                             [](const Money& value) { return value.amount(); })
      .def_property_readonly("currency", &Money::currency)
      .def("__str__", &Money::toString)
      .def(py::self == py::self);

  py::class_<InstrumentId>(module, "InstrumentId")
      .def(py::init<Symbol, Venue>(), py::arg("symbol"), py::arg("venue"))
      .def_readonly("symbol", &InstrumentId::symbol)
      .def_readonly("venue", &InstrumentId::venue)
      .def("__str__", &InstrumentId::toString)
      .def(py::self == py::self);

  py::enum_<OrderSide>(module, "OrderSide")
      .value("BUY", OrderSide::Buy)
      .value("SELL", OrderSide::Sell);
  py::enum_<TimeInForce>(module, "TimeInForce")
      .value("GTC", TimeInForce::Gtc)
      .value("IOC", TimeInForce::Ioc)
      .value("FOK", TimeInForce::Fok);
  py::enum_<OrderStatus>(module, "OrderStatus")
      .value("INITIALIZED", OrderStatus::Initialized)
      .value("SUBMITTED", OrderStatus::Submitted)
      .value("ACCEPTED", OrderStatus::Accepted)
      .value("PARTIALLY_FILLED", OrderStatus::PartiallyFilled)
      .value("FILLED", OrderStatus::Filled)
      .value("PENDING_UPDATE", OrderStatus::PendingUpdate)
      .value("PENDING_CANCEL", OrderStatus::PendingCancel)
      .value("CANCELED", OrderStatus::Canceled)
      .value("DENIED", OrderStatus::Denied)
      .value("REJECTED", OrderStatus::Rejected)
      .value("EXPIRED", OrderStatus::Expired);

  py::class_<SubmitOrder>(module, "SubmitOrder")
      .def(
          py::init([](ClientOrderId id, InstrumentId instrument, OrderSide side,
                      Quantity quantity, Price price, TimeInForce time_in_force,
                      std::optional<ExecutionClientId> execution_client_id,
                      bool reduce_only) {
            SubmitOrder order{std::move(id), std::move(instrument), side,
                              quantity, price};
            order.time_in_force = time_in_force;
            order.execution_client_id = std::move(execution_client_id);
            order.reduce_only = reduce_only;
            return order;
          }),
          py::arg("client_order_id"), py::arg("instrument_id"), py::arg("side"),
          py::arg("quantity"), py::arg("price"),
          py::arg("time_in_force") = TimeInForce::Gtc,
          py::arg("execution_client_id") = py::none(),
          py::arg("reduce_only") = false)
      .def_readonly("client_order_id", &SubmitOrder::client_order_id)
      .def_readonly("instrument_id", &SubmitOrder::instrument_id)
      .def_readonly("side", &SubmitOrder::side)
      .def_readonly("quantity", &SubmitOrder::quantity)
      .def_readonly("price", &SubmitOrder::limit_price)
      .def_readonly("strategy_id", &SubmitOrder::strategy_id)
      .def_readonly("execution_client_id", &SubmitOrder::execution_client_id)
      .def_readonly("time_in_force", &SubmitOrder::time_in_force)
      .def_readonly("reduce_only", &SubmitOrder::reduce_only);

  py::class_<QuoteTick>(module, "QuoteTick")
      .def_readonly("instrument_id", &QuoteTick::instrument_id)
      .def_readonly("bid", &QuoteTick::bid)
      .def_readonly("ask", &QuoteTick::ask)
      .def_readonly("bid_size", &QuoteTick::bid_size)
      .def_readonly("ask_size", &QuoteTick::ask_size)
      .def_readonly("sequence", &QuoteTick::sequence)
      .def_readonly("ts_event", &QuoteTick::ts_event);

  py::class_<OrderAccepted>(module, "OrderAccepted")
      .def_readonly("client_order_id", &OrderAccepted::client_order_id)
      .def_readonly("venue_order_id", &OrderAccepted::venue_order_id)
      .def_readonly("ts_event", &OrderAccepted::ts_event);
  py::class_<OrderFilled>(module, "OrderFilled")
      .def_readonly("client_order_id", &OrderFilled::client_order_id)
      .def_readonly("venue_order_id", &OrderFilled::venue_order_id)
      .def_readonly("instrument_id", &OrderFilled::instrument_id)
      .def_readonly("side", &OrderFilled::side)
      .def_readonly("quantity", &OrderFilled::quantity)
      .def_readonly("price", &OrderFilled::price)
      .def_readonly("trade_id", &OrderFilled::trade_id)
      .def_readonly("fee", &OrderFilled::fee)
      .def_readonly("ts_event", &OrderFilled::ts_event);
  py::class_<OrderDenied>(module, "OrderDenied")
      .def_readonly("client_order_id", &OrderDenied::client_order_id)
      .def_readonly("reason", &OrderDenied::reason)
      .def_readonly("ts_event", &OrderDenied::ts_event);
  py::class_<OrderRejected>(module, "OrderRejected")
      .def_readonly("client_order_id", &OrderRejected::client_order_id)
      .def_readonly("reason", &OrderRejected::reason)
      .def_readonly("ts_event", &OrderRejected::ts_event);

  py::class_<OrderState>(module, "OrderState")
      .def_readonly("order", &OrderState::order)
      .def_readonly("account_id", &OrderState::account_id)
      .def_readonly("execution_client_id", &OrderState::execution_client_id)
      .def_readonly("status", &OrderState::status)
      .def_readonly("venue_order_id", &OrderState::venue_order_id)
      .def_readonly("filled_quantity", &OrderState::filled_quantity)
      .def_readonly("leaves_quantity", &OrderState::leaves_quantity)
      .def_readonly("average_price", &OrderState::average_price)
      .def_readonly("fees", &OrderState::fees)
      .def_readonly("last_reason", &OrderState::last_reason);
  py::class_<Position>(module, "Position")
      .def_readonly("id", &Position::id)
      .def_readonly("account_id", &Position::account_id)
      .def_readonly("instrument_id", &Position::instrument_id)
      .def_property_readonly("quantity", &Position::signedQuantity)
      .def_readonly("average_open_price", &Position::average_open_price)
      .def_readonly("realized_pnl", &Position::realized_pnl)
      .def_readonly("fees", &Position::fees);

  py::class_<PythonStrategyContext, std::shared_ptr<PythonStrategyContext>>(
      module, "StrategyContext")
      .def_property_readonly("strategy_id", &PythonStrategyContext::strategyId)
      .def("submit_order", &PythonStrategyContext::submitOrder)
      .def("quote", &PythonStrategyContext::quote)
      .def("order", &PythonStrategyContext::order)
      .def("position", &PythonStrategyContext::position);
}

}  // namespace

void registerPythonBindings() {}

}  // namespace trading

PYBIND11_EMBEDDED_MODULE(easytrader, module) { trading::bindModule(module); }
