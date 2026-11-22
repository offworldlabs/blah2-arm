#include "IqCache.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{

/// @brief Exact for integer int16 values; anything else is rounded (half
/// away from zero) or clipped, and clears exact. NaN stores as -32768.
inline int16_t to_int16(double v, bool &exact)
{
  if (v >= -32768.0 && v <= 32767.0)
  {
    const int16_t s = static_cast<int16_t>(v);
    if (static_cast<double>(s) == v)
      return s;
    exact = false;
    return static_cast<int16_t>(v < 0 ? v - 0.5 : v + 0.5);
  }
  exact = false;
  return v > 0 ? 32767 : -32768;
}

inline uint64_t load_acquire(uint64_t &v)
{
  return std::atomic_ref<uint64_t>(v).load(std::memory_order_acquire);
}

inline void store_release(uint64_t &v, uint64_t x)
{
  std::atomic_ref<uint64_t>(v).store(x, std::memory_order_release);
}

std::runtime_error sys_error(const std::string &what, const std::string &path, int err)
{
  return std::runtime_error("IqCache: " + what + " " + path + ": " + std::strerror(err));
}

}

IqCache::IqCache(uint32_t _nSlots, uint32_t _nSamples, const Run &run)
{
  if (_nSlots < iqshm::kMinSlots)
    throw std::invalid_argument("IqCache needs at least " + std::to_string(iqshm::kMinSlots) + " slots");
  if (_nSamples == 0)
    throw std::invalid_argument("IqCache needs a non-zero CPI length");
  nSlots = _nSlots;
  nSamples = _nSamples;
  path = run.path;
  geometry = iqshm::geometry(nSlots, nSamples);

  // Reuse a same-size file; replace any other, never shrink it in place.
  fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0)
    throw sys_error("cannot open", path, errno);
  struct stat st{};
  if (::fstat(fd, &st) != 0)
  {
    const int err = errno;
    ::close(fd);
    throw sys_error("cannot stat", path, err);
  }
  if (st.st_size != 0 && uint64_t(st.st_size) != geometry.totalBytes)
  {
    ::close(fd);
    ::unlink(path.c_str());
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
      throw sys_error("cannot recreate", path, errno);
  }
  ::fchmod(fd, 0644);

  // Reserve every page now, so running out is an error here, not SIGBUS later.
  const int err = ::posix_fallocate(fd, 0, off_t(geometry.totalBytes));
  if (err != 0)
  {
    ::close(fd);
    ::unlink(path.c_str());
    throw sys_error("cannot reserve " + std::to_string(geometry.totalBytes) + " bytes for", path, err);
  }

  void *mapped = ::mmap(nullptr, geometry.totalBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED)
  {
    const int mapErr = errno;
    ::close(fd);
    throw sys_error("cannot map", path, mapErr);
  }
  base = static_cast<unsigned char *>(mapped);

  // runId is 0 while initialising and published last.
  iqshm::Header *h = header();
  store_release(h->runId, 0);
  store_release(h->nWritten, 0);
  store_release(h->nTuning, 0);
  store_release(h->nInexact, 0);
  std::memcpy(h->magic, iqshm::kMagic, sizeof(h->magic));
  h->layoutVersion = iqshm::kLayoutVersion;
  h->fs = run.fs;
  h->cpi = run.cpi;
  h->nSamples = nSamples;
  h->nSlots = nSlots;
  h->tuningCapacity = iqshm::kTuningCapacity;
  h->tuningEntryBytes = sizeof(iqshm::TuningEntry);
  h->slotStride = geometry.slotStride;
  h->slotTableOffset = geometry.slotTableOffset;
  h->configOffset = geometry.configOffset;
  h->tuningOffset = geometry.tuningOffset;
  h->slotsOffset = geometry.slotsOffset;
  h->totalBytes = geometry.totalBytes;
  std::memset(h->imageTag, 0, sizeof(h->imageTag));
  std::memcpy(h->imageTag, run.imageTag.data(), std::min(run.imageTag.size(), sizeof(h->imageTag) - 1));
  const uint64_t configBytes = std::min<uint64_t>(run.configText.size(), iqshm::kConfigCapacity);
  std::memset(base + geometry.configOffset, 0, iqshm::kConfigCapacity);
  std::memcpy(base + geometry.configOffset, run.configText.data(), configBytes);
  h->configBytes = configBytes;
  for (uint32_t s = 0; s < nSlots; s++)
    slot_table()[s] = iqshm::SlotEntry{iqshm::kUnwritten, 0};
  store_release(h->runId, run.runId == 0 ? 1 : run.runId);
}

IqCache::~IqCache()
{
  ::munmap(base, geometry.totalBytes);
  ::close(fd);
}

bool IqCache::write(const IqData &x, const IqData &y, uint64_t timestamp)
{
  if (x.get_length() < nSamples || y.get_length() < nSamples)
    throw std::invalid_argument("IqCache::write given fewer samples than a CPI");

  iqshm::Header *h = header();
  const uint64_t id = std::atomic_ref<uint64_t>(h->nWritten).load(std::memory_order_relaxed);
  const uint32_t slot = uint32_t(id % nSlots);

  bool exact = true;
  int16_t *out = slot_samples(slot);
  for (uint32_t i = 0; i < nSamples; i++)
  {
    const std::complex<double> &a = x[i];
    const std::complex<double> &b = y[i];
    out[0] = to_int16(a.real(), exact);
    out[1] = to_int16(a.imag(), exact);
    out[2] = to_int16(b.real(), exact);
    out[3] = to_int16(b.imag(), exact);
    out += 4;
  }

  slot_table()[slot] = iqshm::SlotEntry{id, timestamp};
  if (!exact)
    std::atomic_ref<uint64_t>(h->nInexact).fetch_add(1, std::memory_order_relaxed);
  store_release(h->nWritten, id + 1);
  return exact;
}

void IqCache::append_tuning(const TuningLog::Entry &e)
{
  iqshm::Header *h = header();
  const uint64_t i = std::atomic_ref<uint64_t>(h->nTuning).load(std::memory_order_relaxed);
  iqshm::TuningEntry record{};
  record.timestamp = e.timestamp;
  record.kind = static_cast<uint8_t>(e.kind);
  record.tuner = e.tuner;
  record.fc = e.fc;
  record.generation = e.generation;
  record.gainReductionA = e.gainReductionA;
  record.gainReductionB = e.gainReductionB;
  record.lnaState = e.lnaState;
  record.gRdB = e.gRdB;
  record.lnaGRdB = e.lnaGRdB;
  record.systemGainDb = e.systemGainDb;
  auto *log = reinterpret_cast<iqshm::TuningEntry *>(base + geometry.tuningOffset);
  log[iqshm::tuning_position(i)] = record;
  store_release(h->nTuning, i + 1);
}

uint64_t IqCache::oldest_readable(uint64_t n) const
{
  const uint64_t span = nSlots - iqshm::kLapMargin - 1;
  return n > span ? n - span : 0;
}

std::vector<IqCache::FrameInfo> IqCache::index() const
{
  iqshm::Header *h = header();
  const uint64_t n = load_acquire(h->nWritten);
  std::vector<FrameInfo> frames;
  for (uint64_t id = oldest_readable(n); id < n; id++)
  {
    const iqshm::SlotEntry e = slot_table()[id % nSlots];
    if (e.cpiId == id)
      frames.push_back(FrameInfo{e.cpiId, e.timestamp});
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint64_t lo = oldest_readable(load_acquire(h->nWritten));
  frames.erase(frames.begin(), std::find_if(frames.begin(), frames.end(),
    [lo](const FrameInfo &f) { return f.cpiId >= lo; }));
  return frames;
}

bool IqCache::read(uint64_t timestamp, FrameInfo &info, std::vector<int16_t> &out) const
{
  iqshm::Header *h = header();
  const uint64_t n = load_acquire(h->nWritten);
  const uint64_t lo = oldest_readable(n);
  for (uint64_t id = n; id-- > lo;)  // newest first
  {
    const uint32_t slot = uint32_t(id % nSlots);
    const iqshm::SlotEntry e = slot_table()[slot];
    if (e.cpiId != id || e.timestamp != timestamp)
      continue;
    const int16_t *begin = slot_samples(slot);
    out.assign(begin, begin + uint64_t(nSamples) * 4);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (load_acquire(h->nWritten) >= id + nSlots - iqshm::kLapMargin)
      return false;  // lapped while copying
    info = FrameInfo{e.cpiId, e.timestamp};
    return true;
  }
  return false;
}

bool IqCache::remove(const std::string &path)
{
  return ::unlink(path.c_str()) == 0;
}

uint64_t IqCache::get_n_inexact() const
{
  return load_acquire(header()->nInexact);
}
