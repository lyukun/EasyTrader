# Architecture

This document describes the components, runtime boundaries, and event flows of
the C++ trading core. The design is inspired by NautilusTrader, but the diagrams
below describe only the components and behavior implemented in this repository.

The editable Mermaid source is
[docs/architecture/architecture.mermaid.md](docs/architecture/architecture.mermaid.md).

## System boundary

One `TradingNode` instance is the live system boundary. It owns the adapter
executor, concrete adapters, and `LiveEventLoop`. Its `TradingKernel` owns the
shared cache, portfolio, engines, message bus, and trader. External adapters may
produce events concurrently, but all mutable trading state is processed serially
on the core thread.

Applications configure strategies and run the node:

```cpp
trading::TradingNode node;
node.addStrategy(std::make_unique<trading::OrderBookImbalanceStrategy>(config));
node.run();
```

The architecture follows five main rules:

1. `TradingKernel` is the common core and owns every shared trading component.
2. Adapters depend on `EventSink` and `ExecutionClient` ports, not core internals.
3. Every cross-thread event enters through `LiveEventLoop`.
4. Engines update state before publishing an event to strategies.
5. `MessageBus` callbacks run synchronously on the core thread.

## System architecture

![System architecture](docs/architecture/system-architecture.svg)

Like the upstream NautilusTrader diagram, this view shows logical engine
boundaries. In the live runtime, adapter events first cross `EventSink` and
`LiveEventLoop`, as shown in the threading diagram below.

The diagram uses the same top-level shape as NautilusTrader: adapters surround a
kernel containing the engines, portfolio, trader, cache, and messaging. The
arrows show logical dependencies, not separate worker threads. Every kernel
component and strategy callback runs on the same core thread.

## Component responsibilities

| Component | Responsibility | Does not own |
| --- | --- | --- |
| `TradingNode` | Live runtime, adapter composition, and lifecycle | Trading rules |
| `EventSink` | Adapter-facing port for runtime messages | Scheduling policy |
| `LiveEventLoop` | Unbounded MPSC ingress and batches sorted by priority | Domain validation |
| `TradingKernel` | Common-core ownership and typed runtime dispatch | Venue protocol logic |
| `DataEngine` | Validates data, updates the cache, then publishes | Portfolio accounting |
| `RiskEngine` | Pre-trade validation, trading state, inflight reservations | Venue acceptance |
| `ExecutionEngine` | Execution-client routing and order lifecycle handling | Account calculations |
| `TwapAlgorithm` | Exact child sizing, timer scheduling, primary progress | Venue protocol logic |
| `Scheduler` | Core-thread timed callbacks and cancellation | Trading decisions |
| `Portfolio` | Valuation, fill accounting, positions, PnL, and equity | Long-term persistence |
| `Cache` | In-memory state, indexes, histories, books, atomic fill commit | Accounting policy |
| `MessageBus` | Synchronous typed publish/subscribe on the owner thread | Cross-thread transport |
| `Trader` | Strategy registration, startup, shutdown, and context injection | Engine state |
| `PythonRuntime` | Embedded CPython lifetime and core-thread ownership | Trading rules |
| `PythonStrategy` | Python lifecycle and event callbacks through the strategy interface | Engine state |

## Threading and dispatch

![Threading and dispatch](docs/architecture/threading-and-dispatch.svg)

`FakeMarketDataClient` and `FakeExecutionClient` run work on
`folly::CPUThreadPoolExecutor`. They send domain events through `EventSink`
instead of mutating the cache or portfolio or calling strategy handlers.

`LiveEventLoop` takes up to 128 messages from its single MPSC queue per batch.
It stably sorts the batch by priority: trading-state changes first, order events
second, and market data last. Messages with equal priority retain their dequeue
order. Messages arriving while a batch executes wait for a later batch; priority
does not move them ahead of queued messages outside the current batch. Full
batches schedule a continuation through `EventBase`, allowing timers and other
callbacks to run between batches. Handler errors stop the loop and propagate
to its caller.

The ingress queue is an unbounded `folly::UMPSCQueue`. `send()` returns `true`
after enqueueing; sustained producer overload grows memory usage, and allocation
failures propagate to the caller.

`MessageBus` asserts thread ownership. Strategies call
`context().submitOrder(order)` synchronously on the core thread. The context
invokes `TradingKernel::submitOrder` directly, including routing, risk checks,
and the execution-client call. TWAP targets enter the algorithm synchronously;
its first child is immediate and later children run on core-thread timers.
Orders never enter the event queue; only incoming
market data, lifecycle events, and trading-state events use that queue.

Local route and risk denials publish `OrderDenied` before `submitOrder` returns.
Strategy request state must therefore be established before submission. Venue
acceptance and fills arrive asynchronously through the adapter event queue.

An order is correlated by `client_order_id`, so its submission, acceptance, and
fills may be produced by different executor tasks.

`TradingKernel` registers the portfolio and risk handlers before any strategy
can subscribe. Price events therefore update portfolio valuations, and order
events refresh risk reservations, before strategy callbacks observe them.

## Event routing overview

`RuntimeMessage` is a closed `std::variant`. The following diagram covers every variant
currently accepted by `TradingKernel::dispatch`.

![Event routing overview](docs/architecture/event-routing-overview.svg)

`OrderBookImbalanceSignal` is an internal `MessageBus` publication, not a
`RuntimeMessage` variant. It is produced synchronously by the example
strategy after it reads the updated public book.

## Market-data event flows

### Quote and trade ticks

![Quote and trade flow](docs/architecture/quote-and-trade-flow.svg)

State is current before publication. Long positions use the bid and short
positions use the ask. Without a quote, valuation falls back to the latest
trade and then the latest accepted bar close.

### Bars

![Bar flow](docs/architecture/bar-flow.svg)

### Public order books

![Public order book flow](docs/architecture/public-order-book-flow.svg)

Strategies declare the public books they require when they start:

```cpp
context().ensureOrderBook(
    trading::InstrumentId{"BTC-USD", "SIM"}, trading::BookType::L2Mbp);
```

`OrderBookDepth` atomically initializes or replaces the current book.
`OrderBookDeltas` applies `Add`, `Update`, `Delete`, and `Clear` operations.
Strategy callbacks read the updated book through the cache:

```cpp
const auto* book = kernel.cache().orderBook(
    trading::InstrumentId{"BTC-USD", "SIM"});
const auto best_bid = book->bestBidPrice();
const auto best_ask = book->bestAskPrice();
```

`BookSyncTracker` belongs to the adapter and maintains one instrument's stream
position, snapshot request ownership, and bounded recovery buffer. It uses
update ID ranges for MBP frames with absolute level sizes: `first_sequence`
comes from the wire frame and `OrderBookDeltas::sequence` is its final ID.
Venue-specific linkage rules belong in the adapter.

The tracker suppresses duplicate and stale updates, and claims a snapshot
request when a range leaves a gap. The adapter fetches and retries that request,
passing its `SnapshotRequestId` to `acceptSnapshot`. `cancelSnapshot` releases an
abandoned request so a later update can claim recovery again. `reset` clears
stream progress and invalidates earlier requests. The adapter must discard
frames from obsolete transport connections before passing them to the tracker.

A snapshot is accepted only when it bridges all buffered updates and covers
any updates evicted from the buffer. It becomes a snapshot-flagged `Clear`
followed by `Add` operations, with `kBookFlagLast` on the closing delta. An empty
snapshot is a single flagged `Clear`. Snapshot and replay writes share the
tracker lock, preventing concurrent ingress from inserting a live update
between them. `EventSink` must enqueue these writes without calling downstream
handlers synchronously. Empty wire updates advance sequencing without
publishing an empty domain batch.

`DataEngine` requires complete groups ending in `kBookFlagLast`, validates
snapshot group structure, and publishes each group after updating the cached
book. It does not enforce wire sequence continuity or fetch snapshots.
Recovery replay produces one callback per event group; the tracker's `Synced`
state describes adapter progress, not a core-thread acknowledgement or trading
pause. The fake market client uses this synchronization path for its initial
snapshot and normal updates.

## Order event flows

### Submit order

This path describes direct orders and each child submitted by TWAP. TWAP primary
orders enter the algorithm first, rather than being sent to an execution client.

![Submit order flow](docs/architecture/submit-order-flow.svg)

`SubmitOrder` is a request passed directly to the synchronous submission API;
it is not a `RuntimeMessage`. The call returns after local processing and client
submission, while venue acceptance and fills still arrive later as events.

The call returns `void`; invariant errors propagate as exceptions. Applications
composing a kernel can call `kernel.submitOrder(order)` on the core thread.
Calls from a different thread are rejected before cache mutation.

Routing priority is explicit `ExecutionClientId`, then the client registered for
the instrument venue, then the default client. The selected client determines
the account used by risk, accounting, and order indexes.

Each `ExecutionClient` exposes an `ExecutionClientId`, `AccountId`, and `Venue`.
`SubmitOrder` selects an optional execution client rather than carrying an
account ID:

```cpp
SubmitOrder order{
    "ORDER-1",
    InstrumentId{"BTC-USD", "SIM"},
    OrderSide::Buy,
    Quantity::fromString("1"),
    Price::fromString("100"),
    "strategy-1",
    ExecutionClientId{"SIM-EXEC-2"},
};
```

One strategy can select different accounts through their execution clients.
Multiple clients for the same venue require explicit selection unless one is
installed as that venue's route.

### Pre-trade risk

Orders enter the cache as `Initialized` before risk evaluation. Admitted orders
become `Submitted`; local validation failures produce `OrderDenied`.

`RiskEngineConfig::max_notional_per_order` stores quote-currency limits by
`InstrumentId`. Notional uses exact arithmetic over price, quantity, and the
instrument contract multiplier. Cash-account checks require the account
currency to match the instrument quote currency.

The engine reserves cash for open buys and position quantity for open sells.
Later orders therefore see settled state and unreflected inflight exposure.
Reservations follow leaves quantity after updates and partial fills, and are
released for filled, canceled, denied, rejected, or expired orders.

Cash accounts cannot open short positions. `Reducing` orders must not cross
through a position; margin accounts leave funding and leverage checks to a
future margin model.

### Accepted, updated, canceled, denied, rejected, and expired

![Order lifecycle flow](docs/architecture/order-lifecycle-flow.svg)

This shared path covers:

- `OrderAccepted`: submitted to accepted, records the venue order ID.
- `OrderUpdated`: changes quantity, price, and leaves quantity for an open order.
- `OrderCanceled`: moves an open order to canceled.
- `OrderDenied`: records a local risk failure for a cached order.
- `OrderRejected`: records an external venue or broker rejection.
- `OrderExpired`: moves an open order to expired.

Local route failure is the exception: because no execution client and account
can be resolved, `TradingKernel` publishes `OrderDenied` directly without adding
an order to the cache.

### Filled

`OrderFilled` has a stronger consistency boundary than other lifecycle events
because one fill changes the order, account, and position together.

![Fill flow](docs/architecture/fill-flow.svg)

All validation and allocation completes before the cache changes domain state.
If price, quantity, fee currency, balance, or arithmetic validation fails, the
order, account, position, and fill registry remain unchanged.

## Control event flows

### Trading state

![Trading state flow](docs/architecture/trading-state-flow.svg)

`Active` permits orders that pass normal validation. `Reducing` permits only
valid reduce-only orders. `Halted` denies every new order.

### Shutdown

![Shutdown flow](docs/architecture/shutdown-flow.svg)

Shutdown is a runtime lifecycle operation, not a domain event in
`RuntimeMessage`.

## Strategy lifecycle and internal signals

![Strategy lifecycle](docs/architecture/strategy-lifecycle.svg)

Strategies are registered only while the trader is `Ready`. Startup follows
registration order; shutdown and partial-start rollback use reverse order.
Strategies subscribe to typed `MessageBus` events during `start()` and may
publish internal signals synchronously.

`Trader` injects the shared strategy context and rejects duplicate strategy IDs.

The included order-book imbalance strategy follows this path:

![Order book imbalance signal flow](docs/architecture/order-book-imbalance-signal-flow.svg)

`OrderBookImbalanceStrategy` compares sizes at the best bid and ask. When the
configured ratio is crossed, it publishes `OrderBookImbalanceSignal` and submits
a FOK limit order: buy at the best ask when bids are larger, sell at the best
bid otherwise. Quantity is capped by the thinner side and `max_signal_size`.
Cooldown and this strategy's inflight orders suppress repeat triggers.
`submitCount()` includes requests denied by local risk. Signal subscribers
observe the decision; the strategy itself submits the order. This example does
not claim a trading edge.

## Python strategy execution

`PythonStrategy` implements the existing C++ `Strategy` interface. `Trader`
registers, starts, and stops it alongside native strategies. The bridge subscribes
to typed `MessageBus` events and calls a cached Python method for each event.
Python callbacks execute on the same core thread as the engines:

```text
DataEngine -> MessageBus -> PythonStrategy -> Python on_quote
Python context.submit_order -> StrategyContext -> TradingKernel::submitOrder
```

Applications construct a shared interpreter before creating Python strategies:

```cpp
auto runtime = std::make_shared<trading::PythonRuntime>("python/strategies");
trading::TradingNode node;
node.addStrategy(std::make_unique<trading::PythonStrategy>(
    runtime, "python-example", "example_strategy", "ExampleStrategy"));
node.run();
```

The constructor imports the module and instantiates its class without arguments.
Strategy IDs come from the C++ registration. Methods may be omitted or set to
`None`; other values must be callable:

| Python method | Arguments | Trigger |
| --- | --- | --- |
| `on_start` | `context` | Strategy startup, after event subscriptions are installed |
| `on_stop` | None | Shutdown or rollback after a partial start |
| `on_quote` | `QuoteTick` | Quote publication after cache and valuation updates |
| `on_order_accepted` | `OrderAccepted` | Venue acceptance after the order is updated |
| `on_order_filled` | `OrderFilled` | Fill publication after order and portfolio commit |
| `on_order_denied` | `OrderDenied` | Local route or risk denial |
| `on_order_rejected` | `OrderRejected` | Venue rejection |

Callbacks receive the same publications as native subscribers. A strategy can
filter events by instrument or client order ID. The first binding supports
quotes and these order events; trade ticks, bars, order books, and Python TWAP
configuration are outside this initial API.

The embedded `easytrader` module exposes `InstrumentId`, `Price`, `Quantity`,
`Decimal`, `Money`, `OrderSide`, `TimeInForce`, and a limit-order `SubmitOrder`.
Prices and quantities are constructed with `from_string`; Python `float` values
are not accepted. Fixed-point values expose exact `raw`, `precision`, string
formatting, and comparisons.

The context provides four operations:

- `submit_order(order)` copies the request, sets the registered strategy ID, and
  invokes synchronous C++ routing, risk checks, and execution-client submission.
- `quote(instrument_id)` returns the latest cached quote or `None`.
- `order(client_order_id)` returns the cached order state or `None`.
- `position(account_id, instrument_id)` returns the settled position or `None`.

Events and query results are read-only copies. Python can retain them after a
callback or kernel destruction without holding references into the cache.
The context itself checks core-thread ownership before touching engine state,
including for reads. Python background threads cannot query or submit through
it. Queries remain available during `on_stop`, but new submissions are disabled;
the context becomes invalid after stopping, even if Python retains it.

Local denial can reenter Python before `submit_order` returns, including from
`on_start`. Strategies must establish request state before submission. The
bridge holds no mutex across Python callbacks and retains the existing engine's
synchronous behavior.

`PythonRuntime` releases the GIL while C++ runs and waits. The bridge acquires it
for Python calls and Python object destruction. Strategies retain shared
ownership of the runtime, so the interpreter outlives their Python references.
Construction, strategy management, and final interpreter destruction must occur
on the core thread. Only one embedded interpreter can run at a time; an already
initialized external interpreter is rejected.

Python exceptions become C++ errors containing the strategy ID, hook name, and
traceback. Callback failures follow the existing event-loop stop and cleanup
path; start failures use `Trader` rollback. Long calculations or blocking I/O
inside a Python callback delay all other core-thread work. The embedded
interpreter provides no process isolation or callback timeout.

## Cache state and portfolio queries

Instruments and accounts are registered before processing market data or
orders. `TradingNode` installs the demo `BTC-USD.SIM` instrument, `SIM-001`
account, and `SIM-EXEC` execution client. Custom composition roots register
definitions and clients through `TradingKernel::addInstrument`, `addAccount`,
and `addExecutionClient`.

`InstrumentId` combines a `Symbol` and `Venue`. `InstrumentId{"BTC-USD", "SIM"}`
renders as `BTC-USD.SIM`, keeping identical symbols from different venues as
distinct cache keys.

Quote and trade histories use `CacheConfig::tick_capacity`; bar histories use
`bar_capacity`. Index zero is the newest value. A bar with the same type and
event timestamp replaces the current value. An older bar is ignored and not
published.

Orders retain filled and leaves quantities, average fill price, accumulated
fees, timestamps, venue IDs, and terminal reasons. Secondary indexes support
queries by instrument, strategy, account, venue order ID, and lifecycle state.
Positions retain signed quantity, average open price, net realized PnL, fees,
and open/close timestamps:

```cpp
const auto* account = kernel.cache().account("SIM-001");
const auto* position = kernel.cache().positionForInstrument(
    "SIM-001", trading::InstrumentId{"BTC-USD", "SIM"});
const auto recent_trades =
    kernel.cache().trades(trading::InstrumentId{"BTC-USD", "SIM"});
```

`Money` couples balances, fees, and PnL to a `Currency`. `AccountBalance`
separates total, free, and locked money, although current cash accounting keeps
locked money at zero. Risk reservations track inflight exposure separately.

`Portfolio` exposes `unrealizedPnl`, `realizedPnl`, `totalPnl`, `markValue`,
`netExposure`, and `equity` queries. Valuation follows the
[quote and trade flow](#quote-and-trade-ticks); the atomic fill boundary and
missing-price behavior are described in the
[state consistency guarantees](#state-consistency-guarantees).

## State consistency guarantees

- The core thread serializes all engine and strategy state transitions.
- Market data and books are updated before their events are published.
- Execution lifecycle state is updated before strategy callbacks run.
- A new fill commits order, account, position, and fill identity as one logical
  transition.
- Exact fill replay is idempotent; conflicting reuse of a trade ID is rejected.
- Missing valuation prices produce `std::nullopt`, not a zero valuation.
- Prices, quantities, money, PnL, and fees use fixed-point arithmetic.
- `Money` rejects arithmetic across different currencies.

These guarantees are in-memory guarantees. The current core has no durable
cache backing, event store, startup reconciliation, or external message-bus
backing.

## Current scope compared with NautilusTrader

The component boundaries intentionally resemble NautilusTrader, but this core
implements a smaller surface:

- One serialized runtime context, with no separate backtest/live kernels.
- Typed in-process publish/subscribe only; no request/response or external bus.
- In-memory cache only; no database persistence or restart reconstruction.
- Public order books only; no own-order book.
- Net positions only; no hedging OMS or multiple position IDs per instrument.
- Cash-style linear accounting only; no FX conversion, derivative settlement,
  inverse/quanto PnL, margin, leverage, or liquidation model.
- No execution reconciliation, emulation, or throttling.
- One limit-order TWAP algorithm; no market-order pricing or automatic child retries.

Unsupported behavior should be rejected explicitly rather than approximated by
the implemented cash and linear-product rules.

## TWAP execution

The kernel owns an optional `TwapAlgorithm` when a scheduler is configured.
`TradingNode` supplies `LiveEventLoop` as the scheduler. Algorithm child orders
call the existing synchronous routing/risk/execution path directly.

![TWAP execution flow](docs/architecture/twap-execution-flow.svg)

Strategies select `exec_algorithm_id = "TWAP"` with typed time parameters:

```cpp
SubmitOrder order{
    "TARGET-1", InstrumentId{"BTC-USD", "SIM"}, OrderSide::Buy,
    Quantity::fromString("3"), Price::fromString("100.05"), "my-strategy"};
order.time_in_force = TimeInForce::Ioc;
order.exec_algorithm_id = "TWAP";
order.twap = TwapConfig{
    std::chrono::milliseconds(300), std::chrono::milliseconds(100)};
context().submitOrder(order);
```

This target submits its first child immediately, followed by two children
100 ms apart. Every child passes through client routing and risk checks.

The primary's `TwapExecutionState` is separate from cached venue orders. Child
IDs use the primary ID plus `-E1`, `-E2`, etc.; each child records `exec_spawn_id`.
Primary and planned child IDs remain reserved for the algorithm instance's
lifetime. Children retain the primary limit price, side, strategy ID,
execution-client selection, and reduce-only flag.

Query progress with `kernel.twapExecution("TARGET-1")` or
`node.twapExecution("TARGET-1")`. `submitted_quantity` counts child requests,
including local denials; `filled_quantity` sums actual cached fills. `Completed`
means the target quantity was filled.

Timing and quantity rules:

- Horizon and interval are positive integer milliseconds, with horizon at least
  interval. Requested slice count is `floor(horizon / interval)`.
- Each child is a positive multiple of instrument lot size. Remainder lots go
  to the earliest children; planned quantities sum exactly to the target.
- Slice count is reduced when required by minimum child quantity. Every child
  must also satisfy the instrument maximum.
- Limit orders require IOC or FOK. Market orders and resting GTC orders are
  rejected; NautilusTrader's native TWAP supports market orders.
- Each instance accepts at most 1024 requested slices and 1024 retained
  executions. Live timer intervals fit a 32-bit unsigned millisecond duration.

A denial or an unfilled terminal child stops subsequent submissions and marks
the primary execution `Failed`. A changed child quantity or limit price also
stops the schedule. Automatic retries and restoration of unfilled child quantity
are not implemented. Progress continues to reflect fills of already submitted
children after a stop or failure.

`cancelTwap` cancels future scheduling, not venue orders. Runtime shutdown stops
algorithm timers and prevents new targets. A delayed timer sends one child,
then waits another interval; it does not issue a catch-up burst. The horizon
selects the slice count, not a guaranteed wall-clock completion deadline.

`TradingKernel(client, risk_config, &scheduler)` accepts a custom scheduler;
without one, TWAP requests are denied. `LiveEventLoop` implements the scheduler
with Folly timers. The deterministic test scheduler advances time explicitly.
Timer errors stop the live loop and propagate to its caller.

`TwapExampleStrategy` submits one target on its first quote. `twap_demo` uses
quantity 3, horizon 300 ms, and interval 100 ms, checking three filled children
and final position 3.

Upstream NautilusTrader includes an [EMACrossTWAP test strategy][upstream-twap].
Its backtest acceptance test registers `TwapAlgorithm` and supplies horizon and
interval parameters. The [execution algorithm guide][upstream-algorithms]
describes its primary/spawned order model.

[upstream-twap]: https://github.com/nautechsystems/nautilus_trader/blob/develop/python/tests/strategies/ema_cross_twap.py
[upstream-algorithms]: https://github.com/nautechsystems/nautilus_trader/blob/develop/docs/concepts/execution/algorithms.md
