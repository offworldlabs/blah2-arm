#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace blah2 {
// All lengths are configuration-derived. No capture backend or site assumptions.
struct GpuGeometry {
  uint32_t range, doppler, delays, channels;
  int32_t delayMin;
  // Optional batched clutter geometry. Zero samples keeps ambiguity-only
  // source callers compatible; GPU_ABI 3 rejects binary geometry/contract mismatches.
  uint32_t clutterSamples = 0, clutterBins = 0;
  int32_t clutterDelayMin = 0;
};
struct GpuDevice { std::string id, name; uint64_t memoryBytes; };
class GpuBackend {
public:
  virtual ~GpuBackend() = default;
  virtual GpuDevice device() const = 0;
  virtual void process(const std::vector<std::complex<float>>& reference,
    const std::vector<std::complex<float>>& surveillance,
    std::vector<std::complex<float>>& output) = 0;
};
// Optional interfaces deliberately live beside, rather than inside, GpuBackend:
// existing test/in-process backends keep the version-1 virtual table unchanged.
struct GpuFrameBuffers {
  std::complex<float>* reference = nullptr;
  size_t referenceCount = 0;
  std::complex<float>* surveillance = nullptr;
  size_t surveillanceCount = 0;
  std::complex<float>* output = nullptr;
  size_t outputCount = 0;
};
class GpuFrameBackend {
public:
  virtual ~GpuFrameBackend() = default;
  virtual GpuFrameBuffers frameBuffers() = 0;
  virtual void processFrame() = 0;
};
class GpuBufferBackend {
public:
  virtual ~GpuBufferBackend() = default;
  virtual void processBuffers(const std::complex<float>* reference, size_t referenceCount,
    const std::complex<float>* surveillance, size_t surveillanceCount,
    std::complex<float>* output, size_t outputCount) = 0;
};
struct GpuClutterBuffers {
  std::complex<float>* reference = nullptr;
  size_t referenceCount = 0;
  std::complex<float>* surveillance = nullptr;
  size_t surveillanceCount = 0;
  // The output is the estimated clutter, not the filtered surveillance. The
  // parent subtracts it from its immutable FP64 input to avoid cancellation in
  // FP32 while retaining GPU FFT/filter execution.
  std::complex<float>* output = nullptr;
  size_t outputCount = 0;
};
class GpuClutterFrameBackend {
public:
  virtual ~GpuClutterFrameBackend() = default;
  virtual bool clutterAvailable() const = 0;
  virtual GpuClutterBuffers clutterBuffers() = 0;
  // False is a data/precision rejection: the worker remains healthy and the
  // caller must run this frame on CPU. Exceptions remain transport/device faults.
  virtual bool processClutterFrame() = 0;
};
class GpuClutterBufferBackend {
public:
  virtual ~GpuClutterBufferBackend() = default;
  virtual bool processClutterBuffers(const std::complex<float>* reference, size_t referenceCount,
    const std::complex<float>* surveillance, size_t surveillanceCount,
    std::complex<float>* output, size_t outputCount) = 0;
};
// Module ABI is versioned, built with the executable, and loaded only by explicit
// path beside the executable (never an untrusted current-working-directory path).
constexpr unsigned GPU_ABI = 3;
using GpuCreate = GpuBackend* (*)(unsigned, const GpuGeometry*, const char*);
using GpuList = std::vector<GpuDevice> (*)();
}
