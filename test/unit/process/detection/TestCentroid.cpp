/// @file TestCentroid.cpp
/// @brief Unit test for Centroid.cpp.
/// @details A detection is dropped when a stronger one lies within nDelay delay
/// bins and nDoppler Doppler bins of it. That has to hold at every delay,
/// including those nearer than nDelay, where the window used to wrap.

#include <catch2/catch_test_macros.hpp>

#include "data/Detection.h"
#include "process/detection/Centroid.h"

#include <memory>
#include <vector>

namespace
{

bool has(Detection &d, double delay, double doppler)
{
  std::vector<double> de = d.get_delay(), dp = d.get_doppler();
  for (size_t i = 0; i < de.size(); i++)
    if (de[i] == delay && dp[i] == doppler) return true;
  return false;
}

} // namespace

/// @details The control: far from the start of the delay axis.
TEST_CASE("Weaker_Neighbour_Dropped", "[centroid]")
{
  Centroid centroid(6, 6, 2.0);
  Detection in({100.0, 103.0}, {20.0, 20.0}, {10.0, 20.0});
  std::unique_ptr<Detection> out = centroid.process(&in);
  CHECK(out->get_nDetections() == 1);
  CHECK(has(*out, 103.0, 20.0));
}

/// @details The shipped settings: minDelay 5 and nCentroid 6 make delay 5 the
/// one tested delay nearer than nCentroid.
TEST_CASE("Weaker_Neighbour_Dropped_Near_Range", "[centroid]")
{
  Centroid centroid(6, 6, 2.0);
  Detection in({5.0, 7.0}, {20.0, 20.0}, {10.0, 20.0});
  std::unique_ptr<Detection> out = centroid.process(&in);
  CHECK(out->get_nDetections() == 1);
  CHECK(has(*out, 7.0, 20.0));
}

/// @details Detections at the same near delay, a few Doppler bins apart, must
/// merge too. Before the fix none of them could remove another.
TEST_CASE("Same_Delay_Doppler_Neighbours_Merge_Near_Range", "[centroid]")
{
  Centroid centroid(6, 6, 2.0);
  Detection in({5.0, 5.0, 5.0}, {50.0, 54.0, 58.0}, {12.0, 20.0, 16.0});
  std::unique_ptr<Detection> out = centroid.process(&in);
  CHECK(out->get_nDetections() == 1);
  CHECK(has(*out, 5.0, 54.0));
}

/// @details The window is the same size near range as anywhere else: a
/// detection outside it is kept.
TEST_CASE("Outside_Window_Kept_Near_Range", "[centroid]")
{
  Centroid centroid(6, 6, 2.0);
  Detection in({5.0, 11.0, 5.0}, {20.0, 20.0, 40.0}, {10.0, 20.0, 9.0});
  std::unique_ptr<Detection> out = centroid.process(&in);
  CHECK(out->get_nDetections() == 3);
}
