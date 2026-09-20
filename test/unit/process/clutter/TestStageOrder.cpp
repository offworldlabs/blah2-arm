// Splitting the radar loop at the clutter filter moves the spectrum stage from
// before the filter to after it, because the front stage is the slow half and
// the spectrum reads only the reference channel. That is output-neutral if and
// only if the clutter filter leaves the reference channel untouched, so this
// proves that on data rather than by reading the source.
//
// Also checks the slot recycle. Ambiguity drains all but a partial batch, and
// the serial loop pushed the next CPI on top and let IqData::push_back evict
// the remainder; the back stage now clears the slot instead. Those have to
// leave the same samples behind.

#include "data/IqData.h"
#include "process/clutter/WienerHopf.h"
#include "process/spectrum/SpectrumAnalyser.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

using Complex = std::complex<double>;

static int failures = 0;

static void require(bool condition, const char* message)
{
  std::printf("%-64s %s\n", message, condition ? "ok" : "FAIL");
  if (!condition) failures++;
}

// A CPI with strong correlated clutter at several delays plus a weak echo, so
// the filter has real work to do and the solve is not degenerate.
static void fill(IqData* x, IqData* y, uint32_t n)
{
  std::vector<Complex> ref(n);
  for (uint32_t i = 0; i < n; i++)
  {
    ref[i] = {0.7 * std::sin(i * 0.013) + 0.3 * std::sin(i * 0.0007 + 1.1),
              0.7 * std::cos(i * 0.011) + 0.3 * std::cos(i * 0.0009 + 0.3)};
  }
  for (uint32_t i = 0; i < n; i++)
  {
    Complex s = 0.9 * ref[i];
    if (i >= 5) s += 0.45 * ref[i - 5];
    if (i >= 40) s += 0.20 * ref[i - 40];
    if (i >= 137) s += 0.01 * ref[i - 137];
    s += Complex(1e-4 * std::sin(i * 0.37), 1e-4 * std::cos(i * 0.41));
    x->push_back(ref[i]);
    y->push_back(s);
  }
}

static bool same(const std::vector<Complex>& a, const std::vector<Complex>& b)
{
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (a[i].real() != b[i].real() || a[i].imag() != b[i].imag()) return false;
  return true;
}

// The spectrum and frequency arrays, which is what the spectrum stage
// produces. IqData::to_json also emits min, max and mean, which are declared
// but never assigned anywhere in the codebase, so every IqData serialises
// whatever happened to be in those bytes. Two objects therefore differ there
// for reasons that have nothing to do with this test: comparing whole JSON
// passes on x86 and fails on aarch64 purely on allocator luck.
static std::string spectrum_of(const std::string& json)
{
  const size_t start = json.find("\"frequency\"");
  return start == std::string::npos ? json : json.substr(start);
}

int main()
{
  const uint32_t n = 200000;
  const int32_t delayMin = -10;
  const int32_t delayMax = 400;
  const double spectrumBandwidth = 2000;

  // The clutter filter must not touch the reference channel.
  {
    IqData x(n), y(n);
    fill(&x, &y, n);
    const std::vector<Complex> before = x.get_data();

    WienerHopf filter(delayMin, delayMax, n);
    require(filter.process(&x, &y), "clutter filter converged on the test signal");
    require(same(before, x.get_data()),
            "reference channel bit-identical after the clutter filter");

    // Confirm the filter actually cancelled, so the check above is not vacuous.
    IqData xRaw(n), yRaw(n);
    fill(&xRaw, &yRaw, n);
    double residual = 0, signal = 0;
    const auto filtered = y.get_data();
    const auto raw = yRaw.get_data();
    for (uint32_t i = n / 2; i < n; i++)
    {
      residual += std::norm(filtered[i]);
      signal += std::norm(raw[i]);
    }
    const double cancelDb = 10.0 * std::log10(signal / residual);
    std::printf("   clutter cancellation on the test signal: %.1f dB\n", cancelDb);
    require(cancelDb > 20.0, "filter cancelled enough that the test is not vacuous");
  }

  // Spectrum output is the same whichever side of the filter it runs on.
  {
    IqData xa(n), ya(n);
    fill(&xa, &ya, n);
    SpectrumAnalyser serialOrder(n, spectrumBandwidth);
    serialOrder.process(&xa);
    const std::string jsonBefore = xa.to_json(1234567890ULL);
    WienerHopf filterA(delayMin, delayMax, n);
    filterA.process(&xa, &ya);

    IqData xb(n), yb(n);
    fill(&xb, &yb, n);
    WienerHopf filterB(delayMin, delayMax, n);
    filterB.process(&xb, &yb);
    SpectrumAnalyser pipelineOrder(n, spectrumBandwidth);
    pipelineOrder.process(&xb);
    const std::string jsonAfter = xb.to_json(1234567890ULL);

    require(!spectrum_of(jsonBefore).empty(), "spectrum was actually produced");
    require(spectrum_of(jsonBefore) == spectrum_of(jsonAfter),
            "spectrum byte-identical whichever side of the filter it runs");
    require(same(ya.get_data(), yb.get_data()),
            "filtered surveillance channel identical under the reordering");
  }

  // Recycling a slot by clearing it matches the serial refill-with-eviction.
  {
    const uint32_t leftover = 78;  // what ambiguity's partial batch leaves
    IqData serial(n), pipelined(n);
    for (uint32_t i = 0; i < n; i++)
    {
      const Complex v = {(double)i, -(double)i};
      serial.push_back(v);
      pipelined.push_back(v);
    }
    for (uint32_t i = 0; i < n - leftover; i++)
    {
      serial.pop_front();
      pipelined.pop_front();
    }
    require(serial.get_length() == leftover, "leftover partial batch reproduced");

    pipelined.clear();
    for (uint32_t i = 0; i < n; i++)
    {
      const Complex v = {(double)i * 3.5, (double)i * -0.25};
      serial.push_back(v);
      pipelined.push_back(v);
    }
    require(same(serial.get_data(), pipelined.get_data()),
            "clear-then-refill matches the serial refill-with-eviction");
  }

  // The two stages plan at 2 threads each rather than both claiming 4, so the
  // transforms are not the ones the serial build planned. FFTW parallelises
  // across a Cooley-Tukey factor, so a different thread count sums in a
  // different order and the numbers move. Bound how far: the FFT length work
  // already accepted 6e-14 relative, about 264 dB below signal, so this has to
  // be no worse.
  {
    fftw_init_threads();
    const uint32_t m = 200000;
    std::vector<Complex> seed(m);
    for (uint32_t i = 0; i < m; i++)
      seed[i] = {std::sin(i * 0.001), std::cos(i * 0.0007)};

    auto transform = [&](int threads) {
      std::vector<Complex> buffer(seed);
      fftw_plan_with_nthreads(threads);
      fftw_plan plan = fftw_plan_dft_1d(
          m, reinterpret_cast<fftw_complex*>(buffer.data()),
          reinterpret_cast<fftw_complex*>(buffer.data()), FFTW_FORWARD, FFTW_ESTIMATE);
      fftw_execute(plan);
      fftw_destroy_plan(plan);
      return buffer;
    };

    // The single global count this change moves away from: blah2 used to plan
    // every transform with four threads. It is no longer a named constant in
    // the source, so it lives here purely as the comparison point.
    constexpr int legacyPlannerThreads = 4;
    const std::vector<Complex> serial = transform(legacyPlannerThreads);
    const std::vector<Complex> pipelined = transform(blah2::kBackStageThreads);

    double peak = 0, worst = 0;
    for (uint32_t i = 0; i < m; i++)
    {
      peak = std::max(peak, std::abs(serial[i]));
      worst = std::max(worst, std::abs(serial[i] - pipelined[i]));
    }
    const double relative = worst / peak;
    std::printf("   planner %d threads vs %d: %.3g of peak (%.0f dB below)\n",
                legacyPlannerThreads, blah2::kBackStageThreads, relative,
                20.0 * std::log10(peak / worst));
    require(relative < 6e-14,
            "per-stage thread count moves an FFT less than the accepted 6e-14");
  }

  std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
  return failures ? 1 : 0;
}
