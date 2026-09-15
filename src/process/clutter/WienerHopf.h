/// @file WienerHopf.h
/// @class WienerHopf
/// @brief A class to implement a Wiener-Hopf clutter filter.
/// @details Implements a <a href="https://en.wikipedia.org/wiki/Wiener_filter#Finite_impulse_response_Wiener_filter_for_discrete_series">Wiener-Hopf filter</a>.
/// Uses <a href="https://en.wikipedia.org/wiki/Cholesky_decomposition">Cholesky decomposition</a> to speed up matrix inversion, as the Toeplitz matrix is positive-definite and Hermitian.
/// @author 30hours
/// @todo Fix the segmentation fault from clutter filter numerical instability.

#ifndef WIENERHOPF_H
#define WIENERHOPF_H

#include "data/IqData.h"
#include "process/meta/FftLength.h"
#include <stdint.h>
#include <fftw3.h>
#include <armadillo>

class WienerHopf
{
private:
  /// @brief Minimum clutter filter delay (bins).
  int32_t delayMin;

  /// @brief Maximum clutter filter delay (bins).
  int32_t delayMax;

  /// @brief Number of bins (delayMax - delayMin + 1).
  uint32_t nBins;

  /// @brief Number of samples per CPI.
  uint32_t nSamples;

  /// @brief Length of each correlation and convolution block.
  uint32_t nBlock;

  /// @brief Samples of new signal consumed per block (nBlock - nBins + 1).
  uint32_t nHop;

  /// @brief True if clutter filter processing is successful.
  bool success;

  /// @brief FFTW plans, all at the block length rather than the CPI length.
  /// @{
  fftw_plan fftBlockRef, fftBlockRefHop, fftBlockSur, ifftBlockA, ifftBlockB,
      fftBlockTaps, fftBlockConv, ifftBlockConv;
  /// @}

  /// @brief Full-CPI storage: the shifted reference, the surveillance, and the
  /// filter output. Allocated once, not per CPI: at the shipped geometry each
  /// is 16 MB and the whole point of this class's rewrite was to stop moving
  /// that much memory around.
  /// @{
  std::complex<double> *dataX, *dataY, *dataFiltered;
  /// @}

  /// @brief Block storage. Each of these is nBlock points, small enough to
  /// stay in L2, which is the entire point of the block formulation.
  /// @{
  std::complex<double> *blockRef, *blockRefHop, *blockSur, *blockA, *blockB,
      *blockTaps, *blockConv;
  /// @}

  /// @brief Autocorrelation toeplitz matrix.
  arma::cx_mat A;

  /// @brief Autocorrelation vector.
  arma::cx_vec a;

  /// @brief Cross-correlation vector.
  arma::cx_vec b;

  /// @brief Weights vector.
  arma::cx_vec w;

  /// @brief Fill a and b with the circular correlations of the reference with
  /// itself and with the surveillance, for lags 0 to nBins-1.
  /// @return Void.
  void correlate();

  /// @brief Apply the nBins-tap weights to the reference.
  /// @param out Destination for the first nSamples convolution outputs.
  /// @return Void.
  void convolve(std::complex<double> *out);

public:
  /// @brief Constructor.
  /// @param delayMin Minimum clutter filter delay (bins).
  /// @param delayMax Maximum clutter filter delay (bins).
  /// @param nSamples Number of samples per CPI.
  /// @return The object.
  WienerHopf(int32_t delayMin, int32_t delayMax, uint32_t nSamples);

  /// @brief Length the correlation and convolution blocks are planned at.
  /// @return Number of points.
  uint32_t filter_fft_length() const { return nBlock; }

  /// @brief Destructor.
  /// @return Void.
  ~WienerHopf();

  /// @brief Implement the clutter filter.
  /// @param x Reference samples.
  /// @param y Surveillance samples.
  /// @return True if clutter filter successful.
  bool process(IqData *x, IqData *y);
};

#endif