#include "TuningLog.h"

#include <stdexcept>

TuningLog::TuningLog(size_t _capacity)
{
  if (_capacity < 2)
    throw std::invalid_argument("TuningLog needs room for the config entry and one change");
  capacity = _capacity;
  haveConfig = false;
  nDropped = 0;
}

void TuningLog::set_config(uint64_t timestamp, uint32_t fc, int32_t gainReductionA,
  int32_t gainReductionB, int32_t lnaState)
{
  std::lock_guard<std::mutex> lock(mutex);
  if (haveConfig)
    throw std::logic_error("TuningLog config entry set twice");
  Entry entry{};
  entry.timestamp = timestamp;
  entry.kind = Kind::Config;
  entry.fc = fc;
  entry.gainReductionA = gainReductionA;
  entry.gainReductionB = gainReductionB;
  entry.lnaState = lnaState;
  log.push_front(entry);
  haveConfig = true;
}

void TuningLog::add_retune(uint64_t timestamp, int64_t generation, uint32_t fc,
  int32_t gainReductionA, int32_t gainReductionB, int32_t lnaState)
{
  Entry entry{};
  entry.timestamp = timestamp;
  entry.kind = Kind::Retune;
  entry.generation = generation;
  entry.fc = fc;
  entry.gainReductionA = gainReductionA;
  entry.gainReductionB = gainReductionB;
  entry.lnaState = lnaState;
  append(entry);
}

void TuningLog::add_gain_event(uint64_t timestamp, uint8_t tuner, uint32_t gRdB,
  uint32_t lnaGRdB, double systemGainDb)
{
  Entry entry{};
  entry.timestamp = timestamp;
  entry.kind = Kind::GainEvent;
  entry.tuner = tuner;
  entry.gRdB = gRdB;
  entry.lnaGRdB = lnaGRdB;
  entry.systemGainDb = systemGainDb;
  append(entry);
}

void TuningLog::append(const Entry &entry)
{
  std::lock_guard<std::mutex> lock(mutex);
  if (log.size() == capacity)
  {
    log.erase(log.begin() + (haveConfig ? 1 : 0));
    nDropped++;
  }
  log.push_back(entry);
  if (sink)
    sink(entry);
}

void TuningLog::attach(std::function<void(const Entry &)> _sink)
{
  std::lock_guard<std::mutex> lock(mutex);
  if (!haveConfig)
    throw std::logic_error("TuningLog::attach before set_config");
  for (const Entry &entry : log)
    _sink(entry);
  sink = std::move(_sink);
}

std::vector<TuningLog::Entry> TuningLog::entries() const
{
  std::lock_guard<std::mutex> lock(mutex);
  return std::vector<Entry>(log.begin(), log.end());
}

uint64_t TuningLog::get_n_dropped() const
{
  std::lock_guard<std::mutex> lock(mutex);
  return nDropped;
}
