/// @file IqData.h
/// @class IqData
/// @brief A class to store IQ data.
/// @details Implements a fixed-capacity FIFO of IQ samples. Storage is one
/// contiguous buffer of @c n samples, allocated once in the constructor and
/// never reallocated: the queue is a ring over it, so pushing, popping and
/// clearing only move two indices.
///
/// It used to hold a @c std::deque. libstdc++ chunks a deque at 512 bytes, so
/// 32 of these 16-byte samples, which made a 1e6-sample CPI 31,250 separate
/// allocations. Across both channels the pipeline churned roughly 375,000
/// chunk malloc/free pairs per CPI, and on a Pi 5 that was measured at about
/// 20 ms of every CPI. Reads were never the problem: indexed access through a
/// deque measured the same as through a vector, because every read here is
/// sequential with a wrap. The cost was all in push, pop and clear.
///
/// The ring trades memory for CPU, deliberately. It commits `n` samples per
/// object at construction, where the deque only ever materialised what it held
/// and shared that memory between objects through the allocator. Live on a Pi 5
/// that is +45 MB of RSS at a 300 Hz doppler span and +41 MB at 1000 Hz, which
/// buys about 8% of per-CPI work and 8 to 16 points of CPU. Most of the cost is
/// the capture buffers committing a `tBuffer` of 1.5 CPIs that a node keeping up
/// never uses, so sizing those to actual need would recover the bulk of it.
/// @author 30hours

#ifndef IQDATA_H
#define IQDATA_H

#include <complex>
#include <mutex>
#include <stdint.h>
#include <string>
#include <vector>

class IqData
{
private:
  /// @brief Maximum number of samples.
  uint32_t n;

  /// @brief True if should not push to buffer (mutex).
  std::mutex mutex_lock;

  /// @brief Sample storage, exactly n entries, allocated once.
  std::vector<std::complex<double>> data;

  /// @brief Index in data of the oldest sample held.
  uint32_t head;

  /// @brief Number of samples currently held.
  uint32_t count;

  /// @brief Minimum value.
  double min;

  /// @brief Maximum value.
  double max;

  /// @brief Mean value.
  double mean;

  /// @brief Spectrum vector.
  std::vector<std::complex<double>> spectrum;

  /// @brief Frequency vector (Hz).
  std::vector<double> frequency;

  /// @brief Reduce an index modulo the capacity.
  /// @details A conditional subtract rather than a division, which is correct
  /// only while index < 2n. Every caller satisfies that: head < n and the
  /// offset added to it is at most count, which is at most n.
  /// @param index Index to reduce.
  /// @return Index in [0, n).
  inline uint32_t wrap(uint64_t index) const
  {
    return static_cast<uint32_t>(index >= n ? index - n : index);
  }

public:
  /// @brief Constructor.
  /// @param n Number of samples.
  /// @return The object.
  IqData(uint32_t n);

  /// @brief Getter for maximum number of samples.
  /// @return Maximum number of samples.
  uint32_t get_n() const { return n; }

  /// @brief Getter for current data length.
  /// @return Number of samples currently in data.
  uint32_t get_length() const { return count; }

  /// @brief Locker for mutex.
  /// @return Void.
  void lock();

  /// @brief Unlocker for mutex.
  /// @return Void.
  void unlock();

  /// @brief Sample at an offset from the front, counting in FIFO order.
  /// @details Unchecked, because the callers are million-iteration stage
  /// loops that have already bounded i by get_length(). Reading at or beyond
  /// get_length() returns a stale sample rather than throwing; the deque form
  /// was equally undefined there.
  /// @param i Offset from the front.
  /// @return The sample.
  inline const std::complex<double> &operator[](uint32_t i) const
  {
    return data[wrap(uint64_t(head) + i)];
  }

  /// @brief Getter for data, copied out in FIFO order.
  /// @details Copies, so it is not for the per-CPI path. Stage code should
  /// index with operator[], which copies nothing.
  /// @return IQ data.
  std::vector<std::complex<double>> get_data() const;

  /// @brief Push a sample to the queue, dropping the oldest when full.
  /// @param sample A single sample.
  /// @return Void.
  void push_back(std::complex<double> sample);

  /// @brief Pop the front of the queue.
  /// @return Sample from the front of the queue.
  std::complex<double> pop_front();

  /// @brief Clear samples from the queue.
  /// @details Constant time. It does not scrub the storage, so samples past
  /// get_length() remain readable through operator[]; nothing may read there.
  /// @return Void.
  void clear();

  /// @brief Update the time differences and names.
  /// @param spectrum Spectrum vector.
  /// @return Void.
  void update_spectrum(std::vector<std::complex<double>> spectrum);

  /// @brief Read-only access to the spectrum.
  /// @details A plain vector in natural order, unlike the sample ring, so a
  /// reference is meaningful here. to_json() reduces it to 2 decimal places of
  /// dB, which is too coarse to compare implementations against each other.
  /// @return Const reference to the spectrum.
  const std::vector<std::complex<double>> &get_spectrum() const { return spectrum; }

  /// @brief Update the time differences and names.
  /// @param frequency Frequency vector.
  /// @return Void.
  void update_frequency(std::vector<double> frequency);

  /// @brief Generate JSON of the signal and metadata.
  /// @param timestamp Current time (POSIX ms).
  /// @return JSON string.
  std::string to_json(uint64_t timestamp);
};

#endif
