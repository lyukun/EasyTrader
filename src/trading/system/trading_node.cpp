#include "trading/system/trading_node.h"

#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

namespace trading {
namespace {

std::size_t validateIoThreads(std::size_t io_threads) {
  if (io_threads == 0) {
    throw std::invalid_argument("node must have at least one I/O thread");
  }
  return io_threads;
}

}  // namespace

TradingNode::TradingNode(TradingNodeConfig config)
    : config_(std::move(config)),
      io_executor_(validateIoThreads(config_.io_threads)),
      execution_client_(event_loop_, io_executor_),
      kernel_(execution_client_, {}, &event_loop_),
      market_data_client_(event_loop_, io_executor_) {
  if (config_.run_duration <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("node run duration must be positive");
  }
  kernel_.addInstrument(Instrument{InstrumentId{"BTC-USD", "SIM"},
                                   "BTC",
                                   "USD",
                                   2,
                                   8,
                                   Price::fromString("0.01"),
                                   Quantity::fromString("0.00000001"),
                                   Quantity::fromString("0.00000001"),
                                   Quantity::fromString("1000")});
  const auto starting_balance = Money::fromString("1000000", "USD");
  kernel_.addAccount(Account{
      "SIM-001",
      "SIM",
      "USD",
      starting_balance,
      AccountBalance::cash(starting_balance),
      Money::zero("USD"),
      Money::zero("USD"),
  });
  kernel_.setMaxNotionalPerOrder(InstrumentId{"BTC-USD", "SIM"},
                                 config_.max_notional);
}

void TradingNode::run() {
  if (state_ != State::Ready) {
    throw std::logic_error("trading node can only be run once");
  }
  state_ = State::Running;

  event_loop_.bind([this](RuntimeMessage message) {
    kernel_.dispatch(std::move(message));
  });
  try {
    kernel_.trader().start();
    market_data_client_.start(config_.market_data_ticks,
                              config_.market_data_interval);
    io_executor_.add([this] {
      std::this_thread::sleep_for(config_.run_duration);
      stop();
    });
    event_loop_.run();
  } catch (...) {
    const auto run_error = std::current_exception();
    try {
      finishRun();
    } catch (...) {
    }
    std::rethrow_exception(run_error);
  }
  finishRun();
}

void TradingNode::stop() {
  event_loop_.stop();
}

void TradingNode::finishRun() {
  market_data_client_.stop();
  kernel_.stopAlgorithms();

  std::exception_ptr stop_error;
  try {
    kernel_.trader().stop();
  } catch (...) {
    stop_error = std::current_exception();
  }
  io_executor_.join();
  state_ = State::Stopped;
  if (stop_error != nullptr) {
    std::rethrow_exception(stop_error);
  }
}

}  // namespace trading
