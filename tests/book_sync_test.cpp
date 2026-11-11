#include <cstdlib>
#include <future>
#include <functional>
#include <iostream>
#include <latch>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "trading/adapters/book_sync.h"
#include "trading/adapters/fake_clients.h"
#include "trading/data/data_engine.h"

namespace {

using namespace trading;

const InstrumentId kInstrument{"BTC-USD", "SIM"};

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename Callback>
void requireInvalid(Callback callback) {
  try {
    callback();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid book input was accepted");
}

OrderBookDepth depth(std::uint64_t sequence, std::string_view size = "2") {
  return {kInstrument,
          {{OrderSide::Buy, Price::fromString("99"), Quantity::fromString("10"), 1}},
          {{OrderSide::Sell, Price::fromString("101"), Quantity::fromString(size), 2}},
          sequence, sequence, sequence};
}

OrderBookDeltas update(std::uint64_t sequence, std::string_view size = "3") {
  return {kInstrument,
          {{kInstrument, BookAction::Update,
            {OrderSide::Sell, Price::fromString("101"), Quantity::fromString(size), 2},
            kBookFlagLast, sequence, sequence, sequence}},
          sequence, sequence, sequence};
}

class QueuedSink final : public EventSink {
 public:
  bool send(RuntimeMessage message) override {
    if (before_send) {
      before_send();
    }
    if (reject) {
      return false;
    }
    messages.push_back(std::move(message));
    return true;
  }

  std::vector<RuntimeMessage> messages;
  std::function<void()> before_send;
  bool reject{false};
};

struct Pipeline {
  Pipeline() {
    cache.addInstrument(Instrument{kInstrument, "BTC", "USD", 2, 8,
                                   Price::fromString("0.01"),
                                   Quantity::fromString("0.00000001"),
                                   Quantity::fromString("0.00000001"),
                                   Quantity::fromString("1000")});
    engine.addOrderBook(kInstrument, BookType::L2Mbp);
    bus.subscribe<OrderBookDeltas>([this](const auto& deltas) {
      const auto* book = cache.orderBook(kInstrument);
      require(book->bids().size() == 1 && book->asks().size() == 1,
              "callback saw an incomplete snapshot");
      observed.emplace_back(deltas.sequence, *book->bestAskSize());
    });
  }

  void drain(QueuedSink& sink) {
    auto messages = std::exchange(sink.messages, {});
    for (const auto& message : messages) {
      if (const auto* deltas = std::get_if<OrderBookDeltas>(&message)) {
        require(!engine.process(*deltas), "normalized batch failed in DataEngine");
      }
    }
  }

  Cache cache;
  MessageBus bus;
  DataEngine engine{cache, bus};
  std::vector<std::pair<std::uint64_t, Quantity>> observed;
};

void testBootstrapGapAndReplay() {
  Pipeline pipeline;
  QueuedSink sink;
  BookSyncTracker tracker(kInstrument, sink);
  require(tracker.phase() == BookPhase::Waiting && !tracker.position(),
          "new tracker was already synced");
  const auto initial = tracker.handleDeltas(update(11), 11);
  require(initial && sink.messages.empty(), "initial increment escaped before a snapshot");
  require(!tracker.handleDeltas(update(11), 11), "duplicate started another recovery");
  require(tracker.acceptSnapshot(*initial, depth(10)), "bootstrap snapshot was rejected");
  require(tracker.position() == 11 && pipeline.observed.empty(),
          "adapter progress was confused with core progress");
  pipeline.drain(sink);
  require(pipeline.observed ==
              std::vector<std::pair<std::uint64_t, Quantity>>{
                  {10, Quantity::fromString("2")}, {11, Quantity::fromString("3")}},
          "bootstrap did not emit snapshot then one replay");
  require(!tracker.acceptSnapshot(*initial, depth(9)),
          "completed request accepted a late snapshot");
  require(!tracker.handleDeltas(update(10, "9"), 10) &&
              !tracker.handleDeltas(update(11, "9"), 11) && sink.messages.empty(),
          "stale or duplicate increment was emitted");

  const auto recovery = tracker.handleDeltas(update(13, "5"), 13);
  require(recovery && tracker.phase() == BookPhase::Recovering && !tracker.position(),
          "sequence gap did not suppress output");
  require(!tracker.handleDeltas(update(14, "6"), 14), "gap started competing snapshot requests");
  require(!tracker.acceptSnapshot(*recovery, depth(11)) && sink.messages.empty(),
          "unbridgeable snapshot was partially emitted");
  require(pipeline.cache.orderBook(kInstrument)->bestAskSize() == Quantity::fromString("3"),
          "adapter recovery mutated core Cache");
  require(tracker.acceptSnapshot(*recovery, depth(12, "4")), "recovery snapshot was rejected");
  pipeline.drain(sink);
  require(pipeline.observed.size() == 5 &&
              pipeline.observed[2] ==
                  std::make_pair(std::uint64_t{12}, Quantity::fromString("4")) &&
              pipeline.observed[3] ==
                  std::make_pair(std::uint64_t{13}, Quantity::fromString("5")) &&
              pipeline.observed[4] ==
                  std::make_pair(std::uint64_t{14}, Quantity::fromString("6")),
          "callbacks did not observe complete replay states");

  require(!tracker.handleDeltas(update(17, "7"), 15) && tracker.position() == 17,
          "a continuous multi-update range was rejected");
  pipeline.drain(sink);
  require(pipeline.cache.orderBook(kInstrument)->bestAskSize() == Quantity::fromString("7"),
          "normal output did not resume after recovery");
}

void testRecoveryOwnershipAndBufferLoss() {
  QueuedSink sink;
  BookSyncTracker tracker(kInstrument, sink, 1);
  const auto old_request = tracker.reset();
  const auto current_request = tracker.reset();
  require(!tracker.acceptSnapshot(old_request, depth(100)), "old connection snapshot was accepted");
  require(!tracker.handleDeltas(update(105), 105), "pending snapshot gained another owner");
  require(!tracker.handleDeltas(update(101), 101), "buffer overflow gained another owner");

  // The remaining 101 looks continuous from 100, but the evicted 105 was not covered
  require(!tracker.acceptSnapshot(current_request, depth(100)) && sink.messages.empty(),
          "buffer eviction silently lost a newer update");
  require(tracker.acceptSnapshot(current_request, depth(105)),
          "fresh snapshot did not cover buffer loss");
  require(sink.messages.size() == 1 && tracker.position() == 105,
          "snapshot-covered increments were replayed");

  sink.messages.clear();
  const auto cancelled = tracker.reset();
  tracker.handleDeltas(update(11), 11);
  tracker.cancelSnapshot(cancelled);
  const auto replacement = tracker.handleDeltas(update(12), 12);
  require(replacement && *replacement != cancelled, "abandoned request blocked future recovery");
  require(!tracker.acceptSnapshot(cancelled, depth(100)), "cancelled request accepted a snapshot");
  require(tracker.acceptSnapshot(*replacement, depth(11)) && sink.messages.size() == 2,
          "cancelled recovery could not restart with its bounded buffer");
}

void testInputValidationAndSequenceLimits() {
  QueuedSink sink;
  BookSyncTracker tracker(kInstrument, sink);
  const auto request = tracker.reset();
  auto incomplete = update(1);
  incomplete.deltas.back().flags = 0;
  requireInvalid([&] { tracker.handleDeltas(incomplete, 1); });
  requireInvalid([&] { tracker.handleDeltas(update(1), 0); });
  requireInvalid([&] { tracker.handleDeltas(update(1), 2); });
  auto wrong_instrument = update(1);
  wrong_instrument.deltas.front().instrument_id = {"ETH-USD", "SIM"};
  requireInvalid([&] { tracker.handleDeltas(wrong_instrument, 1); });
  auto malformed = depth(0);
  malformed.asks.front().size = Quantity{};
  requireInvalid([&] { tracker.acceptSnapshot(request, malformed); });
  require(sink.messages.empty() && !tracker.position(), "invalid data changed sync progress");
  require(tracker.acceptSnapshot(request, depth(0)), "zero bootstrap sequence was rejected");
  const auto limit = std::numeric_limits<std::uint64_t>::max();
  tracker.handleDeltas(update(limit), 1);
  tracker.handleDeltas(update(limit), limit);
  require(tracker.position() == limit && sink.messages.size() == 2,
          "sequence limit overflowed or emitted a duplicate");

  const auto empty_request = tracker.reset();
  auto empty = depth(0);
  empty.bids.clear();
  empty.asks.clear();
  require(tracker.acceptSnapshot(empty_request, empty), "empty snapshot was rejected");
  const auto& empty_batch = std::get<OrderBookDeltas>(sink.messages.back());
  require(empty_batch.deltas.size() == 1 &&
              empty_batch.deltas.front().action == BookAction::Clear &&
              empty_batch.deltas.front().flags == (kBookFlagSnapshot | kBookFlagLast),
          "empty snapshot did not close its event group");

  sink.reject = true;
  try {
    tracker.handleDeltas(update(1), 1);
    require(false, "receiver failure was ignored");
  } catch (const std::runtime_error&) {
    require(tracker.phase() == BookPhase::Recovering && !tracker.position(),
            "receiver failure left the stream synced");
  }
}

void testConcurrentIngressDoesNotInterleaveReplay() {
  QueuedSink sink;
  BookSyncTracker tracker(kInstrument, sink);
  const auto request = tracker.handleDeltas(update(11), 11);
  std::latch snapshot_started(1);
  std::latch release_snapshot(1);
  bool first_send = true;
  sink.before_send = [&] {
    if (first_send) {
      first_send = false;
      snapshot_started.count_down();
      release_snapshot.wait();
    }
  };
  auto recovery = std::async(std::launch::async, [&] {
    return tracker.acceptSnapshot(*request, depth(10));
  });
  snapshot_started.wait();
  auto live = std::async(std::launch::async, [&] {
    return tracker.handleDeltas(update(12), 12);
  });
  release_snapshot.count_down();
  require(recovery.get() && !live.get(), "concurrent recovery or increment failed");
  require(sink.messages.size() == 3 &&
              std::get<OrderBookDeltas>(sink.messages[0]).sequence == 10 &&
              std::get<OrderBookDeltas>(sink.messages[1]).sequence == 11 &&
              std::get<OrderBookDeltas>(sink.messages[2]).sequence == 12,
          "live increment interleaved snapshot and replay");
}

void testEmptyUpdatesAdvanceOnlyAdapterProgress() {
  Pipeline pipeline;
  QueuedSink sink;
  BookSyncTracker tracker(kInstrument, sink);
  const auto request = tracker.reset();
  auto empty = update(11);
  empty.deltas.clear();
  tracker.handleDeltas(empty, 11);
  require(tracker.acceptSnapshot(request, depth(10)) && tracker.position() == 11 &&
              sink.messages.size() == 1,
          "buffered empty frame was emitted or lost its sequence");
  empty.sequence = 12;
  tracker.handleDeltas(empty, 12);
  require(tracker.position() == 12 && sink.messages.size() == 1,
          "live empty frame did not advance adapter progress");
  tracker.handleDeltas(update(13), 13);
  pipeline.drain(sink);
  require(pipeline.observed.size() == 2 && pipeline.observed.back().first == 13,
          "DataEngine imposed wire continuity on normalized batches");
}

void testDataEngineBatchBoundaries() {
  Pipeline pipeline;
  require(!pipeline.engine.process(depth(10)), "initial depth failed");
  auto partial = update(11);
  partial.deltas.back().flags = 0;
  require(pipeline.engine.process(partial) == BookError::InvalidBatch &&
              pipeline.observed.empty() && pipeline.cache.orderBook(kInstrument)->sequence() == 10,
          "incomplete event group mutated or published the book");

  QueuedSink sink;
  BookSyncTracker tracker(kInstrument, sink);
  const auto request = tracker.reset();
  tracker.handleDeltas(update(12, "4"), 12);
  require(tracker.acceptSnapshot(request, depth(11, "3")), "snapshot assembly failed");
  auto combined = std::get<OrderBookDeltas>(sink.messages[0]);
  const auto& next = std::get<OrderBookDeltas>(sink.messages[1]);
  combined.deltas.insert(combined.deltas.end(), next.deltas.begin(), next.deltas.end());
  combined.sequence = next.sequence;
  combined.ts_event = next.ts_event;
  combined.ts_init = next.ts_init;
  require(!pipeline.engine.process(combined) && pipeline.observed.size() == 2 &&
              pipeline.observed[0] ==
                  std::make_pair(std::uint64_t{11}, Quantity::fromString("3")) &&
              pipeline.observed[1] ==
                  std::make_pair(std::uint64_t{12}, Quantity::fromString("4")),
          "DataEngine collapsed snapshot and increment event boundaries");

  auto mixed = std::get<OrderBookDeltas>(sink.messages[0]);
  mixed.deltas.back().flags = kBookFlagLast;
  require(pipeline.engine.process(mixed) == BookError::InvalidBatch &&
              pipeline.observed.size() == 2 &&
              pipeline.cache.orderBook(kInstrument)->sequence() == 12,
          "malformed snapshot partly cleared the book");
  auto invalid_quantity = std::get<OrderBookDeltas>(sink.messages[0]);
  invalid_quantity.deltas.back().order.size = Quantity{};
  require(pipeline.engine.process(invalid_quantity) == BookError::InvalidQuantity &&
              pipeline.observed.size() == 2 &&
              pipeline.cache.orderBook(kInstrument)->bestAskSize() == Quantity::fromString("4"),
          "failed snapshot changed state or triggered a callback");
}

void testFakeClientUsesNormalizedBookIngress() {
  QueuedSink sink;
  folly::CPUThreadPoolExecutor executor(1);
  FakeMarketDataClient client(sink, executor);
  client.start(1, std::chrono::milliseconds(0));
  executor.join();
  require(sink.messages.size() == 5, "fake client did not emit its normalized market events");
  const auto& snapshot = std::get<OrderBookDeltas>(sink.messages[0]);
  const auto& increment = std::get<OrderBookDeltas>(sink.messages[1]);
  require(snapshot.sequence == 1 && snapshot.deltas.size() == 5 &&
              snapshot.deltas.front().action == BookAction::Clear &&
              snapshot.deltas.back().flags == (kBookFlagSnapshot | kBookFlagLast) &&
              increment.sequence == 2,
          "fake client bypassed snapshot normalization or ordering");
}

}  // namespace

int main() {
  try {
    testBootstrapGapAndReplay();
    testRecoveryOwnershipAndBufferLoss();
    testInputValidationAndSequenceLimits();
    testConcurrentIngressDoesNotInterleaveReplay();
    testEmptyUpdatesAdvanceOnlyAdapterProgress();
    testDataEngineBatchBoundaries();
    testFakeClientUsesNormalizedBookIngress();
    std::cout << "all book synchronization tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& e) {
    std::cerr << "book synchronization test failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
}
