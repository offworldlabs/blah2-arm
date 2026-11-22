// IqCache: round trips, layout, file lifecycle, tuning in the file, and the
// lock-free reader protocol. See docs/iq-cache.md.
#include "data/IqCache.h"
#include "data/IqData.h"
#include "data/IqShmLayout.h"
#include "data/TuningLog.h"

#include <atomic>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using Complex = std::complex<double>;

static void require(bool condition, const std::string &message)
{
  if (!condition) throw std::runtime_error(message);
}

static std::string temp_path(const std::string &name)
{
  return "/tmp/blah2-test-iq-" + name + "-" + std::to_string(::getpid());
}

// Same formula as python/test_blah2_iq.py.
static int16_t pattern(uint64_t frame, uint32_t i, uint32_t c)
{
  const uint64_t v = frame * 7919u + uint64_t(i) * 31u + c * 7u;
  return static_cast<int16_t>(int64_t(v % 65536u) - 32768);
}

static void fill(IqData &x, IqData &y, uint64_t frame, uint32_t n)
{
  x.clear();
  y.clear();
  for (uint32_t i = 0; i < n; i++)
  {
    x.push_back(Complex(pattern(frame, i, 0), pattern(frame, i, 1)));
    y.push_back(Complex(pattern(frame, i, 2), pattern(frame, i, 3)));
  }
}

static bool matches(const int16_t *out, uint64_t frame, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++)
    for (uint32_t c = 0; c < 4; c++)
      if (out[size_t(4) * i + c] != pattern(frame, i, c)) return false;
  return true;
}

static IqCache::Run run_for(const std::string &path, uint64_t runId = 42)
{
  IqCache::Run run;
  run.path = path;
  run.fs = 2000000;
  run.cpi = 0.5;
  run.runId = runId;
  run.imageTag = "blah2:test";
  run.configText = "process:\n  iqCache:\n    enable: true\n";
  return run;
}

// A second, read-only mapping, as another process would have.
struct ReaderMap
{
  int fd = -1;
  size_t size = 0;
  const unsigned char *base = nullptr;
  explicit ReaderMap(const std::string &path)
  {
    fd = ::open(path.c_str(), O_RDONLY);
    require(fd >= 0, "reader cannot open " + path);
    struct stat st{};
    ::fstat(fd, &st);
    size = size_t(st.st_size);
    void *m = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    require(m != MAP_FAILED, "reader cannot map");
    base = static_cast<const unsigned char *>(m);
  }
  ~ReaderMap()
  {
    ::munmap(const_cast<unsigned char *>(base), size);
    ::close(fd);
  }
  const iqshm::Header &header() const { return *reinterpret_cast<const iqshm::Header *>(base); }
};

static ino_t inode_of(const std::string &path)
{
  struct stat st{};
  require(::stat(path.c_str(), &st) == 0, "stat " + path);
  return st.st_ino;
}

static void round_trip_and_wrap()
{
  const std::string path = temp_path("wrap");
  const uint32_t n = 257, nSlots = 6;
  {
    IqCache cache(nSlots, n, run_for(path));
    IqData x(n), y(n);
    require(cache.index().empty(), "a new cache should hold nothing");

    for (uint64_t f = 0; f < 3 * nSlots + 2; f++)
    {
      fill(x, y, f, n);
      require(cache.write(x, y, 1000 + 500 * f), "integer samples should store exactly");

      const std::vector<IqCache::FrameInfo> frames = cache.index();
      const uint64_t held = (f + 1 < nSlots - 2) ? f + 1 : nSlots - 2;
      require(frames.size() == held, "index length");
      for (size_t k = 0; k < frames.size(); k++)
      {
        const uint64_t want = f + 1 - held + k;
        require(frames[k].cpiId == want, "index order or cpiId");
        require(frames[k].timestamp == 1000 + 500 * want, "index timestamp");
      }

      std::vector<int16_t> out;
      IqCache::FrameInfo info{};
      for (const IqCache::FrameInfo &h : frames)
      {
        require(cache.read(h.timestamp, info, out), "readable frame not read");
        require(info.cpiId == h.cpiId, "read returned the wrong frame");
        require(out.size() == size_t(4) * n && matches(out.data(), h.cpiId, n), "frame contents or layout");
      }
      if (f + 1 > held)
        require(!cache.read(1000 + 500 * (f - held), info, out), "frame inside the lap margin was readable");
    }
    require(cache.get_n_inexact() == 0, "no frame was inexact");
  }
  IqCache::remove(path);
}

static void header_and_layout()
{
  const std::string path = temp_path("header");
  const uint32_t n = 1000, nSlots = 5;
  {
    IqCache cache(nSlots, n, run_for(path, 1234));
    const iqshm::Geometry g = iqshm::geometry(nSlots, n);
    ReaderMap map(path);
    const iqshm::Header &h = map.header();
    require(std::memcmp(h.magic, iqshm::kMagic, 8) == 0, "magic");
    require(h.layoutVersion == iqshm::kLayoutVersion, "layout version");
    require(h.runId == 1234 && h.fs == 2000000 && h.cpi == 0.5, "run facts");
    require(h.nSamples == n && h.nSlots == nSlots, "ring shape");
    require(h.tuningCapacity == iqshm::kTuningCapacity && h.tuningEntryBytes == 64, "tuning shape");
    require(h.slotTableOffset == g.slotTableOffset && h.configOffset == g.configOffset
            && h.tuningOffset == g.tuningOffset && h.slotsOffset == g.slotsOffset
            && h.slotStride == g.slotStride && h.totalBytes == g.totalBytes, "offsets");
    for (uint64_t off : {h.slotTableOffset, h.configOffset, h.tuningOffset, h.slotsOffset, h.slotStride})
      require(off % iqshm::kPage == 0, "region not page-aligned");
    require(map.size == g.totalBytes && cache.get_total_bytes() == g.totalBytes, "file size");
    require(std::string(h.imageTag) == "blah2:test", "image tag");
    const std::string config(reinterpret_cast<const char *>(map.base + h.configOffset), h.configBytes);
    require(config == run_for(path).configText, "config text");
    const auto *table = reinterpret_cast<const iqshm::SlotEntry *>(map.base + h.slotTableOffset);
    for (uint32_t s = 0; s < nSlots; s++)
      require(table[s].cpiId == iqshm::kUnwritten, "unwritten slot entry");
  }
  IqCache::remove(path);
}

static void independent_mapping()
{
  const std::string path = temp_path("map");
  const uint32_t n = 300, nSlots = 4;
  {
    IqCache cache(nSlots, n, run_for(path));
    IqData x(n), y(n);
    for (uint64_t f = 0; f < 6; f++)
    {
      fill(x, y, f, n);
      cache.write(x, y, 77 + f);
    }
    ReaderMap map(path);
    const iqshm::Header &h = map.header();
    require(h.nWritten == 6, "nWritten");
    const auto *table = reinterpret_cast<const iqshm::SlotEntry *>(map.base + h.slotTableOffset);
    for (uint64_t id = 6 - (nSlots - 2); id < 6; id++)
    {
      const uint32_t slot = uint32_t(id % nSlots);
      require(table[slot].cpiId == id && table[slot].timestamp == 77 + id, "slot table");
      const auto *samples = reinterpret_cast<const int16_t *>(map.base + h.slotsOffset + slot * h.slotStride);
      require(matches(samples, id, n), "samples through an independent mapping");
    }
  }
  IqCache::remove(path);
}

static void reuse_and_resize()
{
  const std::string path = temp_path("life");
  const uint32_t n = 100;
  IqData x(n), y(n);
  fill(x, y, 0, n);

  ino_t first;
  {
    IqCache a(4, n, run_for(path, 1));
    for (int f = 0; f < 5; f++) a.write(x, y, uint64_t(f));
    first = inode_of(path);
  }
  require(inode_of(path) == first, "the file must outlive blah2");

  IqCache b(4, n, run_for(path, 2));
  require(inode_of(path) == first, "a same-size restart must reuse the file");
  {
    ReaderMap map(path);
    require(map.header().runId == 2 && map.header().nWritten == 0, "restart resets the run");
  }
  require(b.index().empty(), "a restarted cache holds nothing readable");

  ReaderMap old(path);
  {
    IqCache c(6, n, run_for(path, 3));
    require(inode_of(path) != first, "a resize must replace the file, not shrink it");
    require(old.header().runId == 2, "the old mapping must stay readable after a resize");
    ReaderMap fresh(path);
    require(fresh.header().runId == 3 && fresh.header().nSlots == 6, "the new file");
  }
  IqCache::remove(path);
}

static void remove_and_failures()
{
  const std::string path = temp_path("remove");
  {
    IqCache cache(3, 10, run_for(path));
  }
  require(IqCache::remove(path), "removing an existing file");
  struct stat st{};
  require(::stat(path.c_str(), &st) != 0, "file still present after remove");
  require(!IqCache::remove(path), "removing a missing file reports nothing removed");

  bool threwSlots = false;
  try { IqCache bad(iqshm::kMinSlots - 1, 10, run_for(path)); }
  catch (const std::invalid_argument &) { threwSlots = true; }
  require(threwSlots, "too few slots to read any frame should be refused");

  bool threwPath = false;
  try { IqCache bad(3, 10, run_for("/nonexistent-dir/blah2-iq")); }
  catch (const std::runtime_error &) { threwPath = true; }
  require(threwPath, "an unusable path should fail at construction");
}

static void wrapped_source_and_lengths()
{
  const std::string path = temp_path("src");
  const uint32_t n = 100;
  {
    IqData x(n), y(n);
    for (uint32_t i = 0; i < n + 37; i++)
    {
      x.push_back(Complex(-1, -1));
      y.push_back(Complex(-1, -1));
    }
    for (uint32_t i = 0; i < n; i++)
    {
      x.push_back(Complex(pattern(0, i, 0), pattern(0, i, 1)));
      y.push_back(Complex(pattern(0, i, 2), pattern(0, i, 3)));
    }
    IqCache cache(3, n, run_for(path));
    cache.write(x, y, 42);
    std::vector<int16_t> out;
    IqCache::FrameInfo info{};
    require(cache.read(42, info, out) && matches(out.data(), 0, n), "a wrapped IqData was not read in FIFO order");

    IqData shortX(n), shortY(n);
    for (uint32_t i = 0; i < n - 1; i++)
    {
      shortX.push_back(Complex(0, 0));
      shortY.push_back(Complex(0, 0));
    }
    bool threw = false;
    try { cache.write(shortX, shortY, 8); }
    catch (const std::invalid_argument &) { threw = true; }
    require(threw, "a short CPI should be refused");
  }
  IqCache::remove(path);
}

static void inexact()
{
  const std::string path = temp_path("inexact");
  {
    const uint32_t n = 6;
    IqCache cache(3, n, run_for(path));
    IqData x(n), y(n);
    const double re[n] = {32767.0, -32768.0, 1.4, -1.6, 40000.0, -1e9};
    for (uint32_t i = 0; i < n; i++)
    {
      x.push_back(Complex(re[i], 0));
      y.push_back(Complex(0, 0));
    }
    require(!cache.write(x, y, 1), "inexact samples should be reported");
    require(cache.get_n_inexact() == 1, "the inexact frame should be counted");
    std::vector<int16_t> out;
    IqCache::FrameInfo info{};
    require(cache.read(1, info, out), "frame missing");
    const int16_t want[n] = {32767, -32768, 1, -2, 32767, -32768};
    for (uint32_t i = 0; i < n; i++)
      require(out[size_t(4) * i] == want[i], "rounding or clipping value " + std::to_string(i));
  }
  IqCache::remove(path);
}

static void tuning_in_file()
{
  const std::string path = temp_path("tuning");
  {
    TuningLog log;
    log.set_config(1000, 503000000u, 40, 40, 4);
    log.add_gain_event(1001, 1, 40, 0, 52.5);
    IqCache cache(3, 10, run_for(path));
    log.attach([&cache](const TuningLog::Entry &e) { cache.append_tuning(e); });
    log.add_retune(2000, 7, 213000000u, 30, 35, 6);

    ReaderMap map(path);
    const iqshm::Header &h = map.header();
    const auto *entries = reinterpret_cast<const iqshm::TuningEntry *>(map.base + h.tuningOffset);
    require(h.nTuning == 3, "replayed and new entries counted");
    require(entries[0].kind == 0 && entries[0].fc == 503000000u && entries[0].gainReductionA == 40
            && entries[0].lnaState == 4 && entries[0].timestamp == 1000, "config entry");
    require(entries[1].kind == 2 && entries[1].tuner == 1 && entries[1].systemGainDb == 52.5, "replayed gain event");
    require(entries[2].kind == 1 && entries[2].generation == 7 && entries[2].fc == 213000000u
            && entries[2].gainReductionB == 35, "retune after attach");

    const uint64_t extra = iqshm::kTuningCapacity + 10;
    for (uint64_t i = 0; i < extra; i++)
      log.add_gain_event(3000 + i, 2, uint32_t(i), 0, 0.0);
    const uint64_t total = 3 + extra;
    require(h.nTuning == total, "nTuning after wrap");
    require(entries[0].kind == 0 && entries[0].timestamp == 1000, "config entry overwritten");
    const iqshm::TuningEntry &newest = entries[iqshm::tuning_position(total - 1)];
    require(newest.kind == 2 && newest.timestamp == 3000 + extra - 1, "newest entry position");

    bool threw = false;
    TuningLog early;
    try { early.attach([](const TuningLog::Entry &) {}); }
    catch (const std::logic_error &) { threw = true; }
    require(threw, "attach before set_config should be refused");
  }
  IqCache::remove(path);
}

// Lock-free by design, so ThreadSanitizer would flag it; skipped there.
static void concurrent_reader()
{
#if defined(__SANITIZE_THREAD__)
  std::printf("concurrent_reader skipped under ThreadSanitizer (lock-free by design)\n");
  return;
#else
  const std::string path = temp_path("race");
  const uint32_t n = 4096, nSlots = iqshm::kMinSlots;
  const uint64_t nFrames = 3000;
  {
    IqCache cache(nSlots, n, run_for(path));
    std::atomic<bool> done{false};
    std::atomic<uint64_t> accepted{0}, refused{0};
    std::string failure;

    std::thread reader([&] {
      std::vector<int16_t> out;
      IqCache::FrameInfo info{};
      while (!done.load())
      {
        for (const IqCache::FrameInfo &h : cache.index())
        {
          if (!cache.read(h.timestamp, info, out)) { refused++; continue; }
          if (info.timestamp != h.timestamp || !matches(out.data(), info.cpiId, n))
          {
            failure = "torn frame accepted at cpiId " + std::to_string(info.cpiId);
            done.store(true);
            return;
          }
          accepted++;
        }
      }
    });

    IqData x(n), y(n);
    for (uint64_t f = 0; f < nFrames && !done.load(); f++)
    {
      fill(x, y, f, n);
      cache.write(x, y, f);
    }
    done.store(true);
    reader.join();
    require(failure.empty(), failure);
    require(accepted.load() > 0, "the reader never got a frame, so nothing was tested");
    std::printf("concurrent_reader: %llu accepted, %llu refused as lapped\n",
                (unsigned long long)accepted.load(), (unsigned long long)refused.load());
  }
  IqCache::remove(path);
#endif
}

int main()
{
  try
  {
    round_trip_and_wrap();
    header_and_layout();
    independent_mapping();
    reuse_and_resize();
    remove_and_failures();
    wrapped_source_and_lengths();
    inexact();
    tuning_in_file();
    concurrent_reader();
  }
  catch (const std::exception &e)
  {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
  std::printf("testIqCache: all checks passed\n");
  return 0;
}
