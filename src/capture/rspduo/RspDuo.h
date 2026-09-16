/// @file RspDuo.h
/// @class RspDuo
/// @brief A class to capture data on the SDRplay RspDuo.
/// @details Loosely based upon the sdrplay_api_sample_app.c and sdr_play.c examples
/// provided in the SDRplay API V3 documentation.
/// This should be read in conjuction with that documentation
///
/// For coherent operation the use of sdrplay_api_Tuner_Both is most important
/// This clue was provided by Gustaw Mazurek of WUT
/// <https://github.com/fventuri/gr-sdrplay/issues/2>
/// <https://github.com/g4eev/RSPduoEME/blob/main/rspduointerface.cpp>
///
/// Reference for using C style callback API with a C++ wrapper:
/// <https://stackoverflow.com/questions/63768893/pointer-problem-using-functions-from-non-object-api-in-objects?rq=1>
/// @author 30hours
/// @author Michael P
/// @todo Remove max time.

#ifndef RSPDUO_H
#define RSPDUO_H

#include "sdrplay_api.h"
#include "capture/Source.h"
#include "data/IqData.h"

#include <stdint.h>
#include <string>
#include <cstdint>

class PairedCpiQueue;

#define BUFFER_SIZE_NR 1024

class RspDuo : public Source
{
private:
  /// @brief AGC bandwidth (Hz)
  int agc_bandwidth_nr;
  /// @brief AGC set point (dBfs)
  int agc_set_point_nr;
  /// @brief AGC enable value derived from agc_bandwidth_nr in
  /// set_device_parameters(), applied identically to both tuners there and
  /// reapplied in process() - see the AGC comment in set_device_parameters().
  sdrplay_api_AgcControlT agc_enable_nr;
  /// @brief Gain reduction for tuner A / reference channel (dB).
  int gain_reduction_nr_a;
  /// @brief Gain reduction for tuner B / surveillance channel (dB).
  int gain_reduction_nr_b;
  /// @brief LNA state
  int lna_state_nr;
  /// @brief Decimation factor (integer).
  int nDecimation;
  /// @brief MW and FM notch filters.
  bool rf_notch_fg;
  /// @brief DAB notch filter.
  bool dab_notch_fg;
  /// @brief USB bulk transfer mode.
  bool usb_bulk_fg;
  /// @brief SDRplay IF bandwidth enum.
  sdrplay_api_Bw_MHzT bwType;
  /// @brief SDRplay IF mode enum.
  sdrplay_api_If_kHzT ifType;
  /// @brief True when running from replay() — no real device to update, so
  /// retune() simulates success against local state only. Test-support only.
  bool replay_mode_fg;

  /// @brief Maximum frequency (Hz).
  static const double MAX_FREQUENCY_NR;
  /// @brief Minimum AGC set point.
  static const int MIN_AGC_SET_POINT_NR;
  /// @brief Minimum gain reduction.
  static const int MIN_GAIN_REDUCTION_NR;
  /// @brief Maximum gain reduction.
  static const int MAX_GAIN_REDUCTION_NR;
  /// @brief Min LNA state.
  static const int MIN_LNA_STATE_NR;
  /// @brief Max LNA state.
  static const int MAX_LNA_STATE_NR;
  /// @brief Default sample rate.
  static const int DEF_SAMPLE_RATE_NR;

  /// @brief Check parameters for valid for capture device.
  /// @return The object.
  void validate();

  /// @brief Start API functions.
  /// @return The object.
  void open_api();

  /// @brief Device selection function.
  /// @return The object.
  void get_device();

  /// @brief Set device parameters.
  /// @return The object.
  void set_device_parameters();

  /// @brief Wrapper for C style callback function for stream_a_callback().
  /// @param xi Pointer to real part of sample.
  /// @param xq Pointer to imag part of sample.
  /// @param params As defined in SDRplay API.
  /// @param numSamples Number of samples in block.
  /// @param reset As defined in SDRplay API.
  /// @param cbContext As defined in SDRplay API.
  /// @return Void.
  static void _stream_a_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext)
  {
    auto *self = static_cast<RspDuo *>(cbContext);
    if (!self) return;
    try { self->stream_a_callback(xi, xq, params, numSamples, reset, cbContext); }
    catch (...) { self->callback_failure(); }
  };

  /// @brief Wrapper for C style callback function for stream_b_callback().
  /// @param xi Pointer to real part of sample.
  /// @param xq Pointer to imag part of sample.
  /// @param params As defined in SDRplay API.
  /// @param numSamples Number of samples in block.
  /// @param reset As defined in SDRplay API.
  /// @param cbContext As defined in SDRplay API.
  /// @return Void.
  static void _stream_b_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext)
  {
    auto *self = static_cast<RspDuo *>(cbContext);
    if (!self) return;
    try { self->stream_b_callback(xi, xq, params, numSamples, reset, cbContext); }
    catch (...) { self->callback_failure(); }
  };

  /// @brief Wrapper for C style callback function for event_callback().
  /// @param eventId As defined in SDRplay API.
  /// @param tuner As defined in SDRplay API.
  /// @param params As defined in SDRplay API.
  /// @param cbContext As defined in SDRplay API.
  /// @return Void.
  static void _event_callback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext)
  {
    auto *self = static_cast<RspDuo *>(cbContext);
    if (!self) return;
    try { self->event_callback(eventId, tuner, params, cbContext); }
    catch (...) { self->callback_failure(false); }
  };

  /// @brief Tuner a callback as defined in SDRplay API.
  /// @param xi Pointer to real part of sample.
  /// @param xq Pointer to imag part of sample.
  /// @param params As defined in SDRplay API.
  /// @param numSamples Number of samples in block.
  /// @param reset As defined in SDRplay API.
  /// @param cbContext As defined in SDRplay API.
  /// @return Void.
  void stream_a_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext);

  /// @brief Tuner b callback as defined in SDRplay API.
  /// @param xi Pointer to real part of sample.
  /// @param xq Pointer to imag part of sample.
  /// @param params As defined in SDRplay API.
  /// @param numSamples Number of samples in block.
  /// @param reset As defined in SDRplay API.
  /// @param cbContext As defined in SDRplay API.
  /// @return Void.
  void stream_b_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext);

  /// @brief Event callback function as defined in SDRplay API.
  /// @param eventId As defined in SDRplay API.
  /// @param tuner As defined in SDRplay API.
  /// @param params As defined in SDRplay API.
  /// @param cbContext As defined in SDRplay API.
  /// @return Void.
  void event_callback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext);

  /// @brief Start running capture callback function.
  /// @return Void.
  void initialise_device();

  /// @brief Stop running capture callback function.
  /// @return Void.
  void uninitialise_device();

  static void callback_failure(bool pair = true) noexcept;

public:
  // Opt-in paired raw CPI path; configure before starting SDK callbacks and
  // clear only after Uninit has stopped them.
  static void set_output_queue(PairedCpiQueue *queue);
  static uint64_t rejected_callback_pairs();
  static uint64_t uncertain_counter_wraps();
  static uint64_t fifo_wait_ns();
  static uint64_t fifo_wait_max_ns();
  static uint64_t fifo_lock_calls();
  /// @brief Constructor.
  /// @param fc Center frequency (Hz).
  /// @param path Path to save IQ data.
  /// @param verbose Enable verbose stats logging.
  /// @return The object.
  RspDuo(std::string type, uint32_t fc, uint32_t fs,
    std::string path, bool *saveIq, int agcSetPoint,
    int bandwidthNumber, int gainReductionA, int gainReductionB,
    int lnaState, bool dabNotch, bool rfNotch, bool verbose = false);

  /// @brief Implement capture function on RSPduo.
  /// @param buffer1 Pointer to reference buffer.
  /// @param buffer2 Pointer to surveillance buffer.
  /// @return Void.
  void process(IqData *buffer1, IqData *buffer2) override;

  /// @brief Get file name from path.
  /// @return String of file name based on current time.
  std::string set_file(std::string path);

  /// @brief Call methods to start capture.
  /// @return Void.
  void start() override;

  /// @brief Call methods to gracefully stop capture.
  /// @return Void.
  void stop() override;

  void request_stop() noexcept override;

  /// @brief Implement replay function on RSPduo.
  /// @param buffer1 Pointer to reference buffer.
  /// @param buffer2 Pointer to surveillance buffer.
  /// @param file Path to file to replay data from.
  /// @param loop True if samples should loop at EOF.
  /// @return Void.
  void replay(IqData *buffer1, IqData *buffer2, std::string file, bool loop) override;

  /// @brief Live retune fc, per-tuner gain reduction and LNA state on the
  /// open device.
  /// @details Applies sdrplay_api_Update on the already-initialised device,
  /// mirroring retina-spectrum's in-place retune pattern. Bounds are
  /// validated against the same limits as validate(). LNA state is shared
  /// across both tuners (the SDRplay device has no independent per-tuner
  /// LNA control), unlike gain reduction.
  /// @param fc Center frequency (Hz).
  /// @param gainReductionA Gain reduction for tuner A (dB).
  /// @param gainReductionB Gain reduction for tuner B (dB).
  /// @param lnaState LNA state (shared across both tuners).
  /// @param fcChanged Set true if the applied fc differs from the previous fc.
  /// @return True if the retune was applied.
  bool retune(uint32_t fc, int gainReductionA, int gainReductionB,
    int lnaState, bool &fcChanged) override;

  /// @brief Read per-tuner RF overload state from SDRplay events.
  /// @param overloadA Set to tuner A overload state.
  /// @param overloadB Set to tuner B overload state.
  /// @return True (overload reporting is supported on RspDuo).
  bool get_overload(bool &overloadA, bool &overloadB) override;

  /// @brief Read the monotonic count of per-tuner overload onsets.
  /// @param countA Set to tuner A onset count since start.
  /// @param countB Set to tuner B onset count since start.
  /// @return True (overload counting is supported on RspDuo).
  bool get_overload_counts(unsigned long &countA, unsigned long &countB) override;

  /// @brief Read per-tuner peak sample level since the last read.
  /// @param dbfsA Set to tuner A peak level (dBFS, 0 = full scale).
  /// @param dbfsB Set to tuner B peak level (dBFS, 0 = full scale).
  /// @return True (peak dBFS reporting is supported on RspDuo).
  bool get_peak_dbfs(double &dbfsA, double &dbfsB) override;

};

#endif
