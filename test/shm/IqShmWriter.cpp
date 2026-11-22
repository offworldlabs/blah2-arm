// Writer stand-in for python/test_blah2_iq.py, built by test/shm/run.sh (not
// CMake, whose test runner executes every binary it builds).
// Usage: IqShmWriter <path> <nSlots> <nSamples> <nFrames> <delayMs> <runId>
#include "data/IqCache.h"
#include "data/IqData.h"
#include "data/TuningLog.h"

#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

static int16_t pattern(uint64_t frame, uint32_t i, uint32_t c)
{
  const uint64_t v = frame * 7919u + uint64_t(i) * 31u + c * 7u;
  return static_cast<int16_t>(int64_t(v % 65536u) - 32768);
}

int main(int argc, char **argv)
{
  if (argc != 7)
  {
    std::fprintf(stderr, "usage: %s path nSlots nSamples nFrames delayMs runId\n", argv[0]);
    return 2;
  }
  const std::string path = argv[1];
  const uint32_t nSlots = uint32_t(std::stoul(argv[2]));
  const uint32_t nSamples = uint32_t(std::stoul(argv[3]));
  const uint64_t nFrames = std::stoull(argv[4]);
  const int delayMs = std::stoi(argv[5]);

  IqCache::Run run;
  run.path = path;
  run.fs = 2000000;
  run.cpi = 0.5;
  run.runId = std::stoull(argv[6]);
  run.imageTag = "blah2:shm-test";
  run.configText = "capture:\n  fs: 2000000\n";
  IqCache cache(nSlots, nSamples, run);

  TuningLog log;
  log.set_config(500, 503000000u, 40, 40, 4);
  log.attach([&cache](const TuningLog::Entry &e) { cache.append_tuning(e); });

  IqData x(nSamples), y(nSamples);
  for (uint64_t f = 0; f < nFrames; f++)
  {
    x.clear();
    y.clear();
    for (uint32_t i = 0; i < nSamples; i++)
    {
      x.push_back(std::complex<double>(pattern(f, i, 0), pattern(f, i, 1)));
      y.push_back(std::complex<double>(pattern(f, i, 2), pattern(f, i, 3)));
    }
    cache.write(x, y, 1000 + f);
    if (f % 10 == 0)
      log.add_gain_event(1000 + f, f % 20 == 0 ? 1 : 2, 40, 6, 58.0);
    if (delayMs > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
  }
  std::printf("wrote %llu frames\n", (unsigned long long)nFrames);
  return 0;
}
