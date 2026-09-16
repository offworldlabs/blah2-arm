#include "process/spectrum/SpectrumAnalyser.h"
#include <algorithm>
#include <stdexcept>
#include <complex>
#include <iostream>
#include <deque>
#include <vector>
#include <math.h>
#include <limits>

// constructor
SpectrumAnalyser::SpectrumAnalyser(uint32_t _n, double _bandwidth,
  double _centerFrequency, double sampleRate)
{
  if (_n == 0 || _n > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
      !std::isfinite(_bandwidth) || _bandwidth <= 0 ||
      !std::isfinite(_centerFrequency) || !std::isfinite(sampleRate) || sampleRate <= 0)
    throw std::invalid_argument("Invalid spectrum geometry");

  // input
  n = _n;
  bandwidth = _bandwidth;
  centerFrequency = _centerFrequency;

  // Keep only complete decimation groups, then derive their physical spacing
  // from the supplied capture rate rather than a fixed receiver setting.
  resolution = sampleRate / n;
  decimation = static_cast<uint32_t>(std::min<double>(n,
    std::max(1.0, std::ceil(bandwidth / resolution))));
  nSpectrum = n / decimation;
  nfft = nSpectrum * decimation;

  // compute FFTW plans in constructor
  // For sparse, regularly spaced bins, fold all samples before the short FFT.
  // Preserve the legacy shifted selection, including its one-bin offset.
  folded = decimation >= 8;
  const uint32_t transformLength = folded ? nSpectrum : nfft;
  if (folded) {
    const uint32_t remainder = (nfft / 2 + 1) % decimation;
    const double phase = -2.0 * std::acos(-1.0) * remainder;
    blockPhase.resize(decimation);
    binPhase.resize(nSpectrum);
    for (uint32_t t = 0; t < decimation; ++t)
      blockPhase[t] = std::polar(1.0, phase * t / decimation);
    for (uint32_t l = 0; l < nSpectrum; ++l)
      binPhase[l] = std::polar(1.0, phase * l / nfft);
  }
  // Build the throwing vector state before acquiring raw FFTW scratch.
  dataX = new std::complex<double>[transformLength];
  fftX = fftw_plan_dft_1d(transformLength, reinterpret_cast<fftw_complex *>(dataX),
                           reinterpret_cast<fftw_complex *>(dataX), FFTW_FORWARD, FFTW_ESTIMATE);
  if (!fftX) {
    delete[] dataX;
    dataX = nullptr;
    throw std::runtime_error("Could not create spectrum FFT plan");
  }
}

SpectrumAnalyser::~SpectrumAnalyser()
{
  fftw_destroy_plan(fftX);
  delete[] dataX;
}

void SpectrumAnalyser::set_center_frequency(double frequency)
{
  if (!std::isfinite(frequency)) throw std::invalid_argument("Invalid spectrum frequency");
  centerFrequency = frequency;
}

void SpectrumAnalyser::process(IqData *x)
{  
  // load data and FFT
  uint32_t i;
  if (!x) throw std::invalid_argument("Null spectrum input");
  const auto& data = x->view_data();
  if (data.size() < nfft)
    throw std::invalid_argument("Spectrum input shorter than FFT length");
  if (folded) {
    std::fill(dataX, dataX + nSpectrum, std::complex<double>{});
    for (uint32_t t = 0; t < decimation; ++t) {
      const uint64_t start = uint64_t(t) * nSpectrum;
      const auto phase = blockPhase[t];
      for (uint32_t l = 0; l < nSpectrum; ++l)
        dataX[l] += data[start + l] * phase;
    }
    for (uint32_t l = 0; l < nSpectrum; ++l) dataX[l] *= binPhase[l];
  } else {
    for (i = 0; i < nfft; ++i) dataX[i] = data[i];
  }
  fftw_execute(fftX);
  std::vector<std::complex<double>> spectrum;
  spectrum.reserve(nSpectrum);
  if (folded) {
    const uint32_t shift = (nfft / 2 + 1) / decimation;
    for (i = 0; i < nSpectrum; ++i)
      spectrum.push_back(dataX[(shift + i) % nSpectrum] / static_cast<double>(nfft));
  } else {
    for (i = 0; i < nfft; i += decimation)
      spectrum.push_back(dataX[(i + int(nfft / 2) + 1) % nfft] / static_cast<double>(nfft));
  }
  x->update_spectrum(spectrum);

  // update frequency
  std::vector<double> frequency;
  frequency.reserve(nSpectrum);
  for (i = 0; i < nfft; i += decimation)
    frequency.push_back((centerFrequency +
      (static_cast<double>(i) - nfft / 2.0) * resolution) / 1000.0);
  x->update_frequency(frequency);

  return;
}
