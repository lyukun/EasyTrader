import threading

from easytrader import Decimal, InstrumentId, OrderSide, OrderStatus, Price, Quantity, SubmitOrder

instances = {}


class Probe:
    execution_client_id = None

    def __init__(self):
        instances[type(self).__name__] = self
        self.calls = []
        self.quotes = []
        self.context = None
        self.in_submit = False
        self.denied_during_submit = False
        self.stop_count = 0

    def on_start(self, context):
        self.context = context
        self.thread = threading.get_ident()
        self.calls.append("start")

    def on_quote(self, quote):
        assert threading.get_ident() == self.thread
        assert self.context.quote(quote.instrument_id).sequence == quote.sequence
        assert self.context.quote(InstrumentId("MISSING", "SIM")) is None
        self.quotes.append(quote)
        self.calls.append("quote")

    def request(self, quote):
        self.in_submit = True
        self.context.submit_order(
            SubmitOrder(
                self.context.strategy_id + "-1",
                quote.instrument_id,
                OrderSide.BUY,
                Quantity.from_string("1"),
                quote.ask,
                execution_client_id=self.execution_client_id,
            )
        )
        self.in_submit = False
        self.calls.append("returned")

    def on_order_accepted(self, event):
        self.accepted = event
        self.accepted_order = self.context.order(event.client_order_id)
        assert self.accepted_order.status == OrderStatus.ACCEPTED
        self.calls.append("accepted")

    def on_order_filled(self, event):
        self.fill = event
        assert self.context.order(event.client_order_id).status == OrderStatus.FILLED
        self.position = self.context.position("SIM-001", event.instrument_id)
        self.position_raw = self.position.quantity.raw
        self.calls.append("filled")

    def on_order_denied(self, event):
        assert threading.get_ident() == self.thread
        self.denied_during_submit = self.in_submit
        self.denial = event
        self.denied_order = self.context.order(event.client_order_id)
        self.calls.append("denied")

    def on_order_rejected(self, event):
        self.rejection = event
        assert self.context.order(event.client_order_id).status == OrderStatus.REJECTED
        self.calls.append("rejected")

    def on_stop(self):
        self.stop_count += 1
        self.calls.append("stop")
        self.context.order("MISSING")
        try:
            self.context.submit_order(
                SubmitOrder(
                    "STOP",
                    InstrumentId("BTC-USD", "SIM"),
                    OrderSide.BUY,
                    Quantity.from_string("1"),
                    Price.from_string("100"),
                )
            )
        except RuntimeError:
            self.stopping_submit_rejected = True
        else:
            raise AssertionError("submission was allowed during stop")

    def check_inactive(self):
        instrument = InstrumentId("BTC-USD", "SIM")
        for action in (
            lambda: self.context.quote(instrument),
            lambda: self.context.order("MISSING"),
            lambda: self.context.position("SIM-001", instrument),
            lambda: self.context.submit_order(
                SubmitOrder(
                    "LATE",
                    instrument,
                    OrderSide.BUY,
                    Quantity.from_string("1"),
                    Price.from_string("100"),
                )
            ),
        ):
            try:
                action()
            except RuntimeError as e:
                assert "no longer active" in str(e)
            else:
                raise AssertionError("stopped context remained usable")

    def check_snapshots(self):
        assert str(self.quotes[0].ask) == "100.05"
        assert str(self.quotes[-1].ask) == "101.05"
        assert self.accepted_order.status == OrderStatus.ACCEPTED
        assert self.accepted_order.filled_quantity.raw == 0
        assert self.position.quantity.raw == 1_000_000_000
        assert self.fill.quantity.raw == 1_000_000_000
        try:
            self.quotes[0].ask = Price.from_string("0")
        except AttributeError:
            pass
        else:
            raise AssertionError("event snapshot was writable")


class SubmitOnQuote(Probe):
    def on_quote(self, quote):
        super().on_quote(quote)
        if len(self.quotes) == 1:
            self.request(quote)


class MissingRoute(SubmitOnQuote):
    execution_client_id = "MISSING"


class SubmitOnStart(Probe):
    def on_start(self, context):
        super().on_start(context)
        self.in_submit = True
        context.submit_order(
            SubmitOrder(
                context.strategy_id + "-1",
                InstrumentId("BTC-USD", "SIM"),
                OrderSide.BUY,
                Quantity.from_string("1"),
                Price.from_string("100"),
            )
        )
        self.in_submit = False
        self.calls.append("returned")


class ThreadProbe(Probe):
    def on_quote(self, quote):
        super().on_quote(quote)
        self.thread_errors = []

        def worker():
            for action in (
                lambda: self.context.quote(quote.instrument_id),
                lambda: self.context.order("MISSING"),
                lambda: self.context.position("SIM-001", quote.instrument_id),
                lambda: self.context.submit_order(
                    SubmitOrder(
                        "THREAD",
                        quote.instrument_id,
                        OrderSide.BUY,
                        Quantity.from_string("1"),
                        quote.ask,
                    )
                ),
            ):
                try:
                    action()
                except RuntimeError as e:
                    self.thread_errors.append(str(e))

        thread = threading.Thread(target=worker)
        thread.start()
        thread.join(timeout=2)
        assert not thread.is_alive()
        assert len(self.thread_errors) == 4
        assert all("outside the core thread" in e for e in self.thread_errors)


class QuoteFailure(Probe):
    def on_quote(self, quote):
        raise ValueError("intentional Python quote failure")


class StartFailure(Probe):
    def on_start(self, context):
        super().on_start(context)
        raise ValueError("intentional Python start failure")


class StopFailure(Probe):
    def on_stop(self):
        super().on_stop()
        raise ValueError("intentional Python stop failure")


class InvalidHook:
    on_quote = 123


class ConstructorFailure:
    def __init__(self):
        raise ValueError("intentional Python constructor failure")


def check_types():
    for cls in (Price, Quantity, Decimal):
        value = cls.from_string("9007199.254740993")
        assert value.raw == 9_007_199_254_740_993
        assert str(value) == "9007199.254740993"
        assert value > cls.from_string("9007199.254740992")
        try:
            cls.from_string(0.1)
        except TypeError:
            pass
        else:
            raise AssertionError("float silently entered fixed-point parsing")
    assert Decimal.from_string("-0.000000001").raw == -1
    try:
        Quantity.from_string("0.0000000001")
    except IndexError:
        pass
    else:
        raise AssertionError("excess precision was silently rounded")
