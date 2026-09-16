#include "process/ambiguity/Ambiguity.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <deque>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Complex = std::complex<double>;

void options(int workers, int rows, int rangeThreads, const char* plan) {
  setenv("OWL_AMBIG_WORKERS", std::to_string(workers).c_str(), 1);
  setenv("OWL_AMBIG_ROWS", std::to_string(rows).c_str(), 1);
  setenv("OWL_AMBIG_RANGE_THREADS", std::to_string(rangeThreads).c_str(), 1);
  setenv("OWL_AMBIG_PLAN", plan, 1);
  fftw_plan_with_nthreads(4);
}

void require_direct_oracle(bool hamming, bool impulse) {
  constexpr uint32_t n = 1024;
  constexpr int32_t lo = -3;
  constexpr int32_t hi = 8;
  Ambiguity ambiguity(lo, hi, -16, 16, 8192, n, hamming);
  IqData x(n), y(n);
  std::vector<Complex> reference(n), surveillance(n);
  std::mt19937_64 random(0x41524d4241544348ULL + hamming + impulse * 2);
  std::uniform_real_distribution<double> value(-1, 1);
  for (uint32_t i = 0; i < n; ++i) {
    if (impulse) {
      if (i == 3) reference[i] = {1, 2};
      if (i == 5) surveillance[i] = {2, -1};
    } else {
      reference[i] = {value(random), value(random)};
      surveillance[i] = {value(random), value(random)};
    }
    x.push_back(reference[i]);
    y.push_back(surveillance[i]);
  }
  const int rows = ambiguity.get_n_doppler_bins();
  const int samplesPerRow = ambiguity.get_n_corr();
  const auto* map = ambiguity.process_owned(&x, &y);
  if (fftw_planner_nthreads() != 4) throw std::runtime_error("Ambiguity changed planner state");
  constexpr double pi = 3.14159265358979323846;
  for (int lag = lo; lag <= hi; ++lag) {
    std::vector<Complex> correlation(rows);
    for (int row = 0; row < rows; ++row)
      for (int i = std::max(0, lag); i < std::min(samplesPerRow, samplesPerRow + lag); ++i)
        correlation[row] += surveillance[row * samplesPerRow + i] *
          std::conj(reference[row * samplesPerRow + i - lag]);
    for (int row = 0; row < rows; ++row) {
      const int bin = (row + rows / 2 + 1) % rows;
      Complex direct{};
      for (int source = 0; source < rows; ++source)
        direct += correlation[source] * std::exp(Complex(0, -2.0 * pi * bin * source / rows));
      const auto actual = map->data[row][lag - lo];
      if (!std::isfinite(actual.real()) || !std::isfinite(actual.imag()) ||
          std::abs(actual - direct) > 1e-10)
        throw std::runtime_error("Ambiguity direct DFT gate failed");
    }
  }
  if (x.view_data() != std::deque<Complex>(reference.begin(), reference.end()) ||
      y.view_data() != std::deque<Complex>(surveillance.begin(), surveillance.end()))
    throw std::runtime_error("Owned ambiguity changed input samples");
}

void require_shifted_legacy_oracle() {
  constexpr uint32_t n = 1024;
  Ambiguity ambiguity(-3, 8, -8, 24, 8192, n, false);
  IqData x(n), y(n);
  std::vector<Complex> reference(n), surveillance(n);
  for (uint32_t i = 0; i < n; ++i) {
    reference[i] = {std::sin(i * .13), std::cos(i * .17)};
    surveillance[i] = {std::cos(i * .07), std::sin(i * .19)};
    x.push_back(reference[i]); y.push_back(surveillance[i]);
  }
  const auto* map = ambiguity.process(&x, &y);
  const int rows = ambiguity.get_n_doppler_bins(), rowLength = ambiguity.get_n_corr();
  for (uint32_t i = 0; i < n; ++i)
    reference[i] *= std::exp(Complex(0, 2.0 * 3.14159265358979323846 * 8.0 * i / 8192.0));
  for (int lag = -3; lag <= 8; ++lag) for (int row = 0; row < rows; ++row) {
    Complex direct{};
    const int bin = (row + rows / 2 + 1) % rows;
    for (int source = 0; source < rows; ++source) {
      Complex correlation{};
      for (int i = std::max(0, lag); i < std::min(rowLength, rowLength + lag); ++i)
        correlation += surveillance[source * rowLength + i] * std::conj(reference[source * rowLength + i - lag]);
      direct += correlation * std::exp(Complex(0, -2.0 * 3.14159265358979323846 * bin * source / rows));
    }
    if (std::abs(map->data[row][lag + 3] - direct) > 1e-10)
      throw std::runtime_error("Shifted legacy ambiguity differs from direct DFT");
  }
}
}

int main() {
  if (!fftw_init_threads()) return 2;
  options(0, 1, 1, "estimate");
  require_direct_oracle(false, false);
  require_direct_oracle(true, false);
  require_direct_oracle(false, true);
  options(0, 4, 2, "measure"); // 33 rows gives a partial final tile.
  require_direct_oracle(false, false);
  options(2, 1, 2, "measure");
  require_direct_oracle(true, false);
  require_shifted_legacy_oracle();
  Ambiguity owned(-3, 8, -16, 16, 8192, 1024);
  const uint32_t used = uint32_t(owned.get_n_corr()) * owned.get_n_doppler_bins();
  IqData reference(1024), surveillance(1024);
  for (uint32_t i = 0; i < 1024; ++i) {
    reference.push_back({double(i), -double(i)});
    surveillance.push_back({double(i) * .2, double(i) * .3});
  }
  auto rejects = [](auto call) {
    bool rejected = false;
    try { call(); } catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected) throw std::runtime_error("Invalid owned-input call accepted");
  };
  rejects([&] { owned.process_owned(&reference, &reference); });
  rejects([&] { owned.process_owned(nullptr, &surveillance); });
  IqData shortInput(used);
  for (uint32_t i = 0; i + 1 < used; ++i) shortInput.push_back({1, 0});
  rejects([&] { owned.process_owned(&shortInput, &surveillance); });
  const uint32_t tooLong = used + owned.get_nfft() + 1;
  IqData oversized(tooLong);
  for (uint32_t i = 0; i < tooLong; ++i) oversized.push_back({1, 0});
  rejects([&] { owned.process_owned(&oversized, &surveillance); });
  Ambiguity shifted(-3, 8, -8, 24, 8192, 1024);
  rejects([&] { shifted.process_owned(&reference, &surveillance); });
  setenv("OWL_AMBIG_ROWS", "3", 1);
  bool badOption = false;
  try { Ambiguity invalid(-3, 8, -16, 16, 8192, 1024); } catch (const std::invalid_argument&) { badOption = true; }
  if (!badOption) return 1;
  setenv("OWL_AMBIG_ROWS", "1", 1);
  setenv("OWL_AMBIG_PLAN", "invalid", 1);
  badOption = false;
  try { Ambiguity invalid(-3, 8, -16, 16, 8192, 1024); } catch (const std::invalid_argument&) { badOption = true; }
  if (!badOption) return 1;
  options(2, 1, 2, "measure");
  // A complete configured CPI includes the small unused correlation tail.
  // The measured path preserves it; truncating to used would change capture.
  const auto originalX = reference.get_data(), originalY = surveillance.get_data();
  owned.process_owned(&reference, &surveillance);
  if (reference.view_data() != originalX || surveillance.view_data() != originalY)
    throw std::runtime_error("Bounded owned-input tail was altered");
  owned.process(&reference, &surveillance);
  if (reference.get_length() != 1024 - used || surveillance.get_length() != 1024 - used ||
      !std::equal(reference.view_data().begin(), reference.view_data().end(), originalX.begin() + used) ||
      !std::equal(surveillance.view_data().begin(), surveillance.view_data().end(), originalY.begin() + used))
    throw std::runtime_error("Legacy consuming path changed the tail");
  IqData unevenX(used + 3), unevenY(used + 7);
  for (uint32_t i = 0; i < used + 3; ++i) unevenX.push_back({double(i), -double(i)});
  for (uint32_t i = 0; i < used + 7; ++i) unevenY.push_back({double(i) * .3, double(i) * .4});
  const auto unevenOriginalX = unevenX.get_data(), unevenOriginalY = unevenY.get_data();
  owned.process(&unevenX, &unevenY);
  if (unevenX.get_length() != 3 || unevenY.get_length() != 7 ||
      !std::equal(unevenX.view_data().begin(), unevenX.view_data().end(), unevenOriginalX.begin() + used) ||
      !std::equal(unevenY.view_data().begin(), unevenY.view_data().end(), unevenOriginalY.begin() + used))
    throw std::runtime_error("Unequal legacy tails differ");

  // The shipped geometry consumes 999,922 samples of a one-million-sample
  // CPI. Ownership preserves its bounded 78-sample tail; legacy consumes only
  // the configured rows and restores that exact tail.
  constexpr uint32_t shipped = 1000000;
  Ambiguity shippedAmbiguity(-10, 400, -300, 300, 2000000, shipped, true);
  const uint32_t shippedUsed = uint32_t(shippedAmbiguity.get_n_corr()) * shippedAmbiguity.get_n_doppler_bins();
  if (shippedUsed != 999922) throw std::runtime_error("Shipped ambiguity geometry changed");
  IqData shippedX(shipped), shippedY(shipped);
  for (uint32_t i = 0; i < shipped; ++i) {
    shippedX.push_back({std::sin(i * .001), std::cos(i * .003)});
    shippedY.push_back({std::cos(i * .002), std::sin(i * .004)});
  }
  const auto shippedOriginalX = shippedX.get_data(), shippedOriginalY = shippedY.get_data();
  shippedAmbiguity.process_owned(&shippedX, &shippedY);
  if (shippedX.view_data() != shippedOriginalX || shippedY.view_data() != shippedOriginalY)
    throw std::runtime_error("Shipped owned CPI changed its bounded tail");
  shippedAmbiguity.process(&shippedX, &shippedY);
  if (shippedX.get_length() != shipped - shippedUsed || shippedY.get_length() != shipped - shippedUsed ||
      !std::equal(shippedX.view_data().begin(), shippedX.view_data().end(), shippedOriginalX.begin() + shippedUsed) ||
      !std::equal(shippedY.view_data().begin(), shippedY.view_data().end(), shippedOriginalY.begin() + shippedUsed))
    throw std::runtime_error("Shipped legacy CPI tail differs");
  IqData alias(1024);
  for (uint32_t i = 0; i < 1024; ++i) alias.push_back({double(i), -double(i)});
  bool rejected = false;
  try { owned.process(&alias, &alias); } catch (const std::runtime_error&) { rejected = true; }
  if (!rejected || fftw_planner_nthreads() != 4) return 1;
  return 0;
}
