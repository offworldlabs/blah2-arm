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
#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace blah2
{

/// @brief During streaming keep the legacy lookahead sample; after a finite
/// capture ends, accept a final exact CPI and then terminate cleanly.
inline bool fifo_frame_ready(uint32_t a, uint32_t b, uint32_t n, bool done)
{
  return done ? a >= n && b >= n : a > n && b > n;
}

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

  /// @brief Sample sequence of a raw paired CPI. Valid only in queue mode.
  uint32_t firstSample = 0;
  uint64_t captureEpoch = 0;
  bool captureDiscontinuity = false;
  bool pairedCapture = false;

  /// @brief Ready the slot for a fresh CPI.
  void reset()
  {
    time.clear();
    timingName.clear();
    timingTime.clear();
    fcChanged = false;
    fc = 0;
    firstSample = 0;
    captureEpoch = 0;
    captureDiscontinuity = false;
    pairedCapture = false;
  }
};

/// @brief Blocking handoff between pipeline stages.
/// @details The fixed slot count bounds storage. Closing unblocks consumers
/// after queued slots drain, including when a worker fails.
template <typename T>
class BlockingQueue
{
public:
  bool push(T value)
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (closed) return false;
      queue.push_back(std::move(value));
    }
    condition.notify_one();
    return true;
  }

  T pop()
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [this] { return !queue.empty() || closed; });
    if (queue.empty()) return T{};
    T value = std::move(queue.front());
    queue.pop_front();
    return value;
  }

  void close()
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      closed = true;
    }
    condition.notify_all();
  }

private:
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<T> queue;
  bool closed = false;
};

/// @brief Coordinate first worker failure and unblock both pipeline stages.
/// @details Stop hooks are called once. Each is attempted even if another
/// hook throws, so capture teardown cannot prevent queue wakeups.
class PipelineStop
{
public:
  PipelineStop(std::function<void()> stopCapture,
    std::function<void()> closeInput,
    std::function<void()> closeFree,
    std::function<void()> closeFiltered)
    : stopCapture_(std::move(stopCapture)), closeInput_(std::move(closeInput)),
      closeFree_(std::move(closeFree)), closeFiltered_(std::move(closeFiltered)) {}

  bool stopping() const noexcept { return stopping_.load(); }

  template <typename F>
  void run(F &&work) noexcept
  {
    try { work(); }
    catch (...) { fail(std::current_exception()); }
  }

  void fail(std::exception_ptr error) noexcept
  {
    try
    {
      std::lock_guard<std::mutex> lock(errorMutex_);
      if (!error_) error_ = error;
    }
    catch (...) {}
    stop();
  }

  void stop() noexcept
  {
    if (stopping_.exchange(true)) return;
    try { stopCapture_(); } catch (...) {}
    try { closeInput_(); } catch (...) {}
    try { closeFree_(); } catch (...) {}
    try { closeFiltered_(); } catch (...) {}
  }

  std::exception_ptr error() const
  {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return error_;
  }

private:
  std::function<void()> stopCapture_, closeInput_, closeFree_, closeFiltered_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex errorMutex_;
  std::exception_ptr error_;
};

}

#endif
