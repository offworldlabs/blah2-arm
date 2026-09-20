// The block clutter filter.
//
// Two things need holding down. That the block length stays self-consistent
// across every geometry, since a block at or below the tap count leaves no
// valid outputs. And that the filter still computes the right answer, checked
// against an independent direct convolution rather than against another FFT,
// so a fault in the transform path cannot hide behind itself.
//
// The geometries below deliberately straddle the point where the block is
// capped to the whole CPI. Anything short enough to run as a SINGLE block
// leaves the partial-sum accumulation untested, and that is where this
// formulation goes wrong: a correlation numerator taking the full window on
// both sides, or a normalisation by the CPI length rather than the block
// length, both survive a single-block test and are both badly wrong.
#include "process/clutter/WienerHopf.h"
#include "process/meta/FftLength.h"

#include <algorithm>
#include <complex>
#include <cstdint>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>

using Complex = std::complex<double>;

static void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static void run(unsigned samples, int first, int last) {
  const unsigned taps = last - first;
  IqData reference(samples), surveillance(samples);
  WienerHopf filter(first, last, samples);

  // Overlap-save is alias-free per block rather than over the whole CPI, so
  // the property to hold down is that a block is longer than the tap overlap
  // it has to discard. A block at or below the tap count leaves no valid
  // outputs and the hop would not advance.
  const uint32_t block = filter.filter_fft_length();
  require(block > taps, "Block length must exceed the tap count");
  require(block - taps + 1 >= 1, "Hop must advance");
  require(block <= uint64_t(samples) + taps,
    "Block longer than the signal plus its overlap wastes the whole point");

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

    // Capture anything the filter writes to cerr. Zero lag is sum |x[n]|^2 and
    // lands on the diagonal of A, so it has to be exactly real or the matrix
    // is not Hermitian and armadillo warns on every single CPI. The oracle
    // below uses arma::solve, which does not care, and chol only warns rather
    // than failing, so the return value cannot see it either. Watching cerr
    // catches that and any other per-CPI complaint the filter might acquire.
    //
    // The single-transform code got a real zero lag for free from X * conj(X),
    // whose imaginary part is exactly zero in IEEE. The block form multiplies
    // two different sequences, so nothing forces that cancellation.
    std::ostringstream captured;
    std::streambuf* const previous = std::cerr.rdbuf(captured.rdbuf());
    const bool accepted = filter.process(&reference, &surveillance);
    std::cerr.rdbuf(previous);
    require(captured.str().empty(), "Filter wrote to cerr during a healthy CPI");

    require(accepted, "Full-rank fixture rejected");
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
    // The block length the filter actually runs at. The shipped geometry must
    // get the measured constant; anything that cannot (too few taps to fit the
    // overlap, or a CPI shorter than a block) has to stay self-consistent
    // rather than silently produce a block with no valid outputs.
    require(blah2::clutterBlockLength(1000000, 410) == blah2::kClutterBlockLength,
      "Shipped geometry no longer uses the measured block length");
    for (uint32_t samples : {31u, 32u, 64u, 127u, 257u, 4096u, 1000000u})
      for (uint32_t taps : {1u, 4u, 8u, 32u, 410u, 2048u, 5000u}) {
        if (taps > samples) continue;
        const uint32_t block = blah2::clutterBlockLength(samples, taps);
        require(block > taps, "Block must exceed the tap count");
        require(block <= uint64_t(samples) + taps, "Block longer than it can use");
      }
    std::cout << "PASS block length uses " << blah2::kClutterBlockLength << '\n';

    for (auto bad : {std::pair<uint32_t, uint32_t>{0, 4}, {4, 0}}) {
      bool rejected = false;
      try { (void)blah2::clutterBlockLength(bad.first, bad.second); }
      catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "Degenerate block geometry accepted");
    }
    std::cout << "PASS degenerate block geometry rejected\n";

    // Correctness across a spread of geometries, including positive delayMin,
    // which used to read the wrong sample: `i - delayMin` promoted to unsigned
    // and wrapped at 2^32. Negative and zero cancelled exactly, so the shipped
    // -10 was unaffected and the fault stayed hidden.
    for (unsigned samples : {64u, 127u, 257u})
      for (int first : {-3, 0, 2}) run(samples, first, first + 8);
    run(128, -2, -1);
    run(31, -2, 2);
    run(32, 0, 32);  // tap count equal to the CPI sample count

    // Everything above is short enough that the block length is capped at the
    // whole CPI, so it only ever runs as ONE block. That leaves the partial-sum
    // accumulation completely untested, which is precisely where the block
    // formulation can go wrong: a correlation numerator taking the full window
    // on both sides, or a normalisation by the CPI length instead of the block
    // length, both give a plausible-looking but wholly wrong answer, and both
    // survive a single-block test. These run several blocks, including a CPI
    // that is not a whole number of hops.
    // The shipped geometry itself. Every case above is small enough that the
    // roundoff this is watching for stays inside armadillo's Hermitian
    // tolerance: the zero-lag imaginary residue only trips it past roughly 300
    // blocks, and it is 611 at the shipped size. So a regression that spams a
    // live radar with a warning on every CPI is invisible to all of them.
    //
    // No direct-convolution oracle here, that would be 410 million operations.
    // The oracle cases above cover correctness; this covers scale.
    {
      const unsigned samples = 1000000;
      const int first = -10, last = 400;
      IqData reference(samples), surveillance(samples);
      WienerHopf filter(first, last, samples);
      std::mt19937 rng(4241);
      std::normal_distribution<double> random;
      for (unsigned i = 0; i < samples; ++i) {
        reference.push_back({random(rng), random(rng)});
        surveillance.push_back({random(rng), random(rng)});
      }
      std::ostringstream captured;
      std::streambuf* const previous = std::cerr.rdbuf(captured.rdbuf());
      const bool accepted = filter.process(&reference, &surveillance);
      std::cerr.rdbuf(previous);
      require(captured.str().empty(),
        "Filter complained at the shipped geometry: zero lag is probably not exactly real");
      require(accepted, "Shipped geometry rejected");
      require(surveillance.get_length() == samples, "Output sample count changed");
      std::cout << "PASS shipped geometry samples=" << samples << " taps="
                << (last - first) << " block=" << filter.filter_fft_length() << '\n';
    }

    for (const auto& geometry : {std::tuple<unsigned, int, int>{4096, -3, 5},
                                 {5000, 0, 16}, {5000, -20, 4}, {2500, 10, 20},
                                 {6143, -1, 31}})
      run(std::get<0>(geometry), std::get<1>(geometry), std::get<2>(geometry));

    for (const auto& bounds : {std::pair<int, int>{0, 0}, {5, 2}, {0, 65},
                              {INT32_MIN, INT32_MAX}}) {
      bool rejected = false;
      try { WienerHopf reject(bounds.first, bounds.second, 64); }
      catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "Degenerate delay range accepted");
    }
    std::cout << "PASS degenerate delay ranges rejected\n";

    std::cout << "All clutter FFT checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
