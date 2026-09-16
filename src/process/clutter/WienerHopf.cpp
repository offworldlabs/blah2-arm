#include "WienerHopf.h"

#include "BulkRotation.h"
#include "CorrelationWorker.h"
#include "HermitianToeplitz.h"
#include "ToeplitzAudit.h"
#ifdef OWL_ENABLE_GPU_FIR
#include "gpu/OwlGpuFilter.h"
#endif

#include <armadillo>
#include <fftw3.h>

#include <algorithm>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#ifdef OWL_CLUTTER_TEST_PLAN_FAILURE
namespace owl_clutter_test {
thread_local unsigned failAtPlan = 0;
thread_local unsigned planOrdinal = 0;
void failNextConstructionAtPlan(unsigned ordinal)
{
  failAtPlan = ordinal;
  planOrdinal = 0;
}
}  // namespace owl_clutter_test
#endif

namespace {

using Complex = std::complex<double>;
constexpr uint32_t kFilterBatch = 4;

bool useGpuFir()
{
  const char* value = std::getenv("OWL_GPU_FILTER_PERCENT");
  if (!value || std::string(value) == "0") return false;
  if (std::string(value) == "50")
  {
#ifdef OWL_ENABLE_GPU_FIR
    return true;
#else
    throw std::runtime_error("GPU FIR requested but this build has OWL_GPU_FIR=OFF");
#endif
  }
  throw std::invalid_argument("OWL_GPU_FILTER_PERCENT must be 0 or 50");
}

#ifdef OWL_ENABLE_GPU_FIR
struct GpuCycle
{
  OwlGpuFilter* gpu;
  ~GpuCycle() { if (gpu) gpu->abort(); }
};
#endif

uint32_t powerOfTwoLength(uint64_t minimum, uint32_t floor,
                          const char* error)
{
  uint32_t length = floor;
  while (uint64_t(length) < minimum)
  {
    if (length > uint32_t(std::numeric_limits<int>::max()) / 2)
      throw std::invalid_argument(error);
    length *= 2;
  }
  return length;
}

unsigned planningFlags()
{
  const char* value = std::getenv("OWL_CLUTTER_PLAN");
  if (!value || std::string(value) == "estimate") return FFTW_ESTIMATE;
  if (std::string(value) == "measure") return FFTW_MEASURE;
  throw std::invalid_argument(
      "OWL_CLUTTER_PLAN must be 'estimate' or 'measure'");
}

bool denseOnly()
{
  const char* value = std::getenv("OWL_CLUTTER_DENSE_ONLY");
  if (!value || std::string(value) == "0") return false;
  if (std::string(value) == "1") return true;
  throw std::invalid_argument("OWL_CLUTTER_DENSE_ONLY must be '0' or '1'");
}

bool useCorrelationWorker()
{
  const char* value = std::getenv("OWL_CLUTTER_CORR_WORKERS");
  if (!value || std::string(value) == "1") return false;
  if (std::string(value) == "2") return true;
  throw std::invalid_argument(
      "OWL_CLUTTER_CORR_WORKERS must be '1' or '2'");
}

std::mutex& plannerMutex()
{
  static std::mutex value;
  return value;
}

void initialiseFftwThreads()
{
  static std::once_flag once;
  static bool ready = false;
  std::call_once(once, [] { ready = fftw_init_threads() != 0; });
  if (!ready) throw std::runtime_error("FFTW thread initialisation failed");
}

struct FftwFree
{
  void operator()(Complex* value) const { fftw_free(value); }
};
using Buffer = std::unique_ptr<Complex, FftwFree>;

Buffer allocate(uint64_t count)
{
  if (!count || count > std::numeric_limits<size_t>::max() / sizeof(Complex))
    throw std::invalid_argument("Clutter buffer size is not representable");
  Buffer result(static_cast<Complex*>(fftw_malloc(sizeof(Complex) * count)));
  if (!result) throw std::bad_alloc();
  return result;
}

struct PlanFree
{
  void operator()(fftw_plan_s* value) const
  {
    if (value) fftw_destroy_plan(value);
  }
};
using Plan = std::unique_ptr<fftw_plan_s, PlanFree>;
static_assert(std::is_nothrow_move_assignable<Plan>::value,
              "Completed FFTW plan transfer must not throw");

Plan checkedPlan(fftw_plan value)
{
#ifdef OWL_CLUTTER_TEST_PLAN_FAILURE
  if (owl_clutter_test::failAtPlan &&
      ++owl_clutter_test::planOrdinal == owl_clutter_test::failAtPlan)
  {
    // Simulate FFTW returning null after earlier plans succeeded. The real
    // plan returned by FFTW is released here while the planner lock is held.
    if (value) fftw_destroy_plan(value);
    throw std::runtime_error("Injected clutter FFT plan failure");
  }
#endif
  if (!value) throw std::runtime_error("FFTW could not create clutter plan");
  return Plan(value);
}

}  // namespace

struct WienerHopf::Impl
{
  const int32_t delayMin;
  const uint32_t taps;
  const uint32_t samples;
  const uint32_t correlationLength;
  const uint32_t filterLength;
  const bool forceDense;
  const bool workerEnabled;
#ifdef OWL_ENABLE_GPU_FIR
  std::unique_ptr<OwlGpuFilter> gpu;
#endif

  arma::cx_mat matrix;
  arma::cx_vec autocorrelation;
  arma::cx_vec crosscorrelation;
  arma::cx_vec weights;

  Buffer dataX;
  Buffer dataY;
  Buffer correlationBuffer;
  Buffer correlationA;
  Buffer correlationB;
  Buffer filterX;
  Buffer filterW;
  Buffer filterOutput;

  Plan correlationForward;
  Plan correlationInverseA;
  Plan correlationInverseB;
  Plan filterForwardX;
  Plan filterForwardW;
  Plan filterInverse;

  // Declared after every borrowed plan and input buffer. The destructor also
  // resets it explicitly before destroying plans, including outstanding work.
  std::unique_ptr<owl_corr::Worker> worker;

  static uint32_t validateTaps(int32_t first, int32_t last,
                               uint32_t sampleCount)
  {
    const int64_t count = int64_t(last) - first;
    if (!sampleCount ||
        sampleCount > uint32_t(std::numeric_limits<int>::max()) ||
        count <= 0 || uint64_t(count) > sampleCount)
      throw std::invalid_argument(
          "Clutter filter needs a non-empty half-open delay range no longer "
          "than the CPI");
    return static_cast<uint32_t>(count);
  }

  Impl(int32_t firstDelay, int32_t lastDelay, uint32_t sampleCount)
      : delayMin(firstDelay),
        taps(validateTaps(firstDelay, lastDelay, sampleCount)),
        samples(sampleCount),
        correlationLength(powerOfTwoLength(
            uint64_t(taps) * 2, 4096, "Correlation block FFT too large")),
        filterLength(powerOfTwoLength(
            uint64_t(taps) * 2, 1024, "Clutter block FFT too large")),
        forceDense(denseOnly()),
        workerEnabled(useCorrelationWorker()),
        matrix(taps, taps),
        autocorrelation(taps),
        crosscorrelation(taps),
        weights(taps),
        dataX(allocate(samples)),
        dataY(allocate(samples)),
        correlationBuffer(allocate(uint64_t(correlationLength) * 3)),
        correlationA(allocate(correlationLength)),
        correlationB(allocate(correlationLength)),
        filterX(allocate(uint64_t(filterLength) * kFilterBatch)),
        filterW(allocate(filterLength)),
        filterOutput(allocate(uint64_t(filterLength) * kFilterBatch))
  {
    const bool gpuRequested = useGpuFir();
#ifdef OWL_ENABLE_GPU_FIR
    // GPU setup may throw. Complete it before FFTW plans are transferred to
    // members, whose exceptional cleanup must remain under the planner lock.
    std::unique_ptr<OwlGpuFilter> newGpu;
    if (gpuRequested) newGpu = std::make_unique<OwlGpuFilter>(samples, taps);
#else
    (void)gpuRequested;
#endif
    initialiseFftwThreads();
    const unsigned flags = planningFlags();
    {
      std::lock_guard<std::mutex> lock(plannerMutex());
      // Planning is deliberately single-threaded. The optional second
      // executor only runs an already-created forward plan. Keep each plan in
      // a local owner until every plan and the worker are constructed. On any
      // exception these locals are destroyed before the lock guard unwinds;
      // member-plan cleanup cannot race another instance's FFTW planning.
      fftw_plan_with_nthreads(1);
      int correlation = static_cast<int>(correlationLength);
      Plan newCorrelationForward = checkedPlan(fftw_plan_many_dft(
          1, &correlation, 3,
          reinterpret_cast<fftw_complex*>(correlationBuffer.get()), nullptr,
          1, correlation,
          reinterpret_cast<fftw_complex*>(correlationBuffer.get()), nullptr,
          1, correlation, FFTW_FORWARD, flags));
      Plan newCorrelationInverseA = checkedPlan(fftw_plan_dft_1d(
          correlation, reinterpret_cast<fftw_complex*>(correlationA.get()),
          reinterpret_cast<fftw_complex*>(correlationA.get()), FFTW_BACKWARD,
          flags));
      Plan newCorrelationInverseB = checkedPlan(fftw_plan_dft_1d(
          correlation, reinterpret_cast<fftw_complex*>(correlationB.get()),
          reinterpret_cast<fftw_complex*>(correlationB.get()), FFTW_BACKWARD,
          flags));

      int filter = static_cast<int>(filterLength);
      Plan newFilterForwardX = checkedPlan(fftw_plan_many_dft(
          1, &filter, kFilterBatch,
          reinterpret_cast<fftw_complex*>(filterX.get()), nullptr, 1, filter,
          reinterpret_cast<fftw_complex*>(filterX.get()), nullptr, 1, filter,
          FFTW_FORWARD, flags));
      Plan newFilterForwardW = checkedPlan(fftw_plan_dft_1d(
          filter, reinterpret_cast<fftw_complex*>(filterW.get()),
          reinterpret_cast<fftw_complex*>(filterW.get()), FFTW_FORWARD,
          flags));
      Plan newFilterInverse = checkedPlan(fftw_plan_many_dft(
          1, &filter, kFilterBatch,
          reinterpret_cast<fftw_complex*>(filterOutput.get()), nullptr, 1,
          filter, reinterpret_cast<fftw_complex*>(filterOutput.get()), nullptr,
          1, filter, FFTW_BACKWARD, flags));

      if (workerEnabled)
        worker = std::make_unique<owl_corr::Worker>(correlationLength,
                                                    correlationBuffer.get());

      // unique_ptr moves are noexcept; after this point no constructor work
      // can throw, and the normal destructor destroys these under the lock.
      correlationForward = std::move(newCorrelationForward);
      correlationInverseA = std::move(newCorrelationInverseA);
      correlationInverseB = std::move(newCorrelationInverseB);
      filterForwardX = std::move(newFilterForwardX);
      filterForwardW = std::move(newFilterForwardW);
      filterInverse = std::move(newFilterInverse);
#ifdef OWL_ENABLE_GPU_FIR
      gpu = std::move(newGpu);
#endif
    }
  }

  ~Impl()
  {
    worker.reset();
    std::lock_guard<std::mutex> lock(plannerMutex());
    filterInverse.reset();
    filterForwardW.reset();
    filterForwardX.reset();
    correlationInverseB.reset();
    correlationInverseA.reset();
    correlationForward.reset();
  }

  void correlateSerial()
  {
    const uint32_t hop = correlationLength - taps + 1;
    for (uint64_t begin = 0; begin < samples; begin += hop)
    {
      owl_corr::transform(
          {dataX.get(), dataY.get(), samples, taps, correlationLength, begin,
           correlationForward.get()},
          correlationBuffer.get());
      owl_corr::accumulate(correlationBuffer.get(), correlationLength,
                           correlationA.get(), correlationB.get());
    }
  }

  bool process(IqData* reference, IqData* surveillance)
  {
    if (!reference || !surveillance ||
        reference->get_length() != samples ||
        surveillance->get_length() != samples)
      throw std::invalid_argument("Clutter input length differs from CPI");

    const auto& x = reference->view_data();
    const auto& y = surveillance->view_data();
    owl_clutter::copyRotation(x, samples, delayMin, dataX.get());
    std::copy_n(y.begin(), samples, dataY.get());
#ifdef OWL_ENABLE_GPU_FIR
    if (gpu) gpu->reference(dataX.get());
    GpuCycle gpuCycle{gpu.get()};
#endif

    std::fill(correlationA.get(), correlationA.get() + correlationLength,
              Complex{});
    std::fill(correlationB.get(), correlationB.get() + correlationLength,
              Complex{});
    if (worker)
      owl_corr::correlate(*worker, dataX.get(), dataY.get(), samples, taps,
                          correlationLength, correlationForward.get(),
                          correlationBuffer.get(), correlationA.get(),
                          correlationB.get());
    else
      correlateSerial();

    fftw_execute(correlationInverseA.get());
    fftw_execute(correlationInverseB.get());
    for (uint32_t i = 0; i < taps; ++i)
    {
      autocorrelation[i] =
          std::conj(correlationA.get()[i]) / double(correlationLength);
      crosscorrelation[i] =
          correlationB.get()[i] / double(correlationLength);
    }

    owl_toeplitz::Result fastSolve;
    if (!forceDense)
      fastSolve = owl_toeplitz::solve(
          autocorrelation.memptr(), crosscorrelation.memptr(),
          weights.memptr(), taps, correlationBuffer.get(),
          correlationBuffer.get() + taps);
#ifdef OWL_TOEPLITZ_AUDIT
    owlToeplitzAudit.record(fastSolve);
#endif
    if (!fastSolve.accepted())
    {
      matrix = arma::toeplitz(autocorrelation);
      for (uint32_t row = 0; row < taps; ++row)
        for (uint32_t column = 0; column < row; ++column)
          matrix(row, column) = std::conj(matrix(row, column));

      if (!arma::chol(matrix, matrix))
      {
        std::cerr << "Chol decomposition failed, skip clutter filter\n";
        return false;
      }
      if (!arma::solve(weights, arma::trimatu(matrix),
                       arma::solve(arma::trimatl(arma::trans(matrix)),
                                   crosscorrelation)))
      {
        std::cerr << "Solve failed, skip clutter filter\n";
        return false;
      }
    }

    uint32_t cpuStart = 0;
#ifdef OWL_ENABLE_GPU_FIR
    if (gpu) cpuStart = gpu->start(dataX.get(), weights.memptr());
#endif
    std::copy_n(weights.memptr(), taps, filterW.get());
    std::fill(filterW.get() + taps, filterW.get() + filterLength, Complex{});
    fftw_execute(filterForwardW.get());

    const uint32_t hop = filterLength - taps + 1;
    for (uint64_t base = cpuStart; base < samples;
         base += uint64_t(kFilterBatch) * hop)
    {
      for (uint32_t lane = 0; lane < kFilterBatch; ++lane)
      {
        Complex* slot = filterX.get() + uint64_t(lane) * filterLength;
        const int64_t first = int64_t(base) + int64_t(lane) * hop -
                              (taps - 1);
        const int64_t lower = std::max<int64_t>(0, first);
        const int64_t upper =
            std::min<int64_t>(samples, first + filterLength);
        if (lower >= upper)
        {
          std::fill(slot, slot + filterLength, Complex{});
          continue;
        }
        const size_t left = static_cast<size_t>(lower - first);
        const size_t count = static_cast<size_t>(upper - lower);
        std::fill(slot, slot + left, Complex{});
        std::copy(dataX.get() + lower, dataX.get() + upper, slot + left);
        std::fill(slot + left + count, slot + filterLength, Complex{});
      }

      fftw_execute(filterForwardX.get());
      for (uint32_t lane = 0; lane < kFilterBatch; ++lane)
        for (uint32_t i = 0; i < filterLength; ++i)
        {
          const uint64_t index = uint64_t(lane) * filterLength + i;
          filterOutput.get()[index] =
              filterX.get()[index] * filterW.get()[i];
        }
      fftw_execute(filterInverse.get());

      for (uint32_t lane = 0; lane < kFilterBatch; ++lane)
      {
        const uint64_t begin = base + uint64_t(lane) * hop;
        if (begin >= samples) break;
        const uint32_t count = static_cast<uint32_t>(
            std::min<uint64_t>(hop, samples - begin));
        const Complex* valid = filterOutput.get() +
                               uint64_t(lane) * filterLength + taps - 1;
        for (uint32_t i = 0; i < count; ++i)
          dataY.get()[begin + i] -= valid[i] / double(filterLength);
      }
    }

#ifdef OWL_ENABLE_GPU_FIR
    if (gpu) gpu->finish(dataY.get());
#endif

    surveillance->assign_samples(dataY.get(), samples);
    return true;
  }
};

WienerHopf::WienerHopf(int32_t delayMin, int32_t delayMax,
                       uint32_t nSamples)
    : impl_(std::make_unique<Impl>(delayMin, delayMax, nSamples))
{
}

WienerHopf::~WienerHopf() = default;

uint32_t WienerHopf::filter_fft_length() const
{
  return impl_->filterLength;
}

bool WienerHopf::process(IqData* x, IqData* y)
{
  return impl_->process(x, y);
}
