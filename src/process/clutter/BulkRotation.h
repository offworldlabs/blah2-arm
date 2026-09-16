#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace owl_clutter {

// Copy a signed circular rotation in contiguous spans. This preserves the
// WienerHopf convention dest[i] = src[(i - delay) mod samples] without doing a
// modulo operation for every sample.
template <class Container, class Value>
void copyRotation(const Container& source, uint32_t samples, int32_t delay,
                  Value* destination)
{
  if (!samples || source.size() < samples || !destination)
    throw std::invalid_argument("Invalid clutter rotation span");

  const int64_t signedStart = (-int64_t(delay)) % int64_t(samples);
  uint32_t position = static_cast<uint32_t>(
      signedStart < 0 ? signedStart + samples : signedStart);
  uint32_t copied = 0;
  while (copied < samples)
  {
    const uint32_t count = std::min(samples - copied, samples - position);
    std::copy_n(source.begin() + position, count, destination + copied);
    copied += count;
    position = 0;
  }
}

}  // namespace owl_clutter
