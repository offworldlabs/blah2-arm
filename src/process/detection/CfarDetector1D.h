/// @file CfarDetector1D.h
/// @class CfarDetector1D
/// @brief A class to implement a 1D CFAR detector.
/// @details Converts an AmbiguityMap to DetectionData. 1D CFAR operates across delay, to minimise detections from the zero-Doppler line.
/// @author 30hours

#ifndef CFARDETECTOR1D_H
#define CFARDETECTOR1D_H

#include "data/Map.h"
#include "data/Detection.h"
#include <stdint.h>
#include <complex>
#include <memory>

class CfarDetector1D
{
private:
  /// @brief Probability of false alarm, numeric in [0,1]
  double pfa;

  /// @brief Number of single-sided guard cells.
  int32_t nGuard;

  /// @brief Number of single-sided training cells.
  int32_t nTrain;

  /// @brief Minimum delay to process detections (bins).
  int32_t minDelay;

  /// @brief Minimum absolute Doppler to process detections (Hz).
  double minDoppler;

  /// @brief Maximum absolute Doppler to process detections (Hz).
  double maxReportedDoppler;

  /// @brief Pointer to detection data to store result.
  Detection *detection;

public:
  /// @brief Constructor.
  /// @param pfa Probability of false alarm, numeric in [0,1].
  /// @param nGuard Number of single-sided guard cells.
  /// @param nTrain Number of single-sided training cells.
  /// @param minDelay Minimum delay to process detections (bins).
  /// @param minDoppler Minimum absolute Doppler to process detections (Hz).
  /// @param maxReportedDoppler Maximum absolute Doppler to process detections (Hz).
  /// @details The limits only choose which cells are tested. Training windows
  /// still read cells outside them, so a limit never moves a threshold.
  /// maxReportedDoppler is not the map's dopplerMax: the map stays wide so fast
  /// targets are not folded into the reported band. See config/README.md.
  /// @return The object.
  CfarDetector1D(double pfa, int32_t nGuard, int32_t nTrain,
    int32_t minDelay, double minDoppler, double maxReportedDoppler);

  /// @brief Destructor.
  /// @return Void.
  ~CfarDetector1D();

  /// @brief Implement the 1D CFAR detector.
  /// @param x Ambiguity map data of IQ samples.
  /// @return Detections from the 1D CFAR detector.
  std::unique_ptr<Detection> process(Map<std::complex<double>> *x);
};

#endif
