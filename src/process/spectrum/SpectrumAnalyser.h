/// @file SpectrumAnalyser.h
/// @class SpectrumAnalyser
/// @brief A class to generate frequency spectrum plots.
/// @details Folds the CPI down to the spectrum length and transforms that,
/// rather than transforming the whole CPI and discarding almost all of it.
///
/// It used to run an @c nfft -point FFT, which at the shipped geometry is
/// 1,000,000 points, and then keep every @c decimation -th bin: 2000 of them,
/// throwing away 99.8% of the result. Taking every Dth bin of an N-point DFT is
/// identical to an M-point DFT of a fold, where N = M*D, so the same 2000
/// numbers come out of a 2000-point transform.
///
/// With k = mD + s, splitting n = qM + r gives
///
///   X[mD+s] = sum_r ( B[r] * sum_q x[qM+r] * A[q] ) * exp(-2i*pi*m*r/M)
///
/// so the fold weights each of the D segments by @c foldA and the folded result
/// by @c foldB, both fixed tables built once in the constructor. That is one
/// complex multiply-add per input sample and no trigonometry in the loop.
///
/// The offset s is @c nfft/2 + 1, which is where the fftshift the old code
/// applied to its output ends up once it is pushed back into the input.
///
/// Not bit-identical: the same terms are summed in a different order. Measured
/// against the full-length transform the worst bin differs by 9.5e-16 of the
/// peak, and the spectrum is a display product that nothing downstream of the
/// detector reads.
/// @author 30hours
/// @todo Potentially create k spectrum plots from sub-CPIs.

#ifndef SPECTRUMANALYSER_H
#define SPECTRUMANALYSER_H

#include "data/IqData.h"
#include <complex>
#include <stdint.h>
#include <vector>
#include <fftw3.h>

class SpectrumAnalyser
{
private:
  /// @brief Number of samples on input.
  uint32_t n;

  /// @brief Minimum bandwidth of frequency bin (Hz).
  double bandwidth;

  /// @brief Decimation factor.
  uint32_t decimation;

  /// @brief FFTW plan, now at the spectrum length rather than the CPI length.
  fftw_plan fftX;

  /// @brief FFTW storage, nSpectrum entries rather than nfft.
  std::complex<double> *dataX;

  /// @brief Number of samples the equivalent full-length FFT would use.
  uint32_t nfft;

  /// @brief Number of samples in decimated spectrum.
  uint32_t nSpectrum;

  /// @brief Resolution of spectrum (Hz).
  double resolution;

  /// @brief Per-segment fold weight, exp(-2i*pi*s*q/decimation).
  std::vector<std::complex<double>> foldA;

  /// @brief Post-fold weight, exp(-2i*pi*s*r/nfft).
  std::vector<std::complex<double>> foldB;

  /// @brief Fold accumulator, allocated once so process() allocates nothing.
  std::vector<std::complex<double>> fold;

public:
  /// @brief Constructor.
  /// @param n Number of samples on input.
  /// @param bandwidth Minimum bandwidth of frequency bin (Hz).
  /// @return The object.
  SpectrumAnalyser(uint32_t n, double bandwidth);

  /// @brief Destructor.
  /// @return Void.
  ~SpectrumAnalyser();

  /// @brief Length of the transform actually performed.
  /// @return Number of points.
  uint32_t fft_length() const { return nSpectrum; }

  /// @brief Length of the full-CPI transform this replaces.
  /// @return Number of points.
  uint32_t equivalent_fft_length() const { return nfft; }

  /// @brief Process spectrum data.
  /// @param x Reference samples.
  /// @return Void.
  void process(IqData *x);
};

#endif
