// The bounded overlap-save clutter convolution.
//
// Two things need holding down: the block has an alias-free overlap-save
// window, and the filter still computes the right answer. The latter is checked
// against an independent direct convolution rather than another FFT.
#include "process/clutter/WienerHopf.h"

#include <armadillo>

#include <algorithm>
#include <atomic>
#include <complex>
#include <cstdint>
#include <exception>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef OWL_CLUTTER_TEST_PLAN_FAILURE
namespace owl_clutter_test {
void failNextConstructionAtPlan(unsigned ordinal);
}
#endif

using Complex = std::complex<double>;

static void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

#ifdef OWL_CLUTTER_TEST_PLAN_FAILURE
static void checkConcurrentPlanFailure() {
  for (unsigned failingPlan = 1; failingPlan <= 6; ++failingPlan) {
    std::atomic<bool> start{false};
    bool injectedRejected = false, concurrentSucceeded = false;
    std::exception_ptr failingUnexpected, concurrentUnexpected;
    std::thread failing([&] {
      owl_clutter_test::failNextConstructionAtPlan(failingPlan);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      try {
        WienerHopf rejected(0, 8, 64);
      } catch (const std::runtime_error& error) {
        injectedRejected =
            std::string(error.what()) == "Injected clutter FFT plan failure";
      } catch (...) {
        failingUnexpected = std::current_exception();
      }
    });
    std::thread concurrent([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      try {
        WienerHopf valid(0, 8, 64);
        concurrentSucceeded = valid.filter_fft_length() == 1024;
      } catch (...) {
        concurrentUnexpected = std::current_exception();
      }
    });
    start.store(true, std::memory_order_release);
    failing.join();
    concurrent.join();
    if (failingUnexpected) std::rethrow_exception(failingUnexpected);
    if (concurrentUnexpected) std::rethrow_exception(concurrentUnexpected);
    require(injectedRejected && concurrentSucceeded,
      "Concurrent constructor did not survive injected FFTW plan failure");
  }
  std::cout << "PASS six injected FFTW plan failures during concurrent construction\n";
}
#endif

static void run(unsigned samples, int first, int last) {
  const unsigned taps = last - first;
  IqData reference(samples), surveillance(samples);
  WienerHopf filter(first, last, samples);

  // A block at least twice the tap count has a non-empty alias-free window.
  const uint32_t length = filter.filter_fft_length();
  require(length >= 2 * uint64_t(taps),
    "Filter FFT length is below the overlap-save invariant");
  require(length >= 1024 && (length & (length - 1)) == 0,
    "Filter FFT length is not a supported power-of-two block");

  std::mt19937 rng(9211);
  std::normal_distribution<double> random;
  for (int repeat = 0; repeat < 3; ++repeat) {
    reference.clear();
    surveillance.clear();
    arma::cx_vec x(samples), y(samples);
    for (unsigned i = 0; i < samples; ++i)
      reference.push_back({random(rng), random(rng)});
    const auto originalReference = reference.get_data();
    for (unsigned i = 0; i < samples; ++i) {
      const int64_t shifted = (int64_t(i) - first) % samples;
      x[i] = originalReference[shifted < 0 ? shifted + samples : shifted];
      y[i] = x[i] * Complex(.7, .2) + Complex(.2 * random(rng), .2 * random(rng));
      surveillance.push_back(y[i]);
    }

    // Independent circular correlations and a direct linear convolution, not
    // another FFT implementation. The filter is reused across three CPIs.
    arma::cx_vec a(taps, arma::fill::zeros), b(taps, arma::fill::zeros);
    for (unsigned lag = 0; lag < taps; ++lag)
      for (unsigned i = 0; i < samples; ++i) {
        a[lag] += std::conj(x[(i + lag) % samples]) * x[i];
        b[lag] += y[(i + lag) % samples] * std::conj(x[i]);
      }
    arma::cx_mat matrix = arma::toeplitz(a);
    for (unsigned row = 0; row < taps; ++row)
      for (unsigned col = 0; col < row; ++col)
        matrix(row, col) = std::conj(matrix(row, col));
    const arma::cx_vec weights = arma::solve(matrix, b);

    require(filter.process(&reference, &surveillance), "Full-rank fixture rejected");
    require(surveillance.get_length() == samples, "Output sample count changed");
    const auto filtered = surveillance.get_data();
    for (unsigned i = 0; i < samples; ++i) {
      Complex expected = y[i];
      for (unsigned tap = 0; tap < taps && tap <= i; ++tap)
        expected -= weights[tap] * x[i - tap];
      require(std::abs(expected - filtered[i]) < 1e-9,
        "Overlap-save clutter differs from direct convolution");
    }
    require(reference.get_data() == originalReference, "Reference mutated");
  }
  std::cout << "PASS samples=" << samples << " taps=" << taps
            << " first=" << first << " fft=" << length << '\n';
}

int main() {
  try {
#ifdef OWL_CLUTTER_TEST_PLAN_FAILURE
    checkConcurrentPlanFailure();
#endif
    // The shipped CPI uses a 1024-point overlap-save block, not a whole-CPI
    // transform. Its 615 valid samples per block leave a 10-sample tail.
    {
      constexpr uint32_t shippedSamples = 1000000;
      constexpr uint32_t shippedTaps = 410;
      WienerHopf shippedFilter(-10, 400, shippedSamples);
      const uint32_t block = shippedFilter.filter_fft_length();
      require(block == 1024, "Shipped clutter block is not 1024 points");
      const uint32_t hop = block - shippedTaps + 1;
      require(hop == 615, "Shipped clutter valid window changed");
      require((shippedSamples % hop) == 10,
        "Shipped clutter partial tail changed");
      require(uint64_t(block) < shippedSamples,
        "Shipped clutter unexpectedly uses a whole-CPI FFT");
      std::cout << "PASS shipped overlap-save geometry: block=" << block
                << " hop=" << hop << " tail=" << shippedSamples % hop << '\n';
    }

    // Tap growth is rounded to the next power of two while maintaining
    // filterLength >= 2*taps; 512 is the last tap count fitting 1024.
    WienerHopf atBoundary(0, 512, 1024);
    WienerHopf aboveBoundary(0, 513, 1024);
    require(atBoundary.filter_fft_length() == 1024 &&
            aboveBoundary.filter_fft_length() == 2048,
      "Overlap-save block growth at tap boundary changed");

    // Correctness across a spread of geometries, including positive delayMin,
    // which used to read the wrong sample: `i - delayMin` promoted to unsigned
    // and wrapped at 2^32. Negative and zero cancelled exactly, so the shipped
    // -10 was unaffected and the fault stayed hidden.
    for (unsigned samples : {64u, 127u, 257u})
      for (int first : {-3, 0, 2}) run(samples, first, first + 8);
    run(128, -2, -1);
    run(31, -2, 2);
    run(32, 0, 32);  // tap count equal to the CPI sample count
    run(5000, 3, 34);  // multiple correlation/filter blocks and a partial tail

    for (const auto& bounds : {std::pair<int, int>{0, 0}, {5, 2}, {0, 65},
                              {INT32_MIN, INT32_MAX}}) {
      bool rejected = false;
      try { WienerHopf reject(bounds.first, bounds.second, 64); }
      catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "Degenerate delay range accepted");
    }
    std::cout << "PASS degenerate delay ranges rejected\n";

    {
      WienerHopf filter(-1, 1, 8);
      IqData full(8), shortInput(8);
      for (unsigned i = 0; i < 8; ++i) full.push_back({double(i), 0});
      for (unsigned i = 0; i < 7; ++i) shortInput.push_back({double(i), 0});
      bool nullRejected = false, lengthRejected = false;
      try { (void)filter.process(nullptr, &full); }
      catch (const std::invalid_argument&) { nullRejected = true; }
      try { (void)filter.process(&full, &shortInput); }
      catch (const std::invalid_argument&) { lengthRejected = true; }
      require(nullRejected && lengthRejected,
        "Clutter process accepted an invalid CPI boundary");
    }

    // The selected output write replaces the deque only after all validation.
    {
      IqData samples(2);
      samples.push_back({1, 2});
      const auto original = samples.get_data();
      bool nullRejected = false, boundsRejected = false;
      try { samples.assign_samples(nullptr, 1); }
      catch (const std::invalid_argument&) { nullRejected = true; }
      Complex tooMany[3]{};
      try { samples.assign_samples(tooMany, 3); }
      catch (const std::invalid_argument&) { boundsRejected = true; }
      require(nullRejected && boundsRejected,
        "IqData accepted an invalid bounded replacement");
      require(samples.get_data() == original,
        "Invalid IqData replacement changed existing samples");
      samples.assign_samples(nullptr, 0);
      require(samples.get_length() == 0,
        "Zero-length IqData replacement did not clear samples");
    }

    std::cout << "All clutter FFT checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
