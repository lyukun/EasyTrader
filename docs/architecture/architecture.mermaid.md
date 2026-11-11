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

The architecture follows five main rules:

1. `TradingKernel` is the common core and owns every shared trading component.
2. Adapters depend on `EventSink` and `ExecutionClient` ports, not core internals.
3. Every cross-thread event enters through `LiveEventLoop`.
4. Engines update state before publishing an event to strategies.
5. `MessageBus` callbacks run synchronously on the core thread.

## System architecture

```mermaid
flowchart LR
    DataClients[Data clients]
    ExecClients[Execution clients]

    subgraph kernel[TradingKernel]
        Data[DataEngine]
        Risk[RiskEngine]
        Execution[ExecutionEngine]
        Twap[TwapAlgorithm]
        Portfolio[Portfolio]
        Cache[(Cache)]
        Bus[MessageBus]
        Trader[Trader and strategies]
    end

    DataClients -->|market data| Data

    Data -->|market history and books| Cache
    Data -->|data events| Bus

    Trader -->|direct order| Risk
    Trader -->|TWAP target| Twap
    Twap -->|synchronous child orders| Risk
    Trader -->|read-only state| Cache
    Trader -->|strategy portfolio access| Portfolio
    Risk -->|read state| Cache
    Risk -->|read portfolio state| Portfolio
    Risk -->|admitted order| Execution
    Execution <--> ExecClients
    Execution -->|order lifecycle state| Cache
    Execution -->|fills| Portfolio
    Execution -->|execution events| Bus
    Bus -->|synchronous callbacks| Trader
    Bus -->|price events| Portfolio
    Bus -->|order events| Risk
    Portfolio <-->|state and atomic fill commit| Cache
```

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
| `LiveEventLoop` | Unbounded MPSC ingress and serialized draining | Domain validation |
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

## Threading and dispatch

```mermaid
sequenceDiagram
    participant Producer as Adapter worker
    participant Queue as Unbounded MPSC queue
    participant Runner as LiveEventLoop
    participant Kernel as TradingKernel
    participant Engine as Selected engine
    participant Bus as MessageBus
    participant Strategy as Strategy callback

    Producer->>Queue: send(RuntimeMessage)
    Producer-->>Producer: continue asynchronously
    Queue->>Runner: schedule drain once
    Runner->>Kernel: dispatch(next RuntimeMessage)
    Kernel->>Engine: validate and update state
    Engine->>Bus: publish(event)
    Bus->>Strategy: synchronous callback
    Strategy-->>Bus: return
    Bus-->>Engine: publication completes
    Engine-->>Kernel: return
    Kernel-->>Runner: process next queued event
```

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

`TradingKernel` registers the portfolio and risk handlers before any strategy
can subscribe. Price events therefore update portfolio valuations, and order
events refresh risk reservations, before strategy callbacks observe them.

## Event routing overview

`RuntimeMessage` is a closed `std::variant`. The following diagram covers every variant
currently accepted by `TradingKernel::dispatch`.

```mermaid
flowchart TD
    Event[RuntimeMessage from LiveEventLoop] --> Visit{Message type}

    Visit -->|QuoteTick, TradeTick| PriceData[DataEngine process]
    Visit -->|Bar| BarData[DataEngine process and stale check]
    Visit -->|OrderBookDepth| Depth[DataEngine apply atomic snapshot]
    Visit -->|OrderBookDeltas| Deltas[DataEngine apply incremental changes]

    Visit -->|OrderAccepted| Accepted[ExecutionEngine accept]
    Visit -->|OrderUpdated| Updated[ExecutionEngine update]
    Visit -->|OrderFilled| Filled[ExecutionEngine and Portfolio fill]
    Visit -->|OrderCanceled| Canceled[ExecutionEngine cancel]
    Visit -->|OrderDenied| Denied[ExecutionEngine deny]
    Visit -->|OrderRejected| Rejected[ExecutionEngine reject]
    Visit -->|OrderExpired| Expired[ExecutionEngine expire]

    Visit -->|TradingStateChanged| State[RiskEngine set state]

    PriceData --> CachePrice[Cache history]
    CachePrice --> PublishData[MessageBus publish]
    PublishData --> Valuation[Portfolio valuation subscriber]
    BarData -->|accepted| CachePrice
    BarData -->|stale| DropBar[Ignore]
    Depth -->|valid| PublishBook[MessageBus publish]
    Deltas -->|valid| PublishBook
    Depth -->|error| BookError[Record BookError]
    Deltas -->|error| BookError

    Accepted --> PublishExec[Update Cache then publish]
    Updated --> PublishExec
    Filled --> PublishFill[Atomic fill commit then publish]
    Canceled --> PublishExec
    Denied --> PublishExec
    Rejected --> PublishExec
    Expired --> PublishExec
    PublishExec --> Refresh[RiskEngine refresh reservation]
    PublishFill --> Refresh

    State --> PublishState[MessageBus publish]
```

`OrderBookImbalanceSignal` is an internal `MessageBus` publication, not a
`RuntimeMessage` variant. It is produced synchronously by the example
strategy after it reads the updated public book.

## Market-data event flows

### Quote and trade ticks

```mermaid
sequenceDiagram
    participant Adapter as MarketDataClient
    participant Runner as LiveEventLoop
    participant Kernel as TradingKernel
    participant Data as DataEngine
    participant Book as L1 OrderBook
    participant Cache as Cache
    participant Portfolio as Portfolio
    participant Bus as MessageBus
    participant Strategy as Strategy

    Adapter->>Runner: send(QuoteTick or TradeTick)
    Runner->>Kernel: dispatch(event)
    Kernel->>Data: process(event)
    Data->>Data: validate instrument and fields
    opt QuoteTick with an L1 book
        Data->>Book: updateQuote(quote)
    end
    Data->>Cache: store bounded history
    Data->>Bus: publish(event)
    Bus->>Portfolio: update valuation price
    Bus->>Strategy: callback(event)
    Strategy->>Cache: read current market state
    Strategy->>Portfolio: read current PnL or equity
```

State is current before publication. Long positions use the bid and short
positions use the ask. Without a quote, valuation falls back to the latest
trade and then the latest accepted bar close.

### Bars

```mermaid
sequenceDiagram
    participant Adapter as MarketDataClient
    participant Data as DataEngine
    participant Cache as Cache
    participant Portfolio as Portfolio
    participant Bus as MessageBus

    Adapter->>Data: Bar
    Data->>Data: validate OHLC and interval
    Data->>Cache: storeBar(bar)
    alt New bar or same-timestamp replacement
        Cache-->>Data: accepted
        Data->>Bus: publish(Bar)
        Bus->>Portfolio: update bar-close fallback
    else Older than newest bar
        Cache-->>Data: stale
        Data-->>Adapter: ignore without publication
    end
```

### Public order books

```mermaid
sequenceDiagram
    participant Adapter as MarketDataClient
    participant Data as DataEngine
    participant Cache as Cache
    participant Book as OrderBook
    participant Bus as MessageBus
    participant Strategy as Strategy

    Adapter->>Data: OrderBookDepth or OrderBookDeltas
    Data->>Cache: resolve managed book
    Cache-->>Data: OrderBook pointer
    alt OrderBookDepth
        Data->>Book: atomically replace snapshot
    else OrderBookDeltas
        Data->>Book: apply ordered changes
    end
    alt Update succeeds
        Data->>Bus: publish original event
        Bus->>Strategy: callback(event)
        Strategy->>Cache: read updated book
    else Book missing or update invalid
        Data->>Data: store last BookError
        Data-->>Adapter: do not publish
    end
```

## Order event flows

### Submit order

This path describes direct orders and each child submitted by TWAP. TWAP primary
orders enter the algorithm first, rather than being sent to an execution client.

```mermaid
sequenceDiagram
    participant Strategy as Strategy
    participant Context as StrategyContext
    participant Kernel as TradingKernel
    participant Execution as ExecutionEngine
    participant Cache as Cache
    participant Risk as RiskEngine
    participant Client as ExecutionClient
    participant Bus as MessageBus

    Strategy->>Context: submitOrder(order)
    Context->>Kernel: synchronous submitOrder(order)
    Kernel->>Execution: route(order)
    alt No matching execution client
        Execution-->>Kernel: no route
        Kernel->>Bus: publish OrderDenied
        Note over Cache: Order is not cached because account route is unknown
    else Route resolved
        Execution-->>Kernel: ExecutionClientId and AccountId
        Kernel->>Cache: add order as Initialized
        Kernel->>Risk: execute(order, account)
        alt Risk denies
            Risk-->>Kernel: OrderDenied
            Kernel->>Execution: process(OrderDenied)
            Execution->>Cache: mark Denied
            Execution->>Bus: publish OrderDenied
        else Risk admits
            Risk->>Risk: reserve cash or position
            Kernel->>Execution: submit(order, route)
            Execution->>Cache: mark Submitted
            Execution->>Client: submit(order)
        end
    end
```

`SubmitOrder` is a request passed directly to the synchronous submission API;
it is not a `RuntimeMessage`. The call returns after local processing and client
submission, while venue acceptance and fills still arrive later as events.

Routing priority is explicit `ExecutionClientId`, then the client registered for
the instrument venue, then the default client. The selected client determines
the account used by risk, accounting, and order indexes.

### Accepted, updated, canceled, denied, rejected, and expired

```mermaid
sequenceDiagram
    participant Client as ExecutionClient or local producer
    participant Runner as LiveEventLoop
    participant Kernel as TradingKernel
    participant Execution as ExecutionEngine
    participant Cache as Cache
    participant Bus as MessageBus
    participant Strategy as Strategy
    participant Risk as RiskEngine

    Client->>Runner: send(lifecycle event)
    Runner->>Kernel: dispatch(event)
    Kernel->>Execution: process(event)
    Execution->>Cache: validate transition and update order
    Execution->>Bus: publish(event)
    Bus->>Risk: refreshReservation(client_order_id)
    Bus->>Strategy: synchronous callback
    alt Order remains open
        Risk->>Risk: resize reservation to leaves quantity
    else Terminal status
        Risk->>Risk: release reservation
    end
```

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

```mermaid
sequenceDiagram
    participant Client as ExecutionClient
    participant Kernel as TradingKernel
    participant Execution as ExecutionEngine
    participant Portfolio as Portfolio
    participant Position as Position
    participant Cache as Cache
    participant Bus as MessageBus
    participant Risk as RiskEngine

    Client->>Kernel: OrderFilled
    Kernel->>Execution: process(fill)
    Execution->>Portfolio: apply(fill)
    Portfolio->>Cache: find prior fill by TradeId
    alt Identical fill already applied
        Cache-->>Portfolio: same fill
        Portfolio-->>Execution: idempotent no-op
        Note over Bus: Event is not published again
    else TradeId conflicts
        Cache-->>Portfolio: different fill
        Portfolio-->>Execution: throw invalid_argument
    else New fill
        Portfolio->>Portfolio: calculate next OrderState
        Portfolio->>Position: apply fill to a copied Position
        Position-->>Portfolio: net realized PnL delta
        Portfolio->>Portfolio: calculate copied Account state
        Portfolio->>Cache: commit order, account, position, and fill atomically
        Portfolio-->>Execution: applied
        Execution->>Bus: publish OrderFilled
    end
    Bus->>Risk: refreshReservation(client_order_id)
```

All validation and allocation completes before the cache changes domain state.
If price, quantity, fee currency, balance, or arithmetic validation fails, the
order, account, position, and fill registry remain unchanged.

## Control event flows

### Trading state

```mermaid
sequenceDiagram
    participant Operator as Operator or application
    participant Kernel as TradingKernel
    participant Risk as RiskEngine
    participant Bus as MessageBus
    participant Strategy as Strategy

    Operator->>Kernel: TradingStateChanged
    Kernel->>Risk: setTradingState(Active, Reducing, or Halted)
    Kernel->>Bus: publish(TradingStateChanged)
    Bus->>Strategy: callback(event)
```

`Active` permits orders that pass normal validation. `Reducing` permits only
valid reduce-only orders. `Halted` denies every new order.

### Shutdown

```mermaid
sequenceDiagram
    participant Application as Application or timer
    participant Node as TradingNode
    participant Runner as LiveEventLoop
    participant EventBase as Folly EventBase
    participant Trader as Trader
    participant Executor as Adapter executor

    Application->>Node: stop()
    Node->>Runner: stop()
    Runner->>EventBase: terminateLoopSoon()
    EventBase-->>Node: core loop exits
    Node->>Trader: stop strategies in reverse order
    Node->>Executor: join adapter tasks
```

Shutdown is a runtime lifecycle operation, not a domain event in
`RuntimeMessage`.

## Strategy lifecycle and internal signals

```mermaid
stateDiagram-v2
    [*] --> Ready
    Ready --> Running: Trader.start()
    Running --> Stopped: Trader.stop()
    Ready --> Stopped: strategy start failure and rollback
    Stopped --> [*]
```

Strategies are registered only while the trader is `Ready`. Startup follows
registration order; shutdown and partial-start rollback use reverse order.
Strategies subscribe to typed `MessageBus` events during `start()` and may
publish internal signals synchronously.

The included order-book imbalance strategy follows this path:

```mermaid
flowchart LR
    D[OrderBookDeltas] --> Data[DataEngine]
    Data --> Book[(Updated public book)]
    Data --> Bus[MessageBus]
    Bus --> Strategy[OrderBookImbalanceStrategy]
    Book --> Strategy
    Strategy -->|threshold crossed| Signal[OrderBookImbalanceSignal]
    Signal --> Bus
    Bus --> Subscribers[Signal subscribers]
    Strategy -->|synchronous submitOrder| Kernel[TradingKernel]
    Kernel --> Risk[RiskEngine]
    Risk -->|admitted| Execution[ExecutionEngine]
    Execution --> Client[ExecutionClient]
```

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

```mermaid
sequenceDiagram
    participant Strategy
    participant Kernel as TradingKernel
    participant Twap as TwapAlgorithm
    participant Timer as Core-thread scheduler
    participant Risk as RiskEngine
    participant Execution as ExecutionEngine
    participant Client as ExecutionClient

    Strategy->>Kernel: submitOrder(primary with TWAP parameters)
    Kernel->>Twap: submit(primary)
    Twap->>Twap: validate and allocate exact lots
    Twap->>Kernel: submitDirect(first child)
    Kernel->>Risk: validate child
    Risk-->>Kernel: admitted or denied
    alt Admitted
        Kernel->>Execution: submit child
        Execution->>Client: submit child
        Twap->>Timer: schedule next interval
        Timer->>Twap: callback on core thread
        Twap->>Kernel: submitDirect(next child)
    else Denied
        Kernel->>Twap: child denial via MessageBus
        Twap->>Twap: mark Failed and stop remaining schedule
    end
    Note over Twap,Client: Venue events arrive asynchronously, only actual full target fills mean Completed
```

The primary's `TwapExecutionState` is separate from cached venue orders. Child
IDs use the primary ID plus `-E1`, `-E2`, etc.; each child records `exec_spawn_id`.
The primary is never sent to the venue in addition to its children.

Planned quantity is conserved with integer lot arithmetic. The requested number
of slices is `floor(horizon / interval)`, reduced when required by minimum child
quantity. Maximum child quantity and timer/resource bounds are checked before
execution. Children retain the primary limit price and require IOC or FOK.

A denial or an unfilled terminal child stops subsequent submissions and marks
the primary execution `Failed`. A changed child quantity or limit price also
stops the schedule. Automatic retries and restoration of unfilled child quantity
are not implemented. Progress continues to reflect fills of already submitted
children after a stop or failure.

`cancelTwap` cancels future scheduling, not venue orders. Runtime shutdown stops
algorithm timers and prevents new targets. A delayed timer sends one child,
then waits another interval; it does not issue a catch-up burst.
