#pragma once

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>

namespace blah2 {
// FFTW supports these small factors efficiently. Permit at most one factor of
// 11 or 13. Enumerate bounded candidates instead of an unbounded integer scan.
inline uint32_t nextFastFftLength(uint64_t minimum) {
  constexpr uint64_t limit = std::numeric_limits<int>::max();
  if (!minimum || minimum > limit)
    throw std::invalid_argument("FFT length must fit a positive FFTW int");
  uint64_t best = limit + 1;
  for (uint64_t extra : {1u, 11u, 13u})
    for (uint64_t a = extra; a <= limit && a < best; a *= 2)
      for (uint64_t b = a; b <= limit && b < best; b *= 3)
        for (uint64_t c = b; c <= limit && c < best; c *= 5)
          for (uint64_t d = c; d <= limit && d < best; d *= 7)
            if (d >= minimum) best = d;
  if (best > limit)
    throw std::invalid_argument("No supported padded FFT length fits FFTW");
  return static_cast<uint32_t>(best);
}
}
