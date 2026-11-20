// IqCache, the RAM ring of int16 CPI frames.
//
// What matters is integrity: a frame read back must be bit-for-bit the CPI
// written, in the .iq recorder's interleave, under the right timestamp, and
// never a mix of two CPIs while the writer runs. Each check below targets one
// way that could fail.
#include "data/IqCache.h"
#include "data/IqData.h"

#include <atomic>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Complex = std::complex<double>;

static void require(bool condition, const std::string &message)
{
  if (!condition) throw std::runtime_error(message);
}

// A deterministic int16 value for (frame, sample, component), so a read can be
// checked against what was written without keeping a copy.
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

static bool matches(const std::vector<int16_t> &out, uint64_t frame, uint32_t n)
{
  if (out.size() != size_t(4) * n) return false;
  for (uint32_t i = 0; i < n; i++)
    for (uint32_t c = 0; c < 4; c++)
      if (out[size_t(4) * i + c] != pattern(frame, i, c)) return false;
  return true;
}

static void round_trip_and_wrap()
{
  const uint32_t n = 257, nSlots = 5;
  IqCache cache(nSlots, n);
  IqData x(n), y(n);
  require(cache.get_bytes() == size_t(nSlots) * n * 4 * sizeof(int16_t), "storage size");
  require(cache.index().empty(), "a new cache should hold nothing");

  for (uint64_t f = 0; f < 3 * nSlots + 2; f++)
  {
    fill(x, y, f, n);
    require(cache.write(x, y, 1000 + 500 * f), "integer samples should store exactly");

    // index: the most recent min(f+1, nSlots) frames, oldest first.
    const std::vector<IqCache::FrameInfo> frames = cache.index();
    const uint64_t held = (f + 1 < nSlots) ? f + 1 : nSlots;
    require(frames.size() == held, "index length");
    for (size_t k = 0; k < frames.size(); k++)
    {
      const uint64_t want = f + 1 - held + k;
      require(frames[k].cpiId == want, "index order or cpiId");
      require(frames[k].timestamp == 1000 + 500 * want, "index timestamp");
    }

    // every held frame reads back bit for bit; the evicted one is gone.
    std::vector<int16_t> out;
    IqCache::FrameInfo info{};
    for (const IqCache::FrameInfo &h : frames)
    {
      require(cache.read(h.timestamp, info, out), "held frame not readable");
      require(info.cpiId == h.cpiId, "read returned the wrong frame");
      require(matches(out, h.cpiId, n), "frame contents or interleave differ");
    }
    if (f >= nSlots)
      require(!cache.read(1000 + 500 * (f - nSlots), info, out), "evicted frame still readable");
  }
  require(cache.get_n_inexact() == 0, "no frame was inexact");
}

// The front stage hands over IqData objects whose ring head is wherever the
// last CPI left it. The cache must read in FIFO order, not storage order.
static void wrapped_source()
{
  const uint32_t n = 100;
  IqData x(n), y(n);
  for (uint32_t i = 0; i < n + 37; i++)
  {
    x.push_back(Complex(-1, -1));
    y.push_back(Complex(-1, -1));
  }
  // Now full with head at 37: push n more so FIFO order is the new samples.
  for (uint32_t i = 0; i < n; i++)
  {
    x.push_back(Complex(pattern(9, i, 0), pattern(9, i, 1)));
    y.push_back(Complex(pattern(9, i, 2), pattern(9, i, 3)));
  }
  IqCache cache(2, n);
  cache.write(x, y, 42);
  std::vector<int16_t> out;
  IqCache::FrameInfo info{};
  require(cache.read(42, info, out), "frame missing");
  require(matches(out, 9, n), "a wrapped IqData was not read in FIFO order");
}

// Only the first nSamples are a CPI; a longer source stores just those, and a
// shorter one is refused rather than padded with stale samples.
static void lengths()
{
  const uint32_t n = 10;
  IqCache cache(1, n);
  IqData x(2 * n), y(2 * n);
  for (uint32_t i = 0; i < 2 * n; i++)
  {
    x.push_back(Complex(pattern(3, i, 0), pattern(3, i, 1)));
    y.push_back(Complex(pattern(3, i, 2), pattern(3, i, 3)));
  }
  cache.write(x, y, 7);
  std::vector<int16_t> out;
  IqCache::FrameInfo info{};
  require(cache.read(7, info, out) && matches(out, 3, n), "longer source");

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

  bool threwZero = false;
  try { IqCache bad(0, n); }
  catch (const std::invalid_argument &) { threwZero = true; }
  require(threwZero, "zero slots should be refused");
}

// Values that are not int16 are rounded or clipped and the frame is counted,
// never stored as something else silently.
static void inexact()
{
  const uint32_t n = 6;
  IqCache cache(1, n);
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

  // The edges of int16 alone are exact.
  IqData ex(2), ey(2);
  ex.push_back(Complex(32767.0, -32768.0));
  ex.push_back(Complex(0.0, -0.0));
  ey.push_back(Complex(-32768.0, 32767.0));
  ey.push_back(Complex(1.0, -1.0));
  IqCache edge(1, 2);
  require(edge.write(ex, ey, 2), "int16 extremes should be exact");
  require(edge.get_n_inexact() == 0, "an exact frame should not be counted");
}

// A reader racing the writer must only ever see whole frames. Every frame's
// samples encode its own cpiId, so a torn copy fails the pattern check.
static void concurrent_reader()
{
  const uint32_t n = 4096, nSlots = 3;
  const uint64_t nFrames = 600;
  IqCache cache(nSlots, n);
  std::atomic<bool> done{false};
  std::atomic<uint64_t> reads{0};
  std::string failure;

  std::thread reader([&] {
    std::vector<int16_t> out;
    IqCache::FrameInfo info{};
    while (!done.load())
    {
      for (const IqCache::FrameInfo &h : cache.index())
      {
        if (!cache.read(h.timestamp, info, out)) continue;  // evicted meanwhile
        if (info.timestamp != h.timestamp || !matches(out, info.cpiId, n))
        {
          failure = "torn or mislabelled frame at cpiId " + std::to_string(info.cpiId);
          done.store(true);
          return;
        }
        reads++;
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
  require(reads.load() > 0, "the reader never got a frame, so nothing was tested");
}

int main()
{
  try
  {
    round_trip_and_wrap();
    wrapped_source();
    lengths();
    inexact();
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
