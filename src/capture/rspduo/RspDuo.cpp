#include "SdkSampleClock.h"
#include "UsbMode.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <time.h>
// Metadata only, while the existing callback-pair mutex is held. No IQ,
// stream flags, queue behavior, or normal callback ordering is changed.
static void owlSdkTrace(uint32_t first, uint32_t count, bool resetA, bool resetB,
                        int grChanged, int rfChanged, int fsChanged) {
  static int fd=[] { const char* p=std::getenv("OWL_SDK_META_LOG");
    return p ? ::open(p,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600) : -1; }();
  static bool have=false; static uint32_t expected=0,previousFirst=0,previousCount=0;
  static uint64_t callbacks=0,events=0;
  if(fd>=0 && (!have || first!=expected || resetA || resetB || grChanged || rfChanged || fsChanged)) {
    if(events++<64) {
      timespec ts{};clock_gettime(CLOCK_MONOTONIC,&ts);char line[768];
      int n=std::snprintf(line,sizeof(line),"{\"monotonic_ns\":%llu,\"callback\":%llu,\"have_previous\":%s,\"first\":%u,\"expected\":%u,\"previous_first\":%u,\"previous_count\":%u,\"count\":%u,\"reset_a\":%s,\"reset_b\":%s,\"gr_changed\":%d,\"rf_changed\":%d,\"fs_changed\":%d}\n",
        (unsigned long long)(uint64_t(ts.tv_sec)*1000000000+ts.tv_nsec),(unsigned long long)callbacks,
        have?"true":"false",first,expected,previousFirst,previousCount,count,
        resetA?"true":"false",resetB?"true":"false",grChanged,rfChanged,fsChanged);
      if(n>0 && n<int(sizeof(line))) { const auto wrote=::write(fd,line,size_t(n));(void)wrote; }
    }
  }
  have=true;expected=first+count;previousFirst=first;previousCount=count;++callbacks;
}
static void owlCounterTrace(uint32_t raw,uint32_t count,const SdkSampleClock::Result& result) {
  if(!result.sequence_break && !result.wrapped)return;
  static int fd=[] {const char* p=std::getenv("OWL_SDK_COUNTER_LOG");
    return p?::open(p,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600):-1;}();
  static unsigned events=0;if(fd<0 || events++>=64)return;
  timespec ts{};clock_gettime(CLOCK_MONOTONIC,&ts);char line[512];
  int n=std::snprintf(line,sizeof(line),"{\"monotonic_ns\":%llu,\"raw_first\":%u,\"logical_first\":%u,\"count\":%u,\"sequence_break\":%s,\"wrapped\":%s,\"phases_before\":%u,\"phases_after\":%u}\n",
    (unsigned long long)(uint64_t(ts.tv_sec)*1000000000+ts.tv_nsec),raw,result.first,count,
    result.sequence_break?"true":"false",result.wrapped?"true":"false",result.phases_before,result.phases_after);
  if(n>0 && n<int(sizeof(line))){const auto wrote=::write(fd,line,size_t(n));(void)wrote;}
}
#include "RspDuo.h"
#include "capture/PairedCpiQueue.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <fstream>
#include <unordered_map>
#include <iostream>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdlib>

// class static constants
const double RspDuo::MAX_FREQUENCY_NR = 2000000000;
const int RspDuo::MIN_AGC_SET_POINT_NR = -72;        // min agc set point
const int RspDuo::MIN_GAIN_REDUCTION_NR = 20;        // min gain reduction
const int RspDuo::MAX_GAIN_REDUCTION_NR = 59;        // max gain reduction
const int RspDuo::MIN_LNA_STATE_NR = 1;              // min lna state
const int RspDuo::MAX_LNA_STATE_NR = 9;              // max lna state
const int RspDuo::DEF_SAMPLE_RATE_NR = 2000000;      // default sample rate

// global variables (SDRPlay)
sdrplay_api_DeviceT *chosenDevice = NULL;
sdrplay_api_DeviceT devs[1023];
sdrplay_api_DeviceParamsT *deviceParams = NULL;
sdrplay_api_ErrT err;
sdrplay_api_CallbackFnsT cbFns;
sdrplay_api_RxChannelParamsT *chParams;
// serialises sdrplay_api_Update() calls: the SDRplay event-callback thread
// (overload acks) and the retune-poll thread both call it on the same device
std::mutex sdrplay_update_mutex;
// true once initialise_device() has run and the device accepts live updates
std::atomic<bool> device_ready_fg{false};
// per-tuner RF overload state from sdrplay_api_PowerOverloadChange events
std::atomic<bool> overload_a_fg{false};
std::atomic<bool> overload_b_fg{false};
// Monotonic count of overload *onsets* per tuner. The flags above are a
// level, and a consumer polling them can only ever sample: this device
// routinely clips and recovers within a fraction of a second, so a
// detect/correct pair that completes between two polls is invisible in the
// level while being exactly the condition worth knowing about. Measured on a
// live node: 9 detect/correct cycles inside 90s, with every level sample in
// that window reading false. Counters are incremented here, in the driver's
// own event callback, rather than derived by watching the flags change --
// anything that samples can miss an event, including the 250ms capture-status
// loop that publishes them.
std::atomic<unsigned long> overload_count_a_fg{0};
std::atomic<unsigned long> overload_count_b_fg{0};

// global variables
FILE *file_replay = NULL;
short *buffer_16_ar = NULL;
static std::vector<short> callbackStorage;
std::string file;
short max_a_nr = 0;
short max_b_nr = 0;
// per-tuner peak sample magnitude since the last get_peak_dbfs() read, used
// to compute a continuous saturation-margin readout (0 dBFS = full scale,
// always on regardless of stats_fg/verbose, unlike max_a_nr/max_b_nr above)
static const double FULL_SCALE_NR = 32767.0;
std::atomic<short> peak_hold_a_fg{0};
std::atomic<short> peak_hold_b_fg{0};
std::atomic<bool> run_fg{true};
bool stats_fg = true;
bool *capture_fg;
std::ofstream* saveIqFileLocal;
IqData *buffer1;
IqData *buffer2;


namespace {
PairedCpiQueue* pairedQueue=nullptr;
SdkSampleClock sampleClock;
bool scaledClock=false;
std::mutex callbackPairMutex;
uint32_t pendingFirstA=0,pendingCountA=0;
bool pendingResetA=false,pendingA=false;
std::atomic<uint64_t> rejectedPairs{0};
std::atomic<uint64_t> uncertainCounterWraps{0};
uint64_t fifoWaitNs=0,fifoWaitMaxNs=0,fifoLockCalls=0;
}
void RspDuo::set_output_queue(PairedCpiQueue* queue) {
  // Caller has stopped callbacks before changing or destroying the queue.
  std::lock_guard<std::mutex> lock(callbackPairMutex);
  if(pendingA) { buffer_16_ar=nullptr;pendingA=false; }
  if(queue) {
    const char* mode=std::getenv("OWL_SDK_COUNTER_SCALE");
    if(mode && std::string(mode)!="1" && std::string(mode)!="3")throw std::invalid_argument("Invalid SDK counter scale");
    scaledClock=mode && std::string(mode)=="3";
    sampleClock.configure(scaledClock?3:1);
  } else sampleClock.clear();
  pairedQueue=queue;
  if(!queue) { std::vector<short>().swap(callbackStorage); buffer_16_ar=nullptr; }
}
void RspDuo::callback_failure(bool pair) noexcept {
  // Exceptions must never cross the SDK C callback boundary.
  try {
    std::lock_guard<std::mutex> lock(callbackPairMutex);
    buffer_16_ar=nullptr;pendingA=false;
    if (pair) ++rejectedPairs;
    if(pairedQueue)pairedQueue->close();
  } catch (...) {}
  run_fg=false;
}
uint64_t RspDuo::rejected_callback_pairs() { return rejectedPairs; }
uint64_t RspDuo::uncertain_counter_wraps() { return uncertainCounterWraps; }
uint64_t RspDuo::fifo_wait_ns() { return fifoWaitNs; }
uint64_t RspDuo::fifo_wait_max_ns() { return fifoWaitMaxNs; }
uint64_t RspDuo::fifo_lock_calls() { return fifoLockCalls; }

// constructor
RspDuo::RspDuo(std::string _type, uint32_t _fc,
  uint32_t _fs, std::string _path, bool *_saveIq,
  int _agcSetPoint, int _bandwidthNumber,
  int _gainReductionA, int _gainReductionB, int _lnaState,
  bool _dabNotch, bool _rfNotch, bool _verbose)
  : Source(_type, _fc, _fs, _path, _saveIq)
{
  stats_fg = _verbose;
  std::unordered_map<int, int> decimationMap = {
    {2000000, 1},
    {1000000, 2},
    {500000, 4},
    {250000, 8},
    {125000, 16},
    {62500, 32}
  };
  std::unordered_map<int, sdrplay_api_Bw_MHzT> ifBandwidthMap = {
    {2000000, sdrplay_api_BW_1_536},
    {1000000, sdrplay_api_BW_0_600},
    {500000, sdrplay_api_BW_0_300},
    {250000, sdrplay_api_BW_0_200},
    {125000, sdrplay_api_BW_0_200},
    {62500, sdrplay_api_BW_0_200}
  };
  std::unordered_map<int, sdrplay_api_If_kHzT> ifModeMap = {
    {2000000, sdrplay_api_IF_1_620},
    {1000000, sdrplay_api_IF_1_620},
    {500000, sdrplay_api_IF_1_620},
    {250000, sdrplay_api_IF_1_620},
    {125000, sdrplay_api_IF_1_620},
    {62500, sdrplay_api_IF_1_620}
  };
  nDecimation = decimationMap[fs];
  bwType = ifBandwidthMap[fs];
  ifType = ifModeMap[fs];
  usb_bulk_fg = owlUsbBulkMode(std::getenv("OWL_SDK_USB_MODE"));
  replay_mode_fg = false;
  capture_fg = saveIq;
  saveIqFileLocal = &saveIqFile;
  agc_bandwidth_nr = _bandwidthNumber;
  agc_set_point_nr = _agcSetPoint;
  gain_reduction_nr_a = _gainReductionA;
  gain_reduction_nr_b = _gainReductionB;
  lna_state_nr = _lnaState;
  rf_notch_fg = _rfNotch;
  dab_notch_fg = _dabNotch;
}

void RspDuo::start()
{
  open_api();
  get_device();
  set_device_parameters();
  validate();
}

void RspDuo::stop()
{
  uninitialise_device();
}

void RspDuo::request_stop() noexcept
{
  run_fg.store(false);
}

bool RspDuo::get_peak_dbfs(double &dbfsA, double &dbfsB)
{
  short peakA = peak_hold_a_fg.exchange(0);
  short peakB = peak_hold_b_fg.exchange(0);
  dbfsA = 20 * std::log10(std::max(peakA, (short)1) / FULL_SCALE_NR);
  dbfsB = 20 * std::log10(std::max(peakB, (short)1) / FULL_SCALE_NR);
  return true;
}

void RspDuo::process(IqData *_buffer1, IqData *_buffer2)
{
  buffer1 = _buffer1;
  buffer2 = _buffer2;

  initialise_device();

  {
    std::lock_guard<std::mutex> lock(sdrplay_update_mutex);

    // Re-stage the per-tuner gain/LNA fields here, after Init().
    //
    // set_device_parameters() already wrote these before Init(), but they
    // do not survive it. Read the structs back immediately after Init()
    // returns and rxChannelB holds channel A's values, not the ones staged
    // for B: stage A gRdB=20 / B gRdB=59 and it reads back 20 / 20
    // (measured on owl, RSPduo, API 3.15). Without re-staging, the Update
    // below reports Success and pushes A's gain to tuner B, silently
    // running surveillance at the reference channel's gain. Confirmed the
    // other way round too - stage A=59 / B=20 and tuner B ends up at 59,
    // so B follows A regardless of what B was configured with.
    //
    // Re-writing the values is the part that matters, not retrying the
    // Update. A sleep(1) before the Update and a background retry thread
    // were both tried and both failed: nothing is wrong with the Update
    // itself, the value it would push is simply gone by the time it runs.
    //
    // This is not specific to gain. Every rxChannelB field tested reads
    // back as channel A's value after Init() (bwType, ifType, decimation,
    // notches and the AGC fields below), so anything that needs to differ
    // per tuner has to be re-staged here and pushed with an Update.
    deviceParams->rxChannelA->tunerParams.gain.gRdB = gain_reduction_nr_a;
    deviceParams->rxChannelA->tunerParams.gain.LNAstate = lna_state_nr;
    deviceParams->rxChannelB->tunerParams.gain.gRdB = gain_reduction_nr_b;
    deviceParams->rxChannelB->tunerParams.gain.LNAstate = lna_state_nr;

    // update gains after initialization
    if ((err = sdrplay_api_Update(chosenDevice->dev, sdrplay_api_Tuner_A,
                        sdrplay_api_Update_Tuner_Gr,
                        sdrplay_api_Update_Ext1_None)) != sdrplay_api_Success) {
        std::cerr << "Failed to update Tuner A gain: " << sdrplay_api_GetErrorString(err) << std::endl;
        sdrplay_api_Close();
        exit(1);
    }

    if ((err = sdrplay_api_Update(chosenDevice->dev, sdrplay_api_Tuner_B,
                      sdrplay_api_Update_Tuner_Gr,
                      sdrplay_api_Update_Ext1_None)) != sdrplay_api_Success) {
        std::cerr << "Failed to update Tuner B gain: " << sdrplay_api_GetErrorString(err) << std::endl;
        sdrplay_api_Close();
        exit(1);
    }

    // Same re-stage-then-Update pattern as the gain reduction above, and
    // for the same reason: what was staged on rxChannelB before Init() is
    // not what the struct holds afterwards. See the AGC comment in
    // set_device_parameters() for why both tuners get AGC identically.
    deviceParams->rxChannelA->ctrlParams.agc.enable = agc_enable_nr;
    deviceParams->rxChannelB->ctrlParams.agc.enable = agc_enable_nr;
    if (agc_enable_nr != sdrplay_api_AGC_DISABLE)
    {
      int agc_setpoint_dbfs_nr = (0 < agc_set_point_nr) ? 0 : agc_set_point_nr;
      deviceParams->rxChannelA->ctrlParams.agc.setPoint_dBfs = agc_setpoint_dbfs_nr;
      deviceParams->rxChannelB->ctrlParams.agc.setPoint_dBfs = agc_setpoint_dbfs_nr;
    }

    if ((err = sdrplay_api_Update(chosenDevice->dev, sdrplay_api_Tuner_A,
                      sdrplay_api_Update_Ctrl_Agc,
                      sdrplay_api_Update_Ext1_None)) != sdrplay_api_Success) {
        std::cerr << "Failed to update Tuner A AGC: " << sdrplay_api_GetErrorString(err) << std::endl;
        sdrplay_api_Close();
        exit(1);
    }

    if ((err = sdrplay_api_Update(chosenDevice->dev, sdrplay_api_Tuner_B,
                      sdrplay_api_Update_Ctrl_Agc,
                      sdrplay_api_Update_Ext1_None)) != sdrplay_api_Success) {
        std::cerr << "Failed to update Tuner B AGC: " << sdrplay_api_GetErrorString(err) << std::endl;
        sdrplay_api_Close();
        exit(1);
    }
  }

  device_ready_fg.store(true);

  // control loop
  while (run_fg)
  {
    if (stats_fg)
    {
      std::cerr << "[RspDuo]" << " max_a_nr: " << max_a_nr << 
        " max_b_nr: " << max_b_nr << std::endl;
      max_a_nr = 0;
      max_b_nr = 0;
    }
    sleep(1);
  }
  throw std::runtime_error("RSPduo callback or IQ recording failed");
}

bool RspDuo::retune(uint32_t _fc, int _gainReductionA, int _gainReductionB,
  int _lnaState, bool &fcChanged)
{
  fcChanged = false;

  if (!device_ready_fg.load())
  {
    std::cerr << "[RspDuo] Retune rejected: device not ready" << std::endl;
    return false;
  }
  if (_fc < 1 || _fc > MAX_FREQUENCY_NR)
  {
    std::cerr << "[RspDuo] Retune rejected: fc out of range" << std::endl;
    return false;
  }
  if (_gainReductionA < MIN_GAIN_REDUCTION_NR || _gainReductionA > MAX_GAIN_REDUCTION_NR ||
      _gainReductionB < MIN_GAIN_REDUCTION_NR || _gainReductionB > MAX_GAIN_REDUCTION_NR)
  {
    std::cerr << "[RspDuo] Retune rejected: gain reduction out of range" << std::endl;
    return false;
  }
  if (_lnaState < MIN_LNA_STATE_NR || _lnaState > MAX_LNA_STATE_NR)
  {
    std::cerr << "[RspDuo] Retune rejected: lna state out of range" << std::endl;
    return false;
  }

  std::lock_guard<std::mutex> lock(sdrplay_update_mutex);

  bool changed = (_fc != fc);

  if (replay_mode_fg)
  {
    // No real SDRplay device in replay mode (chosenDevice/deviceParams/
    // chParams are never populated) — simulate a successful retune against
    // local state only, so the ack/TuneState/tracker-reset chain is
    // exercisable without hardware.
    fc = _fc;
    gain_reduction_nr_a = _gainReductionA;
    gain_reduction_nr_b = _gainReductionB;
    lna_state_nr = _lnaState;
    fcChanged = changed;
    return true;
  }

  if (changed)
  {
    // rfFreq lives on channel A only; in dual-tuner mode the frequency is
    // shared, so the update targets the device's own tuner selection (Both)
    chParams->tunerParams.rfFreq.rfHz = _fc;
    if ((err = sdrplay_api_Update(chosenDevice->dev, chosenDevice->tuner,
          sdrplay_api_Update_Tuner_Frf,
          sdrplay_api_Update_Ext1_None)) != sdrplay_api_Success)
    {
      std::cerr << "[RspDuo] Retune fc failed: " <<
        sdrplay_api_GetErrorString(err) << std::endl;
      chParams->tunerParams.rfFreq.rfHz = fc;
      return false;
    }
    fc = _fc;
  }

  // gRdB and LNAstate are both applied via the same Update_Tuner_Gr reason
  // (see SDRplay API Specification 3.14) — LNA state has no independent
  // update reason of its own. It has no per-tuner control on this device,
  // so the same value is written to both channels.
  deviceParams->rxChannelA->tunerParams.gain.gRdB = _gainReductionA;
  deviceParams->rxChannelA->tunerParams.gain.LNAstate = _lnaState;
  deviceParams->rxChannelB->tunerParams.gain.gRdB = _gainReductionB;
  deviceParams->rxChannelB->tunerParams.gain.LNAstate = _lnaState;
  if ((err = sdrplay_api_Update(chosenDevice->dev, sdrplay_api_Tuner_A,
        sdrplay_api_Update_Tuner_Gr,
        sdrplay_api_Update_Ext1_None)) != sdrplay_api_Success)
  {
    std::cerr << "[RspDuo] Retune Tuner A gain failed: " <<
      sdrplay_api_GetErrorString(err) << std::endl;
    return false;
  }
  if ((err = sdrplay_api_Update(chosenDevice->dev, sdrplay_api_Tuner_B,
        sdrplay_api_Update_Tuner_Gr,
        sdrplay_api_Update_Ext1_None)) != sdrplay_api_Success)
  {
    std::cerr << "[RspDuo] Retune Tuner B gain failed: " <<
      sdrplay_api_GetErrorString(err) << std::endl;
    return false;
  }
  gain_reduction_nr_a = _gainReductionA;
  gain_reduction_nr_b = _gainReductionB;
  lna_state_nr = _lnaState;

  fcChanged = changed;
  return true;
}

bool RspDuo::get_overload(bool &overloadA, bool &overloadB)
{
  overloadA = overload_a_fg.load();
  overloadB = overload_b_fg.load();
  return true;
}

bool RspDuo::get_overload_counts(unsigned long &countA, unsigned long &countB)
{
  countA = overload_count_a_fg.load();
  countB = overload_count_b_fg.load();
  return true;
}

void RspDuo::replay(IqData *_buffer1, IqData *_buffer2, std::string _file, bool _loop)
{
  buffer1 = _buffer1;
  buffer2 = _buffer2;

  // No real device to update in replay mode — retune() simulates success
  // against local state instead of touching the (uninitialised) SDRplay
  // pointers, so the retune/ack/tracker-reset chain is testable without
  // hardware.
  replay_mode_fg = true;
  device_ready_fg.store(true);

  short i1, q1, i2, q2;
  int rv;
  file_replay = fopen(_file.c_str(), "rb");
  if (!file_replay)
  {
    device_ready_fg.store(false);
    throw std::runtime_error("Cannot open IQ replay file");
  }

  try
  {
  while (run_fg.load())
  {
    rv = fread(&i1, 1, sizeof(short), file_replay);
    if (rv != sizeof(short)) break; 
    rv = fread(&q1, 1, sizeof(short), file_replay);
    if (rv != sizeof(short)) break; 
    rv = fread(&i2, 1, sizeof(short), file_replay);
    if (rv != sizeof(short)) break; 
    rv = fread(&q2, 1, sizeof(short), file_replay);
    if (rv != sizeof(short)) break; 
    std::unique_lock<IqData> lockA(*buffer1);
    std::unique_lock<IqData> lockB(*buffer2);
    if (buffer1->get_length() < buffer1->get_n())
    {
      buffer1->push_back({(double)i1, (double)q1});
      buffer2->push_back({(double)i2, (double)q2});
    }
  }
  }
  catch (...)
  {
    fclose(file_replay);
    file_replay = nullptr;
    device_ready_fg.store(false);
    throw;
  }
  fclose(file_replay);
  file_replay = nullptr;
  device_ready_fg.store(false);
}

void RspDuo::validate() {
    // validate decimation
    if (nDecimation != 1 && nDecimation != 2 && nDecimation != 4 &&
        nDecimation != 8 && nDecimation != 16 && nDecimation != 32) {
        std::cerr << "Error: Decimation must be in {1, 2, 4, 8, 16, 32}" << std::endl;
        exit(1);
    }

    // validate fc
    if (fc < 1 || fc > MAX_FREQUENCY_NR) {
        std::cerr << "Error: Frequency must be between 1 and " << 
          MAX_FREQUENCY_NR << std::endl;
        exit(1);
    }

    // validate agc
    if (agc_bandwidth_nr != 0 && agc_bandwidth_nr != 5 && 
      agc_bandwidth_nr != 50 && agc_bandwidth_nr != 100) {
        std::cerr << "Error: AGC bandwidth must be in {0, 5, 50, 100}" << std::endl;
        exit(1);
    }
    if (agc_set_point_nr > 0 || agc_set_point_nr < MIN_AGC_SET_POINT_NR) {
        std::cerr << "Error: AGC set point must be between " << 
          MIN_AGC_SET_POINT_NR << " and 0" << std::endl;
        exit(1);
    }

    // validate LNA
    if (gain_reduction_nr_a < MIN_GAIN_REDUCTION_NR || gain_reduction_nr_a > MAX_GAIN_REDUCTION_NR) {
        std::cerr << "Error: Gain reduction must be between " << MIN_GAIN_REDUCTION_NR << " and " << MAX_GAIN_REDUCTION_NR << std::endl;
        exit(1);
    }
    if (gain_reduction_nr_b < MIN_GAIN_REDUCTION_NR || gain_reduction_nr_b > MAX_GAIN_REDUCTION_NR) {
        std::cerr << "Error: Gain reduction must be between " << MIN_GAIN_REDUCTION_NR << " and " << MAX_GAIN_REDUCTION_NR << std::endl;
        exit(1);
    }
    if (lna_state_nr < MIN_LNA_STATE_NR || lna_state_nr > MAX_LNA_STATE_NR) {
        std::cerr << "Error: LNA state must be between " << MIN_LNA_STATE_NR
          << " and " << MAX_LNA_STATE_NR << std::endl;
        exit(1);
    }

    // validate notch filters

    // print them out
    std::cerr << "[RspDuo] Print config" << std::endl;
    std::cerr << "fc (Hz)                       : " << fc << std::endl;
    std::cerr << "fs (Hz)                       : " << fs << std::endl;
    std::cerr << "file                          : " << file.c_str() << std::endl;
    std::cerr << "agc_bandwidth_nr (Hz)         : " << agc_bandwidth_nr << std::endl;
    std::cerr << "agc_set_point_nr (dBfs)       : " << agc_set_point_nr << std::endl;
    std::cerr << "gain_reduction_nr_a (dB)      : " << gain_reduction_nr_a << std::endl;
    std::cerr << "gain_reduction_nr_b (dB)      : " << gain_reduction_nr_b << std::endl;
    std::cerr << "lna_state_nr                  : " << lna_state_nr << std::endl;
    std::cerr << "N_DECIMATION                  : " << nDecimation << std::endl;
    std::cerr << "rf_notch_fg                   : " << (rf_notch_fg ? "true" : "false") << std::endl;
    std::cerr << "dab_notch_fg                  : " << (dab_notch_fg ? "true" : "false") << std::endl;
    std::cerr << "usb_bulk_fg                   : " << (usb_bulk_fg ? "true" : "false") << std::endl;
    std::cerr << "stats_fg                      : " << (stats_fg ? "true" : "false") << std::endl;
    std::cerr << "\n";
}

void RspDuo::open_api()
{
  float ver = 0.0;
  // open the sdrplay api
  if ((err = sdrplay_api_Open()) != sdrplay_api_Success)
  {
    std::cerr << "Error: API open failed " << 
      sdrplay_api_GetErrorString(err) << std::endl;
    exit(1);
  }
  // check api versions match
  if ((err = sdrplay_api_ApiVersion(&ver)) != sdrplay_api_Success)
  {
    std::cerr << "Error: Set API version failed " << 
      sdrplay_api_GetErrorString(err) << std::endl;
    sdrplay_api_Close();
    exit(1);
  }
  if (ver != SDRPLAY_API_VERSION)
  {
    std::cerr << "Error: API versions do not match, local= " << SDRPLAY_API_VERSION << "API= " << ver << std::endl;
    sdrplay_api_Close();
    exit(1);
  }
}

void RspDuo::get_device()
{
  unsigned int i;
  unsigned int ndev;
  unsigned int chosenIdx = 0;

  // lock api while device selection is performed
  if ((err = sdrplay_api_LockDeviceApi()) != sdrplay_api_Success)
  {
    std::cerr << "Error: Lock API during device selection failed " << 
      sdrplay_api_GetErrorString(err) << std::endl;
    sdrplay_api_Close();
    exit(1);
  }

  // fetch list of available devices
  if ((err = sdrplay_api_GetDevices(devs, &ndev, 
    sizeof(devs) / sizeof(sdrplay_api_DeviceT))) != sdrplay_api_Success)
  {
    std::cerr << "Error: sdrplay_api_GetDevices failed " << 
      sdrplay_api_GetErrorString(err) << std::endl;
    sdrplay_api_UnlockDeviceApi();
    sdrplay_api_Close();
    exit(1);
  }

  std::cerr << "[RspDuo] MaxDevs=" << sizeof(devs) / 
    sizeof(sdrplay_api_DeviceT) << " NumDevs=" << ndev << std::endl;

  if (ndev == 0)
  {
    std::cerr << "Error: No devices found" << std::endl;
    sdrplay_api_UnlockDeviceApi();
    sdrplay_api_Close();
    exit(1);
  }

  // pick first RSPduo
  for (i = 0; i < ndev; i++)
  {
    if (devs[i].hwVer == SDRPLAY_RSPduo_ID)
    {
      chosenIdx = i;
      break;
    }
  }

  if (i == ndev)
  {
    std::cerr << "Error: Could not find RSPduo device to open" << std::endl;
    sdrplay_api_UnlockDeviceApi();
    sdrplay_api_Close();
    exit(1);
  }

  chosenDevice = &devs[chosenIdx];
  chosenDevice->tuner = sdrplay_api_Tuner_Both;
  chosenDevice->rspDuoMode = sdrplay_api_RspDuoMode_Dual_Tuner;

  std::cerr << "[RspDuo] Device ID " << chosenIdx << std::endl;
  std::cerr << "[RspDuo] Serial Number " << devs[i].SerNo << std::endl;
  std::cerr << "[RspDuo] Hardware Version " << std::to_string(devs[i].hwVer) << std::endl;
  std::cerr << "[RspDuo] Tuner " << std::hex << chosenDevice->tuner << std::dec << std::endl;
  std::cerr << "[RspDuo] RspDuoMode " << std::hex << chosenDevice->rspDuoMode << std::dec << std::endl;

  // select chosen device
  if ((err = sdrplay_api_SelectDevice(chosenDevice)) != sdrplay_api_Success)
  {
    std::cerr << "Error: Select device failed " << 
      sdrplay_api_GetErrorString(err) << std::endl;
    sdrplay_api_UnlockDeviceApi();
    sdrplay_api_Close();
    exit(1);
  }

  // unlock api now that device is selected
  if ((err = sdrplay_api_UnlockDeviceApi()) != sdrplay_api_Success)
  {
    std::cerr << "Error: Unlock device API failed " << 
      sdrplay_api_GetErrorString(err) << std::endl;
    sdrplay_api_Close();
    exit(1);
  }

  // enable debug logging output
  if ((err = sdrplay_api_DebugEnable(chosenDevice->dev, sdrplay_api_DbgLvl_Verbose)) != sdrplay_api_Success)
  {
    std::cerr << "Error: Debug enable failed " << 
      sdrplay_api_GetErrorString(err) << std::endl;
    sdrplay_api_Close();
    exit(1);
  }

  return;
}

void RspDuo::set_device_parameters()
{
  // retrieve device parameters so they can be changed if wanted
  if ((err = sdrplay_api_GetDeviceParams(chosenDevice->dev, &deviceParams)) != sdrplay_api_Success)
  {
    std::cout << "Error: sdrplay_api_GetDeviceParams failed " + 
      std::string(sdrplay_api_GetErrorString(err)) << std::endl;
    sdrplay_api_Close();
    exit(1);
  }

  // check for NULL pointer before changing settings
  if (deviceParams == NULL)
  {
    std::cout << "Error: Device parameters pointer is null" << std::endl;
    sdrplay_api_Close();
    exit(1);
  }

  if (deviceParams->devParams == NULL)
    throw std::runtime_error("SDK device ADC parameters pointer is null");
  std::cerr << "OWL_SDK_CLOCK adc_fs=" << deviceParams->devParams->fsFreq.fsHz << " duo_fs=" << chosenDevice->rspDuoSampleFreq << std::endl;
  const SdkSampleClock::ScaledDuoConfig clockConfig{
    deviceParams->devParams->fsFreq.fsHz, chosenDevice->rspDuoSampleFreq,
    fs, static_cast<unsigned>(nDecimation), ifType==sdrplay_api_IF_1_620,
    chosenDevice->rspDuoMode==sdrplay_api_RspDuoMode_Dual_Tuner};
  if(scaledClock && !SdkSampleClock::supportsRatio3(clockConfig))
    throw std::runtime_error("Scaled SDK counter is validated only for dual-tuner 6MHz ADC / 2MSps output");
  // set USB mode
  if (usb_bulk_fg)
  {
    deviceParams->devParams->mode = sdrplay_api_BULK;
  }
  else
  {
    deviceParams->devParams->mode = sdrplay_api_ISOCH;
  }

  chParams = deviceParams->rxChannelA;

  // check for NULL pointer before changing settings
  if (chParams == NULL)
  {
    std::cerr << "Error: Channel parameters pointer is null" << std::endl;
    sdrplay_api_Close();
    exit(1);
  }

  // set center frequency
  chParams->tunerParams.rfFreq.rfHz = fc;

  // Set AGC on both tuners, identically and explicitly.
  //
  // Why AGC is a last resort at all. Detection runs off the cross-ambiguity
  // function
  //
  //   CAF(tau, fd) = integral of
  //                    ref(t) . conj(surv(t - tau)) . exp(-j.2.pi.fd.t) dt
  //
  // which assumes the relationship between the two channels is stable
  // across the whole CPI (0.5 s in this config). Fixed per-channel gains
  // are just a constant scale factor outside the integral and cost
  // nothing. AGC replaces that constant with a time-varying g(t) inside
  // the integrand, and a time-varying amplitude weighting inside a
  // Doppler-extracting integral is exactly what broadens the Doppler
  // response and leaks energy into the sidelobes. It spends coherent
  // processing gain, so it costs real detection SNR: worse Pd for a given
  // Pfa. That is also the concrete reason 5 Hz is the gentlest setting and
  // 50/100 Hz are worse - it is how many uncontrolled gain steps land
  // inside one 0.5 s coherent integration window.
  //
  // AGC running on both channels is worse than on one. Reference sees a
  // strong and relatively stable direct path; surveillance sees a weak,
  // noisier mix of leakage, echoes and noise, and chases a different
  // setpoint. The two loops therefore fluctuate independently, and two
  // uncorrelated fluctuations compound (roughly additive in variance in
  // dB) rather than averaging out.
  //
  // It is kept anyway, on both tuners, because clipping is categorically
  // worse than smearing: a saturated ADC destroys the waveform outright,
  // whereas AGC only degrades it. AGC is only ever reached as a last
  // resort, after manual gain/LNA tuning has already failed to find safe
  // values (see Auto-Calibrate's AGC fallback in retina-gui). At that
  // point surveillance is on a fixed guess already known not to work, and
  // leaving it exposed to the one unrecoverable failure mode is the worse
  // trade.
  //
  // Both channels are written explicitly rather than left to the driver.
  // rxChannelB's agc.enable defaults to sdrplay_api_AGC_50HZ with a -60
  // dBFS setpoint (see sdrplay_api_control.h), so configuring channel A
  // alone leaves surveillance running an AGC loop nobody asked for,
  // against a setpoint nobody chose. Measured live: with AGC configured
  // through channel A only, both tuners run their own converging loops,
  // A falling 41 -> 25 dB while B climbs 52 -> 59 dB.
  agc_enable_nr = sdrplay_api_AGC_DISABLE;
  if (agc_bandwidth_nr == 5)
  {
    agc_enable_nr = sdrplay_api_AGC_5HZ;
  }
  else if (agc_bandwidth_nr == 50)
  {
    agc_enable_nr = sdrplay_api_AGC_50HZ;
  }
  else if (agc_bandwidth_nr == 100)
  {
    agc_enable_nr = sdrplay_api_AGC_100HZ;
  }
  deviceParams->rxChannelA->ctrlParams.agc.enable = agc_enable_nr;
  deviceParams->rxChannelB->ctrlParams.agc.enable = agc_enable_nr;
  if (agc_enable_nr != sdrplay_api_AGC_DISABLE)
  {
    int agc_setpoint_dbfs_nr = (0 < agc_set_point_nr) ? 0 : agc_set_point_nr;
    deviceParams->rxChannelA->ctrlParams.agc.setPoint_dBfs = agc_setpoint_dbfs_nr;
    deviceParams->rxChannelB->ctrlParams.agc.setPoint_dBfs = agc_setpoint_dbfs_nr;
  }

  // set gain reduction and lna sate
  deviceParams->rxChannelA->tunerParams.gain.gRdB = gain_reduction_nr_a;
  deviceParams->rxChannelA->tunerParams.gain.LNAstate = lna_state_nr;
  deviceParams->rxChannelB->tunerParams.gain.gRdB = gain_reduction_nr_b;
  deviceParams->rxChannelB->tunerParams.gain.LNAstate = lna_state_nr;

  // set decimation and IF frequency and analog bandwidth
  chParams->ctrlParams.decimation.enable = 1;
  chParams->ctrlParams.decimation.decimationFactor = nDecimation;
  chParams->tunerParams.ifType = ifType;
  chParams->tunerParams.bwType = bwType;

  // configure notch filters
  chParams->rspDuoTunerParams.rfNotchEnable = rf_notch_fg;
  chParams->rspDuoTunerParams.rfDabNotchEnable = dab_notch_fg;

  // assign callback functions to be passed to sdrplay_api_Init()
  cbFns.StreamACbFn = _stream_a_callback;
  cbFns.StreamBCbFn = _stream_b_callback;
  cbFns.EventCbFn = _event_callback;

  return;
}

void RspDuo::stream_a_callback(short *xi, short *xq, 
sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, 
unsigned int reset, void *cbContext)
{
  std::unique_lock<std::mutex> pairLock(callbackPairMutex,std::defer_lock);
  if ((numSamples && (!xi || !xq || !params)) || (pairedQueue && !params))
    throw std::invalid_argument("Invalid tuner A callback inputs");
  if(pairedQueue) {
    pairLock.lock();
    if(!numSamples) {
      if(reset) {buffer_16_ar=nullptr;pendingA=false;sampleClock.clear();pairedQueue->discard_pending();}
      return;
    }
    if(pendingA) {  buffer_16_ar=nullptr;
      ++rejectedPairs; sampleClock.clear();pairedQueue->discard_pending(); }
    pendingFirstA=params->firstSampleNum;pendingCountA=numSamples;
    pendingResetA=reset!=0;pendingA=true;
  }

  unsigned int i = 0;
  unsigned int j = 0;

  // process stream callback data
  const size_t needed=size_t(numSamples)*4;
  if(callbackStorage.size()<needed) callbackStorage.resize(needed);
  buffer_16_ar=callbackStorage.data();

  // IIQQxxxx
  for (i = 0; i < numSamples; i++)
  {
    // add tuner A data
    buffer_16_ar[j++] = xi[i];
    buffer_16_ar[j++] = xq[i];
    // skip tuner B data
    j++;
    j++;
  }

  // find max for stats
  if (stats_fg)
  {
    for (i = 0; i < numSamples; i++)
    {
      if (xi[i] > max_a_nr)
      {
        max_a_nr = xi[i];
      }
    }
  }

  // track peak sample magnitude for the live dBFS readout (always on,
  // independent of stats_fg); ratchet the atomic hold up to this block's
  // peak so it accumulates across every callback until get_peak_dbfs() reads
  // and resets it
  {
    short blockPeak = 0;
    for (i = 0; i < numSamples; i++)
    {
      blockPeak = std::max({blockPeak, (short)std::abs(xi[i]),
        (short)std::abs(xq[i])});
    }
    short prev = peak_hold_a_fg.load();
    while (blockPeak > prev
      && !peak_hold_a_fg.compare_exchange_weak(prev, blockPeak)) {}
  }

  return;
}

void RspDuo::stream_b_callback(short *xi, short *xq,
sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, 
unsigned int reset, void *cbContext)
{
  std::unique_lock<std::mutex> pairLock(callbackPairMutex,std::defer_lock);
  if ((numSamples && (!xi || !xq || !params)) || (pairedQueue && !params))
    throw std::invalid_argument("Invalid tuner B callback inputs");
  if(pairedQueue) {
    pairLock.lock();
    if(!numSamples) {
      if(reset) {buffer_16_ar=nullptr;pendingA=false;sampleClock.clear();pairedQueue->discard_pending();}
      return;
    }
    if(!pendingA || !buffer_16_ar || pendingCountA!=numSamples ||
        pendingFirstA!=params->firstSampleNum) {
      buffer_16_ar=nullptr;pendingA=false;
      ++rejectedPairs;sampleClock.clear();pairedQueue->discard_pending();return;
    }
  }

  unsigned int i = 0;
  unsigned int j = 0;

  // xxxxIIQQ
  for (i = 0; i < numSamples; i++)
  {
    // skip tuner A data
    j++;
    j++;
    // add tuner B data
    buffer_16_ar[j++] = xi[i];
    buffer_16_ar[j++] = xq[i];
  }

  if(pairedQueue) {
    owlSdkTrace(params->firstSampleNum,numSamples,pendingResetA,reset!=0,params->grChanged,params->rfChanged,params->fsChanged);
    if (scaledClock && params->fsChanged)
      throw std::runtime_error("SDK sample rate changed after scaled counter validation");
    const auto clock=sampleClock.observe(params->firstSampleNum,numSamples,
      reset!=0 || pendingResetA || params->fsChanged || params->rfChanged);
    if (clock.wrapped && clock.phases_after > 1) ++uncertainCounterWraps;
    owlCounterTrace(params->firstSampleNum,numSamples,clock);
    pairedQueue->push(buffer_16_ar,numSamples,clock.first,clock.sequence_break);
  } else {
  // write data to IqData
#ifdef OWL_CAPTURE_PROFILE
  const auto lockBegin=std::chrono::steady_clock::now();
#endif
  buffer1->lock();
  buffer2->lock();
#ifdef OWL_CAPTURE_PROFILE
  const uint64_t waitNs=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-lockBegin).count();
  fifoWaitNs+=waitNs;fifoWaitMaxNs=std::max(fifoWaitMaxNs,waitNs);++fifoLockCalls;
#endif
  for (i = 0; i < numSamples*4; i+=4)
  {
    buffer1->push_back({(double)buffer_16_ar[i], (double)buffer_16_ar[i+1]});
    buffer2->push_back({(double)buffer_16_ar[i+2], (double)buffer_16_ar[i+3]});
  }
  buffer1->unlock();
  buffer2->unlock();

  }

  // write data to file
  if (*capture_fg)
  {
    saveIqFileLocal->write(reinterpret_cast<char*>(buffer_16_ar), 
      sizeof(short) * numSamples * 4);
    
    if (!(*saveIqFileLocal))
    {
      std::cout << "Error: stream_b_callback, not enough samples received" << std::endl;
      buffer_16_ar=nullptr;pendingA=false;
      run_fg = false;
      return;
    }
  }

  buffer_16_ar=nullptr;pendingA=false;

  // find max for stats
  if (stats_fg)
  {
    for (i = 0; i < numSamples; i++)
    {
      if (xi[i] > max_b_nr)
      {
        max_b_nr = xi[i];
      }
    }
  }

  // track peak sample magnitude for the live dBFS readout — see the
  // matching block in stream_a_callback for details
  {
    short blockPeak = 0;
    for (i = 0; i < numSamples; i++)
    {
      blockPeak = std::max({blockPeak, (short)std::abs(xi[i]),
        (short)std::abs(xq[i])});
    }
    short prev = peak_hold_b_fg.load();
    while (blockPeak > prev
      && !peak_hold_b_fg.compare_exchange_weak(prev, blockPeak)) {}
  }

  return;
}

void RspDuo::event_callback(sdrplay_api_EventT eventId, 
sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, 
void *cbContext)
{
  std::string tuner_str = (tuner == sdrplay_api_Tuner_A) ? 
    "sdrplay_api_Tuner_A" : "sdrplay_api_Tuner_B";
  switch (eventId)
  {
  case sdrplay_api_GainChange:
    std::cerr << "[RspDuo] Gain change, tuner=" << tuner_str << " ";
    std::cerr << "gRdB=" << params->gainParams.gRdB << " ";
    std::cerr << "lnaGRdB=" << params->gainParams.lnaGRdB << " ";
    std::cerr << "systemGain=" << params->gainParams.currGain << std::endl;
    break;

  case sdrplay_api_PowerOverloadChange:
  {
    bool detected = (params->powerOverloadParams.powerOverloadChangeType
      == sdrplay_api_Overload_Detected);
    std::cerr << "[RspDuo] PowerOverloadChange, tuner=" << tuner_str << " ";
    std::cerr << "powerOverloadChangeType=" <<
      (detected ? "sdrplay_api_Overload_Detected" :
      "sdrplay_api_Overload_Corrected") << std::endl;
    if (tuner == sdrplay_api_Tuner_A)
    {
      // Count onsets only: a correction is the recovery from an onset
      // already counted, so counting both would double every episode.
      if (detected) overload_count_a_fg.fetch_add(1);
      overload_a_fg.store(detected);
    }
    else
    {
      if (detected) overload_count_b_fg.fetch_add(1);
      overload_b_fg.store(detected);
    }
    // send update message to acknowledge power overload message received
    {
      std::lock_guard<std::mutex> lock(sdrplay_update_mutex);
      sdrplay_api_Update(chosenDevice->dev, tuner,
        sdrplay_api_Update_Ctrl_OverloadMsgAck, sdrplay_api_Update_Ext1_None);
    }
    break;
  }

  case sdrplay_api_DeviceRemoved:
    std::cerr << "[RspDuo] Device removed" << std::endl;
    callback_failure(false);
    break;

  default:
    std::cerr << "[RspDuo] Unknown event " << eventId << std::endl;
    break;
  }
}

void RspDuo::initialise_device()
{
  if ((err = sdrplay_api_Init(chosenDevice->dev, &cbFns, this)) != sdrplay_api_Success)
  {
    std::cerr << "Error: sdrplay_api_Init failed " << 
      sdrplay_api_GetErrorString(err) << std::endl;
    sdrplay_api_Close();
    exit(1);
  }
  // A single post-Init readback, outside CPI processing. No per-callback I/O.
  std::cerr << "OWL_SDK_USB requested=" << (usb_bulk_fg ? "bulk" : "isoch")
            << " actual=" << int(deviceParams->devParams->mode)
            << " samples_per_packet=" << deviceParams->devParams->samplesPerPkt
            << " adc_fs=" << deviceParams->devParams->fsFreq.fsHz
            << " duo_fs=" << chosenDevice->rspDuoSampleFreq << std::endl;
}

void RspDuo::uninitialise_device()
{
  device_ready_fg.store(false);
  if ((err = sdrplay_api_Uninit(chosenDevice->dev)) != sdrplay_api_Success)
  {
    std::cerr << "Error: sdrplay_api_Uninit failed " << 
      sdrplay_api_GetErrorString(err) << std::endl;
    sdrplay_api_Close();
    exit(1);
  }
  sdrplay_api_ReleaseDevice(chosenDevice);
  sdrplay_api_Close();
}
