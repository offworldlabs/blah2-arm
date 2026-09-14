// Proves the two Ambiguity rewrites are index-for-index identical to the code
// they replace: the dataCorr staging array in the range loop, and the
// get_col/set_col round trip in the doppler loop.
#include "data/Map.h"

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

// What the old code did: stage 2*nDelayBins+1 values into dataCorr, then read a
// window out of it starting at (nDelayBins + delayMin).
static size_t oracleIndex(uint16_t j, uint16_t nDelayBins, int32_t delayMin,
                          uint32_t nfft) {
  std::vector<size_t> dataCorr(size_t(2) * nDelayBins + 1);
  for (uint16_t k = 0; k < nDelayBins; k++)
    dataCorr[k] = nfft - nDelayBins + k;
  for (uint16_t k = 0; k < nDelayBins + 1; k++)
    dataCorr[size_t(k) + nDelayBins] = k;
  const size_t window = size_t(nDelayBins) + delayMin + j;
  require(window < dataCorr.size(), "oracle window outside dataCorr");
  return dataCorr[window];
}

// What the new code does.
static size_t rewriteIndex(uint16_t j, int32_t delayMin, uint32_t nfft) {
  const int64_t k = int64_t(j) + delayMin;
  return size_t(k < 0 ? k + nfft : k);
}

static void checkRange(int32_t delayMin, int32_t delayMax, uint32_t nfft) {
  const uint16_t nDelayBins = uint16_t(delayMax - delayMin + 1);
  for (uint16_t j = 0; j < nDelayBins; j++) {
    const size_t oracle = oracleIndex(j, nDelayBins, delayMin, nfft);
    const size_t rewrite = rewriteIndex(j, delayMin, nfft);
    require(oracle == rewrite, "range-loop index differs from dataCorr staging");
    require(rewrite < nfft, "range-loop index outside dataZi");
  }
  std::cout << "PASS range delay=" << delayMin << ".." << delayMax
            << " bins=" << nDelayBins << " nfft=" << nfft << '\n';
}

// The doppler loop used to read a column out with get_col(), transform it, then
// push it back with set_col(). Both now index map.data directly.
static void checkColumn(uint32_t nRows, uint32_t nCols) {
  Map<Complex> viaAccessors(nRows, nCols), direct(nRows, nCols);
  std::mt19937 rng(17);
  std::normal_distribution<double> random;
  for (uint32_t i = 0; i < nRows; i++)
    for (uint32_t j = 0; j < nCols; j++) {
      const Complex value(random(rng), random(rng));
      viaAccessors.data[i][j] = value;
      direct.data[i][j] = value;
    }

  for (uint32_t col = 0; col < nCols; col++) {
    // Old path.
    std::vector<Complex> profile = viaAccessors.get_col(col);
    std::vector<Complex> shifted;
    for (uint32_t j = 0; j < nRows; j++)
      shifted.push_back(profile[(j + int(nRows / 2) + 1) % nRows]);
    viaAccessors.set_col(col, shifted);

    // New path, in place.
    std::vector<Complex> scratch(nRows);
    for (uint32_t j = 0; j < nRows; j++) scratch[j] = direct.data[j][col];
    for (uint32_t j = 0; j < nRows; j++)
      direct.data[j][col] = scratch[(j + int(nRows / 2) + 1) % nRows];
  }

  for (uint32_t i = 0; i < nRows; i++)
    for (uint32_t j = 0; j < nCols; j++)
      require(viaAccessors.data[i][j] == direct.data[i][j],
        "column read/write differs from get_col/set_col");
  std::cout << "PASS column " << nRows << "x" << nCols << '\n';
}

int main() {
  try {
    // Shipped geometry, both doppler spans in the fleet, and edges.
    checkRange(-10, 400, 6750);
    checkRange(-10, 400, 10000);
    checkRange(-10, 300, 6750);
    checkRange(0, 400, 6750);
    checkRange(1, 400, 6750);
    checkRange(-50, 50, 1024);
    checkRange(-1, 1, 16);
    checkRange(0, 0, 8);

    // The staging array was 2*nDelayBins+1 long and the window started at
    // nDelayBins + delayMin, so the old code read past its end whenever
    // delayMin >= 2. The rewrite has no staging array, so only check that it
    // stays inside dataZi; there is no oracle to compare against here.
    for (int32_t delayMin : {2, 5, 40}) {
      const int32_t delayMax = 400;
      const uint16_t nDelayBins = uint16_t(delayMax - delayMin + 1);
      const uint32_t nfft = 6750;
      bool oracleOverran = false;
      try { (void)oracleIndex(nDelayBins - 1, nDelayBins, delayMin, nfft); }
      catch (const std::runtime_error&) { oracleOverran = true; }
      require(oracleOverran, "expected the old staging array to overrun here");
      for (uint16_t j = 0; j < nDelayBins; j++)
        require(rewriteIndex(j, delayMin, nfft) < nfft,
          "rewrite index outside dataZi for positive delayMin");
      std::cout << "PASS positive delayMin=" << delayMin
                << " (old code overran dataCorr, rewrite does not)\n";
    }

    checkColumn(301, 411);
    checkColumn(201, 411);
    checkColumn(3, 5);
    checkColumn(1, 1);

    std::cout << "All ambiguity indexing checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
