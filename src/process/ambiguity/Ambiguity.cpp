#include <iterator>
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include "process/ambiguity/Ambiguity.h"
#include <complex>
#include <iostream>
#include <deque>
#include <vector>
#include <numeric>
#include <math.h>
#include <chrono>
#include <fstream>
#include <string>
#include <array>
#include <thread>
#include <exception>
#include <initializer_list>
#include <stdexcept>

namespace {
int ambiguity_option(const char* key, int fallback, std::initializer_list<int> allowed) {
  const char* value = std::getenv(key);
  if (!value) return fallback;
  char* end = nullptr;
  long parsed = std::strtol(value, &end, 10);
  if (end != value && *end == 0)
    for (int choice : allowed) if (parsed == choice) return choice;
  throw std::invalid_argument(std::string("Invalid ") + key);
}
}


// constructor
Ambiguity::Ambiguity(int32_t _delayMin, int32_t _delayMax, 
  int32_t _dopplerMin, int32_t _dopplerMax, uint32_t _fs, 
  uint32_t _n, bool _roundHamming)
{
  // init
  delayMin = _delayMin;
  delayMax = _delayMax;
  dopplerMin = _dopplerMin;
  dopplerMax = _dopplerMax;
  fs = _fs;
  nSamples = _n;
  nDelayBins = static_cast<uint16_t>(_delayMax - _delayMin + 1);
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
  cpi = (static_cast<double>(nCorr) * nDopplerBins) / fs;

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

  // other setup
  // Only the requested signed lags must be free of circular aliasing.
  // M >= N + max(abs(lag)) is sufficient; the full window recovers 2*N-1.
  nfft = nCorr + static_cast<uint32_t>(std::max(std::abs(int64_t(delayMin)), std::abs(int64_t(delayMax))));
  if (_roundHamming) {
    nfft = next_hamming(nfft);
  }
  dataCorr.resize(2 * nDelayBins + 1);

  // compute FFTW plans in constructor
  // A bounded tile amortizes dispatch for independent range rows. Derive the
  // tile size from scratch storage during processing, preserving the class ABI.
  const bool rowWorkers = ambiguity_option("OWL_AMBIG_WORKERS", 0, {0,2}) == 2;
  const int rowBatch = ambiguity_option("OWL_AMBIG_ROWS", 1, {1,2,4,8,16,32});
  if (rowWorkers && rowBatch != 1) throw std::invalid_argument("Row workers require one-row FFT plans");
  const int rangeThreads = ambiguity_option("OWL_AMBIG_RANGE_THREADS", 2, {1,2});
  const char* planner = std::getenv("OWL_AMBIG_PLAN");
  if (planner && std::string(planner) != "estimate" && std::string(planner) != "measure")
    throw std::invalid_argument("Invalid OWL_AMBIG_PLAN");
  const unsigned planFlags = planner && std::string(planner) == "measure" ? FFTW_MEASURE : FFTW_ESTIMATE;
  dataXi.resize(size_t(nfft) * 2 * rowBatch);
  if (rowWorkers) { dataYi.resize(size_t(nfft)*2); dataCorr.resize(nfft); }
  else dataYi.clear();
  dataZi.resize(size_t(nfft) * rowBatch);
  dataDoppler.resize(nfft);
  const int savedThreads = fftw_planner_nthreads();
  if (rowWorkers) fftw_plan_with_nthreads(1);
  else if (nfft <= 4096) fftw_plan_with_nthreads(std::min(savedThreads, rangeThreads));
  const int rangeLength = int(nfft);
  fftXi = fftw_plan_many_dft(1, &rangeLength, 2 * rowBatch,
      reinterpret_cast<fftw_complex*>(dataXi.data()), nullptr, 1, rangeLength,
      reinterpret_cast<fftw_complex*>(dataXi.data()), nullptr, 1, rangeLength,
      FFTW_FORWARD, planFlags);
  fftYi = nullptr;
  if (rowBatch == 1) {
    fftZi = fftw_plan_dft_1d(nfft, reinterpret_cast<fftw_complex *>(dataZi.data()),
                           reinterpret_cast<fftw_complex *>(dataZi.data()), FFTW_BACKWARD, planFlags);
  } else {
    fftZi = fftw_plan_many_dft(1, &rangeLength, rowBatch,
      reinterpret_cast<fftw_complex*>(dataZi.data()), nullptr, 1, rangeLength,
      reinterpret_cast<fftw_complex*>(dataZi.data()), nullptr, 1, rangeLength,
      FFTW_BACKWARD, planFlags);
  }
  if (!fftXi || !fftZi) {
    if (fftXi) fftw_destroy_plan(fftXi);
    if (fftZi) fftw_destroy_plan(fftZi);
    fftw_plan_with_nthreads(savedThreads);
    throw std::runtime_error("Could not create ambiguity range FFT plan");
  }
  if (rowWorkers && savedThreads >= 2) {
    // New-array execution is thread-safe, but requires identical alignment.
    // Check before worker startup; never disable SIMD to hide a mismatch.
    if (fftw_alignment_of(reinterpret_cast<double*>(dataZi.data())) !=
        fftw_alignment_of(reinterpret_cast<double*>(dataCorr.data()))) {
      fftw_destroy_plan(fftXi); fftw_destroy_plan(fftZi);
      fftw_plan_with_nthreads(savedThreads);
      throw std::runtime_error("Row worker inverse scratch alignment mismatch");
    }
    fftYi = fftw_plan_many_dft(1, &rangeLength, 2,
      reinterpret_cast<fftw_complex*>(dataYi.data()), nullptr, 1, rangeLength,
      reinterpret_cast<fftw_complex*>(dataYi.data()), nullptr, 1, rangeLength,
      FFTW_FORWARD, planFlags);
    if (!fftYi) {
      fftw_destroy_plan(fftXi);
      fftw_destroy_plan(fftZi);
      fftw_plan_with_nthreads(savedThreads);
      throw std::runtime_error("Could not create ambiguity worker FFT plan");
    }
  }
  fftw_plan_with_nthreads(savedThreads);
  if (nDopplerBins <= 1024) fftw_plan_with_nthreads(1);
  fftDoppler = fftw_plan_dft_1d(nDopplerBins, reinterpret_cast<fftw_complex *>(dataDoppler.data()),
                                reinterpret_cast<fftw_complex *>(dataDoppler.data()), FFTW_FORWARD, FFTW_ESTIMATE);

  fftw_plan_with_nthreads(savedThreads);
  if (!fftDoppler) {
    fftw_destroy_plan(fftXi);
    if (fftYi) fftw_destroy_plan(fftYi);
    fftw_destroy_plan(fftZi);
    throw std::runtime_error("Could not create ambiguity Doppler FFT plan");
  }

}

Ambiguity::~Ambiguity()
{
  fftw_destroy_plan(fftXi);
  if (fftYi) fftw_destroy_plan(fftYi);
  fftw_destroy_plan(fftZi);
  fftw_destroy_plan(fftDoppler);
}

Map<std::complex<double>> *Ambiguity::process(IqData *x, IqData *y)
{ return process_impl(x,y,false); }
Map<std::complex<double>> *Ambiguity::process_owned(IqData *x, IqData *y)
{
  const uint64_t used=uint64_t(nDopplerBins)*nCorr;
  if(!x || !y || x==y || dopplerMiddle!=0 || x->get_length()<used || y->get_length()<used
     || x->get_length()-used>nfft || y->get_length()-used>nfft)
    throw std::invalid_argument("Owned ambiguity requires distinct complete zero-centered CPIs");
  return process_impl(x,y,true);
}
Map<std::complex<double>> *Ambiguity::process_impl(IqData *x, IqData *y, bool retain)
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

  // The FFT scratch can retain the unconsumed tail after all range rows.
  // Fall back for aliased channels, short input or a tail larger than scratch.
  const uint64_t consumed = uint64_t(nDopplerBins) * nCorr;
  const bool bulkInput = x != y && consumed <= x->get_length() && consumed <= y->get_length()
      && x->get_length() - consumed <= nfft
      && y->get_length() - consumed <= nfft;
  auto inputX = x->view_data().begin();
  auto inputY = y->view_data().begin();

  const bool profile = std::getenv("OWL_AMBIG_PROFILE_LOG") != nullptr;
  using Clock = std::chrono::steady_clock;
  std::array<double, 9> costs{};
  auto tick = [&]() { return profile ? Clock::now() : Clock::time_point{}; };
  auto add = [&](int phase, Clock::time_point t) {
    if (profile) costs[phase] += std::chrono::duration<double,std::milli>(Clock::now()-t).count();
  };
  const auto begin = tick();
  const double normalization = 1.0 / double(nfft);
  const size_t rowBatch = dataZi.size() / nfft;
  nSamples = nDopplerBins * nCorr;
  const bool parallelRows = fftYi && bulkInput;
  if (parallelRows) {
    // Each worker retains a one-row working set and performs its whole row.
    // Plans were made on the constructor thread; only execution is concurrent.
    std::array<std::array<double,5>,2> workerCosts{};
    std::array<std::exception_ptr,2> failures{};
    auto work = [&](size_t lane, size_t first, size_t last) {
      try {
        auto* xi = lane ? dataYi.data() : dataXi.data();
        auto* yi = xi + nfft;
        auto* zi = lane ? dataCorr.data() : dataZi.data();
        auto ix = inputX + first*nCorr, iy = inputY + first*nCorr;
        auto note = [&](int phase, Clock::time_point t) {
          if (profile) workerCosts[lane][phase] += std::chrono::duration<double,std::milli>(Clock::now()-t).count();
        };
        for (size_t row = first; row < last; ++row) {
          auto t = tick();
          std::copy_n(ix,nCorr,xi); std::advance(ix,nCorr);
          std::copy_n(iy,nCorr,yi); std::advance(iy,nCorr);
          std::fill(xi+nCorr,xi+nfft,Complex{}); std::fill(yi+nCorr,yi+nfft,Complex{});
          note(0,t); t=tick();
          fftw_execute(lane ? fftYi : fftXi);
          note(1,t); t=tick();
          for (uint32_t j=0;j<nfft;++j) zi[j]=(yi[j]*std::conj(xi[j]))*normalization;
          note(2,t); t=tick();
          fftw_execute_dft(fftZi,reinterpret_cast<fftw_complex*>(zi),reinterpret_cast<fftw_complex*>(zi));
          note(3,t); t=tick();
          for (uint32_t j=0;j<nDelayBins;++j) {
            const int64_t lag=int64_t(delayMin)+j;
            const uint32_t bin=lag<0 ? int64_t(nfft)+lag : lag;
            map->data[row][j]=zi[bin];
          }
          note(4,t);
        }
      } catch (...) { failures[lane]=std::current_exception(); }
    };
    const size_t split=(nDopplerBins+1)/2;
    std::thread other(work,1,split,nDopplerBins);
    work(0,0,split);other.join();
    for (const auto& failure:failures) if(failure) std::rethrow_exception(failure);
    std::advance(inputX,consumed);std::advance(inputY,consumed);
    for(size_t lane=0;lane<2;++lane) for(size_t phase=0;phase<5;++phase) costs[phase]+=workerCosts[lane][phase];
  } else {
  for (size_t base = 0; base < nDopplerBins; base += rowBatch)
  {
    const size_t active = std::min(rowBatch, size_t(nDopplerBins) - base);
    auto t = tick();
    for (size_t lane = 0; lane < rowBatch; ++lane) {
      auto xi = dataXi.begin() + lane * 2 * nfft;
      auto yi = xi + nfft;
      if (lane < active) {
        if (bulkInput) {
          std::copy_n(inputX, nCorr, xi); std::advance(inputX, nCorr);
          std::copy_n(inputY, nCorr, yi); std::advance(inputY, nCorr);
        } else {
          // Preserve the legacy interleaved pop order for aliased/short inputs.
          for (uint32_t j = 0; j < nCorr; ++j) {
            xi[j] = x->pop_front(); yi[j] = y->pop_front();
          }
        }
        std::fill(xi+nCorr, xi+nfft, Complex{});
        std::fill(yi+nCorr, yi+nfft, Complex{});
      } else {
        // The final partial tile must never reuse a previous tile's spectrum.
        std::fill(xi, yi+nfft, Complex{});
      }
    }
    add(0,t); t=tick();
    fftw_execute(fftXi);
    add(1,t); t=tick();
    for (size_t lane = 0; lane < rowBatch; ++lane) {
      const auto* xi = dataXi.data() + lane * 2 * nfft;
      const auto* yi = xi + nfft;
      auto* zi = dataZi.data() + lane * nfft;
      for (uint32_t j = 0; j < nfft; ++j)
        zi[j] = (yi[j] * std::conj(xi[j])) * normalization;
    }
    add(2,t); t=tick();
    fftw_execute(fftZi);
    add(3,t); t=tick();
    for (size_t lane = 0; lane < active; ++lane) {
      const auto* zi = dataZi.data() + lane * nfft;
      for (uint32_t j = 0; j < nDelayBins; ++j) {
        const int64_t lag = int64_t(delayMin) + j;
        const uint32_t bin = lag < 0 ? int64_t(nfft) + lag : lag;
        map->data[base+lane][j] = zi[bin];
      }
    }
    add(4,t);
  }
  }
  const double rangeWall = profile ? std::chrono::duration<double,std::milli>(Clock::now()-begin).count() : 0;

  auto tailStart = tick();
  if (bulkInput && !retain) {
    const uint32_t tailX = x->get_length() - consumed;
    const uint32_t tailY = y->get_length() - consumed;
    std::copy(inputX, x->view_data().end(), dataXi.begin());
    std::copy(inputY, y->view_data().end(), (dataXi.begin() + nfft));
    x->assign_samples(dataXi.data(), tailX);
    y->assign_samples((dataXi.data() + nfft), tailY);
  }

  add(5,tailStart);
  // doppler processing
  for (uint16_t i = 0; i < nDelayBins; i++)
  {
    auto t = tick();
    for (uint32_t j = 0; j < nDopplerBins; ++j)
      dataDoppler[j] = map->data[j][i];

    add(6,t); t=tick();
    fftw_execute(fftDoppler);
    add(7,t); t=tick();

    for (uint32_t j = 0; j < nDopplerBins; ++j)
      map->data[j][i] = dataDoppler[(j + int(nDopplerBins / 2) + 1) % nDopplerBins];
    add(8,t);
  }

  if (profile) {
    const double total = std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
    std::ofstream log(std::getenv("OWL_AMBIG_PROFILE_LOG"), std::ios::app);
    log << "{\"parallel_workers\":" << (parallelRows?2:0) << ",\"range_wall_ms\":" << rangeWall << ",\"rows\":" << rowBatch << ",\"nfft\":" << nfft << ",\"total_ms\":" << total << ",\"phases_ms\":[";
    for (size_t i=0;i<costs.size();++i) log << (i ? "," : "") << costs[i];
    log << "]}\n";
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
