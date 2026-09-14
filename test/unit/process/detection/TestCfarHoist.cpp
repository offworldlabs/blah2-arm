// Two separate things are being checked here.
//
// 1. The hoist is faithful. The per-CPI training table must produce exactly the
//    detections the original per-cell loop produced, given the same training
//    window. The oracle below is a literal transcription of the original loop
//    with the one deliberate change applied (left window k >= 0).
//
// 2. The edge fix is quantified. The original excluded bin 0 from the left
//    training window (k > 0) while including it on the right (k >= 0). Running
//    the oracle both ways shows how many detections that asymmetry moved, so
//    the behaviour change is a measured number rather than an assumption.
#include "data/Map.h"
#include "data/Detection.h"
#include "process/detection/CfarDetector1D.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

using Complex = std::complex<double>;

static void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct Hit {
  double delay, doppler, snr;
  bool operator!=(const Hit& o) const {
    return delay != o.delay || doppler != o.doppler || snr != o.snr;
  }
};

// The original algorithm, verbatim apart from `leftFromZero`.
static std::vector<Hit> oracle(Map<Complex>& x, double pfa, int nGuard,
                               int nTrain, int minDelay, double minDoppler,
                               bool leftFromZero) {
  const int nDelayBins = int(x.get_nCols());
  const int nDopplerBins = int(x.get_nRows());
  std::vector<Hit> hits;
  for (int i = 0; i < nDopplerBins; i++) {
    if (std::abs(x.doppler[i]) < minDoppler) continue;
    std::vector<Complex> mapRow = x.get_row(i);
    std::vector<double> mapRowSquare, mapRowSnr;
    for (int j = 0; j < nDelayBins; j++) {
      mapRowSquare.push_back((double)std::abs(mapRow[j] * mapRow[j]));
      mapRowSnr.push_back((double)10 * std::log10(std::abs(mapRow[j])) - x.noisePower);
    }
    for (int j = 0; j < nDelayBins; j++) {
      if (x.delay[j] < minDelay) continue;
      std::vector<int> iTrain;
      for (int k = j - nGuard - nTrain; k < j - nGuard; k++)
        if ((leftFromZero ? k >= 0 : k > 0) && k < nDelayBins) iTrain.push_back(k);
      for (int k = j + nGuard + 1; k < j + nGuard + nTrain + 1; k++)
        if (k >= 0 && k < nDelayBins) iTrain.push_back(k);

      int nCells = int(iTrain.size());
      if (nCells == 0) continue;  // original produced a NaN threshold here
      double alpha = nCells * (pow(pfa, -1.0 / nCells) - 1);
      double trainNoise = 0.0;
      for (int k = 0; k < nCells; k++) trainNoise += mapRowSquare[iTrain[k]];
      trainNoise /= nCells;
      if (mapRowSquare[j] > alpha * trainNoise)
        hits.push_back({double(j) + x.delay[0], x.doppler[i], mapRowSnr[j]});
    }
  }
  return hits;
}

static std::vector<Hit> fromDetection(Detection& detection) {
  std::vector<Hit> hits;
  std::vector<double> delay = detection.get_delay();
  std::vector<double> doppler = detection.get_doppler();
  std::vector<double> snr = detection.get_snr();
  for (size_t i = 0; i < delay.size(); i++)
    hits.push_back({delay[i], doppler[i], snr[i]});
  return hits;
}

static void run(uint32_t nRows, uint32_t nCols, double pfa, int nGuard,
                int nTrain, int minDelay, double minDoppler, int seed,
                double targetGain) {
  Map<Complex> map(nRows, nCols);
  std::mt19937 rng(seed);
  std::normal_distribution<double> random;

  map.delay.clear();
  for (uint32_t j = 0; j < nCols; j++) map.delay.push_back(-10 + int(j));
  map.doppler.clear();
  for (uint32_t i = 0; i < nRows; i++)
    map.doppler.push_back((double(i) - double(nRows) / 2) * 2.0);

  for (uint32_t i = 0; i < nRows; i++)
    for (uint32_t j = 0; j < nCols; j++)
      map.data[i][j] = Complex(random(rng), random(rng));

  // Plant targets, including ones right at the start of the delay axis where
  // the left-window edge case actually bites.
  for (uint32_t j : {0u, 1u, 2u, 3u, nCols / 2, nCols - 2})
    if (j < nCols) map.data[nRows / 2][j] *= targetGain;
  map.set_metrics();

  CfarDetector1D detector(pfa, int8_t(nGuard), int8_t(nTrain),
                          int8_t(minDelay), minDoppler);
  std::unique_ptr<Detection> produced = detector.process(&map);
  std::vector<Hit> got = fromDetection(*produced);
  std::vector<Hit> want = oracle(map, pfa, nGuard, nTrain, minDelay, minDoppler, true);

  require(got.size() == want.size(), "hoisted CFAR found a different number of detections");
  for (size_t i = 0; i < got.size(); i++)
    require(!(got[i] != want[i]), "hoisted CFAR detection differs from the oracle");

  std::vector<Hit> legacy = oracle(map, pfa, nGuard, nTrain, minDelay, minDoppler, false);
  size_t moved = legacy.size() > got.size() ? legacy.size() - got.size()
                                            : got.size() - legacy.size();
  std::cout << "PASS " << nRows << "x" << nCols << " pfa=" << pfa
            << " guard=" << nGuard << " train=" << nTrain
            << " -> " << got.size() << " detections, identical to oracle"
            << "; edge fix changed count by " << moved
            << " (was " << legacy.size() << ")\n";
}

int main() {
  try {
    run(301, 411, 1e-5, 2, 8, -10, 0.0, 11, 40.0);
    run(301, 411, 1e-5, 2, 8, -10, 50.0, 12, 40.0);
    run(201, 411, 1e-3, 2, 8, -10, 0.0, 13, 12.0);
    run(51, 61, 1e-5, 1, 4, -10, 0.0, 14, 30.0);
    run(11, 21, 1e-2, 2, 3, -10, 0.0, 15, 8.0);
    // Degenerate windows: guard wide enough to push training off the axis.
    run(9, 9, 1e-5, 4, 2, -10, 0.0, 16, 30.0);
    std::cout << "All CFAR hoist checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
