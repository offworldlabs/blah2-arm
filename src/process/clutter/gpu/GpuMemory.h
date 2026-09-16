#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace blah2::gpu_memory {
// These values intentionally match Vulkan 1.0 VkMemoryPropertyFlagBits. Keeping
// the policy independent of Vulkan makes capability and alignment decisions
// testable without a physical GPU.
constexpr uint32_t deviceLocal = 0x1;
constexpr uint32_t hostVisible = 0x2;
constexpr uint32_t hostCoherent = 0x4;
constexpr uint32_t hostCached = 0x8;
constexpr uint32_t noType = std::numeric_limits<uint32_t>::max();
constexpr uint64_t maximumDeviceBytes = 2ULL << 30;
inline uint64_t heapBudget(uint64_t heapBytes) {
  return std::min(maximumDeviceBytes, heapBytes / 4);
}

struct Type { uint32_t flags = 0, heap = 0; };
struct Heap { uint64_t bytes = 0; bool device = false; };
struct Properties { std::vector<Type> types; std::vector<Heap> heaps; };

// Reserve actual Vulkan allocation sizes, including FFT-owned tables/scratch.
// Types that alias one heap must share its allowance. Subtraction before the
// comparison also rejects oversized requests without overflowing a counter.
class AllocationBudget {
  Properties properties_;
  uint64_t limit_, used_ = 0, peak_ = 0;
  std::vector<uint64_t> heapUsed_;
public:
  AllocationBudget(uint64_t limit, Properties properties)
    : properties_(std::move(properties)), limit_(limit),
      heapUsed_(properties_.heaps.size()) {}
  bool reserve(uint32_t type, uint64_t bytes) {
    if (type >= properties_.types.size() || !bytes || bytes > limit_ - used_)
      return false;
    const auto heap = properties_.types[type].heap;
    if (heap >= heapUsed_.size()) return false;
    const auto heapLimit = heapBudget(properties_.heaps[heap].bytes);
    if (bytes > heapLimit - heapUsed_[heap]) return false;
    heapUsed_[heap] += bytes; used_ += bytes; peak_ = std::max(peak_, used_);
    return true;
  }
  void release(uint32_t type, uint64_t bytes) {
    const auto heap = properties_.types.at(type).heap;
    heapUsed_.at(heap) -= bytes; used_ -= bytes;
  }
  uint64_t used() const { return used_; }
  uint64_t peak() const { return peak_; }
  uint64_t limit() const { return limit_; }
};

inline uint32_t chooseType(uint32_t compatible, uint32_t required,
    uint32_t preferred, const Properties& properties) {
  uint32_t best = noType, bestScore = 0;
  for (uint32_t i = 0; i < properties.types.size() && i < 32; ++i) {
    const auto& type = properties.types[i];
    if (!(compatible & (1u << i)) || (type.flags & required) != required ||
        type.heap >= properties.heaps.size()) continue;
    const uint32_t score = __builtin_popcount(type.flags & preferred);
    if (best == noType || score > bestScore) { best = i; bestScore = score; }
  }
  return best;
}

// AUTO uses direct mappings only for a unified-memory topology: an integrated
// device whose selected heap is its largest device-local heap. This is more
// conservative than treating HOST_VISIBLE|DEVICE_LOCAL alone as proof that CPU
// access is fast (for example, a discrete-GPU BAR may expose those flags).
inline bool autoDirect(bool integrated, uint32_t heap, uint64_t requiredBytes,
    const Properties& properties) {
  if (!integrated || heap >= properties.heaps.size() || !properties.heaps[heap].device)
    return false;
  uint64_t largest = 0;
  for (const auto& item : properties.heaps) if (item.device) largest = std::max(largest, item.bytes);
  const uint64_t size = properties.heaps[heap].bytes;
  return size == largest && requiredBytes <= heapBudget(size);
}

struct Range { uint64_t offset = 0, bytes = 0; bool whole = false; };
inline Range alignedRange(uint64_t offset, uint64_t bytes, uint64_t allocation,
    uint64_t atom) {
  if (!bytes || offset >= allocation || bytes > allocation - offset || !atom) return {};
  const uint64_t begin = offset - offset % atom;
  const uint64_t endValue = offset + bytes;
  if (endValue > std::numeric_limits<uint64_t>::max() - (atom - 1)) return {};
  const uint64_t end = ((endValue + atom - 1) / atom) * atom;
  if (end >= allocation) return {begin, 0, true};
  return {begin, end - begin, false};
}

// Smallest 2/3/5-smooth value at least minimum. These lengths avoid Bluestein
// transforms for the zero-padded linear clutter convolution. Zero means the
// requested length cannot be represented in the uint32_t VkFFT geometry.
inline uint32_t nextSmooth(uint64_t minimum) {
  if (!minimum || minimum > std::numeric_limits<uint32_t>::max()) return 0;
  constexpr uint64_t limit = std::numeric_limits<uint32_t>::max();
  uint64_t best = std::numeric_limits<uint64_t>::max();
  for (uint64_t two = 1; two <= limit; ) {
    for (uint64_t three = two; three <= limit; ) {
      for (uint64_t five = three; five <= limit; ) {
        if (five >= minimum) { best = std::min(best, five); break; }
        if (five > limit / 5) break;
        five *= 5;
      }
      if (three > limit / 3) break;
      three *= 3;
    }
    if (two > limit / 2) break;
    two *= 2;
  }
  return best <= limit ? uint32_t(best) : 0;
}
}
