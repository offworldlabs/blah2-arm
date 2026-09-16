#include "process/clutter/BulkRotation.h"
#include "process/clutter/CorrelationWorker.h"

#include <fftw3.h>

#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

using Complex = std::complex<double>;

static void require(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
}

struct FftwFree
{
  void operator()(Complex* value) const { fftw_free(value); }
};
using Buffer = std::unique_ptr<Complex, FftwFree>;

static Buffer allocate(uint64_t count)
{
  Buffer value(static_cast<Complex*>(fftw_malloc(sizeof(Complex) * count)));
  if (!value) throw std::bad_alloc();
  return value;
}

static void serial(const std::vector<Complex>& x,
                   const std::vector<Complex>& y, uint32_t taps,
                   uint32_t length, fftw_plan plan, Complex* scratch,
                   std::vector<Complex>& a, std::vector<Complex>& b)
{
  const uint32_t samples = static_cast<uint32_t>(x.size());
  const uint32_t hop = length - taps + 1;
  std::fill(a.begin(), a.end(), Complex{});
  std::fill(b.begin(), b.end(), Complex{});
  for (uint64_t begin = 0; begin < samples; begin += hop)
  {
    owl_corr::transform(
        {x.data(), y.data(), samples, taps, length, begin, plan}, scratch);
    owl_corr::accumulate(scratch, length, a.data(), b.data());
  }
}

static void checkDirectCorrelation(const std::vector<Complex>& x,
                                   const std::vector<Complex>& y,
                                   uint32_t taps, uint32_t length,
                                   const std::vector<Complex>& frequencyA,
                                   const std::vector<Complex>& frequencyB)
{
  Buffer a = allocate(length);
  Buffer b = allocate(length);
  std::copy(frequencyA.begin(), frequencyA.end(), a.get());
  std::copy(frequencyB.begin(), frequencyB.end(), b.get());
  fftw_plan inverseA = fftw_plan_dft_1d(
      static_cast<int>(length), reinterpret_cast<fftw_complex*>(a.get()),
      reinterpret_cast<fftw_complex*>(a.get()), FFTW_BACKWARD, FFTW_ESTIMATE);
  fftw_plan inverseB = fftw_plan_dft_1d(
      static_cast<int>(length), reinterpret_cast<fftw_complex*>(b.get()),
      reinterpret_cast<fftw_complex*>(b.get()), FFTW_BACKWARD, FFTW_ESTIMATE);
  require(inverseA && inverseB, "Could not plan correlation reference inverse");
  fftw_execute(inverseA);
  fftw_execute(inverseB);

  for (uint32_t lag = 0; lag < taps; ++lag)
  {
    Complex directA{}, directB{};
    for (uint32_t i = 0; i < static_cast<uint32_t>(x.size()); ++i)
    {
      directA += std::conj(x[(i + lag) % x.size()]) * x[i];
      directB += y[(i + lag) % y.size()] * std::conj(x[i]);
    }
    const Complex actualA = std::conj(a.get()[lag]) / double(length);
    const Complex actualB = b.get()[lag] / double(length);
    const double scaleA = std::max(1.0, std::abs(directA));
    const double scaleB = std::max(1.0, std::abs(directB));
    require(std::abs(actualA - directA) < 2e-12 * scaleA,
            "Block autocorrelation differs from direct circular reference");
    require(std::abs(actualB - directB) < 2e-12 * scaleB,
            "Block crosscorrelation differs from direct circular reference");
  }
  fftw_destroy_plan(inverseB);
  fftw_destroy_plan(inverseA);
}

static void checkRotation()
{
  for (uint32_t samples : {1u, 7u, 31u, 127u})
  {
    std::deque<Complex> source;
    for (uint32_t i = 0; i < samples; ++i)
      source.emplace_back(i + 1, -double(i));
    std::vector<Complex> output(samples);
    for (int32_t delay : {-130, -3, -1, 0, 1, 2, 130})
    {
      owl_clutter::copyRotation(source, samples, delay, output.data());
      for (uint32_t i = 0; i < samples; ++i)
      {
        const int64_t shifted = (int64_t(i) - delay) % samples;
        const uint32_t index =
            static_cast<uint32_t>(shifted < 0 ? shifted + samples : shifted);
        require(output[i] == source[index],
                "Bulk rotation changed signed delay semantics");
      }
    }
  }

  std::deque<Complex> source(3);
  Complex output[3];
  bool zero = false, shortSource = false, nullOutput = false;
  try { owl_clutter::copyRotation(source, 0, 0, output); }
  catch (const std::invalid_argument&) { zero = true; }
  try { owl_clutter::copyRotation(source, 4, 0, output); }
  catch (const std::invalid_argument&) { shortSource = true; }
  try { owl_clutter::copyRotation(source, 3, 0,
                                  static_cast<Complex*>(nullptr)); }
  catch (const std::invalid_argument&) { nullOutput = true; }
  require(zero && shortSource && nullOutput,
          "Bulk rotation accepted an invalid boundary");
}

int main()
{
  try
  {
    require(fftw_init_threads() != 0, "FFTW threads unavailable");
    fftw_plan_with_nthreads(1);
    checkRotation();

    std::mt19937_64 random(0x19465);
    unsigned cases = 0;
    const std::array<std::array<uint32_t, 3>, 7> dimensions{{
        {{1, 1, 16}},
        {{7, 7, 16}},
        {{29, 5, 16}},
        {{31, 2, 16}},
        {{112, 17, 64}},
        {{4096, 410, 1024}},
        {{11061, 410, 4096}},
    }};

    for (const auto& dimension : dimensions)
    {
      const uint32_t samples = dimension[0];
      const uint32_t taps = dimension[1];
      const uint32_t length = dimension[2];
      int plannedLength = static_cast<int>(length);
      Buffer original = allocate(uint64_t(length) * 3);
      Buffer alternate = allocate(uint64_t(length) * 3);
      fftw_plan plan = fftw_plan_many_dft(
          1, &plannedLength, 3,
          reinterpret_cast<fftw_complex*>(original.get()), nullptr, 1,
          plannedLength, reinterpret_cast<fftw_complex*>(original.get()),
          nullptr, 1, plannedLength, FFTW_FORWARD, FFTW_ESTIMATE);
      require(plan != nullptr, "Could not plan correlation fixture");

      {
        owl_corr::Worker worker(length, original.get());
        for (unsigned mode = 0; mode < 4; ++mode)
        {
          std::vector<Complex> x(samples), y(samples);
          std::vector<Complex> a(length), b(length), parallelA(length),
              parallelB(length);
          for (uint32_t i = 0; i < samples; ++i)
          {
            if (mode == 0)
            {
              x[i] = {double(int64_t(random() % 6001) - 3000),
                      double(int64_t(random() % 6001) - 3000)};
              y[i] = {double(int64_t(random() % 6001) - 3000),
                      double(int64_t(random() % 6001) - 3000)};
            }
            else if (mode == 1)
            {
              x[i] = std::polar(1000.0, double(i) * .197);
              y[i] = std::polar(10.0, double(i) * .193);
            }
            else if (mode == 2)
            {
              if (i == 0 || i + 1 == samples) x[i] = {100, 20};
              if (i == samples / 2) y[i] = {20, -3};
            }
          }
          serial(x, y, taps, length, plan, original.get(), a, b);
          owl_corr::correlate(worker, x.data(), y.data(), samples, taps,
                              length, plan, alternate.get(), parallelA.data(),
                              parallelB.data());
          require(std::memcmp(a.data(), parallelA.data(),
                              length * sizeof(Complex)) == 0 &&
                      std::memcmp(b.data(), parallelB.data(),
                                  length * sizeof(Complex)) == 0,
                  "Ordered worker accumulation is not byte-identical");
          checkDirectCorrelation(x, y, taps, length, parallelA, parallelB);
          ++cases;
        }

        bool invalid = false, emptyFinish = false, doubleStart = false;
        try
        {
          worker.start({nullptr, nullptr, 0, taps, length, 0, plan});
        }
        catch (const std::invalid_argument&) { invalid = true; }
        try { (void)worker.finish(); }
        catch (const std::logic_error&) { emptyFinish = true; }
        std::vector<Complex> x(samples), y(samples);
        worker.start({x.data(), y.data(), samples, taps, length, 0, plan});
        try
        {
          worker.start({x.data(), y.data(), samples, taps, length, 0, plan});
        }
        catch (const std::logic_error&) { doubleStart = true; }
        (void)worker.finish();
        require(invalid && emptyFinish && doubleStart,
                "Correlation worker accepted an invalid lifecycle boundary");
      }

      // Destruction drains a borrowed-input job before the plan and inputs die.
      std::vector<Complex> x(samples), y(samples);
      {
        owl_corr::Worker worker(length, original.get());
        worker.start({x.data(), y.data(), samples, taps, length, 0, plan});
      }
      fftw_destroy_plan(plan);
    }

    std::cout << "{\"pass\":true,\"cases\":" << cases
              << ",\"direct_reference\":true"
              << ",\"bit_exact_ordering\":true"
              << ",\"invalid_boundaries\":true"
              << ",\"outstanding_shutdown\":true}\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
