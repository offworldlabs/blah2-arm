#include "capture/PairedCpiQueue.h"
#include <array>
#include <iostream>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

static void require(bool condition, const char *message)
{
  if (!condition) throw std::runtime_error(message);
}

int main()
{
  PairedCpiQueue queue(4, 3, 12);
  std::array<int16_t, 4 * 12> samples{};
  for (size_t i = 0; i < samples.size(); ++i) samples[i] = int16_t(i);
  queue.push(samples.data(), 6, 100);
  queue.push(samples.data() + 24, 6, 106);
  size_t slot = 0;
  require(queue.acquire(slot, false), "first complete CPI missing");
  require(queue.block(slot).first == 100 && queue.block(slot).epoch == 0,
    "first CPI metadata differs");
  require(queue.block(slot).iq[15] == 15, "paired IQ byte order differs");
  queue.release(slot);
  require(queue.acquire(slot, false), "second CPI missing");
  require(queue.block(slot).first == 104, "split callback index differs");
  queue.release(slot);
  queue.push(samples.data(), 1, 200);
  auto stats = queue.stats();
  require(stats.discontinuities == 1 && stats.discarded_samples == 4,
    "gap must discard the complete queued CPI and partial tail");
  queue.push(samples.data(), 4, 201);
  require(queue.acquire(slot, false), "new epoch CPI missing");
  require(queue.block(slot).first == 200 && queue.block(slot).epoch == 1,
    "new epoch metadata differs");
  queue.release(slot);
  queue.discard_pending();
  require(queue.stats().discarded_samples == 5, "partial discard accounting differs");
  queue.close();
  require(!queue.acquire(slot), "closed queue should unblock empty consumer");
  bool rejected = false;
  try { queue.push(samples.data(), 1, 0); }
  catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "producer accepted closed queue");

  PairedCpiQueue bounded(4, 1);
  bounded.push(samples.data(), 4, 0);
  bounded.push(samples.data() + 16, 4, 4);
  require(bounded.stats().dropped_cpis == 1
    && bounded.stats().discarded_samples == 4, "overflow accounting differs");
  require(bounded.acquire(slot, false) && bounded.block(slot).first == 4,
    "overflow kept stale frame");
  bounded.release(slot);

  // A complete callback owns the partial-CPI state until close can account
  // its tail. Closing from another thread must wait, with no partial races.
  constexpr uint32_t kCount = 1000003;
  PairedCpiQueue concurrent(64, 2);
  std::vector<int16_t> concurrentSamples(size_t(kCount) * 4, 7);
  std::atomic<bool> producerDone{false};
  std::thread producer([&] {
    concurrent.push(concurrentSamples.data(), kCount, 0);
    producerDone.store(true);
  });
  while (concurrent.stats().published < 8 && !producerDone.load())
    std::this_thread::yield();
  require(concurrent.stats().published >= 8, "producer did not enter callback");
  concurrent.close();
  producer.join();
  auto finished = concurrent.stats();
  require(producerDone.load() && finished.published == kCount / 64,
    "close interrupted an active callback");
  require(finished.discarded_samples == finished.dropped_cpis * 64 + kCount % 64,
    "concurrent close lost partial-sample accounting");
  require(concurrent.acquire(slot, false), "close lost a complete ready CPI");
  concurrent.release(slot);

  // Cancellation discards complete queued frames immediately but leaves the
  // producer-owned partial tail for the eventual synchronized close.
  PairedCpiQueue cancelled(4, 2);
  cancelled.push(samples.data(), 5, 0);
  cancelled.cancel();
  require(!cancelled.acquire(slot, false), "cancel delivered stale CPI");
  auto beforeClose = cancelled.stats();
  require(beforeClose.dropped_cpis == 1 && beforeClose.discarded_samples == 4,
    "cancel did not account discarded complete CPI");
  cancelled.close();
  require(cancelled.stats().discarded_samples == 5,
    "cancelled partial tail not accounted at close");
  rejected = false;
  try { cancelled.push(samples.data(), 1, 5); }
  catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "cancelled queue accepted new callback");

  PairedCpiQueue cancelConcurrent(64, 2);
  std::atomic<bool> cancelProducerDone{false};
  std::thread cancelledProducer([&] {
    try { cancelConcurrent.push(concurrentSamples.data(), kCount, 0); }
    catch (const std::runtime_error &) {}
    cancelProducerDone.store(true);
  });
  while (cancelConcurrent.stats().published < 8 && !cancelProducerDone.load())
    std::this_thread::yield();
  require(cancelConcurrent.stats().published >= 8,
    "cancelled producer did not enter callback");
  cancelConcurrent.cancel();
  cancelledProducer.join();
  cancelConcurrent.close();
  require(!cancelConcurrent.acquire(slot, false),
    "concurrent cancel delivered a stale CPI");
  require(cancelConcurrent.stats().discarded_samples == kCount,
    "concurrent cancel hid received callback samples");
  std::cout << "{\"queue_cases\":5,\"pass\":true}\n";
}
