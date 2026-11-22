/// @file IqCache.h
/// @class IqCache
/// @brief The last nSlots CPIs' raw IQ as int16, in a shared-memory file that
/// readers map read-only. See docs/iq-cache.md.
/// @details Invariants: write() has a single caller thread; a frame is
/// published by the release store of nWritten after its samples and slot
/// entry; the file is never shrunk in place (a mapped reader would SIGBUS).

#ifndef IQCACHE_H
#define IQCACHE_H

#include "data/IqData.h"
#include "data/IqShmLayout.h"
#include "data/TuningLog.h"

#include <cstddef>
#include <stdint.h>
#include <string>
#include <vector>

class IqCache
{
public:
  struct FrameInfo
  {
    uint64_t cpiId;
    uint64_t timestamp;
  };

  struct Run
  {
    std::string path = iqshm::kDefaultPath;
    uint32_t fs = 0;
    double cpi = 0;
    uint64_t runId = 0;
    std::string imageTag;
    std::string configText;
  };

  IqCache(uint32_t nSlots, uint32_t nSamples, const Run &run);
  ~IqCache();
  IqCache(const IqCache &) = delete;
  IqCache &operator=(const IqCache &) = delete;

  /// @return False if any sample had to be rounded or clipped.
  bool write(const IqData &x, const IqData &y, uint64_t timestamp);

  /// @brief TuningLog sink; called with the log's mutex held.
  void append_tuning(const TuningLog::Entry &entry);

  /// @brief Reference implementation of the reader protocol, as python/blah2_iq.py.
  std::vector<FrameInfo> index() const;
  bool read(uint64_t timestamp, FrameInfo &info, std::vector<int16_t> &out) const;

  /// @return True if a file was deleted.
  static bool remove(const std::string &path);

  uint32_t get_n_slots() const { return nSlots; }
  uint32_t get_n_samples() const { return nSamples; }
  const std::string &get_path() const { return path; }
  uint64_t get_total_bytes() const { return geometry.totalBytes; }
  uint64_t get_n_inexact() const;

private:
  uint32_t nSlots;
  uint32_t nSamples;
  std::string path;
  iqshm::Geometry geometry;
  int fd;
  unsigned char *base;

  iqshm::Header *header() const { return reinterpret_cast<iqshm::Header *>(base); }
  iqshm::SlotEntry *slot_table() const
  {
    return reinterpret_cast<iqshm::SlotEntry *>(base + geometry.slotTableOffset);
  }
  int16_t *slot_samples(uint32_t slot) const
  {
    return reinterpret_cast<int16_t *>(base + geometry.slotsOffset + uint64_t(slot) * geometry.slotStride);
  }
  uint64_t oldest_readable(uint64_t n) const;
};

#endif
