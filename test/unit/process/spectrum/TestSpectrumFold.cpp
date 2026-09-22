// The folded spectrum, checked against the full-length transform it replaces.
//
// The claim is an identity, not an approximation: taking every Dth bin of an
// N-point DFT is the same as an M-point DFT of a weighted fold, N = M*D. So the
// test computes the old path in full, an nfft-point FFT followed by the shifted
// decimation the old code did, and requires the new path to agree bin for bin.
// The reference is built here rather than taken from the class, so a fault in
// the fold cannot hide behind itself.
#include "data/IqData.h"
#include "process/spectrum/SpectrumAnalyser.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <fftw3.h>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using Complex = std::complex<double>;

static void require(bool condition, const std::string& message)
{
  if (!condition) throw std::runtime_error(message);
}

// What SpectrumAnalyser did before: transform the whole CPI, then read out
// every decimation-th bin starting from the fftshift offset.
static std::vector<Complex> reference(const std::vector<Complex>& x,
                                      uint32_t nfft, uint32_t decimation)
{
  auto* buf = (fftw_complex*)fftw_malloc(sizeof(fftw_complex)*nfft);
  for (uint32_t i = 0; i < nfft; i++) { buf[i][0] = x[i].real(); buf[i][1] = x[i].imag(); }
  fftw_plan p = fftw_plan_dft_1d(int(nfft), buf, buf, FFTW_FORWARD, FFTW_ESTIMATE);
  fftw_execute(p);
  std::vector<Complex> out;
  out.reserve((nfft + decimation - 1)/decimation);
  for (uint32_t i = 0; i < nfft; i += decimation)
  {
    const uint32_t idx = uint32_t((uint64_t(i) + nfft/2 + 1) % nfft);
    out.push_back(Complex(buf[idx][0], buf[idx][1]));
  }
  fftw_destroy_plan(p);
  fftw_free(buf);
  return out;
}

static void run(uint32_t n, double bandwidth, double tolerance)
{
  const uint32_t decimation = uint32_t(n/bandwidth);
  const uint32_t nSpectrum = n/decimation;
  const uint32_t nfft = nSpectrum*decimation;

  std::mt19937 rng(7771 + n);
  std::normal_distribution<double> gauss;
  std::vector<Complex> raw(n);
  IqData iq(n);
  for (uint32_t i = 0; i < n; i++)
  {
    raw[i] = Complex(gauss(rng), gauss(rng));
    iq.push_back(raw[i]);
  }

  SpectrumAnalyser analyser(n, bandwidth);
  require(analyser.fft_length() == nSpectrum, "transform is not at the spectrum length");
  require(analyser.equivalent_fft_length() == nfft, "equivalent length disagrees");
  analyser.process(&iq);

  const std::vector<Complex>& got = iq.get_spectrum();
  const std::vector<Complex> want = reference(raw, nfft, decimation);

  require(got.size() == want.size(),
          "spectrum length " + std::to_string(got.size()) +
          " against " + std::to_string(want.size()));

  double peak = 0;
  for (const Complex& v : want) peak = std::max(peak, std::abs(v));
  require(peak > 0, "reference spectrum is all zero");

  double worst = 0;
  for (size_t i = 0; i < want.size(); i++)
    worst = std::max(worst, std::abs(got[i] - want[i])/peak);

  require(worst < tolerance,
          "worst bin differs by " + std::to_string(worst) + " of peak");
  std::printf("PASS n=%-8u bw=%-7.0f D=%-5u M=%-6u nfft=%-8u worst=%.3e\n",
              n, bandwidth, decimation, nSpectrum, nfft, worst);
}

int main()
{
  try
  {
    // The geometry the radar actually runs: 1,000,000 samples folded to 2000,
    // which is the case the change exists for.
    run(1000000, 2000, 1e-12);

    // Smaller shapes, including a decimation that does not divide n evenly so
    // nfft < n, and an odd spectrum length.
    run(10000, 100, 1e-12);
    run(4096, 64, 1e-12);
    run(9999, 100, 1e-12);
    run(1000, 333, 1e-12);

    // Degenerate: decimation of 1 means the fold is a single segment and the
    // transform is full length. The identity must still hold rather than the
    // code needing a special case.
    run(1000, 1000, 1e-12);

    // An odd decimation, which changes the frequency offset branch.
    run(2001, 23, 1e-12);

    // A bandwidth wider than the CPI cannot produce a spectrum, and is
    // rejected rather than dividing by zero.
    {
      bool rejected = false;
      try { SpectrumAnalyser reject(100, 1000); }
      catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "a bandwidth wider than the CPI was accepted");
      std::printf("PASS bandwidth wider than the CPI rejected\n");
    }

    std::printf("All spectrum fold checks passed\n");
  }
  catch (const std::exception& error)
  {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
