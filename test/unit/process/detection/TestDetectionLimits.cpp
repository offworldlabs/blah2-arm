/// @file TestDetectionLimits.cpp
/// @brief Unit test for the detection config checks, the CFAR upper limits
/// and Centroid at near range.

#include <catch2/catch_test_macros.hpp>

#include "data/Detection.h"
#include "data/Map.h"
#include "process/detection/Centroid.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/DetectionConfig.h"

#include <cmath>
#include <complex>
#include <limits>
#include <memory>
#include <random>
#include <utility>
#include <vector>

using Complex = std::complex<double>;

namespace
{

/// @brief The shipped detection settings.
DetectionConfig shipped()
{
  DetectionConfig c;
  c.pfa = 1e-5;
  c.nGuard = 2;
  c.nTrain = 6;
  c.minDelay = 5;
  c.minDoppler = 15;
  c.nCentroid = 6;
  return c;
}

/// @brief Noise map on the shipped axes (delay -10.., Doppler in 2 Hz bins)
/// with strong targets planted at the given (delay bin, Doppler Hz) cells.
Map<Complex> noise_map(uint32_t nRows, uint32_t nCols,
  const std::vector<std::pair<int, double>> &targets)
{
  Map<Complex> map(nRows, nCols);
  std::mt19937 rng(7);
  std::normal_distribution<double> random;
  map.delay.clear();
  for (uint32_t j = 0; j < nCols; j++) map.delay.push_back(-10 + int(j));
  map.doppler.clear();
  for (uint32_t i = 0; i < nRows; i++)
    map.doppler.push_back((double(i) - double(nRows / 2)) * 2.0);
  for (uint32_t i = 0; i < nRows; i++)
    for (uint32_t j = 0; j < nCols; j++)
      map.data[i][j] = Complex(random(rng), random(rng));
  for (const auto &[delay, doppler] : targets)
  {
    uint32_t row = uint32_t(doppler / 2.0 + double(nRows / 2));
    uint32_t col = uint32_t(delay + 10);
    map.data[row][col] = Complex(1000.0, 0.0);
  }
  map.set_metrics();
  return map;
}

bool has(Detection &d, double delay, double doppler)
{
  std::vector<double> de = d.get_delay(), dp = d.get_doppler();
  for (size_t i = 0; i < de.size(); i++)
    if (de[i] == delay && dp[i] == doppler) return true;
  return false;
}

} // namespace

TEST_CASE("Config_Shipped_Valid", "[config]")
{
  CHECK(validate_detection_config(shipped()).empty());
}

TEST_CASE("Config_Optional_Limits_Default_Open", "[config]")
{
  DetectionConfig c;
  CHECK(c.maxDelay == std::numeric_limits<int32_t>::max());
  CHECK(std::isinf(c.maxDoppler));
}

TEST_CASE("Config_Rejects_Bad_Values", "[config]")
{
  auto bad = [](void (*mutate)(DetectionConfig &)) {
    DetectionConfig c = shipped();
    mutate(c);
    return !validate_detection_config(c).empty();
  };
  CHECK(bad([](DetectionConfig &c) { c.pfa = 0.0; }));
  CHECK(bad([](DetectionConfig &c) { c.pfa = 1.0; }));
  CHECK(bad([](DetectionConfig &c) { c.pfa = std::nan(""); }));
  CHECK(bad([](DetectionConfig &c) { c.nGuard = -1; }));
  CHECK(bad([](DetectionConfig &c) { c.nTrain = 0; }));
  CHECK(bad([](DetectionConfig &c) { c.maxDelay = 4; }));
  CHECK(bad([](DetectionConfig &c) { c.minDoppler = -1.0; }));
  CHECK(bad([](DetectionConfig &c) { c.minDoppler = std::nan(""); }));
  CHECK(bad([](DetectionConfig &c) { c.maxDoppler = 10.0; }));
  CHECK(bad([](DetectionConfig &c) { c.maxDoppler = std::nan(""); }));
  CHECK(bad([](DetectionConfig &c) { c.nCentroid = -1; }));
  CHECK(bad([](DetectionConfig &c) { c.nCentroid = 65536; }));
}

TEST_CASE("Config_Accepts_Edge_Values", "[config]")
{
  DetectionConfig c = shipped();
  c.minDelay = 0;
  c.maxDelay = 0;
  c.minDoppler = 0.0;
  c.maxDoppler = 0.0;
  c.nGuard = 0;
  c.nTrain = 1;
  c.nCentroid = 0;
  CHECK(validate_detection_config(c).empty());
  // Values that overflowed the old int8_t fields are now plain values.
  c = shipped();
  c.minDelay = 200;
  c.maxDelay = 400;
  c.nTrain = 200;
  CHECK(validate_detection_config(c).empty());
}

/// @details Targets inside and outside each limit. The ones outside must go,
/// and the ones inside must match an unlimited run exactly, because the
/// limits pick which cells are tested and never what they train on.
TEST_CASE("Cfar_Upper_Limits", "[cfar]")
{
  std::vector<std::pair<int, double>> targets = {
    {50, 40.0}, {50, -40.0}, {150, 40.0}, {50, 200.0}, {150, -200.0}};
  Map<Complex> map = noise_map(301, 211, targets);

  CfarDetector1D open(1e-5, 2, 6, 5, std::numeric_limits<int32_t>::max(),
    15.0, std::numeric_limits<double>::infinity());
  std::unique_ptr<Detection> all = open.process(&map);
  for (const auto &[delay, doppler] : targets)
    REQUIRE(has(*all, delay, doppler));

  CfarDetector1D limited(1e-5, 2, 6, 5, 100, 15.0, 100.0);
  std::unique_ptr<Detection> some = limited.process(&map);
  CHECK(has(*some, 50, 40.0));
  CHECK(has(*some, 50, -40.0));
  CHECK_FALSE(has(*some, 150, 40.0));
  CHECK_FALSE(has(*some, 50, 200.0));
  CHECK_FALSE(has(*some, 150, -200.0));

  std::vector<double> de = all->get_delay(), dp = all->get_doppler();
  size_t inside = 0;
  for (size_t i = 0; i < de.size(); i++)
  {
    if (de[i] > 100 || std::abs(dp[i]) > 100.0) continue;
    inside++;
    CHECK(has(*some, de[i], dp[i]));
  }
  CHECK(some->get_nDetections() == inside);
}

/// @details Limits are inclusive at both ends, like minDelay and minDoppler.
TEST_CASE("Cfar_Upper_Limits_Inclusive", "[cfar]")
{
  Map<Complex> map = noise_map(301, 211, {{100, 100.0}, {100, -100.0}});
  CfarDetector1D limited(1e-5, 2, 6, 5, 100, 15.0, 100.0);
  std::unique_ptr<Detection> d = limited.process(&map);
  CHECK(has(*d, 100, 100.0));
  CHECK(has(*d, 100, -100.0));
}

/// @details A weaker detection within nCentroid of a stronger one is removed,
/// including near the start of the delay axis where the uint16_t window used
/// to wrap and keep both.
TEST_CASE("Centroid_Near_Range", "[centroid]")
{
  Centroid centroid(6, 6, 2.0);
  Detection in({3.0, 5.0, 100.0, 103.0}, {20.0, 20.0, 20.0, 20.0},
    {10.0, 20.0, 10.0, 20.0});
  std::unique_ptr<Detection> out = centroid.process(&in);
  CHECK(out->get_nDetections() == 2);
  CHECK(has(*out, 5.0, 20.0));
  CHECK(has(*out, 103.0, 20.0));
  CHECK_FALSE(has(*out, 3.0, 20.0));
  CHECK_FALSE(has(*out, 100.0, 20.0));
}
