#include "FftLength.h"

#include <algorithm>
#include <chrono>
#include <iostream>

#include <fftw3.h>

namespace blah2 {

std::vector<uint32_t> fftLengthCandidates(uint64_t minimum, double slack) {
  constexpr uint64_t limit = std::numeric_limits<int>::max();
  if (!minimum || minimum > limit)
    throw std::invalid_argument("FFT length must fit a positive FFTW int");
  if (!(slack >= 0.0))
    throw std::invalid_argument("FFT length slack must not be negative");

  const uint64_t ceiling =
      std::min(limit, minimum + static_cast<uint64_t>(double(minimum) * slack));

  // The unpadded length always stays in the running; padding is only ever an
  // optimisation, never a correctness requirement.
  std::vector<uint32_t> lengths{static_cast<uint32_t>(minimum)};
  for (uint64_t extra : {1u, 11u, 13u})
    for (uint64_t a = extra; a <= ceiling; a *= 2)
      for (uint64_t b = a; b <= ceiling; b *= 3)
        for (uint64_t c = b; c <= ceiling; c *= 5)
          for (uint64_t d = c; d <= ceiling; d *= 7)
            if (d >= minimum) lengths.push_back(static_cast<uint32_t>(d));

  std::sort(lengths.begin(), lengths.end());
  lengths.erase(std::unique(lengths.begin(), lengths.end()), lengths.end());
  return lengths;
}

uint32_t fastestFftLength(uint64_t minimum, double slack) {
  const std::vector<uint32_t> lengths = fftLengthCandidates(minimum, slack);

  uint32_t best = lengths.front();
  double bestSeconds = -1.0;
  for (uint32_t n : lengths) {
    auto *buffer =
        static_cast<fftw_complex *>(fftw_malloc(sizeof(fftw_complex) * size_t(n)));
    if (buffer == nullptr) continue;

    fftw_plan forward =
        fftw_plan_dft_1d(int(n), buffer, buffer, FFTW_FORWARD, FFTW_ESTIMATE);
    fftw_plan backward =
        fftw_plan_dft_1d(int(n), buffer, buffer, FFTW_BACKWARD, FFTW_ESTIMATE);
    if (forward != nullptr && backward != nullptr) {
      for (uint32_t i = 0; i < n; i++) {
        buffer[i][0] = double(i) / double(n);
        buffer[i][1] = 0.0;
      }
      fftw_execute(forward);  // discard the first pass, which warms the caches

      const auto start = std::chrono::steady_clock::now();
      fftw_execute(forward);
      fftw_execute(forward);
      fftw_execute(backward);  // the two-forward, one-backward mix of a CPI
      const double seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
              .count();

      if (bestSeconds < 0.0 || seconds < bestSeconds) {
        bestSeconds = seconds;
        best = n;
      }
    }

    if (forward != nullptr) fftw_destroy_plan(forward);
    if (backward != nullptr) fftw_destroy_plan(backward);
    fftw_free(buffer);
  }

  // Nothing could be planned or allocated; fall back to the static choice.
  if (bestSeconds < 0.0) return nextFastFftLength(minimum);

  std::cout << "Clutter filter FFT length " << best << " (from " << lengths.size()
            << " candidates >= " << minimum << ", " << bestSeconds * 1000.0
            << " ms per CPI of transforms)" << std::endl;
  return best;
}
}
