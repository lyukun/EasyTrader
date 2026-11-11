# EasyTrader

See [architecture.md](architecture.md) for component boundaries, threading,
and event-by-event flow diagrams.

A small live-trading architecture inspired by NautilusTrader. It demonstrates:

- a `TradingKernel` which owns the shared engines, state, messaging, and trader;
- ports-and-adapters boundaries for runtime ingress and execution clients;
- one core `folly::EventBase` thread for all mutable trading state;
- typed, synchronous `MessageBus` dispatch inside the core thread;
- an `Actor` lifecycle abstraction;
- multiple asynchronous producers feeding one consumer through
  an unbounded `folly::UMPSCQueue`;
- application logging through `spdlog` with configurable levels;
- fake market data and execution clients running on a Folly CPU executor;
- cache-first market data handling and risk-before-execution order handling;
- exact fixed-point prices and quantities with up to nine decimal places;
- cached instrument definitions and account state used by risk checks;
- bounded, newest-first quote, trade, and bar histories;
- indexed order lifecycle state from submission through terminal events;
- currency-aware balances, net positions, fees, and realized PnL by account
  and instrument;
- quote-driven unrealized PnL, mark value, net exposure, and account equity;
- managed public order books for L1 MBP, L2 MBP, and L3 MBO data;
- atomic depth snapshots, incremental book deltas, and cache-backed strategy
  access;
- an order-book imbalance strategy which submits FOK limit orders synchronously
  and publishes signals for observation;
- a separate TWAP algorithm with exact lot-size allocation and core-thread timers.

## Modules

All headers and implementation files live together under `src/trading`.

- `model`: exact value types, domain events, commands, and public order books.
- `common`: actor, message bus, event-sink port, and live event-loop primitives.
- `cache`: instrument, market-data, order, account, position, and public-book
  state.
- `data`, `risk`, `execution`, `portfolio`: trading domain engines; `execution`
  also contains the independent TWAP algorithm.
- `system`: the common `TradingKernel`, strategy manager, and live `TradingNode`.
- `trading`: strategy actors, including the order-book imbalance example.
- `adapters`: fake external clients and adapter-owned order-book synchronization.

## Build

Dependencies are installed with Homebrew:

```bash
brew install folly spdlog python@3.14 ninja
```

Link the installed static libraries into the workspace:

```bash
mkdir -p third_party/folly/lib third_party/spdlog/lib third_party/fmt/lib
ln -s "$(brew --prefix folly)/lib/libfolly.a" third_party/folly/lib/libfolly.a
ln -s "$(brew --prefix spdlog)/lib/libspdlog.a" third_party/spdlog/lib/libspdlog.a
ln -s "$(brew --prefix fmt)/lib/libfmt.a" third_party/fmt/lib/libfmt.a
```

Blade is installed from its official repository:

```bash
git clone https://github.com/blade-build/blade-build.git
./blade-build/install
```

Ensure its Python is visible:

```bash
export PATH="/opt/homebrew/opt/python@3.14/libexec/bin:$HOME/bin:$PATH"
```

Then build, test, and run:

```bash
blade build //...
blade run //:core_test
blade run //:book_sync_test
blade run //:trading_demo
blade run //:twap_test
blade run //:twap_demo
```

The build enables `-Wall -Wextra -Wpedantic -Werror`. Homebrew headers are
treated as system headers. Folly, compiled spdlog, and its external fmt dependency
are declared as Blade prebuilt static libraries. The unbounded queue references `glog` symbols,
which are linked through `BLADE_ROOT`; Blade's declared-dependency check reports
these symbols as warnings on this macOS setup.

## Logging

Application logs use [spdlog](https://github.com/gabime/spdlog). The examples
configure synchronous console output with timestamps, severity, and thread IDs.
The default level is `info`: order acceptances, fills, signals, and final positions
are visible; quote updates use `debug`, and order denials and rejections use `warn`.
Prices, quantities, and positions are formatted through their exact `toString()`
representations.

Set `SPDLOG_LEVEL` when launching an example:

```bash
SPDLOG_LEVEL=debug blade run //:trading_demo
SPDLOG_LEVEL=warn blade run //:twap_demo
```

Applications embedding the core configure spdlog themselves. Folly's internal
`glog` dependency remains linked separately from application logging.

## Thread model

`FakeMarketDataClient` and `FakeExecutionClient` run work on
`folly::CPUThreadPoolExecutor`. They never mutate `Cache`, `Portfolio`, or call
strategy handlers directly. Instead, they send domain events through
the `EventSink` port implemented by `LiveEventLoop`.

`LiveEventLoop` uses an unbounded `folly::UMPSCQueue` as the cross-thread ingress and wakes
the single `folly::EventBase` core thread. Each drain takes up to 128 messages
and stably sorts them: trading-state changes first, order events second, and
market data last. Equal-priority messages keep their dequeue order. Messages
arriving during execution wait for a later batch. Full batches yield to
`EventBase` before continuing, so timers can run while ingress remains busy.
`TradingKernel` dispatches each message to engines and `MessageBus` serially.

`send()` returns `true` after enqueueing; there is no queue-full rejection.
The queue allocates storage as needed, so sustained producer overload grows
memory usage. Allocation failures propagate to the caller.

An order is correlated by `client_order_id`; it is not tied to one executor
thread. Submission, acceptance, and fills may be produced by different tasks.

## Synchronous order submission

Strategies call `context().submitOrder(order)` directly on the core thread.
The context invokes `TradingKernel::submitOrder` synchronously: it resolves the
execution client, adds the order to the cache, checks risk, and calls the client
before returning. An order selecting TWAP is passed synchronously to the
algorithm first; its first child is submitted immediately, and later children
are submitted synchronously from core-thread timer callbacks. `SubmitOrder` is
an order request passed to a function; it is not a `RuntimeMessage` and never
enters the event-loop queue.

The call returns `void`. Local route or risk failures publish `OrderDenied`
synchronously, so a strategy must establish its request state before calling
`submitOrder`. Invariant errors propagate as exceptions. A return does not mean
the venue has accepted or filled the order. External acceptance, rejection, and
fill events still enter through `EventSink` and `LiveEventLoop`.

Applications composing a kernel can call `kernel.submitOrder(order)` on the
core thread. Calls from a different thread are rejected before cache mutation.

## Execution routing

Each `ExecutionClient` exposes an `ExecutionClientId`, `AccountId`, and
`Venue`. `SubmitOrder` does not carry an account ID. It can optionally select
an execution client:

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

The execution engine first honors an explicit client ID, then uses the route
for the instrument venue, and finally the default client. The selected client
determines the account used for risk checks, order indexes, fills, and
positions. This permits one strategy to select different accounts without
binding the strategy itself to an account. Multiple clients for the same venue
must be selected explicitly unless one is installed as that venue's route.

## Cache state

Instruments and accounts are registered before market data or orders are
processed. `TradingNode` installs the demo `BTC-USD.SIM` instrument,
`SIM-001` account, and `SIM-EXEC` execution client. A custom composition root
can register definitions and clients through `TradingKernel::addInstrument`,
`TradingKernel::addAccount`, and `TradingKernel::addExecutionClient`.

`InstrumentId` is a value type composed of a `Symbol` and `Venue`. The demo
instrument is constructed as `InstrumentId{"BTC-USD", "SIM"}` and renders as
`BTC-USD.SIM`. This keeps identical symbols from different venues as distinct
cache keys.

Quote and trade histories are bounded by `CacheConfig::tick_capacity`; bar
histories are bounded independently by `bar_capacity`. Index zero is always
the newest value. A bar with the same type and event timestamp replaces the
current value, while a bar older than the newest cached value is ignored and
not published.

Orders retain filled and leaves quantities, average fill price, accumulated
fees, timestamps, venue IDs, and terminal reasons. Secondary indexes support
queries by instrument, strategy, account, venue order ID, and lifecycle state.
An identical fill replay is an idempotent no-op, while reusing a trade ID for
different fill data is rejected.

`Money` couples every balance, fee, and PnL value to a `Currency` and rejects
cross-currency arithmetic. `AccountBalance` separates total, free, and locked
money, although the current cash-only accounting keeps locked money at zero.

`Portfolio` validates and calculates the complete order, account, and position
transition before `Cache` commits it. A failed calculation therefore leaves
all three unchanged. Position state owns its fill transition and includes
signed quantity, average open price, net realized PnL, fees, and open/close
timestamps:

```cpp
const auto* account = kernel.cache().account("SIM-001");
const auto* position =
    kernel.cache().positionForInstrument(
        "SIM-001", trading::InstrumentId{"BTC-USD", "SIM"});
const auto recent_trades =
    kernel.cache().trades(trading::InstrumentId{"BTC-USD", "SIM"});
```

Quote ticks mark long positions at the bid and short positions at the ask,
falling back to the latest trade and then the latest accepted bar close when
no quote is available. Open positions without any valuation price return
`std::nullopt` instead of being silently valued at zero. The portfolio exposes
`unrealizedPnl`, `realizedPnl`, `totalPnl`, `markValue`, `netExposure`, and
`equity` queries. Market data updates the portfolio before event publication,
so subscribers observe current valuations.

The accounting model intentionally remains limited to cash-style linear
products whose account base currency matches the instrument quote currency.
FX conversion, derivative settlement, margin, leverage, and inverse or quanto
PnL are rejected or left unsupported rather than approximated.

## Risk engine

Orders enter the cache as `Initialized` before risk evaluation. The risk
engine either produces a local `OrderDenied` event or admits the order for
submission, at which point it becomes `Submitted`. `OrderRejected` is reserved
for a rejection returned by an execution venue.

`RiskEngineConfig::max_notional_per_order` stores quote-currency limits by
`InstrumentId`. Notional uses exact arithmetic over price, quantity, and the
instrument contract multiplier. Cash-account checks require the account
currency to match the instrument quote currency; currency conversion is
rejected until an exchange-rate model is available.

The engine reserves cash for open buys and position quantity for open sells.
Later orders therefore see both settled account state and unreflected inflight
exposure. Reservations follow leaves quantity after updates and partial fills,
and are released for filled, canceled, denied, rejected, or expired orders.

Trading state can be switched between `Active`, `Reducing`, and `Halted`.
`Reducing` accepts only valid reduce-only orders which cannot cross through a
position, while `Halted` denies all new orders. Cash accounts cannot open short
positions; margin accounts leave funding and leverage checks to a future
margin model.

## Public order books

Strategies declare the public books they require when they start:

```cpp
context().ensureOrderBook(
    trading::InstrumentId{"BTC-USD", "SIM"}, trading::BookType::L2Mbp);
```

`OrderBookDepth` atomically initializes or replaces the current book, while
`OrderBookDeltas` applies `Add`, `Update`, `Delete`, and `Clear` operations.
`DataEngine` updates the cache before publishing either event, so strategy
callbacks can read the current state directly:

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
Venue-specific linkage rules belong in the adapter rather than in `DataEngine`.

The tracker suppresses duplicate and stale updates, and claims a snapshot
request when a range leaves a gap. The adapter fetches and retries that request,
passing its `SnapshotRequestId` to `acceptSnapshot`; `cancelSnapshot` releases an
abandoned request so a later update can claim recovery again. `reset` clears
stream progress and invalidates earlier snapshot requests. The adapter must
also discard frames from obsolete transport connections before passing them
to the tracker.

A snapshot is accepted only when it bridges all buffered updates and covers
any updates evicted from the buffer. It is converted into a snapshot-flagged
`Clear` followed by `Add` operations, with `kBookFlagLast` on the closing delta.
An empty snapshot is a single flagged `Clear`. Snapshot and replay writes share
the tracker lock, preventing concurrent ingress from inserting a live update
between them. `EventSink` must enqueue these writes without calling downstream
handlers synchronously. Empty wire updates advance adapter sequencing without
publishing an empty domain batch.

`DataEngine` requires complete groups ending in `kBookFlagLast`, validates
snapshot group structure before applying it, and publishes each group after
the cached book is updated. It does not enforce wire sequence continuity or
fetch snapshots. Recovery replay still produces one callback per event group;
the tracker's `Synced` state describes adapter progress, not a core-thread
acknowledgement or a trading pause. The fake market client uses this same
synchronization path for its initial snapshot and normal updates.

The included `OrderBookImbalanceStrategy` compares the sizes at the best bid
and ask, then publishes `OrderBookImbalanceSignal` when the configured ratio is
crossed. The strategy then calls `context().submitOrder` with a FOK limit order:
it buys at the best ask when bids are larger and sells at the best bid otherwise.
Quantity is capped by the thinner side and `max_signal_size`. Cooldown and
this strategy's inflight orders suppress repeat triggers. `submitCount()` counts
submission requests, including requests denied by local risk. The signal is an
observation event; subscribers do not execute the order. It is an architecture
example and does not claim a trading edge.

## Runtime composition

`TradingNode` is the live runtime. It owns the executor, `LiveEventLoop`,
adapter clients, and one `TradingKernel`. The kernel owns `MessageBus`, `Cache`,
`Portfolio`, the engines, strategy context, and `Trader`. Applications only
configure strategies and run the node:

```cpp
trading::TradingNode node;
node.addStrategy(std::make_unique<trading::OrderBookImbalanceStrategy>(config));
node.run();
```

`Trader` injects the shared strategy context, rejects duplicate strategy IDs,
starts strategies in registration order, and stops them in reverse order.

## Execution algorithms

`TwapAlgorithm` is separate from `ExecutionEngine`. Strategies select it with
`exec_algorithm_id = "TWAP"` and supply typed time parameters:

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

The first child is submitted immediately; the next two are scheduled 100 ms
apart. Every child goes through client routing and risk before execution. The
algorithm preserves the primary limit price, side, strategy ID, execution-client
selection, and reduce-only flag. Child IDs are `TARGET-1-E1`, `TARGET-1-E2`, and
so on; `exec_spawn_id` identifies their primary order. Primary and planned child
IDs are reserved so another order cannot reuse them.

The primary is retained as `TwapExecutionState`, not as a venue order in `Cache`.
Query it with `kernel.twapExecution("TARGET-1")` or
`node.twapExecution("TARGET-1")`. `submitted_quantity` counts child requests,
including local denials; `filled_quantity` sums actual cached fills. `Completed`
means the target quantity was filled, rather than merely submitted.

Timing and quantity rules:

- Horizon and interval are positive integer milliseconds, with horizon at least
  interval. Requested slice count is `floor(horizon / interval)`.
- Each child is a positive multiple of instrument lot size. Remainder lots are
  distributed across the earliest children; planned quantities sum exactly to
  the primary quantity.
- The slice count is reduced when needed to satisfy minimum child quantity.
  Every child must also satisfy the instrument maximum.
- Limit orders with IOC or FOK are supported. Market orders and resting GTC
  orders are rejected. This differs from NautilusTrader's current native TWAP,
  which supports market orders.
- Late timer callbacks submit one child and preserve spacing before the next;
  they do not burst-submit overdue slices. The horizon selects the slice count,
  not a guaranteed wall-clock completion deadline.
- At most 1024 requested slices and 1024 retained executions are accepted per
  algorithm instance. The live timer interval is bounded by a 32-bit millisecond
  duration. Histories retain order-ID reservations for that instance's lifetime.

A child denial, rejection, cancellation, expiry, or quantity/price change marks
its running TWAP execution `Failed` and stops remaining timers. Actual fills
remain recorded. This implementation does not automatically retry unfilled
children or redistribute their unfilled quantity.

`kernel.cancelTwap(id)` stops future child submissions and records `Stopped`.
It does not send a venue cancel for an already submitted child. Such children
can still fill, and their fills update progress. Runtime shutdown disables new
TWAP targets and cancels pending timers. Venue cancellation/reconciliation is
outside this implementation.

`LiveEventLoop` implements the core-thread `Scheduler` port using Folly timers.
A custom kernel accepts an injected scheduler with
`TradingKernel(client, risk_config, &scheduler)`; without one, TWAP requests are
denied. The deterministic test scheduler advances time explicitly. Timer errors
stop the live loop and propagate to its caller.

`TwapExampleStrategy` submits one target on its first quote. `twap_demo` runs it
with quantity 3, horizon 300 ms, and interval 100 ms, and checks three filled
children and final position 3. The ordinary demo strategies retain their direct
submission behavior.

The parent project includes an `EMACrossTWAP` test strategy at
[`python/tests/strategies/ema_cross_twap.py`](https://github.com/nautechsystems/nautilus_trader/blob/develop/python/tests/strategies/ema_cross_twap.py).
Its backtest acceptance test registers `TwapAlgorithm` and supplies horizon and
interval parameters. See the parent
[execution algorithm guide](https://github.com/nautechsystems/nautilus_trader/blob/develop/docs/concepts/execution/algorithms.md) for its
primary/spawned order model.
