/// @file IqData.h
/// @class IqData
/// @brief A class to store IQ data.
/// @details Implements a FIFO queue to store IQ samples.
/// @author 30hours

#ifndef IQDATA_H
#define IQDATA_H

#include <stdint.h>
#include <deque>
#include <vector>
#include <complex>
#include <mutex>

class IqData
{
private:
  /// @brief Maximum number of samples.
  uint32_t n;

  /// @brief True if should not push to buffer (mutex).
  std::mutex mutex_lock;

  /// @brief Pointer to IQ data.
  std::deque<std::complex<double>> *data;

  /// @brief Legacy JSON placeholder; no minimum statistic is calculated here.
  double min;

  /// @brief Legacy JSON placeholder; no maximum statistic is calculated here.
  double max;

  /// @brief Legacy JSON placeholder; no mean statistic is calculated here.
  double mean;

  /// @brief Spectrum vector.
  std::vector<std::complex<double>> spectrum;

  /// @brief Frequency vector (Hz).
  std::vector<double> frequency;

public:
  /// @brief Constructor.
  /// @param n Number of samples.
  /// @return The object.
  IqData(uint32_t n);
  ~IqData();
  IqData(const IqData&) = delete;
  IqData& operator=(const IqData&) = delete;

  /// @brief Getter for maximum number of samples.
  /// @return Maximum number of samples.
  uint32_t get_n();

  /// @brief Getter for current data length.
  /// @return Number of samples currently in data.
  uint32_t get_length();

  /// @brief Locker for mutex.
  /// @return Void.
  void lock();

  /// @brief Unlocker for mutex.
  /// @return Void.
  void unlock();

  /// @brief Getter for data.
  /// @return IQ data.
  std::deque<std::complex<double>> get_data();

  /// @brief Read-only view of the data, copying nothing.
  /// @details Prefer this to get_data() on the hot path: a CPI is a million
  /// samples, so a copy is 16 MB and roughly 31,000 deque-chunk allocations.
  /// The caller must not hold the reference across anything that mutates the
  /// queue (push_back, pop_front, clear).
  /// @return Const reference to the IQ data.
  const std::deque<std::complex<double>> &view_data() const;

  /// @brief Replace samples from a nonaliasing contiguous buffer.
  /// @details Rejects counts above capacity and null non-empty input without
  /// changing the existing samples.
  void assign_samples(const std::complex<double>* samples, uint32_t count);

  /// Replace both exclusively owned channels from interleaved signed16 IIQQ.
  void assign_paired_i16(const int16_t* samples, uint32_t count, IqData& other);

  /// @brief Push a sample to the queue.
  /// @param sample A single sample.
  /// @return Void.
  void push_back(std::complex<double> sample);

  /// @brief Pop the front of the queue.
  /// @return Sample from the front of the queue.
  std::complex<double> pop_front();

  /// @brief Print to stdout (debug).
  /// @return Void.
  void print();

  /// @brief Clear samples from the queue.
  /// @return Void.
  void clear();

  /// @brief Update the time differences and names.
  /// @param spectrum Spectrum vector.
  /// @return Void.
  void update_spectrum(std::vector<std::complex<double>> spectrum);

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
