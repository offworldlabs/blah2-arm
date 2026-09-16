/// @file WienerHopf.h
/// @class WienerHopf
/// @brief Wiener-Hopf clutter cancellation with bounded block FFT storage.

#ifndef WIENERHOPF_H
#define WIENERHOPF_H

#include "data/IqData.h"

#include <cstdint>
#include <memory>

class WienerHopf
{
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  /// The half-open delay range [delayMin, delayMax) defines the tap count.
  /// Supported settings are OWL_CLUTTER_PLAN=estimate|measure (estimate by
  /// default), OWL_CLUTTER_CORR_WORKERS=1|2 (one/serial by default), and
  /// OWL_CLUTTER_DENSE_ONLY=0|1 (guarded solver enabled by default).
  WienerHopf(int32_t delayMin, int32_t delayMax, uint32_t nSamples);
  ~WienerHopf();

  WienerHopf(const WienerHopf&) = delete;
  WienerHopf& operator=(const WienerHopf&) = delete;
  WienerHopf(WienerHopf&&) = delete;
  WienerHopf& operator=(WienerHopf&&) = delete;

  /// Block length used by the four-lane overlap-save convolution.
  uint32_t filter_fft_length() const;

  /// Cancel clutter in y while leaving x unchanged.
  bool process(IqData* x, IqData* y);
};

#endif
