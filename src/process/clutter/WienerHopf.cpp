#include "WienerHopf.h"
#include "process/meta/FftLength.h"
#include <complex>
#include <iostream>
#include <vector>

// constructor
WienerHopf::WienerHopf(int32_t _delayMin, int32_t _delayMax, uint32_t _nSamples)
{
  // input
  delayMin = _delayMin;
  delayMax = _delayMax;
  const int64_t taps = int64_t(delayMax) - delayMin;
  if (!_nSamples || taps <= 0 || uint64_t(taps) > _nSamples)
    throw std::invalid_argument("Clutter filter needs a non-empty half-open delay range no longer than the CPI");
  nBins = static_cast<uint32_t>(taps);
  nSamples = _nSamples;
  // Pad only the linear convolution; keep taps and circular correlations
  // unchanged. The length is timed rather than derived: under
  // fftw_plan_with_nthreads() the quickest size does not follow from the
  // factorisation, and the ranking differs between machines and thread counts.
  nFilter = blah2::fastestFftLength(uint64_t(nSamples) + nBins + 1);

  // initialise data
  A = arma::cx_mat(nBins, nBins);
  a = arma::cx_vec(nBins);
  b = arma::cx_vec(nBins);
  w = arma::cx_vec(nBins);

  // compute FFTW plans in constructor
  dataX = new std::complex<double>[nSamples];
  dataY = new std::complex<double>[nSamples];
  dataOutX = new std::complex<double>[nSamples];
  dataOutY = new std::complex<double>[nSamples];
  dataA = new std::complex<double>[nSamples];
  dataB = new std::complex<double>[nSamples];
  filtX = new std::complex<double>[nFilter];
  filtW = new std::complex<double>[nFilter];
  filt = new std::complex<double>[nFilter];
  fftX = fftw_plan_dft_1d(nSamples, reinterpret_cast<fftw_complex *>(dataX),
                          reinterpret_cast<fftw_complex *>(dataOutX), FFTW_FORWARD, FFTW_ESTIMATE);
  fftY = fftw_plan_dft_1d(nSamples, reinterpret_cast<fftw_complex *>(dataY),
                          reinterpret_cast<fftw_complex *>(dataOutY), FFTW_FORWARD, FFTW_ESTIMATE);
  fftA = fftw_plan_dft_1d(nSamples, reinterpret_cast<fftw_complex *>(dataA),
                          reinterpret_cast<fftw_complex *>(dataA), FFTW_BACKWARD, FFTW_ESTIMATE);
  fftB = fftw_plan_dft_1d(nSamples, reinterpret_cast<fftw_complex *>(dataB),
                          reinterpret_cast<fftw_complex *>(dataB), FFTW_BACKWARD, FFTW_ESTIMATE);
  fftFiltX = fftw_plan_dft_1d(nFilter, reinterpret_cast<fftw_complex *>(filtX),
                              reinterpret_cast<fftw_complex *>(filtX), FFTW_FORWARD, FFTW_ESTIMATE);
  fftFiltW = fftw_plan_dft_1d(nFilter, reinterpret_cast<fftw_complex *>(filtW),
                              reinterpret_cast<fftw_complex *>(filtW), FFTW_FORWARD, FFTW_ESTIMATE);
  fftFilt = fftw_plan_dft_1d(nFilter, reinterpret_cast<fftw_complex *>(filt),
                             reinterpret_cast<fftw_complex *>(filt), FFTW_BACKWARD, FFTW_ESTIMATE);
}

WienerHopf::~WienerHopf()
{
  fftw_destroy_plan(fftX);
  fftw_destroy_plan(fftY);
  fftw_destroy_plan(fftA);
  fftw_destroy_plan(fftB);
  fftw_destroy_plan(fftFiltX);
  fftw_destroy_plan(fftFiltW);
  fftw_destroy_plan(fftFilt);
}

bool WienerHopf::process(IqData *x, IqData *y)
{
  uint32_t i, j;
  xData = x->get_data();
  yData = y->get_data();

  // change deque to std::complex
  for (i = 0; i < nSamples; i++)
  {
    // Signed arithmetic: `i - delayMin` promotes to unsigned, so a positive
    // delayMin wraps at 2^32 and lands on the wrong sample.
    const int64_t shifted = (int64_t(i) - delayMin) % int64_t(nSamples);
    dataX[i] = xData[shifted < 0 ? shifted + nSamples : shifted];
    dataY[i] = yData[i];
  }

  // pre-compute FFT of signals
  fftw_execute(fftX);
  fftw_execute(fftY);

  // auto-correlation matrix A
  for (i = 0; i < nSamples; i++)
  {
    dataA[i] = (dataOutX[i] * std::conj(dataOutX[i]));
  }
  fftw_execute(fftA);
  for (i = 0; i < nBins; i++)
  {
    a[i] = std::conj(dataA[i]) / (double)nSamples;
  }
  A = arma::toeplitz(a);

  // conjugate upper diagonal as arma does not
  for (i = 0; i < nBins; i++)
  {
    for (j = 0; j < nBins; j++)
    {
      if (i > j)
      {
        A(i, j) = std::conj(A(i, j));
      }
    }
  }

  // cross-correlation vector b
  for (i = 0; i < nSamples; i++)
  {
    dataB[i] = (dataOutY[i] * std::conj(dataOutX[i]));
  }
  fftw_execute(fftB);
  for (i = 0; i < nBins; i++)
  {
    b[i] = dataB[i] / (double)nSamples;
  }

  // compute weights
  success = arma::chol(A, A);
  if (!success)
  {
    std::cerr << "Chol decomposition failed, skip clutter filter" << std::endl;
    return false;
  }
  success = arma::solve(w, arma::trimatu(A), arma::solve(arma::trimatl(arma::trans(A)), b));
  if (!success)
  {
    std::cerr << "Solve failed, skip clutter filter" << std::endl;
    return false;
  }

  // assign and pad x
  for (i = 0; i < nSamples; i++)
  {
    filtX[i] = dataX[i];
  }
  for (i = nSamples; i < nFilter; i++)
  {
    filtX[i] = {0, 0};
  }

  // assign and pad w
  for (i = 0; i < nBins; i++)
  {
    filtW[i] = w[i];
  }
  for (i = nBins; i < nFilter; i++)
  {
    filtW[i] = {0, 0};
  }

  // compute fft
  fftw_execute(fftFiltX);
  fftw_execute(fftFiltW);

  // compute convolution/filter
  for (i = 0; i < nFilter; i++)
  {
    filt[i] = (filtW[i] * filtX[i]);
  }
  fftw_execute(fftFilt);

  // update surveillance signal
  y->clear();
  for (i = 0; i < nSamples; i++)
  {
    y->push_back(dataY[i] - (filt[i] / (double)nFilter));
  }

  return true;
}