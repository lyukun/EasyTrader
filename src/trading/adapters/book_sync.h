#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "trading/common/event_sink.h"

namespace trading {

enum class BookPhase { Waiting, Synced, Recovering };
using SnapshotRequestId = std::uint64_t;

// Adapter-owned synchronization for MBP updates with absolute level sizes,
// update ID ranges may overlap, other venue linkage rules stay in their adapters.
class BookSyncTracker {
 public:
  BookSyncTracker(InstrumentId instrument_id, EventSink& event_sink,
                  std::size_t buffer_capacity = 1024);

  // Clears stream progress and invalidates earlier snapshot requests
  SnapshotRequestId reset();

  // Returns a newly claimed snapshot request which the adapter must fetch,
  // the adapter retries that request until accepted, or cancels it if fetching stops,
  // first_sequence is the wire frame's first ID, deltas.sequence is its final ID.
  std::optional<SnapshotRequestId> handleDeltas(OrderBookDeltas deltas,
                                               std::uint64_t first_sequence);
  bool acceptSnapshot(SnapshotRequestId request_id, const OrderBookDepth& depth);
  void cancelSnapshot(SnapshotRequestId request_id);

  BookPhase phase() const;
  std::optional<std::uint64_t> position() const;

 private:
  struct BufferedUpdate {
    OrderBookDeltas deltas;
    std::uint64_t first_sequence;
  };

  SnapshotRequestId claimSnapshot();
  void buffer(BufferedUpdate update);
  void send(OrderBookDeltas deltas);

  InstrumentId instrument_id_;
  EventSink& event_sink_;
  std::size_t buffer_capacity_;

  // Both ingress streams and their queue writes share this lock,
  // EventSink must enqueue without dispatching callbacks into this tracker.
  mutable std::mutex mutex_;
  BookPhase phase_{BookPhase::Waiting};
  std::optional<std::uint64_t> position_;
  std::deque<BufferedUpdate> buffer_;
  std::uint64_t minimum_snapshot_sequence_{0};
  SnapshotRequestId next_request_id_{0};
  std::optional<SnapshotRequestId> pending_request_;
};

}  // namespace trading
