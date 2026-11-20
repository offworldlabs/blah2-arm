// TuningLog, the per-run record of every tuning change.
//
// A frame's tuning is read from this log by timestamp, so what matters is that
// every change is kept in order with the fields its kind carries, that the
// configured baseline can never be lost, and that writers on the capture and
// SDRplay callback threads cannot corrupt it.
#include "data/TuningLog.h"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Kind = TuningLog::Kind;

static void require(bool condition, const std::string &message)
{
  if (!condition) throw std::runtime_error(message);
}

static void order_and_fields()
{
  TuningLog log;
  log.set_config(1000, 503000000u, 40, 40, 4);
  log.add_gain_event(1001, 1, 40, 0, 52.5);
  log.add_gain_event(1001, 2, 40, 0, 52.4);
  log.add_retune(2000, 1, 213000000u, 30, 35, 6);
  log.add_gain_event(2001, 1, 30, 12, 44.0);

  const std::vector<TuningLog::Entry> e = log.entries();
  require(e.size() == 5, "entry count");

  require(e[0].kind == Kind::Config && e[0].timestamp == 1000, "config first");
  require(e[0].fc == 503000000u && e[0].gainReductionA == 40 && e[0].gainReductionB == 40
          && e[0].lnaState == 4, "config fields");
  require(e[0].tuner == 0 && e[0].systemGainDb == 0.0 && e[0].generation == 0,
          "config carries no event or retune fields");

  require(e[1].kind == Kind::GainEvent && e[1].tuner == 1 && e[1].gRdB == 40
          && e[1].lnaGRdB == 0 && e[1].systemGainDb == 52.5, "gain event fields");
  require(e[1].fc == 0 && e[1].gainReductionA == 0, "gain event carries no config fields");
  require(e[2].tuner == 2, "second tuner");

  require(e[3].kind == Kind::Retune && e[3].generation == 1 && e[3].fc == 213000000u
          && e[3].gainReductionA == 30 && e[3].gainReductionB == 35 && e[3].lnaState == 6,
          "retune fields");
  require(e[4].timestamp == 2001 && e[4].lnaGRdB == 12, "order kept");
  require(log.get_n_dropped() == 0, "nothing dropped");
}

// Full, the oldest change goes and the config baseline stays, so the log
// still describes every frame back to startup.
static void bounded_keeps_config()
{
  TuningLog log(4);
  log.set_config(1, 100u, 1, 2, 3);
  for (uint64_t t = 10; t < 20; t++)
    log.add_gain_event(t, 1, uint32_t(t), 0, 0.0);
  const std::vector<TuningLog::Entry> e = log.entries();
  require(e.size() == 4, "capacity respected");
  require(e[0].kind == Kind::Config && e[0].fc == 100u, "config evicted");
  require(e[1].timestamp == 17 && e[2].timestamp == 18 && e[3].timestamp == 19,
          "the newest changes should survive, in order");
  require(log.get_n_dropped() == 7, "drops counted");
}

// A gain event can arrive before main records the config. The config must
// still be first.
static void config_after_event()
{
  TuningLog log;
  log.add_gain_event(5, 1, 40, 0, 50.0);
  log.set_config(4, 503000000u, 40, 40, 4);
  const std::vector<TuningLog::Entry> e = log.entries();
  require(e.size() == 2 && e[0].kind == Kind::Config, "config not first");

  bool threw = false;
  try { log.set_config(6, 1u, 0, 0, 0); }
  catch (const std::logic_error &) { threw = true; }
  require(threw, "a second config entry should be refused");

  bool threwSmall = false;
  try { TuningLog tiny(1); }
  catch (const std::invalid_argument &) { threwSmall = true; }
  require(threwSmall, "a log too small for the config and one change should be refused");
}

// The retune thread and the SDRplay callback append concurrently while a
// reader snapshots. Nothing may be lost or reordered within a writer.
static void concurrent()
{
  const uint64_t perWriter = 5000;
  TuningLog log(2 * perWriter + 1);
  log.set_config(0, 1u, 0, 0, 0);
  std::thread a([&] {
    for (uint64_t i = 1; i <= perWriter; i++) log.add_retune(i, int64_t(i), 0, 0, 0, 0);
  });
  std::thread b([&] {
    for (uint64_t i = 1; i <= perWriter; i++) log.add_gain_event(i, 1, uint32_t(i), 0, 0.0);
  });
  std::thread reader([&] {
    for (int k = 0; k < 200; k++) (void)log.entries();
  });
  a.join();
  b.join();
  reader.join();

  const std::vector<TuningLog::Entry> e = log.entries();
  require(e.size() == 2 * perWriter + 1, "entries lost");
  int64_t lastGeneration = 0;
  uint32_t lastGrdb = 0;
  for (size_t k = 1; k < e.size(); k++)
  {
    if (e[k].kind == Kind::Retune)
    {
      require(e[k].generation == lastGeneration + 1, "retunes reordered");
      lastGeneration = e[k].generation;
    }
    else
    {
      require(e[k].gRdB == lastGrdb + 1, "gain events reordered");
      lastGrdb = e[k].gRdB;
    }
  }
}

int main()
{
  try
  {
    order_and_fields();
    bounded_keeps_config();
    config_after_event();
    concurrent();
  }
  catch (const std::exception &e)
  {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
  std::printf("testTuningLog: all checks passed\n");
  return 0;
}
