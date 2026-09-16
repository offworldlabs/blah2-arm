#pragma once
#include <complex>
#include <cstdint>
#include <memory>

// One WienerHopf instance owns one persistent GPU queue and its buffers.
class OwlGpuFilter {
public:
  struct Impl;
  OwlGpuFilter(uint32_t samples, uint32_t taps);
  ~OwlGpuFilter();
  OwlGpuFilter(const OwlGpuFilter&) = delete;
  OwlGpuFilter& operator=(const OwlGpuFilter&) = delete;
  void reference(const std::complex<double>* x);
  uint32_t start(const std::complex<double>* x,
                 const std::complex<double>* timeDomainWeights);
  void finish(std::complex<double>* surveillance);
  void abort() noexcept;
private:
  std::unique_ptr<Impl> impl_;
};
