from easytrader import OrderSide, Quantity, SubmitOrder


class ExampleStrategy:
    def __init__(self):
        self.submitted = False
        self.context = None

    def on_start(self, context):
        self.context = context
        print(f"[python] started {context.strategy_id}", flush=True)

    def on_quote(self, quote):
        if self.submitted:
            return
        self.submitted = True
        self.context.submit_order(
            SubmitOrder(
                client_order_id="PY-1",
                instrument_id=quote.instrument_id,
                side=OrderSide.BUY,
                quantity=Quantity.from_string("1"),
                price=quote.ask,
            )
        )

    def on_order_accepted(self, event):
        if event.client_order_id == "PY-1":
            print(f"[python] accepted {event.venue_order_id}", flush=True)

    def on_order_filled(self, event):
        if event.client_order_id == "PY-1":
            position = self.context.position("SIM-001", event.instrument_id)
            print(f"[python] filled {event.quantity}, position={position.quantity}", flush=True)

    def on_order_denied(self, event):
        if event.client_order_id == "PY-1":
            print(f"[python] denied {event.reason}", flush=True)

    def on_order_rejected(self, event):
        if event.client_order_id == "PY-1":
            print(f"[python] rejected {event.reason}", flush=True)

    def on_stop(self):
        print("[python] stopped", flush=True)
