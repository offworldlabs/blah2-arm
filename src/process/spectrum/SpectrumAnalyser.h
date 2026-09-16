/// @file SpectrumAnalyser.h
/// @class SpectrumAnalyser
/// @brief A class to generate frequency spectrum plots.
/// @details Simple decimate and FFT on CPI IQ data for frequency spectrum.
/// @author 30hours
/// @todo Potentially create k spectrum plots from sub-CPIs.
/// @todo FFT with HammingNumber class.

#ifndef SPECTRUMANALYSER_H
#define SPECTRUMANALYSER_H

#include "data/IqData.h"
#include <stdint.h>
#include <fftw3.h>
#include <vector>

class SpectrumAnalyser
{
private:
  /// @brief Number of samples on input.
  uint32_t n;

  /// @brief Minimum bandwidth of frequency bin (Hz).
  double bandwidth;

  /// @brief Capture center frequency (Hz).
  double centerFrequency;

  /// @brief Decimation factor.
  uint32_t decimation;

  /// @brief FFTW plans for ambiguity processing.
  fftw_plan fftX;

  /// @brief FFTW storage for ambiguity processing.
  std::complex<double> *dataX;

  /// @brief Number of samples to perform FFT.
  uint32_t nfft;

  /// @brief Number of samples in decimated spectrum.
  uint32_t nSpectrum;

  /// @brief Resolution of the source FFT (Hz).
  double resolution;

  // Sparse, regularly spaced output bins use a weighted input fold.
  bool folded;
  std::vector<std::complex<double>> blockPhase, binPhase;

public:
  /// @brief Constructor.
  /// @param n Number of samples on input.
  /// @param bandwidth Minimum bandwidth of frequency bin (Hz).
  /// @return The object.
  SpectrumAnalyser(uint32_t n, double bandwidth, double centerFrequency, double sampleRate);
  SpectrumAnalyser(const SpectrumAnalyser&) = delete;
  SpectrumAnalyser& operator=(const SpectrumAnalyser&) = delete;

  /// @brief Destructor.
  /// @return Void.
  ~SpectrumAnalyser();

  /// @brief Process spectrum data.
  /// @param x Reference samples.
  /// @return Void.
  void process(IqData *x);

  /// Update metadata after a CPI-latched retune; FFT geometry is unchanged.
  void set_center_frequency(double frequency);
};

#endif
