#include "IqData.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filewritestream.h"

// constructor
IqData::IqData(uint32_t _n)
{
  if (_n == 0)
    throw std::invalid_argument("IqData needs a non-zero capacity");
  n = _n;
  // The one allocation this object ever makes. Committing it here rather than
  // growing into it also means RSS is flat from the first CPI instead of
  // climbing while the buffer fills.
  data.assign(n, std::complex<double>(0.0, 0.0));
  head = 0;
  count = 0;
}

void IqData::lock()
{
  mutex_lock.lock();
}

void IqData::unlock()
{
  mutex_lock.unlock();
}

std::vector<std::complex<double>> IqData::get_data() const
{
  std::vector<std::complex<double>> out;
  out.reserve(count);
  // At most two contiguous runs: head to the end of storage, then the wrap.
  const uint32_t first = std::min(count, n - head);
  out.insert(out.end(), data.begin() + head, data.begin() + head + first);
  if (first < count)
    out.insert(out.end(), data.begin(), data.begin() + (count - first));
  return out;
}

void IqData::push_back(std::complex<double> sample)
{
  if (count == n)
  {
    // Full, so drop the oldest to make room, exactly as the deque form did.
    // The slot holding the front is also the one past the back, so a single
    // write and a head bump does both halves.
    data[head] = sample;
    head = wrap(uint64_t(head) + 1);
  }
  else
  {
    data[wrap(uint64_t(head) + count)] = sample;
    count++;
  }
}

std::complex<double> IqData::pop_front()
{
  if (count == 0)
  {
    throw std::runtime_error("Attempting to pop from an empty buffer");
  }
  const std::complex<double> sample = data[head];
  head = wrap(uint64_t(head) + 1);
  count--;
  return sample;
}

void IqData::clear()
{
  // The deque form popped one sample at a time, which on a full CPI meant a
  // million pops and 31,250 chunk frees, then the refill allocated them all
  // back. Dropping the indices is the same thing in constant time.
  head = 0;
  count = 0;
}

void IqData::update_spectrum(std::vector<std::complex<double>> _spectrum)
{
  spectrum = _spectrum;
}

void IqData::update_frequency(std::vector<double> _frequency)
{
  frequency = _frequency;
}

std::string IqData::to_json(uint64_t timestamp)
{
  rapidjson::Document document;
  document.SetObject();
  rapidjson::Document::AllocatorType &allocator = document.GetAllocator();

  // store frequency array
  rapidjson::Value arrayFrequency(rapidjson::kArrayType);
  for (size_t i = 0; i < frequency.size(); i++)
  {
    arrayFrequency.PushBack(frequency[i], allocator);
  }

  // store spectrum array
  rapidjson::Value arraySpectrum(rapidjson::kArrayType);
  for (size_t i = 0; i < spectrum.size(); i++)
  {
    arraySpectrum.PushBack(10 * std::log10(std::abs(spectrum[i])), allocator);
  }

  document.AddMember("timestamp", timestamp, allocator);
  document.AddMember("min", min, allocator);
  document.AddMember("max", max, allocator);
  document.AddMember("mean", mean, allocator);
  document.AddMember("frequency", arrayFrequency, allocator);
  document.AddMember("spectrum", arraySpectrum, allocator);

  rapidjson::StringBuffer strbuf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
  writer.SetMaxDecimalPlaces(2);
  document.Accept(writer);

  return strbuf.GetString();
}
