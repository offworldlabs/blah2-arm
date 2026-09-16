#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>

namespace owl_toeplitz {
using Complex = std::complex<double>;
enum class Reason { accepted, input, energy, condition, nonfinite, residual };
struct Result {
  Reason reason = Reason::input;
  double inverse_norm_bound = 0;
  double condition_bound = 0;
  double forward_error_bound = 0;
  bool accepted() const { return reason == Reason::accepted; }
};
inline bool finite(Complex z) { return std::isfinite(z.real()) && std::isfinite(z.imag()); }

// Solve the Hermitian Toeplitz system defined by its first row. The caller
// supplies two n-element scratch arrays and falls back to its existing solver
// when this conservative fast path declines. No input or RHS is modified.
inline Result solve(const Complex* row, const Complex* rhs, Complex* x,
                    std::size_t n, Complex* predictor, Complex* column) {
  Result info;
  if (!n || !row || !rhs || !x || !predictor || !column) return info;
  const double diagonal = row[0].real();
  const double epsilon = std::numeric_limits<double>::epsilon();
  if (!finite(row[0]) || !(diagonal > 0) ||
      std::abs(row[0].imag()) > 64 * epsilon * diagonal) return info;
  double normA = 1, normB = 0;
  column[0] = 1;
  for (std::size_t k = 0; k < n; ++k) {
    if (!finite(row[k]) || !finite(rhs[k])) return info;
    const Complex scaledRhs = rhs[k] / diagonal;
    if (!finite(scaledRhs)) return info;
    normB = std::max(normB, std::abs(scaledRhs));
    if (k) {
      column[k] = std::conj(row[k]) / diagonal;
      if (!finite(column[k])) return info;
      normA += 2 * std::abs(column[k]);
    }
  }
  if (!std::isfinite(normA)) return info;
  double energy = 1, inverseBound = 0;
  for (std::size_t m = 0; m < n; ++m) {
    double sumG = 0, maxG = 0;
    Complex projection{};
    for (std::size_t j = 0; j < m; ++j) {
      projection += column[m-j] * x[j];
      const double magnitude = std::abs(predictor[j]);
      sumG += magnitude; maxG = std::max(maxG, magnitude);
    }
    if (!(energy > 1e-10) || !std::isfinite(energy)) { info.reason=Reason::energy; return info; }
    // Block inverse formula: the appended column uses g=J*conj(predictor).
    // Triangle inequalities bound the inverse infinity norm conservatively.
    inverseBound = std::max(inverseBound + maxG * (sumG+1) / energy, (sumG+1) / energy);
    info.inverse_norm_bound = inverseBound;
    info.condition_bound = inverseBound * normA;
    if (!std::isfinite(info.condition_bound) || info.condition_bound > 1e5) {
      info.reason=Reason::condition; return info;
    }
    const Complex last = (rhs[m] / diagonal - projection) / energy;
    if (!finite(last)) { info.reason=Reason::nonfinite; return info; }
    for (std::size_t j = 0; j < m; ++j)
      x[j] -= std::conj(predictor[m-1-j]) * last;
    x[m] = last;
    if (m+1 == n) break;
    Complex prediction{};
    for (std::size_t j = 0; j < m; ++j) prediction += column[m-j] * predictor[j];
    const Complex reflection = (column[m+1] - prediction) / energy;
    const double squared = std::norm(reflection);
    if (!finite(reflection) || !(squared < 1)) { info.reason=Reason::energy; return info; }
    // Update mirrored pairs together so neither reads an already changed value.
    for (std::size_t j = 0; j < (m+1)/2; ++j) {
      const std::size_t k = m-1-j;
      const Complex left = predictor[j], right = predictor[k];
      predictor[j] = left - reflection * std::conj(right);
      if (j != k) predictor[k] = right - reflection * std::conj(left);
    }
    predictor[m] = reflection;
    energy *= 1 - squared;
  }
  double normX = 0, residual = 0;
  for (std::size_t j = 0; j < n; ++j) {
    if (!finite(x[j])) { info.reason=Reason::nonfinite; return info; }
    normX = std::max(normX, std::abs(x[j]));
  }
  for (std::size_t i = 0; i < n; ++i) {
    Complex sum{}, correction{};
    for (std::size_t j = 0; j < n; ++j) {
      const Complex coefficient = i >= j ? column[i-j] : std::conj(column[j-i]);
      const Complex term = coefficient*x[j] - correction;
      const Complex next = sum + term;
      correction = (next-sum)-term; sum=next;
    }
    residual = std::max(residual, std::abs(rhs[i]/diagonal - sum));
  }
  // Allow for dot-product and normalization roundoff in addition to measured
  // residual; difficult systems use the original dense solve instead.
  const double scale = normA*normX + normB;
  if (!std::isfinite(scale) || !std::isfinite(residual)) { info.reason=Reason::nonfinite; return info; }
  info.forward_error_bound = normX ? inverseBound*(residual + (8*n+16)*epsilon*scale)/normX : residual;
  if (residual > 5e-13*scale || info.forward_error_bound > 1e-9) {
    info.reason=Reason::residual; return info;
  }
  info.reason=Reason::accepted; return info;
}
} // namespace owl_toeplitz
