#include "WienerHopf.h"
#include "process/meta/FftLength.h"
#include <complex>
#include <stdexcept>
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

  // Both halves work a block at a time so the transforms stay in cache. The
  // overlap is nBins - 1 points, so each block advances by nHop.
  nBlock = blah2::clutterBlockLength(nSamples, nBins);
  nHop = nBlock - nBins + 1;

  // initialise data
  A = arma::cx_mat(nBins, nBins);
  a = arma::cx_vec(nBins);
  b = arma::cx_vec(nBins);
  w = arma::cx_vec(nBins);

  dataX = new std::complex<double>[nSamples];
  dataY = new std::complex<double>[nSamples];
  dataFiltered = new std::complex<double>[nSamples];

  blockRef = new std::complex<double>[nBlock];
  blockRefHop = new std::complex<double>[nBlock];
  blockSur = new std::complex<double>[nBlock];
  blockA = new std::complex<double>[nBlock];
  blockB = new std::complex<double>[nBlock];
  blockTaps = new std::complex<double>[nBlock];
  blockConv = new std::complex<double>[nBlock];

  // Plan the blocks at their own thread count. There is no caller-set count to
  // preserve: this is the front stage's only transform, so nothing else in the
  // program depends on what the planner held when the constructor was entered.
  fftw_plan_with_nthreads(blah2::kClutterBlockThreads);

  auto plan = [&](std::complex<double> *buffer, int direction) {
    return fftw_plan_dft_1d(nBlock, reinterpret_cast<fftw_complex *>(buffer),
                            reinterpret_cast<fftw_complex *>(buffer), direction, FFTW_ESTIMATE);
  };
  fftBlockRef = plan(blockRef, FFTW_FORWARD);
  fftBlockRefHop = plan(blockRefHop, FFTW_FORWARD);
  fftBlockSur = plan(blockSur, FFTW_FORWARD);
  ifftBlockA = plan(blockA, FFTW_BACKWARD);
  ifftBlockB = plan(blockB, FFTW_BACKWARD);
  fftBlockTaps = plan(blockTaps, FFTW_FORWARD);
  fftBlockConv = plan(blockConv, FFTW_FORWARD);
  ifftBlockConv = plan(blockConv, FFTW_BACKWARD);

  // Put the planner back to what every plan site outside this class wants, so
  // a future one added without an explicit count cannot silently inherit the
  // single thread that suits 2048-point blocks and nothing else.
  fftw_plan_with_nthreads(blah2::kBackStageThreads);
}

WienerHopf::~WienerHopf()
{
  fftw_destroy_plan(fftBlockRef);
  fftw_destroy_plan(fftBlockRefHop);
  fftw_destroy_plan(fftBlockSur);
  fftw_destroy_plan(ifftBlockA);
  fftw_destroy_plan(ifftBlockB);
  fftw_destroy_plan(fftBlockTaps);
  fftw_destroy_plan(fftBlockConv);
  fftw_destroy_plan(ifftBlockConv);

  delete[] dataX;
  delete[] dataY;
  delete[] dataFiltered;
  delete[] blockRef;
  delete[] blockRefHop;
  delete[] blockSur;
  delete[] blockA;
  delete[] blockB;
  delete[] blockTaps;
  delete[] blockConv;
}

// Circular correlations of the reference with itself and with the surveillance,
// for lags 0 to nBins-1, accumulated a block at a time.
//
// These used to be four transforms the length of the whole CPI: forward on each
// channel, then an inverse on each product. At the shipped geometry that is four
// passes over 16 MB arrays to produce 820 numbers, 0.04% of what the inverses
// computed. Every one of those passes goes to DRAM.
//
// Overlap-save gives the identical sums with each transform inside L2. Two
// details matter and both give a silently wrong answer if missed. One side of
// each product must be zero-padded to the hop while the other carries the full
// window: using the full window on both sides yields the block's own
// autocorrelation, which is not a partial sum of the signal's. And the
// normalisation is the block length, not the CPI length, because a
// forward-then-backward FFTW pair is unnormalised per transform.
void WienerHopf::correlate()
{
  std::vector<std::complex<double>> accumulatorA(nBins, {0, 0});
  std::vector<std::complex<double>> accumulatorB(nBins, {0, 0});

  for (uint64_t base = 0; base < nSamples; base += nHop)
  {
    // The full reference window reaches nBins-1 points before the block and
    // wraps, because the correlation this filter needs is circular. dataX is
    // already the reference rotated by delayMin, so the wrap is part of it.
    for (uint32_t i = 0; i < nBlock; i++)
    {
      int64_t index = int64_t(base) - int64_t(nBins) + 1 + i;
      index %= int64_t(nSamples);
      if (index < 0) index += nSamples;
      blockRef[i] = dataX[index];
    }

    // The two hop-length numerators, zero beyond the live samples.
    for (uint32_t i = 0; i < nBlock; i++)
    {
      const uint64_t index = base + i;
      const bool live = (i < nHop) && (index < nSamples);
      blockRefHop[i] = live ? dataX[index] : std::complex<double>{0, 0};
      blockSur[i] = live ? dataY[index] : std::complex<double>{0, 0};
    }

    fftw_execute(fftBlockRef);
    fftw_execute(fftBlockRefHop);
    fftw_execute(fftBlockSur);

    for (uint32_t i = 0; i < nBlock; i++)
    {
      blockA[i] = blockRefHop[i] * std::conj(blockRef[i]);
      blockB[i] = blockSur[i] * std::conj(blockRef[i]);
    }

    fftw_execute(ifftBlockA);
    fftw_execute(ifftBlockB);

    // Lag i lands at (i - nBins + 1) modulo the block length.
    for (uint32_t i = 0; i < nBins; i++)
    {
      int64_t index = int64_t(i) - int64_t(nBins) + 1;
      if (index < 0) index += nBlock;
      accumulatorA[i] += blockA[index];
      accumulatorB[i] += blockB[index];
    }
  }

  for (uint32_t i = 0; i < nBins; i++)
  {
    a[i] = std::conj(accumulatorA[i]) / (double)nBlock;
    b[i] = accumulatorB[i] / (double)nBlock;
  }

  // Zero lag is sum |x[n]|^2, real by definition, and it lands on the diagonal
  // of A, which has to be real for the matrix to be Hermitian and for
  // arma::chol to accept it.
  //
  // The single-transform code got that for free: it formed X * conj(X), whose
  // imaginary part is exactly zero in IEEE arithmetic (xy - yx), so the whole
  // array was exactly real and so was its transform at index 0. Here the two
  // sides of the product are different sequences, one windowed and one padded
  // to the hop, so nothing forces the imaginary part to cancel and a roundoff
  // residue survives. Left alone it makes armadillo warn on every CPI.
  //
  // Discarding it restores an exact property rather than approximating one.
  a[0] = std::complex<double>(a[0].real(), 0.0);
}

// Apply the nBins-tap filter to the reference, writing the first nSamples
// outputs of the linear convolution into `out`.
//
// This used to be three transforms at a padded length just over the CPI, which
// is a striking amount of work for a 410-tap FIR: the taps occupied 410 of
// 1,016,064 points and the rest was zeros. Overlap-save transforms the taps
// once and then does one forward and one inverse per block.
//
// History before sample zero is zero, which is what the old zero-padding of the
// reference into the long transform also meant, so this is the same linear
// convolution rather than a circular one.
void WienerHopf::convolve(std::complex<double> *out)
{
  for (uint32_t i = 0; i < nBins; i++) blockTaps[i] = w[i];
  for (uint32_t i = nBins; i < nBlock; i++) blockTaps[i] = {0, 0};
  fftw_execute(fftBlockTaps);

  for (uint64_t start = 0; start < nSamples; start += nHop)
  {
    for (uint32_t i = 0; i < nBlock; i++)
    {
      const int64_t index = int64_t(start) - int64_t(nBins) + 1 + i;
      blockConv[i] = (index >= 0 && index < int64_t(nSamples))
                         ? dataX[index]
                         : std::complex<double>{0, 0};
    }

    fftw_execute(fftBlockConv);
    for (uint32_t i = 0; i < nBlock; i++) blockConv[i] *= blockTaps[i];
    fftw_execute(ifftBlockConv);

    // The first nBins-1 outputs of each block are the wrapped ones and are
    // discarded; the rest are the valid linear convolution.
    for (uint32_t i = nBins - 1; i < nBlock; i++)
    {
      const uint64_t index = start + (i - (nBins - 1));
      if (index >= nSamples) break;
      out[index] = blockConv[i] / (double)nBlock;
    }
  }
}

bool WienerHopf::process(IqData *x, IqData *y)
{
  uint32_t i, j;
  // Views, not copies: each of these was 16 MB and ~31,000 allocations a CPI.
  // Both are read out into dataX/dataY immediately below and not touched
  // again, so the later y->clear() cannot be observed through yData.
  const std::deque<std::complex<double>> &xData = x->view_data();
  const std::deque<std::complex<double>> &yData = y->view_data();

  // change deque to std::complex
  for (i = 0; i < nSamples; i++)
  {
    // Signed arithmetic: `i - delayMin` promotes to unsigned, so a positive
    // delayMin wraps at 2^32 and lands on the wrong sample.
    const int64_t shifted = (int64_t(i) - delayMin) % int64_t(nSamples);
    dataX[i] = xData[shifted < 0 ? shifted + nSamples : shifted];
    dataY[i] = yData[i];
  }

  // auto-correlation vector a and cross-correlation vector b
  correlate();

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

  // apply the filter
  convolve(dataFiltered);

  // update surveillance signal
  y->clear();
  for (i = 0; i < nSamples; i++)
  {
    y->push_back(dataY[i] - dataFiltered[i]);
  }

  return true;
}
