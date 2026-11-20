#include "IqCache.h"

#include <algorithm>
#include <stdexcept>

namespace
{

/// @brief Convert one value to int16, clearing exact if it changes.
/// @details The RSPduo's samples are integers in int16 range, so for it this
/// is a plain cast. Anything else is rounded and clipped, and flagged, rather
/// than silently stored as a different value.
inline int16_t to_int16(double v, bool &exact)
{
  if (v >= -32768.0 && v <= 32767.0)
  {
    const int16_t s = static_cast<int16_t>(v);
    if (static_cast<double>(s) == v)
      return s;
    exact = false;
    // Round half away from zero. Cannot leave the range: |v| <= 32767.
    return static_cast<int16_t>(v < 0 ? v - 0.5 : v + 0.5);
  }
  exact = false;
  // NaN compares false above and lands here too, stored as the minimum.
  return v > 0 ? 32767 : -32768;
}

}

IqCache::IqCache(uint32_t _nSlots, uint32_t _nSamples)
{
  if (_nSlots == 0)
    throw std::invalid_argument("IqCache needs at least one slot");
  if (_nSamples == 0)
    throw std::invalid_argument("IqCache needs a non-zero CPI length");
  nSlots = _nSlots;
  nSamples = _nSamples;
  frameValues = size_t(4) * nSamples;
  // assign() writes every value, which commits the pages now rather than as
  // the ring first fills, so a node short of RAM fails at startup.
  data.assign(size_t(nSlots) * frameValues, 0);
  slots.assign(nSlots, Slot{FrameInfo{0, 0}, false});
  next = 0;
  nWritten = 0;
  nInexact = 0;
}

bool IqCache::write(const IqData &x, const IqData &y, uint64_t timestamp)
{
  if (x.get_length() < nSamples || y.get_length() < nSamples)
    throw std::invalid_argument("IqCache::write given fewer samples than a CPI");

  // Take the target slot out of the index first, so no reader can copy it
  // while it is being overwritten below.
  uint32_t target;
  uint64_t seq;
  {
    std::lock_guard<std::mutex> lock(mutex);
    target = next;
    seq = nWritten;
    slots[target].valid = false;
  }

  bool exact = true;
  int16_t *out = data.data() + size_t(target) * frameValues;
  for (uint32_t i = 0; i < nSamples; i++)
  {
    const std::complex<double> &a = x[i];
    const std::complex<double> &b = y[i];
    out[0] = to_int16(a.real(), exact);
    out[1] = to_int16(a.imag(), exact);
    out[2] = to_int16(b.real(), exact);
    out[3] = to_int16(b.imag(), exact);
    out += 4;
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    slots[target].info = FrameInfo{seq, timestamp};
    slots[target].valid = true;
    next = (target + 1 == nSlots) ? 0 : target + 1;
    nWritten = seq + 1;
    if (!exact)
      nInexact++;
  }
  return exact;
}

uint64_t IqCache::get_n_inexact() const
{
  std::lock_guard<std::mutex> lock(mutex);
  return nInexact;
}

std::vector<IqCache::FrameInfo> IqCache::index() const
{
  std::lock_guard<std::mutex> lock(mutex);
  std::vector<FrameInfo> frames;
  frames.reserve(nSlots);
  // From next round to next is oldest to newest; empty slots are skipped.
  for (uint32_t k = 0; k < nSlots; k++)
  {
    const Slot &slot = slots[(next + k) % nSlots];
    if (slot.valid)
      frames.push_back(slot.info);
  }
  return frames;
}

bool IqCache::read(uint64_t timestamp, FrameInfo &info, std::vector<int16_t> &out) const
{
  std::lock_guard<std::mutex> lock(mutex);
  for (uint32_t k = 0; k < nSlots; k++)
  {
    const Slot &slot = slots[k];
    if (slot.valid && slot.info.timestamp == timestamp)
    {
      info = slot.info;
      const int16_t *begin = data.data() + size_t(k) * frameValues;
      out.assign(begin, begin + frameValues);
      return true;
    }
  }
  return false;
}
