// Proves to_json_km() is byte-identical to the to_json() + delay_bin_to_km()
// pair it replaces. The pair is kept solely as the oracle for this test.
#include "data/Map.h"
#include "data/Detection.h"

#include <complex>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using Complex = std::complex<double>;

static void compare(const std::string& oracle, const std::string& single,
                    const char* what) {
  if (oracle == single) return;
  // Report the first divergence rather than dumping two huge strings.
  size_t i = 0;
  while (i < oracle.size() && i < single.size() && oracle[i] == single[i]) ++i;
  std::cerr << what << ": diverges at byte " << i << "\n  oracle: ..."
            << oracle.substr(i > 40 ? i - 40 : 0, 120) << "\n  single: ..."
            << single.substr(i > 40 ? i - 40 : 0, 120) << '\n';
  throw std::runtime_error(what);
}

static void runMap(uint32_t nRows, uint32_t nCols, int delayMin, uint32_t fs,
                   uint64_t timestamp, bool extremes) {
  Map<Complex> map(nRows, nCols);
  std::mt19937 rng(4127);
  std::normal_distribution<double> random;

  map.delay.clear();
  for (uint32_t i = 0; i < nCols; i++)
    map.delay.push_back(delayMin + int(i));
  map.doppler.clear();
  for (uint32_t i = 0; i < nRows; i++)
    map.doppler.push_back((double(i) - double(nRows) / 2) * 1.5);

  for (uint32_t i = 0; i < nRows; i++)
    for (uint32_t j = 0; j < nCols; j++)
      map.data[i][j] = Complex(random(rng), random(rng));

  if (extremes) {
    // Values that stress the decimal-places cap and the sign of the exponent.
    map.data[0][0] = Complex(1e-8, 0);
    map.data[0][1] = Complex(1e9, -1e9);
    map.data[1][0] = Complex(-0.005, 0.004);
  }

  map.set_metrics();

  compare(map.delay_bin_to_km(map.to_json(timestamp), fs),
          map.to_json_km(timestamp, fs), "Map JSON");
  std::cout << "PASS map " << nRows << "x" << nCols << " delayMin=" << delayMin
            << " fs=" << fs << (extremes ? " (extremes)" : "") << '\n';
}

static void runDetection(size_t n, uint32_t fs, uint64_t timestamp) {
  std::vector<double> delay, doppler, snr;
  std::mt19937 rng(90210);
  std::uniform_real_distribution<double> random(-400.0, 400.0);
  for (size_t i = 0; i < n; i++) {
    delay.push_back(random(rng));
    doppler.push_back(random(rng));
    snr.push_back(std::abs(random(rng)) / 10);
  }
  Detection detection(delay, doppler, snr);

  compare(detection.delay_bin_to_km(detection.to_json(timestamp), fs),
          detection.to_json_km(timestamp, fs), "Detection JSON");
  std::cout << "PASS detection n=" << n << " fs=" << fs << '\n';
}

int main() {
  try {
    // The shipped geometry, and smaller ones that exercise the edges.
    runMap(301, 411, -10, 2000000, 1789383594875ULL, false);
    runMap(301, 411, -10, 2000000, 1789383594875ULL, true);
    runMap(201, 411, -10, 2000000, 1789383594875ULL, false);
    runMap(3, 5, 0, 2400000, 0ULL, false);
    runMap(1, 1, 7, 1000000, 1ULL, false);

    runDetection(0, 2000000, 1789383594875ULL);
    runDetection(1, 2000000, 1789383594875ULL);
    runDetection(64, 2400000, 42ULL);

    std::cout << "All JSON equivalence checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
