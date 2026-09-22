#include "SpectrumAnalyser.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <iostream>
#include <stdexcept>
#include <vector>

// constructor
SpectrumAnalyser::SpectrumAnalyser(uint32_t _n, double _bandwidth)
{
  // input
  n = _n;
  bandwidth = _bandwidth;

  // compute nfft
  decimation = n/bandwidth;
  if (!decimation)
    throw std::invalid_argument("Spectrum bandwidth is wider than the CPI");
  nSpectrum = n/decimation;
  nfft = nSpectrum*decimation;

  // The fold weights. s is the shift the old code applied to the output of a
  // full-length transform; folding it back into the input is what lets the
  // transform shrink. Both products are reduced before dividing so that a
  // large s cannot lose precision in the phase.
  const int64_t s = int64_t(nfft)/2 + 1;
  foldA.resize(decimation);
  for (uint32_t q = 0; q < decimation; q++)
  {
    const double phase = -2.0*M_PI*double((s*int64_t(q)) % int64_t(decimation))/double(decimation);
    foldA[q] = std::complex<double>(std::cos(phase), std::sin(phase));
  }
  foldB.resize(nSpectrum);
  for (uint32_t r = 0; r < nSpectrum; r++)
  {
    const double phase = -2.0*M_PI*double((s*int64_t(r)) % int64_t(nfft))/double(nfft);
    foldB[r] = std::complex<double>(std::cos(phase), std::sin(phase));
  }
  fold.resize(nSpectrum);

  // compute FFTW plans in constructor
  dataX = new std::complex<double>[nSpectrum];
  fftX = fftw_plan_dft_1d(nSpectrum, reinterpret_cast<fftw_complex *>(dataX),
                           reinterpret_cast<fftw_complex *>(dataX), FFTW_FORWARD, FFTW_ESTIMATE);
}

SpectrumAnalyser::~SpectrumAnalyser()
{
  fftw_destroy_plan(fftX);
  delete[] dataX;
}

void SpectrumAnalyser::process(IqData *x)
{
  uint32_t i;

  // Fold the CPI down to the spectrum length, weighting each segment. This is
  // the whole saving: nfft multiply-adds instead of an nfft-point transform.
  std::fill(fold.begin(), fold.end(), std::complex<double>(0.0, 0.0));
  for (uint32_t q = 0; q < decimation; q++)
  {
    const std::complex<double> weight = foldA[q];
    const uint32_t base = q*nSpectrum;
    for (uint32_t r = 0; r < nSpectrum; r++)
    {
      fold[r] += (*x)[base + r] * weight;
    }
  }
  for (i = 0; i < nSpectrum; i++)
  {
    dataX[i] = fold[i]*foldB[i];
  }
  fftw_execute(fftX);

  // The transform already lands on the shifted, decimated bins the old code
  // picked out of a full-length result, in the same order.
  std::vector<std::complex<double>> spectrum(dataX, dataX + nSpectrum);
  x->update_spectrum(spectrum);

  // update frequency
  std::vector<double> frequency;
  double offset = 0;
  if (decimation % 2 == 0)
  {
    offset = bandwidth/2;
  }
  for (i = -nSpectrum/2; i < nSpectrum/2; i++)
  {
    frequency.push_back(((i*bandwidth)+offset+204640000)/1000);
  }
  x->update_frequency(frequency);

  return;
}
