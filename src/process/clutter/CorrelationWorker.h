#pragma once

#include <fftw3.h>

#include <algorithm>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#endif

namespace owl_corr {

using Complex = std::complex<double>;

struct Block
{
  const Complex* x;
  const Complex* y;
  uint32_t samples;
  uint32_t taps;
  uint32_t length;
  uint64_t begin;
  fftw_plan plan;
};

inline void validate(const Block& block)
{
  if (!block.x || !block.y || !block.samples || !block.taps ||
      block.taps > block.samples || block.taps > block.length ||
      block.begin >= block.samples || !block.plan)
    throw std::invalid_argument("Invalid correlation job");
}

inline void transform(const Block& block, Complex* buffer)
{
  validate(block);
  if (!buffer) throw std::invalid_argument("Invalid correlation buffer");

  const uint32_t hop = block.length - block.taps + 1;
  const uint32_t valid = static_cast<uint32_t>(
      std::min<uint64_t>(hop, block.samples - block.begin));
  Complex* history = buffer;
  Complex* currentX = history + block.length;
  Complex* currentY = currentX + block.length;

  uint32_t position = static_cast<uint32_t>(
      (block.begin + block.samples - (block.taps - 1)) % block.samples);
  for (uint32_t copied = 0; copied < block.length;)
  {
    const uint32_t count =
        std::min(block.length - copied, block.samples - position);
    std::copy(block.x + position, block.x + position + count,
              history + copied);
    copied += count;
    position = 0;
  }

  std::fill(currentX, currentX + block.taps - 1, Complex{});
  std::fill(currentY, currentY + block.taps - 1, Complex{});
  std::copy(block.x + block.begin, block.x + block.begin + valid,
            currentX + block.taps - 1);
  std::copy(block.y + block.begin, block.y + block.begin + valid,
            currentY + block.taps - 1);
  std::fill(currentX + block.taps - 1 + valid,
            currentX + block.length, Complex{});
  std::fill(currentY + block.taps - 1 + valid,
            currentY + block.length, Complex{});

  fftw_execute_dft(block.plan, reinterpret_cast<fftw_complex*>(buffer),
                   reinterpret_cast<fftw_complex*>(buffer));
}

inline void accumulate(const Complex* buffer, uint32_t length,
                       Complex* autocorrelation, Complex* crosscorrelation)
{
  if (!buffer || !length || !autocorrelation || !crosscorrelation)
    throw std::invalid_argument("Invalid correlation accumulation");
  const Complex* history = buffer;
  const Complex* currentX = history + length;
  const Complex* currentY = currentX + length;
  for (uint32_t i = 0; i < length; ++i)
  {
    const Complex reference = std::conj(history[i]);
    autocorrelation[i] += currentX[i] * reference;
    crosscorrelation[i] += currentY[i] * reference;
  }
}

class Worker
{
  struct FftwFree
  {
    void operator()(Complex* value) const { fftw_free(value); }
  };

  const uint32_t length_;
  std::unique_ptr<Complex, FftwFree> buffer_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stopping_ = false;
  bool pending_ = false;
  bool done_ = false;
  bool busy_ = false;
  Block block_{};
  std::exception_ptr error_;
  std::thread thread_;

  void run()
  {
#ifdef __linux__
    pthread_setname_np(pthread_self(), "owl-clutter-fft");
#endif
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;)
    {
      condition_.wait(lock, [&] { return stopping_ || pending_; });
      if (stopping_ && !pending_) return;
      const Block block = block_;
      pending_ = false;
      lock.unlock();
      std::exception_ptr error;
      try
      {
        transform(block, buffer_.get());
      }
      catch (...)
      {
        error = std::current_exception();
      }
      lock.lock();
      error_ = error;
      done_ = true;
      condition_.notify_all();
    }
  }

public:
  Worker(uint32_t length, Complex* plannedBuffer)
      : length_(length),
        buffer_(static_cast<Complex*>(
            fftw_malloc(sizeof(Complex) * uint64_t(length) * 3)))
  {
    if (!length || !plannedBuffer)
      throw std::invalid_argument("Invalid correlation worker geometry");
    if (!buffer_) throw std::bad_alloc();
    if (fftw_alignment_of(reinterpret_cast<double*>(buffer_.get())) !=
        fftw_alignment_of(reinterpret_cast<double*>(plannedBuffer)))
      throw std::runtime_error(
          "Correlation worker FFT alignment differs from planned buffer");
    thread_ = std::thread([this] { run(); });
  }

  ~Worker()
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  void start(const Block& block)
  {
    validate(block);
    if (block.length != length_)
      throw std::invalid_argument("Correlation worker length mismatch");
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_ || stopping_)
      throw std::logic_error("Correlation worker job not consumed");
    block_ = block;
    pending_ = true;
    done_ = false;
    busy_ = true;
    error_ = nullptr;
    condition_.notify_all();
  }

  const Complex* finish()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!busy_) throw std::logic_error("No correlation worker job pending");
    condition_.wait(lock, [&] { return done_; });
    done_ = false;
    busy_ = false;
    if (error_) std::rethrow_exception(error_);
    return buffer_.get();
  }

  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;
};

inline void correlate(Worker& worker, const Complex* x, const Complex* y,
                      uint32_t samples, uint32_t taps, uint32_t length,
                      fftw_plan plan, Complex* callerBuffer,
                      Complex* autocorrelation, Complex* crosscorrelation)
{
  validate(Block{x, y, samples, taps, length, 0, plan});
  if (!callerBuffer || !autocorrelation || !crosscorrelation)
    throw std::invalid_argument("Invalid correlation storage");
  const uint32_t hop = length - taps + 1;
  for (uint64_t begin = 0; begin < samples; begin += uint64_t(hop) * 2)
  {
    const bool second = begin + hop < samples;
    if (second)
      worker.start(Block{x, y, samples, taps, length, begin + hop, plan});
    try
    {
      transform(Block{x, y, samples, taps, length, begin, plan},
                callerBuffer);
      accumulate(callerBuffer, length, autocorrelation, crosscorrelation);
    }
    catch (...)
    {
      if (second)
      {
        try { (void)worker.finish(); } catch (...) {}
      }
      throw;
    }
    // Accumulation stays in block order; only independent forward transforms
    // run concurrently, so the opt-in worker is byte-identical to serial.
    if (second)
      accumulate(worker.finish(), length, autocorrelation, crosscorrelation);
  }
}

}  // namespace owl_corr
