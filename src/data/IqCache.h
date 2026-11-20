/// @file IqCache.h
/// @class IqCache
/// @brief A RAM ring of the most recent CPIs' raw IQ, stored as int16.
/// @details Holds the last nSlots CPIs exactly as the front stage extracted
/// them, before the clutter filter overwrites the surveillance channel, so a
/// held CPI can be processed again offline and reproduce the live output.
/// Per CPI it stores only a timestamp, an id and the samples. Everything else
/// a reader needs is either sent per CPI by retina-telemetry under the same
/// timestamp, reproducible from the samples, or in the run's TuningLog.
///
/// Invariants a future editor must keep:
/// - Storage is allocated and committed once in the constructor and never
///   reallocated, so RSS is flat from the first CPI.
/// - A frame's samples are interleaved I_ref, Q_ref, I_surv, Q_surv, the same
///   layout the `.iq` recorder writes.
/// - A frame is keyed by the timestamp (ms) every live output for that CPI
///   carries, so a reader can ask for the CPI behind a given map or detection.
/// - Readers copy a whole frame under the lock. The writer fills a slot
///   outside the lock only after taking it out of the index, so a reader can
///   never see a half-written frame and never stalls the writer for longer
///   than one copy.
///
/// Design, sizing and measurement: docs/iq-cache.md.
/// @author Josh Poole

#ifndef IQCACHE_H
#define IQCACHE_H

#include "data/IqData.h"

#include <cstddef>
#include <mutex>
#include <stdint.h>
#include <vector>

class IqCache
{
public:
  /// @brief What is known about one held frame.
  struct FrameInfo
  {
    /// @brief CPIs written before this one, from 0 at startup. A reader that
    /// sees it jump has missed frames that were overwritten.
    uint64_t cpiId;

    /// @brief Timestamp (POSIX ms) of the CPI, the same value its map,
    /// detections and telemetry frame carry. The lookup key.
    uint64_t timestamp;
  };

  /// @brief Constructor. Allocates and commits all storage.
  /// @param nSlots Number of CPIs held.
  /// @param nSamples Samples per channel per CPI.
  IqCache(uint32_t nSlots, uint32_t nSamples);

  /// @brief Store one CPI, replacing the oldest held frame once full.
  /// @param x Reference channel, at least nSamples long.
  /// @param y Surveillance channel, at least nSamples long.
  /// @param timestamp Timestamp (ms) of the CPI.
  /// @return True if every sample was stored exactly. Always true for the
  /// RSPduo, whose samples arrive as int16. A false return also counts the
  /// frame in get_n_inexact().
  bool write(const IqData &x, const IqData &y, uint64_t timestamp);

  /// @brief The frames currently held, oldest first.
  std::vector<FrameInfo> index() const;

  /// @brief Copy out the frame with the given timestamp.
  /// @param timestamp Timestamp (ms) of the wanted CPI.
  /// @param info Set to the frame's metadata when found.
  /// @param out Resized to 4 * nSamples and filled with the frame when found.
  /// @return False if no held frame has that timestamp.
  bool read(uint64_t timestamp, FrameInfo &info, std::vector<int16_t> &out) const;

  /// @brief Number of CPIs the cache holds when full.
  uint32_t get_n_slots() const { return nSlots; }

  /// @brief Samples per channel per CPI.
  uint32_t get_n_samples() const { return nSamples; }

  /// @brief Bytes of sample storage, the cache's whole RAM cost bar metadata.
  size_t get_bytes() const { return data.size() * sizeof(int16_t); }

  /// @brief Frames written since startup that needed rounding or clipping.
  uint64_t get_n_inexact() const;

private:
  /// @brief Per-slot metadata. valid is false while empty or being written.
  struct Slot
  {
    FrameInfo info;
    bool valid;
  };

  uint32_t nSlots;
  uint32_t nSamples;

  /// @brief int16 values per frame: 4 per sample (two channels, I and Q).
  size_t frameValues;

  /// @brief All frames back to back, nSlots * frameValues values.
  std::vector<int16_t> data;

  std::vector<Slot> slots;

  /// @brief Slot the next write goes to, which is the oldest once full.
  uint32_t next;

  /// @brief Frames written so far.
  uint64_t nWritten;

  /// @brief Frames written so far that were not stored exactly.
  uint64_t nInexact;

  /// @brief Guards slots, next, nWritten, nInexact, and every read of data.
  mutable std::mutex mutex;
};

#endif
