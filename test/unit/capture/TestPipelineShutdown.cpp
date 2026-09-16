#include "process/utility/CpiPipeline.h"
#include "capture/PairedCpiQueue.h"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

static void require(bool ok, const char *why)
{
  if (!ok) throw std::runtime_error(why);
}

static std::string failure(blah2::PipelineStop &stop)
{
  try { std::rethrow_exception(stop.error()); }
  catch (const std::exception &e) { return e.what(); }
  return "unknown";
}

static void injected_failure(bool frontFails)
{
  blah2::BlockingQueue<int *> freeSlots, filteredSlots;
  PairedCpiQueue raw(4, 2);
  const int16_t iq[16]{};
  raw.push(iq, 4, 0);
  std::atomic<int> captureStopCalls{0};
  std::mutex captureMutex;
  std::condition_variable captureWake;
  blah2::PipelineStop stop(
    [&] { ++captureStopCalls; captureWake.notify_all(); },
    [&] { raw.cancel(); },
    [&] { freeSlots.close(); },
    [&] { filteredSlots.close(); });
  std::thread capture([&] {
    std::unique_lock<std::mutex> lock(captureMutex);
    captureWake.wait(lock, [&] { return captureStopCalls.load() > 0; });
  });
  std::thread front([&] {
    stop.run([&] {
      int *slot = freeSlots.pop();
      if (!slot) return;
      if (frontFails) throw std::runtime_error("injected front DSP failure");
      filteredSlots.push(slot);
      // A downstream failure must wake this producer even if no slot returns.
      freeSlots.pop();
    });
  });
  std::thread back([&] {
    stop.run([&] {
      int *slot = filteredSlots.pop();
      if (!slot) return;
      throw std::runtime_error("injected back DSP failure");
    });
  });
  int slot = 7;
  require(freeSlots.push(&slot), "initial slot rejected");
  front.join(); back.join(); capture.join();
  require(captureStopCalls.load() == 1, "capture stop was not requested once");
  require(stop.stopping(), "pipeline did not stop");
  require(!freeSlots.push(&slot) && !filteredSlots.push(&slot),
    "handoff queues accepted work after failure");
  size_t rawSlot = 0;
  require(!raw.acquire(rawSlot, false),
    "cancelled input delivered a stale frame");
  const auto message = failure(stop);
  require(message == (frontFails ? "injected front DSP failure" :
    "injected back DSP failure"), "first worker error was lost");
}

int main()
{
  injected_failure(true);
  injected_failure(false);
  require(!blah2::fifo_frame_ready(4, 4, 4, false),
    "legacy stream lost its lookahead rule");
  require(blah2::fifo_frame_ready(5, 5, 4, false),
    "legacy stream rejected a ready frame");
  require(blah2::fifo_frame_ready(4, 4, 4, true),
    "finite replay lost an exact last CPI");
  require(!blah2::fifo_frame_ready(4, 3, 4, true),
    "finite replay accepted an incomplete pair");
  std::cout << "{\"shutdown_injections\":2,\"finite_replay_checks\":4,\"pass\":true}\n";
}
