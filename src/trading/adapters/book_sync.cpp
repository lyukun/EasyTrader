#include "trading/adapters/book_sync.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace trading {
namespace {

bool continues(std::uint64_t position, std::uint64_t first_sequence) {
  return position != std::numeric_limits<std::uint64_t>::max() &&
         first_sequence <= position + 1;
}

OrderBookDeltas snapshotDeltas(const OrderBookDepth& depth) {
  OrderBookDeltas result{depth.instrument_id, {}, depth.sequence,
                         depth.ts_event, depth.ts_init};
  result.deltas.reserve(1 + depth.bids.size() + depth.asks.size());
  result.deltas.push_back(OrderBookDelta{
      depth.instrument_id, BookAction::Clear,
      {OrderSide::Buy, Price{}, Quantity{}, 0}, kBookFlagSnapshot,
      depth.sequence, depth.ts_event, depth.ts_init});
  const auto add_side = [&](const auto& orders, OrderSide side) {
    for (const auto& order : orders) {
      if (order.side != side || order.price.raw() <= 0 || order.size.isZero()) {
        throw std::invalid_argument("snapshot order is invalid");
      }
      result.deltas.push_back(OrderBookDelta{
          depth.instrument_id, BookAction::Add, order, kBookFlagSnapshot,
          depth.sequence, depth.ts_event, depth.ts_init});
    }
  };
  add_side(depth.bids, OrderSide::Buy);
  add_side(depth.asks, OrderSide::Sell);
  result.deltas.back().flags |= kBookFlagLast;
  return result;
}

void validateUpdate(const InstrumentId& instrument_id,
                    const OrderBookDeltas& deltas, std::uint64_t first_sequence) {
  if (deltas.instrument_id != instrument_id ||
      first_sequence == 0 || first_sequence > deltas.sequence ||
      (!deltas.deltas.empty() && deltas.deltas.back().sequence != deltas.sequence)) {
    throw std::invalid_argument("book update range is invalid");
  }
  for (std::size_t i = 0; i < deltas.deltas.size(); ++i) {
    const auto& delta = deltas.deltas[i];
    const bool last = (delta.flags & kBookFlagLast) != 0;
    if (delta.instrument_id != instrument_id ||
        delta.sequence < first_sequence || delta.sequence > deltas.sequence ||
        (delta.flags & kBookFlagSnapshot) != 0 ||
        last != (i + 1 == deltas.deltas.size()) ||
        (delta.action != BookAction::Add && delta.action != BookAction::Update &&
         delta.action != BookAction::Delete) ||
        (delta.order.side != OrderSide::Buy && delta.order.side != OrderSide::Sell) ||
        delta.order.price.raw() <= 0 ||
        (delta.action != BookAction::Delete && delta.order.size.isZero())) {
      throw std::invalid_argument("book update is not a complete incremental batch");
    }
  }
}

}  // namespace

BookSyncTracker::BookSyncTracker(InstrumentId instrument_id, EventSink& event_sink,
                                 std::size_t buffer_capacity)
    : instrument_id_(std::move(instrument_id)),
      event_sink_(event_sink),
      buffer_capacity_(buffer_capacity) {
  if (instrument_id_.empty() || buffer_capacity_ == 0) {
    throw std::invalid_argument("book synchronization requires an instrument and buffer");
  }
}

SnapshotRequestId BookSyncTracker::claimSnapshot() {
  if (next_request_id_ == std::numeric_limits<SnapshotRequestId>::max()) {
    throw std::overflow_error("snapshot request IDs exhausted");
  }
  pending_request_ = ++next_request_id_;
  return *pending_request_;
}

SnapshotRequestId BookSyncTracker::reset() {
  const std::lock_guard lock(mutex_);
  const auto request_id = claimSnapshot();
  phase_ = BookPhase::Waiting;
  position_.reset();
  buffer_.clear();
  minimum_snapshot_sequence_ = 0;
  return request_id;
}

void BookSyncTracker::buffer(BufferedUpdate update) {
  if (buffer_.size() == buffer_capacity_) {

    // A replacement snapshot must cover every update evicted from the buffer,
    // checking only the remaining first update would hide loss after reordering.
    minimum_snapshot_sequence_ =
        std::max(minimum_snapshot_sequence_, buffer_.front().deltas.sequence);
    buffer_.pop_front();
  }
  buffer_.push_back(std::move(update));
}

std::optional<SnapshotRequestId> BookSyncTracker::handleDeltas(
    OrderBookDeltas deltas, std::uint64_t first_sequence) {
  validateUpdate(instrument_id_, deltas, first_sequence);
  const std::lock_guard lock(mutex_);
  if (phase_ == BookPhase::Synced) {
    if (deltas.sequence <= *position_) {
      return std::nullopt;
    }
    if (continues(*position_, first_sequence)) {
      const auto sequence = deltas.sequence;
      if (!deltas.deltas.empty()) {
        send(std::move(deltas));
      }
      position_ = sequence;
      return std::nullopt;
    }
    phase_ = BookPhase::Recovering;
  }

  buffer(BufferedUpdate{std::move(deltas), first_sequence});
  if (!pending_request_) {
    phase_ = BookPhase::Recovering;
    return claimSnapshot();
  }
  return std::nullopt;
}

bool BookSyncTracker::acceptSnapshot(SnapshotRequestId request_id,
                                    const OrderBookDepth& depth) {
  const std::lock_guard lock(mutex_);
  if (!pending_request_ || *pending_request_ != request_id) {
    return false;
  }
  if (depth.instrument_id != instrument_id_) {
    throw std::invalid_argument("snapshot instrument does not match book synchronization");
  }
  if (depth.sequence < minimum_snapshot_sequence_) {
    return false;
  }

  // Validate the entire seam before emitting even the snapshot,
  // an HTTP response alone does not prove that the buffered stream is recoverable.
  auto position = depth.sequence;
  for (const auto& update : buffer_) {
    if (update.deltas.sequence <= position) {
      continue;
    }
    if (!continues(position, update.first_sequence)) {
      return false;
    }
    position = update.deltas.sequence;
  }
  auto snapshot = snapshotDeltas(depth);

  // Serialize snapshot and replay writes against newly arriving increments,
  // this is adapter progress, not an acknowledgement that Cache has caught up.
  send(std::move(snapshot));
  auto emitted_position = depth.sequence;
  for (const auto& update : buffer_) {
    if (update.deltas.sequence <= emitted_position) {
      continue;
    }
    // A wire update with no changed levels still advances adapter sequencing,
    // it produces no domain batch and need not advance the cached book metadata.
    if (!update.deltas.deltas.empty()) {
      send(update.deltas);
    }
    emitted_position = update.deltas.sequence;
  }
  phase_ = BookPhase::Synced;
  position_ = position;
  buffer_.clear();
  minimum_snapshot_sequence_ = 0;
  pending_request_.reset();
  return true;
}

void BookSyncTracker::send(OrderBookDeltas deltas) {
  try {
    if (!event_sink_.send(std::move(deltas))) {
      throw std::runtime_error("book event receiver rejected a batch");
    }
  } catch (...) {

    // A failed ingress write must not leave the adapter claiming a synced stream,
    // the caller owns transport teardown or a subsequent fresh snapshot request.
    phase_ = BookPhase::Recovering;
    throw;
  }
}

void BookSyncTracker::cancelSnapshot(SnapshotRequestId request_id) {
  const std::lock_guard lock(mutex_);
  if (pending_request_ && *pending_request_ == request_id) {
    pending_request_.reset();
    phase_ = BookPhase::Recovering;
  }
}

BookPhase BookSyncTracker::phase() const {
  const std::lock_guard lock(mutex_);
  return phase_;
}

std::optional<std::uint64_t> BookSyncTracker::position() const {
  const std::lock_guard lock(mutex_);
  return phase_ == BookPhase::Synced ? position_ : std::nullopt;
}

}  // namespace trading
