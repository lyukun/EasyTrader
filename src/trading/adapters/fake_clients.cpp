#include "trading/adapters/fake_clients.h"

#include <stdexcept>
#include <thread>

#include "trading/adapters/book_sync.h"

namespace trading {

void FakeMarketDataClient::start(std::size_t ticks,
                                 std::chrono::milliseconds interval) {
  running_.store(true);
  executor_.add([this, ticks, interval] {
    const InstrumentId instrument_id{"BTC-USD", "SIM"};
    OrderBookDepth depth{
        instrument_id,
        {{OrderSide::Buy, Price::fromString("99.95"),
          Quantity::fromString("12"), 101},
         {OrderSide::Buy, Price::fromString("99.90"),
          Quantity::fromString("20"), 102}},
        {{OrderSide::Sell, Price::fromString("100.05"),
          Quantity::fromString("2"), 201},
         {OrderSide::Sell, Price::fromString("100.10"),
          Quantity::fromString("18"), 202}},
        1,
        1,
        1};

    // The fake venue retains authoritative depth for snapshot requests,
    // the tracker normalizes snapshots and increments without touching core Cache.
    BookSyncTracker book_sync(instrument_id, event_sink_);
    const auto initial_request = book_sync.reset();
    if (!book_sync.acceptSnapshot(initial_request, depth)) {
      throw std::runtime_error("initial market snapshot could not be synchronized");
    }
    for (std::size_t i = 0; i < ticks && running_.load(); ++i) {
      const auto sequence = static_cast<std::uint64_t>(i) + 2;
      const auto ask_size = i % 2 == 0 ? Quantity::fromString("2")
                                       : Quantity::fromString("10");
      depth.asks.front().size = ask_size;
      depth.sequence = sequence;
      depth.ts_event = sequence;
      depth.ts_init = sequence;
      const auto request = book_sync.handleDeltas(
          OrderBookDeltas{
              instrument_id,
              {{instrument_id,
                BookAction::Update,
                {OrderSide::Sell, Price::fromString("100.05"), ask_size, 201},
                kBookFlagLast,
                sequence,
                sequence,
                sequence}},
              sequence,
              sequence,
              sequence},
          sequence);
      if (request && !book_sync.acceptSnapshot(*request, depth)) {
        throw std::runtime_error("market snapshot could not bridge buffered updates");
      }
      event_sink_.send(QuoteTick{instrument_id,
                             Price::fromString("99.95"),
                             Price::fromString("100.05"),
                             Quantity::fromString("12"),
                             ask_size,
                             sequence,
                             sequence});
      event_sink_.send(TradeTick{instrument_id,
                             Price::fromString("100.05"),
                             Quantity::fromString("1"),
                             AggressorSide::Buyer,
                             "MARKET-" + std::to_string(sequence),
                             sequence,
                             sequence,
                             sequence});
      event_sink_.send(Bar{BarType{instrument_id, 1'000'000'000},
                       Price::fromString("100.00"),
                       Price::fromString("100.10"),
                       Price::fromString("99.90"),
                       Price::fromString("100.05"),
                       Quantity::fromString("10"),
                       sequence,
                       sequence});
      std::this_thread::sleep_for(interval);
    }
  });
}

void FakeExecutionClient::submit(const SubmitOrder& order) {
  executor_.add([this, order] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const VenueOrderId venue_id = "VENUE-" + order.client_order_id;
    event_sink_.send(OrderAccepted{order.client_order_id, venue_id});

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    event_sink_.send(OrderFilled{order.client_order_id, venue_id,
                             order.instrument_id,
                             order.side, order.quantity, order.limit_price,
                             "TRADE-" + order.client_order_id, std::nullopt, 2});
  });
}

}  // namespace trading
