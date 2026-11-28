#include "Ambiguity.h"
#include <algorithm>
#include <complex>
#include <iostream>
#include <deque>
#include <vector>
#include <numeric>
#include <math.h>
#include <cmath>
#include <chrono>
#include <limits>
#include <stdexcept>

// constructor
Ambiguity::Ambiguity(int32_t _delayMin, int32_t _delayMax, 
  int32_t _dopplerMin, int32_t _dopplerMax, uint32_t _fs, 
  uint32_t _n, bool _roundHamming, DopplerWindow _dopplerWindow)
{
  // init
  dopplerMin = _dopplerMin;
  dopplerMax = _dopplerMax;
  fs = _fs;
  nSamples = _n;
  dopplerMiddle = (_dopplerMin + _dopplerMax) / 2.0;
  
  // doppler calculations
  std::deque<double> doppler;
  double resolutionDoppler = 1.0 / (static_cast<double>(_n) / static_cast<double>(_fs));
  doppler.push_back(dopplerMiddle);
  int i = 1;
  while (dopplerMiddle + (i * resolutionDoppler) <= dopplerMax)
  {
    doppler.push_back(dopplerMiddle + (i * resolutionDoppler));
    doppler.push_front(dopplerMiddle - (i * resolutionDoppler));
    i++;
  }
  nDopplerBins = doppler.size();

  // batches constants
  nCorr = _n / nDopplerBins;
  if (nCorr == 0)
  {
    throw std::invalid_argument("Ambiguity needs at least one sample per batch: "
      "the CPI is shorter than the number of Doppler bins");
  }
  cpi = (static_cast<double>(nCorr) * nDopplerBins) / fs;

  // Delay window. Each batch correlates nCorr reference samples against a
  // surveillance segment that is longer by the window, so every delay sees the
  // full batch and the window does not depend on nCorr, fs or the Doppler span.
  // See config/README.md, process.ambiguity.
  if (_delayMax < _delayMin)
  {
    throw std::invalid_argument("Ambiguity needs delayMax >= delayMin");
  }
  const int64_t window = int64_t(_delayMax) - _delayMin + 1;
  if (window > std::numeric_limits<uint16_t>::max() || window > int64_t(_n))
  {
    throw std::invalid_argument("Ambiguity delay window is longer than the CPI");
  }
  delayMin = _delayMin;
  delayMax = _delayMax;
  nDelayBins = static_cast<uint16_t>(window);
  nSegment = nCorr + nDelayBins - 1;

  // update doppler bins to true cpi time
  resolutionDoppler = 1.0 / cpi;

  // create ambiguity map
  map = std::make_unique<Map<Complex>>(nDopplerBins, nDelayBins);

  // delay calculations
  map->delay.resize(nDelayBins);
  std::iota(map->delay.begin(), map->delay.end(), delayMin);

  map->doppler.push_front(dopplerMiddle);
  i = 1;
  while (map->doppler.size() < nDopplerBins)
  {
    map->doppler.push_back(dopplerMiddle + (i * resolutionDoppler));
    map->doppler.push_front(dopplerMiddle - (i * resolutionDoppler));
    i++;
  }

  // A linear correlation of nCorr against nSegment samples, with no wrap
  // into the delays kept, needs nSegment points.
  nfft = nSegment;
  if (_roundHamming) {
    nfft = next_hamming(nfft);
  }

  // Periodic Hann scaled to mean 1, so a target's peak keeps its height and
  // the noise floor rises by the window's 1.76 dB processing loss instead.
  if (_dopplerWindow == DopplerWindow::Hann)
  {
    dopplerTaper.resize(nDopplerBins);
    for (uint16_t k = 0; k < nDopplerBins; k++)
    {
      dopplerTaper[k] = 1.0 - std::cos(2.0 * M_PI * k / nDopplerBins);
    }
  }

  // compute FFTW plans in constructor
  dataXi.resize(nfft);
  dataYi.resize(nfft);
  dataZi.resize(nfft);
  // The Doppler FFT runs over nDopplerBins, which can exceed nfft at a low fs.
  dataDoppler.resize(nDopplerBins);
  fftXi = fftw_plan_dft_1d(nfft, reinterpret_cast<fftw_complex *>(dataXi.data()),
                           reinterpret_cast<fftw_complex *>(dataXi.data()), FFTW_FORWARD, FFTW_ESTIMATE);
  fftYi = fftw_plan_dft_1d(nfft, reinterpret_cast<fftw_complex *>(dataYi.data()),
                           reinterpret_cast<fftw_complex *>(dataYi.data()), FFTW_FORWARD, FFTW_ESTIMATE);
  fftZi = fftw_plan_dft_1d(nfft, reinterpret_cast<fftw_complex *>(dataZi.data()),
                           reinterpret_cast<fftw_complex *>(dataZi.data()), FFTW_BACKWARD, FFTW_ESTIMATE);
  fftDoppler = fftw_plan_dft_1d(nDopplerBins, reinterpret_cast<fftw_complex *>(dataDoppler.data()),
                                reinterpret_cast<fftw_complex *>(dataDoppler.data()), FFTW_FORWARD, FFTW_ESTIMATE);

}

bool Ambiguity::parse_doppler_window(const std::string &name, DopplerWindow &window)
{
  if (name == "none")
  {
    window = DopplerWindow::None;
    return true;
  }
  if (name == "hann")
  {
    window = DopplerWindow::Hann;
    return true;
  }
  return false;
}

Ambiguity::~Ambiguity()
{
  fftw_destroy_plan(fftXi);
  fftw_destroy_plan(fftYi);
  fftw_destroy_plan(fftZi);
  fftw_destroy_plan(fftDoppler);
}

Map<std::complex<double>> *Ambiguity::process(IqData *x, IqData *y)
{
  // shift reference if not 0 centered
  if (dopplerMiddle != 0)
  {
    std::complex<double> j = {0, 1};
    for (uint32_t i = 0; i < x->get_length(); i++)
    {
      x->push_back(x->pop_front() * std::exp(1.0 * j * 2.0 * M_PI * dopplerMiddle * ((double)i / fs)));
    }
  }

  // range processing
  // Batch i correlates reference samples [i*nCorr, (i+1)*nCorr) against
  // surveillance [i*nCorr + delayMin, i*nCorr + delayMax + nCorr), so map
  // column j is delay delayMin + j at full overlap. Surveillance outside the
  // samples held (before the CPI for a negative delayMin, after it for the last
  // batches) is zero: only those batches lose overlap, and only at the edges
  // of the window.
  nSamples = nDopplerBins * nCorr;
  const int64_t nY = y->get_length();
  for (uint32_t i = 0; i < nDopplerBins; i++)
  {
    const int64_t start = int64_t(i) * nCorr;
    for (uint32_t j = 0; j < nCorr; j++)
    {
      dataXi[j] = (*x)[static_cast<uint32_t>(start + j)];
    }
    std::fill(dataXi.begin() + nCorr, dataXi.end(), Complex(0, 0));

    // segment [first, first + nSegment), clipped to [0, nY)
    const int64_t first = start + delayMin;
    const int64_t lo = std::clamp<int64_t>(-first, 0, nSegment);
    const int64_t hi = std::clamp<int64_t>(nY - first, lo, nSegment);
    std::fill(dataYi.begin(), dataYi.begin() + lo, Complex(0, 0));
    for (int64_t m = lo; m < hi; m++)
    {
      dataYi[m] = (*y)[static_cast<uint32_t>(first + m)];
    }
    std::fill(dataYi.begin() + hi, dataYi.end(), Complex(0, 0));

    fftw_execute(fftXi);
    fftw_execute(fftYi);

    // compute correlation
    for (uint32_t j = 0; j < nfft; j++)
    {
      dataZi[j] = (dataYi[j] * std::conj(dataXi[j])) / (double)nfft;
    }

    fftw_execute(fftZi);

    // Lag j of the correlation is delay delayMin + j. nfft >= nSegment, so
    // none of the kept lags wraps.
    std::copy(dataZi.begin(), dataZi.begin() + nDelayBins, map->data[i].begin());
  }

  // Both channels are consumed: drain them, as the pop_front loop this
  // replaced did. Constant time.
  x->clear();
  y->clear();

  // doppler processing
  for (uint16_t i = 0; i < nDelayBins; i++)
  {
    // Read and write the column in place. get_col() built and returned a fresh
    // vector per delay bin, and set_col() took another by value, so a 301-deep
    // column was copied four times over to be transformed once.
    for (uint16_t j = 0; j < nDopplerBins; j++)
    {
      dataDoppler[j] = map->data[j][i];
    }
    for (size_t j = 0; j < dopplerTaper.size(); j++)
    {
      dataDoppler[j] *= dopplerTaper[j];
    }

    fftw_execute(fftDoppler);

    for (uint16_t j = 0; j < nDopplerBins; j++)
    {
      map->data[j][i] = dataDoppler[(j + int(nDopplerBins / 2) + 1) % nDopplerBins];
    }
  }

  return map.get();
}

double Ambiguity::get_doppler_middle() const {
  return dopplerMiddle;
}

uint16_t Ambiguity::get_n_delay_bins() const {
  return nDelayBins;
}

uint16_t Ambiguity::get_n_doppler_bins() const {
  return nDopplerBins;
}

uint16_t Ambiguity::get_n_corr() const {
  return nCorr;
}

double Ambiguity::get_cpi() const {
  return cpi;
}

uint32_t Ambiguity::get_nfft() const {
  return nfft;
}

uint32_t Ambiguity::get_n_samples() const {
  return nSamples;
}