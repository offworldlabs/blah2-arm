#pragma once

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace blah2 {
// Threads handed to the FFTW planner. Which transform length is quickest
// depends on this, so it belongs anywhere the choice is made or remembered.
inline constexpr int kPlannerThreads = 4;

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

// Padded lengths worth considering: the admissible sizes within `slack` of the
// minimum, plus the minimum itself, which is often quickest despite factoring
// badly. Sorted ascending.
std::vector<uint32_t> fftLengthCandidates(uint64_t minimum, double slack = 0.02);

// Where measured lengths are remembered between runs. BLAH2_FFT_CACHE overrides
// the default, which sits in the save directory so it survives a container
// restart.
std::string fftLengthCachePath();

// Returns the quickest candidate, measuring only when the answer is not already
// cached for this geometry, thread count and FFTW build.
//
// Once fftw_plan_with_nthreads() is in play, transform cost stops being
// predictable from the factorisation: FFTW parallelises across a Cooley-Tukey
// factor, so how evenly the factors divide the thread count matters more than
// how small they are. Measured on a Pi 5 at the shipped clutter geometry, 2
// forward plus 1 backward, the smallest admissible length (1002375 =
// 3^6*5^3*11) took 393.63 ms against 255.86 ms for the unpadded 1000411 =
// 269*3719, while 1016064 = 2^8*3^4*7^2 took 94.24 ms. Reversing the thread
// count reverses the ranking, so the length has to be measured on the machine
// and thread count that will run it.
//
// A stale or unreadable cache costs speed, never correctness: every candidate
// is at least the alias-free convolution length, so any of them filters
// correctly.
uint32_t fastestFftLength(uint64_t minimum, double slack = 0.02,
                          int threads = kPlannerThreads);
}
