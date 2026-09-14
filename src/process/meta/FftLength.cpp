#include "FftLength.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

#include <fftw3.h>

namespace blah2 {
namespace {

// Keep a handful of geometries so that moving a node between configurations,
// or back again, does not force a re-measurement each time.
constexpr size_t kMaxCacheEntries = 8;

// Everything that can change which length wins. The FFTW build is in here
// because the codelets it ships decide the ranking; the thread count is in here
// because the ranking inverts between 1 and 4 threads.
std::string cacheKey(uint64_t minimum, double slack, int threads) {
  std::ostringstream key;
  key << minimum << ' ' << std::fixed << std::setprecision(6) << slack << ' '
      << threads << ' ' << fftw_version;
  return key.str();
}

// "<length> <key>", with the key last so an FFTW version string containing
// spaces still round-trips.
std::vector<std::pair<uint32_t, std::string>> readCache(const std::string& path) {
  std::vector<std::pair<uint32_t, std::string>> entries;
  std::ifstream file(path);
  std::string line;
  while (entries.size() < kMaxCacheEntries && std::getline(file, line)) {
    const size_t split = line.find(' ');
    if (split == std::string::npos) continue;
    uint64_t length = 0;
    std::istringstream parse(line.substr(0, split));
    if (!(parse >> length) || length == 0 ||
        length > uint64_t(std::numeric_limits<int>::max()))
      continue;  // a corrupt line only ever costs a re-measurement
    entries.emplace_back(static_cast<uint32_t>(length), line.substr(split + 1));
  }
  return entries;
}

void writeCache(const std::string& path,
                std::vector<std::pair<uint32_t, std::string>> entries,
                uint32_t length, const std::string& key) {
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&key](const std::pair<uint32_t, std::string>& entry) {
                                 return entry.second == key;
                               }),
                entries.end());
  entries.emplace_back(length, key);
  if (entries.size() > kMaxCacheEntries)
    entries.erase(entries.begin(), entries.end() - kMaxCacheEntries);

  // A missing or read-only save directory just means measuring every start.
  std::ofstream file(path, std::ios::trunc);
  if (!file) return;
  for (const std::pair<uint32_t, std::string>& entry : entries)
    file << entry.first << ' ' << entry.second << '\n';
}

}  // namespace

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

std::string fftLengthCachePath() {
  if (const char* override = std::getenv("BLAH2_FFT_CACHE"))
    if (*override != '\0') return override;
  return "/opt/blah2/save/fft-length.cache";
}

uint32_t fastestFftLength(uint64_t minimum, double slack, int threads) {
  const std::vector<uint32_t> lengths = fftLengthCandidates(minimum, slack);
  const std::string path = fftLengthCachePath();
  const std::string key = cacheKey(minimum, slack, threads);
  const std::vector<std::pair<uint32_t, std::string>> cached = readCache(path);

  for (const std::pair<uint32_t, std::string>& entry : cached) {
    // Only trust a remembered length that is still long enough to filter
    // correctly, whatever else may have changed.
    if (entry.second == key && entry.first >= minimum) {
      std::cout << "Clutter filter FFT length " << entry.first << " (cached in "
                << path << ")" << std::endl;
      return entry.first;
    }
  }

  uint32_t best = lengths.front();
  double bestSeconds = -1.0;
  for (uint32_t n : lengths) {
    auto* buffer =
        static_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * size_t(n)));
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

  // Nothing could be planned or allocated; fall back to the static choice
  // without poisoning the cache with a length we never measured.
  if (bestSeconds < 0.0) return nextFastFftLength(minimum);

  writeCache(path, cached, best, key);
  std::cout << "Clutter filter FFT length " << best << " (measured from "
            << lengths.size() << " candidates >= " << minimum << ", "
            << bestSeconds * 1000.0 << " ms per CPI of transforms, cached in "
            << path << ")" << std::endl;
  return best;
}
}
