// IqData's ring buffer, checked against the std::deque it replaced.
//
// The storage changed but not one rule of the queue, so the test that matters
// is differential: drive a reference model built from std::deque exactly the
// way the old class was written, drive IqData with the identical operations,
// and require that every observable agrees at every step. Anything the ring
// gets wrong about wrapping, eviction or ordering shows up as a divergence
// rather than as a plausible-looking wrong number.
#include "data/IqData.h"

#include <complex>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using Complex = std::complex<double>;

static void require(bool condition, const std::string& message)
{
  if (!condition) throw std::runtime_error(message);
}

// The old IqData, transcribed. push_back evicted the front at capacity,
// pop_front threw when empty, clear popped everything.
class DequeModel
{
public:
  explicit DequeModel(uint32_t capacity) : n(capacity) {}

  void push_back(Complex sample)
  {
    if (data.size() < n) data.push_back(sample);
    else { data.pop_front(); data.push_back(sample); }
  }

  Complex pop_front()
  {
    if (data.empty()) throw std::runtime_error("empty");
    const Complex sample = data.front();
    data.pop_front();
    return sample;
  }

  void clear() { while (!data.empty()) data.pop_front(); }

  uint32_t get_length() const { return static_cast<uint32_t>(data.size()); }
  const Complex& operator[](uint32_t i) const { return data[i]; }
  std::vector<Complex> get_data() const { return {data.begin(), data.end()}; }

private:
  uint32_t n;
  std::deque<Complex> data;
};

static void agree(const IqData& ring, const DequeModel& model, const std::string& where)
{
  require(ring.get_length() == model.get_length(),
          where + ": length " + std::to_string(ring.get_length()) +
          " against " + std::to_string(model.get_length()));
  for (uint32_t i = 0; i < model.get_length(); i++)
    require(ring[i] == model[i], where + ": sample " + std::to_string(i) + " differs");
  require(ring.get_data() == model.get_data(), where + ": get_data differs");
}

// Every operation mix the real pipeline performs, plus the boundaries it does
// not: capacity 1, exact fill, eviction, clear mid-wrap.
static void differential(uint32_t capacity, unsigned seed, int steps)
{
  IqData ring(capacity);
  DequeModel model(capacity);
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> pick(0, 99);
  std::normal_distribution<double> value;

  for (int step = 0; step < steps; step++)
  {
    const int roll = pick(rng);
    if (roll < 55)
    {
      const Complex sample(value(rng), value(rng));
      ring.push_back(sample);
      model.push_back(sample);
    }
    else if (roll < 90)
    {
      const bool empty = model.get_length() == 0;
      bool ringThrew = false, modelThrew = false;
      Complex a{}, b{};
      try { a = ring.pop_front(); } catch (const std::runtime_error&) { ringThrew = true; }
      try { b = model.pop_front(); } catch (const std::runtime_error&) { modelThrew = true; }
      require(ringThrew == modelThrew, "pop_front disagreed about being empty");
      require(ringThrew == empty, "pop_front threw on a non-empty queue, or did not on an empty one");
      if (!ringThrew) require(a == b, "pop_front returned a different sample");
    }
    else
    {
      ring.clear();
      model.clear();
    }
    agree(ring, model, "capacity " + std::to_string(capacity) + " step " + std::to_string(step));
  }
  std::printf("PASS differential capacity=%u steps=%d\n", capacity, steps);
}

int main()
{
  try
  {
    // The shapes the pipeline actually uses, and the awkward ones it does not.
    // Small capacities wrap constantly, which is where an off-by-one lives.
    for (uint32_t capacity : {1u, 2u, 3u, 7u, 8u, 64u, 1000u})
      differential(capacity, 4242u + capacity, 1200);

    // The capture pattern: fill past capacity so the queue is permanently full
    // and every push evicts. The ring's head is then somewhere in the middle,
    // which is the state the stage loops index through.
    {
      const uint32_t n = 100;
      IqData ring(n);
      DequeModel model(n);
      for (uint32_t i = 0; i < 5 * n + 37; i++)
      {
        const Complex sample(static_cast<double>(i), -static_cast<double>(i));
        ring.push_back(sample);
        model.push_back(sample);
      }
      agree(ring, model, "steady-state eviction");
      require(ring.get_length() == n, "a saturated queue should hold exactly n");
      require(ring[0] == Complex(5 * n + 37 - n, -static_cast<double>(5 * n + 37 - n)),
              "front is not the oldest surviving sample");
      require(ring[n - 1] == Complex(5 * n + 36, -static_cast<double>(5 * n + 36)),
              "back is not the newest sample");
      std::printf("PASS steady-state eviction\n");
    }

    // The clutter filter's pattern: clear a full CPI and refill it. This is the
    // case the container change exists for, and clear() no longer touches the
    // storage, so a stale sample must not be reachable through the new fill.
    {
      const uint32_t n = 256;
      IqData ring(n);
      DequeModel model(n);
      for (uint32_t cycle = 0; cycle < 4; cycle++)
      {
        ring.clear();
        model.clear();
        require(ring.get_length() == 0, "clear left samples behind");
        require(ring.get_data().empty(), "clear left get_data non-empty");
        for (uint32_t i = 0; i < n; i++)
        {
          const Complex sample(static_cast<double>(cycle * 1000 + i), 0.5);
          ring.push_back(sample);
          model.push_back(sample);
        }
        agree(ring, model, "clear-then-refill cycle " + std::to_string(cycle));
      }
      std::printf("PASS clear then refill\n");
    }

    // Partial drain then refill, so head and the fill point are both mid-buffer
    // and a refill has to straddle the wrap.
    {
      const uint32_t n = 64;
      IqData ring(n);
      DequeModel model(n);
      for (uint32_t i = 0; i < n; i++)
      {
        const Complex s(static_cast<double>(i), 0);
        ring.push_back(s); model.push_back(s);
      }
      for (uint32_t i = 0; i < 40; i++)
      {
        require(ring.pop_front() == model.pop_front(), "drain diverged");
      }
      for (uint32_t i = 0; i < 50; i++)
      {
        const Complex s(1000.0 + i, 0);
        ring.push_back(s); model.push_back(s);
      }
      agree(ring, model, "straddling refill");
      std::printf("PASS partial drain then straddling refill\n");
    }

    // A zero-capacity queue cannot hold the invariants, so it is rejected
    // rather than silently producing a buffer nothing can be pushed to.
    {
      bool rejected = false;
      try { IqData reject(0); }
      catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "zero capacity accepted");
      std::printf("PASS zero capacity rejected\n");
    }

    // Popping an empty queue throws rather than returning a stale sample.
    {
      IqData ring(4);
      bool threw = false;
      try { (void)ring.pop_front(); } catch (const std::runtime_error&) { threw = true; }
      require(threw, "pop_front on an empty queue did not throw");
      ring.push_back({1.0, 2.0});
      require(ring.pop_front() == Complex(1.0, 2.0), "pop_front returned the wrong sample");
      threw = false;
      try { (void)ring.pop_front(); } catch (const std::runtime_error&) { threw = true; }
      require(threw, "pop_front did not throw after the queue drained");
      std::printf("PASS empty pop throws\n");
    }

    std::printf("All IqData checks passed\n");
  }
  catch (const std::exception& error)
  {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
