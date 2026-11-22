/// @file IqShmLayout.h
/// @brief The IQ cache's shared-memory layout, version 1. python/blah2_iq.py
/// mirrors it: change both together and bump kLayoutVersion. See docs/iq-cache.md.

#ifndef IQSHMLAYOUT_H
#define IQSHMLAYOUT_H

#include <cstddef>
#include <stdint.h>

namespace iqshm
{

constexpr char kMagic[8] = {'B', 'L', 'A', 'H', '2', 'I', 'Q', '\0'};
constexpr uint32_t kLayoutVersion = 1;
constexpr uint64_t kPage = 4096;
constexpr uint64_t kConfigCapacity = 64 * 1024;
constexpr uint32_t kTuningCapacity = 4096;
constexpr const char *kDefaultPath = "/dev/shm/blah2-iq";
constexpr uint64_t kLapMargin = 1;
constexpr uint32_t kMinSlots = 3;
constexpr uint64_t kUnwritten = UINT64_MAX;

struct Header
{
  char magic[8];
  uint32_t layoutVersion;
  uint32_t fs;
  uint64_t runId;
  double cpi;
  uint32_t nSamples;
  uint32_t nSlots;
  uint32_t tuningCapacity;
  uint32_t tuningEntryBytes;
  uint64_t slotStride;
  uint64_t slotTableOffset;
  uint64_t configOffset;
  uint64_t configBytes;
  uint64_t tuningOffset;
  uint64_t slotsOffset;
  uint64_t totalBytes;
  uint64_t nWritten;
  uint64_t nTuning;
  uint64_t nInexact;
  char imageTag[128];
};
static_assert(sizeof(Header) == 256 && offsetof(Header, runId) == 16
  && offsetof(Header, nWritten) == 104 && offsetof(Header, imageTag) == 128);

struct SlotEntry
{
  uint64_t cpiId;
  uint64_t timestamp;
};
static_assert(sizeof(SlotEntry) == 16);

struct TuningEntry
{
  uint64_t timestamp;
  uint8_t kind;
  uint8_t tuner;
  uint8_t pad0[2];
  uint32_t fc;
  int64_t generation;
  int32_t gainReductionA;
  int32_t gainReductionB;
  int32_t lnaState;
  uint32_t gRdB;
  uint32_t lnaGRdB;
  uint32_t pad1;
  double systemGainDb;
  uint64_t reserved;
};
static_assert(sizeof(TuningEntry) == 64 && offsetof(TuningEntry, systemGainDb) == 48);

inline uint64_t page_align(uint64_t bytes)
{
  return (bytes + kPage - 1) / kPage * kPage;
}

struct Geometry
{
  uint64_t slotTableOffset;
  uint64_t configOffset;
  uint64_t tuningOffset;
  uint64_t slotsOffset;
  uint64_t slotStride;
  uint64_t totalBytes;
};

inline Geometry geometry(uint32_t nSlots, uint32_t nSamples)
{
  Geometry g{};
  g.slotTableOffset = kPage;
  g.configOffset = g.slotTableOffset + page_align(uint64_t(nSlots) * sizeof(SlotEntry));
  g.tuningOffset = g.configOffset + kConfigCapacity;
  g.slotsOffset = g.tuningOffset + page_align(uint64_t(kTuningCapacity) * sizeof(TuningEntry));
  g.slotStride = page_align(uint64_t(nSamples) * 4 * sizeof(int16_t));
  g.totalBytes = g.slotsOffset + uint64_t(nSlots) * g.slotStride;
  return g;
}

/// @brief Entry 0 (Config) is pinned; the rest form a ring.
inline uint32_t tuning_position(uint64_t i)
{
  return i == 0 ? 0 : uint32_t(1 + (i - 1) % (kTuningCapacity - 1));
}

}

#endif
