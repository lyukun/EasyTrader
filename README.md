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
- Python strategies executing through embedded CPython on the same core thread.

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
- `python`: the optional embedded interpreter, strategy bridge, and Python API.

## Build

Dependencies are installed with Homebrew:

```bash
brew install folly spdlog pybind11 python@3.14 ninja
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

## Python strategies

The optional `python_strategy` library embeds CPython 3.14 with pybind11. Run the
included Python strategy and its integration tests from the repository root:

```bash
blade build //:python_demo //:python_strategy_test
./blade-bin/python_demo python/strategies
./blade-bin/python_strategy_test tests/python
```

The [example strategy](python/strategies/example_strategy.py) submits one limit
order on its first quote. The demo checks the filled order and final position 1.
See [Python strategy execution](architecture.md#python-strategy-execution) for
the callback API, threading, and object lifetime rules. The current build uses
Homebrew's Python 3.14 headers and library path. Blade's symbol checker does not
resolve this custom library path and reports Python symbols as warnings; the
Python targets explicitly declare `#python3.14` and link the interpreter.

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
