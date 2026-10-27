/// @file DetectionConfig.h
/// @brief The detection settings from config, and the checks they must pass.
/// @details Read once at startup. A failing config stops blah2 before the first
/// CPI rather than running a detector nobody chose. Field meanings and the
/// expected effect of each are in config/README.md.

#ifndef DETECTIONCONFIG_H
#define DETECTIONCONFIG_H

#include <cmath>
#include <limits>
#include <stdint.h>
#include <string>

struct DetectionConfig
{
  double pfa = 0.0;
  int32_t nGuard = 0;
  int32_t nTrain = 0;
  int32_t minDelay = 0;
  /// @brief Optional in config. The default tests every delay bin.
  int32_t maxDelay = std::numeric_limits<int32_t>::max();
  double minDoppler = 0.0;
  /// @brief Optional in config. The default tests every Doppler bin.
  double maxDoppler = std::numeric_limits<double>::infinity();
  int32_t nCentroid = 0;
};

/// @brief Check a detection config.
/// @return Empty if valid, otherwise the first problem found.
inline std::string validate_detection_config(const DetectionConfig &c)
{
  // Written so NaN fails each check too.
  if (!(c.pfa > 0.0 && c.pfa < 1.0))
  {
    return "pfa must be greater than 0 and less than 1";
  }
  if (c.nGuard < 0)
  {
    return "nGuard must be 0 or more";
  }
  if (c.nTrain < 1)
  {
    return "nTrain must be 1 or more";
  }
  if (c.maxDelay < c.minDelay)
  {
    return "maxDelay must be at least minDelay";
  }
  if (!(c.minDoppler >= 0.0))
  {
    return "minDoppler must be 0 or more";
  }
  if (!(c.maxDoppler >= c.minDoppler))
  {
    return "maxDoppler must be at least minDoppler";
  }
  // Centroid takes its window as uint16_t.
  if (c.nCentroid < 0 || c.nCentroid > std::numeric_limits<uint16_t>::max())
  {
    return "nCentroid must be between 0 and 65535";
  }
  return "";
}

#endif
