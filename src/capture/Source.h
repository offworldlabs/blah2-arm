/// @file Source.h
/// @class Source
/// @brief An abstract class for capture sources.
/// @author 30hours

#ifndef SOURCE_H
#define SOURCE_H

#include <string>
#include <stdint.h>
#include <fstream>
#include <atomic>
#include "data/IqData.h"

class Source
{
protected:

  /// @brief The capture device type.
  std::string type;

  /// @brief Center frequency (Hz).
  uint32_t fc;

  /// @brief Sampling frequency (Hz).
  uint32_t fs;

  /// @brief Absolute path to IQ save location.
  std::string path;

  /// @brief True if IQ data to be saved.
  bool *saveIq;

  /// @brief File stream to save IQ data.
  std::ofstream saveIqFile;

public:

  Source();

  /// @brief Constructor.
  /// @param type The capture device type.
  /// @param fs Sampling frequency (Hz).
  /// @param fc Center frequency (Hz).
  /// @param path Absolute path to IQ save location.
  /// @return The object.
  Source(std::string type, uint32_t fc, uint32_t fs, 
    std::string path, bool *saveIq);

  /// @brief Implement the capture process.
  /// @param buffer1 Buffer for reference samples.
  /// @param buffer2 Buffer for surveillance samples.
  /// @return Void.
  virtual void process(IqData *buffer1, IqData *buffer2) = 0;

  /// @brief Call methods to start capture.
  /// @return Void.
  virtual void start() = 0;

  /// @brief Call methods to gracefully stop capture.
  /// @return Void.
  virtual void stop() = 0;

  /// @brief Ask a running capture/replay loop to return so its owner can
  /// stop callbacks and join control threads in the normal teardown path.
  virtual void request_stop() noexcept {}

  /// @brief Implement replay function on RSPduo.
  /// @param buffer1 Pointer to reference buffer.
  /// @param buffer2 Pointer to surveillance buffer.
  /// @param file Path to file to replay data from.
  /// @param loop True if samples should loop at EOF.
  /// @return Void.
  virtual void replay(IqData *buffer1, IqData *buffer2,
    std::string file, bool loop) = 0;

  /// @brief Live retune without restarting capture.
  /// @param fc Center frequency (Hz).
  /// @param gainReductionA Gain reduction for tuner A (dB).
  /// @param gainReductionB Gain reduction for tuner B (dB).
  /// @param lnaState LNA state (shared across both tuners).
  /// @param fcChanged Set true if the applied fc differs from the previous fc.
  /// @return True if the retune was applied. Default: unsupported.
  virtual bool retune(uint32_t fc, int gainReductionA, int gainReductionB,
    int lnaState, bool &fcChanged) { (void)fc; (void)gainReductionA;
    (void)gainReductionB; (void)lnaState; (void)fcChanged; return false; }

  /// @brief Read per-tuner RF overload state.
  /// @param overloadA Set to tuner A overload state.
  /// @param overloadB Set to tuner B overload state.
  /// @return True if overload reporting is supported. Default: unsupported.
  virtual bool get_overload(bool &overloadA, bool &overloadB)
    { (void)overloadA; (void)overloadB; return false; }

  /// @brief Read the monotonic count of per-tuner overload onsets.
  /// @param countA Set to tuner A onset count since start.
  /// @param countB Set to tuner B onset count since start.
  /// @return True if overload counting is supported. Default: unsupported.
  /// @note Consumers should prefer these over the level above when asking
  /// "did this operating point clip?" - the level can only be sampled, and
  /// this hardware clips and recovers fast enough to slip between polls.
  virtual bool get_overload_counts(unsigned long &countA, unsigned long &countB)
    { (void)countA; (void)countB; return false; }

  /// @brief Read per-tuner peak sample level since the last read.
  /// @param dbfsA Set to tuner A peak level (dBFS, 0 = full scale).
  /// @param dbfsB Set to tuner B peak level (dBFS, 0 = full scale).
  /// @return True if peak dBFS reporting is supported. Default: unsupported.
  virtual bool get_peak_dbfs(double &dbfsA, double &dbfsB)
    { (void)dbfsA; (void)dbfsB; return false; }

  /// @brief Open a new file to record IQ.
  /// @details First creates a new file from current timestamp.
  /// Files are of format <path>.<type>.iq.
  /// @return String of full path to file.
  std::string open_file();

  /// @brief Close IQ file gracefully.
  /// @return Void.
  void close_file();

  /// @brief Graceful handler for SIGTERM.
  /// @return Void.
  void kill();

};

#endif
