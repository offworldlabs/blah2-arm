/// @file TuningLog.h
/// @class TuningLog
/// @brief Every change to the receiver's tuning since blah2 started.
/// @details The IQ cache stores only a timestamp, an id and the samples per
/// CPI. Front-end gain and centre frequency are needed to interpret those
/// samples, but they change rarely, so they are kept here as a log of changes
/// instead of being repeated in every frame. A frame's tuning is the last
/// entry of each kind at or before its timestamp.
///
/// Invariants a future editor must keep:
/// - Timestamps are wall-clock POSIX ms from std::chrono::system_clock, the
///   clock blah2 stamps every CPI with, so entries and frames compare directly.
/// - The first entry is the configured tuning and is never evicted, so the
///   log always describes every frame back to startup.
/// - append() is called from the SDRplay event callback. It may only ever
///   wait on this class's own mutex, which no code holds across a call into
///   the SDRplay API, so it cannot join the deadlock described in
///   RspDuo::event_callback().
///
/// Design: docs/iq-cache.md.
/// @author Josh Poole

#ifndef TUNINGLOG_H
#define TUNINGLOG_H

#include <cstddef>
#include <deque>
#include <mutex>
#include <stdint.h>
#include <vector>

class TuningLog
{
public:
  /// @brief What caused an entry, which decides which fields it carries.
  enum class Kind : uint8_t
  {
    /// @brief The tuning in config.yml when blah2 started.
    /// Carries fc, gainReductionA, gainReductionB, lnaState.
    Config = 0,

    /// @brief A live retune blah2 applied (POST /capture/retune).
    /// Carries generation, fc, gainReductionA, gainReductionB, lnaState.
    Retune = 1,

    /// @brief The SDRplay API reporting the gain one tuner now runs at.
    /// Carries tuner, gRdB, lnaGRdB, systemGainDb.
    GainEvent = 2
  };

  /// @brief One change. Fields its kind does not carry are zero.
  struct Entry
  {
    /// @brief When it took effect, POSIX ms.
    uint64_t timestamp;

    Kind kind;

    /// @brief Retune: the API's retune generation.
    int64_t generation;

    /// @brief Config, Retune: centre frequency (Hz).
    uint32_t fc;

    /// @brief Config, Retune: requested gain reduction (dB), tuner A (reference).
    int32_t gainReductionA;

    /// @brief Config, Retune: requested gain reduction (dB), tuner B (surveillance).
    int32_t gainReductionB;

    /// @brief Config, Retune: LNA state, one value shared by both tuners.
    int32_t lnaState;

    /// @brief GainEvent: 1 for tuner A (reference), 2 for tuner B (surveillance).
    uint8_t tuner;

    /// @brief GainEvent: IF gain reduction the tuner reports (dB).
    uint32_t gRdB;

    /// @brief GainEvent: LNA gain reduction the tuner reports (dB).
    uint32_t lnaGRdB;

    /// @brief GainEvent: the API's overall system gain (dB). The value that
    /// relates sample amplitude to received power.
    double systemGainDb;
  };

  /// @brief Constructor.
  /// @param capacity Most entries kept, including the Config entry.
  explicit TuningLog(size_t capacity = 4096);

  /// @brief Record the configured tuning. Call once, before capture starts.
  void set_config(uint64_t timestamp, uint32_t fc, int32_t gainReductionA,
    int32_t gainReductionB, int32_t lnaState);

  /// @brief Record an applied live retune.
  void add_retune(uint64_t timestamp, int64_t generation, uint32_t fc,
    int32_t gainReductionA, int32_t gainReductionB, int32_t lnaState);

  /// @brief Record a gain the SDRplay API reports for one tuner.
  void add_gain_event(uint64_t timestamp, uint8_t tuner, uint32_t gRdB,
    uint32_t lnaGRdB, double systemGainDb);

  /// @brief Every entry held, oldest first.
  std::vector<Entry> entries() const;

  /// @brief Entries dropped to stay within capacity, never the Config one.
  uint64_t get_n_dropped() const;

private:
  void append(const Entry &entry);

  size_t capacity;
  std::deque<Entry> log;
  bool haveConfig;
  uint64_t nDropped;
  mutable std::mutex mutex;
};

/// @brief The running process's log, written by capture and read by the cache.
extern TuningLog g_tuningLog;

#endif
