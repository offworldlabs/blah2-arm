#include "process/clutter/WienerHopf.h"
#include "process/meta/FftLength.h"
#include <iostream>
#include <random>
#include <stdexcept>

using Complex = std::complex<double>;

static void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static bool fast(uint32_t value) {
  for (uint32_t factor : {2u, 3u, 5u, 7u})
    while (value % factor == 0) value /= factor;
  return value == 1 || value == 11 || value == 13;
}

static void run(unsigned samples, int first, int last) {
  const unsigned taps = last - first;
  IqData reference(samples), surveillance(samples);
  WienerHopf filter(first, last, samples);
  require(filter.filter_fft_length() == blah2::nextFastFftLength(samples + taps + 1),
    "Filter did not use the selected padded length");
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

    // Independent circular correlations and direct linear convolution, not
    // another FFT implementation. Reuse the filter on three different CPIs.
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
        "Padded clutter differs from direct convolution");
    }
    require(reference.get_data() == originalReference, "Reference mutated");
  }
  std::cout << "PASS samples=" << samples << " taps=" << taps
            << " first=" << first << " fft=" << filter.filter_fft_length() << '\n';
}

int main() {
  try {
    // Independently scan a bounded range to check both admissibility and
    // minimality, including lengths containing repeated factors of 11/13.
    for (uint32_t minimum = 1; minimum <= 4096; ++minimum) {
      uint32_t expected = minimum;
      while (!fast(expected)) ++expected;
      require(blah2::nextFastFftLength(minimum) == expected,
        "Selected FFT length is not the smallest allowed size");
    }
    require(blah2::nextFastFftLength(480211) == 481140,
      "200 ms convolution geometry changed");
    require(blah2::nextFastFftLength(1200211) == 1200500,
      "500 ms convolution geometry changed");
    for (uint64_t invalid : {uint64_t(0), uint64_t(INT32_MAX),
                             uint64_t(INT32_MAX) + 1, UINT64_MAX}) {
      bool rejected = false;
      try { (void)blah2::nextFastFftLength(invalid); }
      catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "Unrepresentable FFT geometry accepted");
    }
    for (unsigned samples : {64u, 127u, 257u})
      for (int first : {-3, 0, 2}) run(samples, first, first + 8);
    run(128, -2, -1);
    run(31, -2, 2); // Already-fast convolution length (36).
    run(32, 0, 32); // Tap count equal to CPI sample count.
    for (const auto& bounds : {std::pair<int, int>{0, 0}, {5, 2}, {0, 65},
                              {INT32_MIN, INT32_MAX}}) {
      bool rejected = false;
      try { WienerHopf invalid(bounds.first, bounds.second, 64); }
      catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "Invalid clutter tap range accepted");
    }
    bool rejected = false;
    try { WienerHopf invalid(0, 1, 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Empty CPI accepted");
    IqData x(64), y(64);
    for (unsigned i = 0; i < 64; ++i) { x.push_back({0, 0}); y.push_back({1, 0}); }
    const auto original = y.get_data();
    WienerHopf filter(0, 8, 64);
    require(!filter.process(&x, &y) && y.get_data() == original,
      "Singular clutter input was accepted or changed");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
