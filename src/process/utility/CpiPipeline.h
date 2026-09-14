/// @file CpiPipeline.h
/// @brief Slot and handoff machinery for processing CPIs in a pipeline.
/// @details The radar loop used to run every stage of a CPI back to back, so
/// throughput was the sum of the stages. Splitting it at the clutter filter
/// makes throughput the slowest stage instead. Output is unchanged: each stage
/// is deterministic, slots carry one CPI's state end to end, and everything
/// holding state across CPIs (the tracker) stays inside one serial stage, so
/// CPIs are still finished in the order they were captured.
/// @author Josh Poole

#ifndef CPIPIPELINE_H
#define CPIPIPELINE_H

#include "data/IqData.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace blah2
{

/// @brief One CPI in flight between pipeline stages.
struct CpiSlot
{
  explicit CpiSlot(uint32_t nSamples)
      : x(std::make_unique<IqData>(nSamples)), y(std::make_unique<IqData>(nSamples))
  {
  }

  /// @brief Reference channel for this CPI.
  std::unique_ptr<IqData> x;

  /// @brief Surveillance channel for this CPI, overwritten by the clutter filter.
  std::unique_ptr<IqData> y;

  /// @brief Stage boundary timestamps (us). time[0] is the extract start, and
  /// is the timestamp every output for this CPI is stamped with.
  std::vector<uint64_t> time;

  /// @brief Names of the stage timings accumulated so far.
  std::vector<std::string> timingName;

  /// @brief Stage timings (ms), in step with timingName.
  std::vector<double> timingTime;

  /// @brief True if a retune landed on this CPI.
  /// @details Latched by the front stage at extract rather than acted on there,
  /// because the tracker it resets lives in the back stage. Consuming the flag
  /// in one stage and acting on it in the other would apply the reset to
  /// whichever CPI happened to be in flight.
  bool fcChanged = false;

  /// @brief Centre frequency to adopt, valid when fcChanged.
  uint32_t fc = 0;

  /// @brief Ready the slot for a fresh CPI.
  void reset()
  {
    time.clear();
    timingName.clear();
    timingTime.clear();
    fcChanged = false;
    fc = 0;
  }
};

/// @brief Blocking handoff between pipeline stages.
/// @details No capacity limit is needed: the number of slots in circulation is
/// fixed at startup, so a stage that runs ahead blocks on the free list. The
/// radar loop runs until the process is killed, so there is no close path.
template <typename T>
class BlockingQueue
{
public:
  void push(T value)
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      queue.push_back(std::move(value));
    }
    condition.notify_one();
  }

  T pop()
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [this] { return !queue.empty(); });
    T value = std::move(queue.front());
    queue.pop_front();
    return value;
  }

private:
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<T> queue;
};

}

#endif
