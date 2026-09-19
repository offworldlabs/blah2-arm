/// @file TuneState.h
/// @brief Cross-thread handoff for live retune events.
/// @details Set by Capture's retune-poll thread after RspDuo::retune()
/// applies a changed center frequency. Latched once per CPI by blah2.cpp's
/// front stage so the new frequency is adopted against the CPI it applies to,
/// in capture order, rather than whenever the retune happened to land.
/// Gain-only retunes never touch this: same geometry, nothing to refresh.
/// @author 30hours

#ifndef TUNESTATE_H
#define TUNESTATE_H

#include <atomic>
#include <stdint.h>

struct TuneState
{
  /// @brief True if fc changed since the processing loop last consumed it.
  std::atomic<bool> fcChanged{false};

  /// @brief The center frequency (Hz) currently applied to the device.
  std::atomic<uint32_t> currentFc{0};
};

extern TuneState g_tuneState;

#endif
