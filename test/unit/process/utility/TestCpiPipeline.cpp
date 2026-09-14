// The radar loop processes a CPI in two overlapped stages rather than one
// serial run, so throughput is the slowest stage instead of their sum. That is
// only output-neutral if the handoff preserves what the serial loop guaranteed:
// every CPI reaches the back stage exactly once, in capture order, with its own
// timing and retune state, and dropped CPIs return their slot without losing a
// pending retune. This exercises those properties with the stages replaced by
// sleeps of the measured owl-ded9 durations.

#include "process/utility/CpiPipeline.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using blah2::BlockingQueue;
using blah2::CpiSlot;

static int failures = 0;

static void require(bool condition, const char* message)
{
  std::printf("%-62s %s\n", message, condition ? "ok" : "FAIL");
  if (!condition) failures++;
}

static uint64_t now_us()
{
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static void work(double ms)
{
  std::this_thread::sleep_for(std::chrono::microseconds((long)(ms * 1000)));
}

// Measured stage times on owl-ded9, scaled so the test runs in seconds. Only
// the ratio between the two stages matters to what is being checked.
static constexpr double kScale = 0.02;
static constexpr double kExtract = 36.0 * kScale;
static constexpr double kClutter = 461.3 * kScale;
static constexpr double kBackStage = (68.5 + 240.4 + 3.3 + 22.0) * kScale;

static constexpr int kCpis = 40;
static constexpr uint64_t kFailingCpi = 7;

int main()
{
  constexpr size_t kPipelineDepth = 2;
  std::vector<std::unique_ptr<CpiSlot>> slots;
  BlockingQueue<CpiSlot*> freeSlots;
  BlockingQueue<CpiSlot*> filteredSlots;
  for (size_t i = 0; i < kPipelineDepth; i++)
  {
    slots.push_back(std::make_unique<CpiSlot>(1));
    freeSlots.push(slots.back().get());
  }

  std::atomic<bool> fcChanged{false};
  std::atomic<bool> stop{false};
  std::vector<uint64_t> backOrder;
  std::vector<uint64_t> retunesApplied;
  uint64_t nextCpiId = 0;
  uint64_t t0 = now_us();

  // Front stage: extract, then clutter filter. CPI kFailingCpi fails the
  // filter, with a retune raised just before it.
  std::thread front([&] {
    while (!stop.load())
    {
      CpiSlot* slot = freeSlots.pop();
      if (stop.load()) break;
      slot->reset();
      slot->time.push_back(now_us());
      uint64_t id = nextCpiId++;
      work(kExtract);
      slot->timingName.push_back("extract_buffer");
      slot->timingTime.push_back(kExtract);

      if (id == kFailingCpi) fcChanged.store(true);
      slot->fcChanged = fcChanged.exchange(false);
      if (slot->fcChanged) slot->fc = 500000000u + (uint32_t)id;

      work(kClutter);
      if (id == kFailingCpi)
      {
        if (slot->fcChanged) fcChanged.store(true);
        slot->x->clear();
        slot->y->clear();
        freeSlots.push(slot);
        continue;
      }
      slot->timingName.push_back("clutter_filter");
      slot->timingTime.push_back(kClutter);
      slot->time.push_back(id);  // stands in for the CPI's payload
      filteredSlots.push(slot);
      if ((int)id >= kCpis) stop.store(true);
    }
  });

  // Back stage: everything downstream, in capture order, holding the state the
  // tracker would.
  std::thread back([&] {
    uint64_t lastSeen = 0;
    bool first = true;
    for (int seen = 0; seen < kCpis - 1; seen++)
    {
      CpiSlot* slot = filteredSlots.pop();
      uint64_t id = slot->time[1];
      backOrder.push_back(id);
      if (slot->fcChanged) retunesApplied.push_back(id);
      if (!first && id <= lastSeen) failures++;
      lastSeen = id;
      first = false;
      work(kBackStage);
      slot->x->clear();
      slot->y->clear();
      freeSlots.push(slot);
    }
  });

  back.join();
  uint64_t elapsed = now_us() - t0;
  stop.store(true);
  freeSlots.push(slots[0].get());  // let the front stage observe stop and exit
  front.join();

  const double perCpiMs = (double)elapsed / 1000.0 / (kCpis - 1);
  const double serialMs = kExtract + kClutter + kBackStage;
  const double frontMs = kExtract + kClutter;

  std::printf("throughput %.2f ms/CPI, serial would be %.2f, front stage %.2f\n\n",
              perCpiMs, serialMs, frontMs);

  require(backOrder.size() == (size_t)(kCpis - 1), "back stage saw every CPI exactly once");

  bool ordered = true;
  for (size_t i = 1; i < backOrder.size(); i++)
    if (backOrder[i] <= backOrder[i - 1]) ordered = false;
  require(ordered, "CPIs reached the back stage in capture order");

  bool contiguous = true;
  for (size_t i = 0; i < backOrder.size(); i++)
  {
    const uint64_t expect = i < kFailingCpi ? i : i + 1;
    if (backOrder[i] != expect) contiguous = false;
  }
  require(contiguous, "only the dropped CPI is missing, no others");

  require(retunesApplied.size() == 1 && retunesApplied[0] == kFailingCpi + 1,
          "retune survived the dropped CPI and landed on the next");

  require(perCpiMs < serialMs * 0.85, "throughput beats the serial sum of stages");
  require(perCpiMs > frontMs * 0.85 && perCpiMs < frontMs * 1.35,
          "throughput tracks the slow stage, not the sum");

  std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
  return failures ? 1 : 0;
}
