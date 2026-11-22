/// @file TuningLog.h
/// @class TuningLog
/// @brief Every change to fc, gain and LNA state since blah2 started. See docs/iq-cache.md.
/// @details Invariants: timestamps use system_clock ms, the CPIs' clock; the
/// Config entry is first and never evicted; append() runs from the SDRplay
/// event callback, so the mutex (and the sink) must never be held across an
/// SDRplay API call.

#ifndef TUNINGLOG_H
#define TUNINGLOG_H

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stdint.h>
#include <vector>

class TuningLog
{
public:
  enum class Kind : uint8_t
  {
    Config = 0,
    Retune = 1,
    GainEvent = 2
  };

  /// @brief Fields a kind does not carry are zero.
  struct Entry
  {
    uint64_t timestamp;
    Kind kind;
    int64_t generation;
    uint32_t fc;
    int32_t gainReductionA;
    int32_t gainReductionB;
    int32_t lnaState;
    uint8_t tuner;
    uint32_t gRdB;
    uint32_t lnaGRdB;
    double systemGainDb;
  };

  explicit TuningLog(size_t capacity = 4096);

  void set_config(uint64_t timestamp, uint32_t fc, int32_t gainReductionA,
    int32_t gainReductionB, int32_t lnaState);
  void add_retune(uint64_t timestamp, int64_t generation, uint32_t fc,
    int32_t gainReductionA, int32_t gainReductionB, int32_t lnaState);
  void add_gain_event(uint64_t timestamp, uint8_t tuner, uint32_t gRdB,
    uint32_t lnaGRdB, double systemGainDb);

  std::vector<Entry> entries() const;
  uint64_t get_n_dropped() const;

  /// @brief Replay held entries into sink, then pass on each new one.
  /// Call after set_config().
  void attach(std::function<void(const Entry &)> sink);

private:
  void append(const Entry &entry);

  size_t capacity;
  std::deque<Entry> log;
  bool haveConfig;
  uint64_t nDropped;
  std::function<void(const Entry &)> sink;
  mutable std::mutex mutex;
};

extern TuningLog g_tuningLog;

#endif
