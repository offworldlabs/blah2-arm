/// @file blah2.cpp
/// @brief A real-time radar.
/// @author 30hours

#include "capture/Capture.h"
#include "capture/PairedCpiQueue.h"
#include "capture/rspduo/RspDuo.h"
#include "data/IqData.h"
#include "data/Map.h"
#include "data/Detection.h"
#include "data/meta/Timing.h"
#include "data/Track.h"
#include "process/ambiguity/Ambiguity.h"
#include "process/clutter/WienerHopf.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/Centroid.h"
#include "process/detection/Interpolate.h"
#include "process/spectrum/SpectrumAnalyser.h"
#include "process/tracker/Tracker.h"
#include "process/utility/CpiPipeline.h"
#include "process/utility/Socket.h"
#include "process/utility/TuneState.h"
#include "process/meta/FftLength.h"
#include "data/meta/Constants.h"

#include <ryml/ryml.hpp>
#include <ryml/ryml_std.hpp> // optional header, provided for std:: interop
#include <c4/format.hpp> // needed for the examples below
#include <sys/types.h>
#include <getopt.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <sys/time.h>
#include <signal.h>
#include <atomic>
#include <memory>
#include <iostream>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <stdexcept>

Capture *CAPTURE_POINTER = NULL;
TuneState g_tuneState;

void signal_callback_handler(int signum);
void getopt_print_help();
std::string getopt_process(int argc, char **argv);
std::string ryml_get_file(const char *filename);
uint64_t current_time_ms();
uint64_t current_time_us();
void timing_helper(std::vector<std::string>& timing_name, 
  std::vector<double>& timing_time, std::vector<uint64_t>& time_us, 
  std::string name);

int main(int argc, char **argv)
{
  // input handling
  signal(SIGTERM, signal_callback_handler);
  std::string file = getopt_process(argc, argv);
  std::ifstream filePath(file);
  if (!filePath.is_open())
  {
    std::cout << "Error: Config file does not exist." << std::endl;
    exit(1);
  }

  // config handling
  std::string contents = ryml_get_file(file.c_str());
  ryml::Tree tree = ryml::parse_in_arena(ryml::to_csubstr(contents));

  // setup capture
  uint32_t fs, fc;
  uint16_t port_capture;
  std::string type, path, replayFile, ip_capture;
  bool saveIq, state, loop;
  tree["capture"]["fs"] >> fs;
  tree["capture"]["fc"] >> fc;
  tree["capture"]["device"]["type"] >> type;
  tree["save"]["iq"] >> saveIq;
  tree["save"]["path"] >> path;
  tree["capture"]["replay"]["state"] >> state;
  tree["capture"]["replay"]["loop"] >> loop;
  tree["capture"]["replay"]["file"] >> replayFile;
  tree["network"]["ip"] >> ip_capture;
  tree["network"]["ports"]["api"] >> port_capture;
  Capture *capture = new Capture(type, fs, fc, path);
  CAPTURE_POINTER = capture;
  if (state)
  {
    capture->set_replay(loop, replayFile);
  }

  // Check BLAH2_VERBOSE env var for logging control
  bool verbose = false;
  const char* verboseEnv = std::getenv("BLAH2_VERBOSE");
  if (verboseEnv != nullptr && std::string(verboseEnv) == "true")
  {
    verbose = true;
  }

  // create shared queue
  double tCpi, tBuffer;
  tree["process"]["data"]["cpi"] >> tCpi;
  tree["process"]["data"]["buffer"] >> tBuffer;
  IqData *buffer1 = new IqData((int) (tCpi*tBuffer*fs));
  IqData *buffer2 = new IqData((int) (tCpi*tBuffer*fs));

  uint32_t nSamples = fs * tCpi;
  const char *queueEnv = std::getenv("BLAH2_CAPTURE_BLOCK_QUEUE");
  const bool usePairedQueue = !state && type == "RspDuo" && queueEnv
    && std::string(queueEnv) == "1";
  std::unique_ptr<PairedCpiQueue> captureQueue;
  std::unique_ptr<PairedCpiConsumer> captureConsumer;
  if (usePairedQueue)
  {
    if (!nSamples || !std::isfinite(tBuffer) || tBuffer < 1 || tBuffer > 64)
      throw std::invalid_argument("Invalid paired capture buffer geometry");
    const char* twoCpiOption = std::getenv("OWL_QUEUE_TWO_CPI");
    if (twoCpiOption && std::string(twoCpiOption) != "0" && std::string(twoCpiOption) != "1")
      throw std::invalid_argument("OWL_QUEUE_TWO_CPI must be 0 or 1");
    const bool twoCpi = twoCpiOption && std::string(twoCpiOption) == "1";
    const uint64_t capacity = uint64_t(tCpi * (twoCpi ? 2.0 : tBuffer) * fs);
    std::cout << "OWL_QUEUE_CAPACITY_SAMPLES=" << capacity << std::endl;
    captureQueue = std::make_unique<PairedCpiQueue>(nSamples,
      std::max<uint64_t>(1, capacity / nSamples), capacity);
    const char *directEnv = std::getenv("OWL_DIRECT_IQ");
    captureConsumer = std::make_unique<PairedCpiConsumer>(nSamples,
      !directEnv || std::string(directEnv) != "0");
    RspDuo::set_output_queue(captureQueue.get());
  }

  const char *prepareEnv = std::getenv("OWL_PREPARE_BEFORE_CAPTURE");
  const bool prepareBeforeCapture = usePairedQueue && prepareEnv
    && std::string(prepareEnv) == "1";
  std::atomic<bool> captureFailed{false};
  std::atomic<bool> captureDone{false};
  std::thread t1;
  auto startCapture = [&] {
    t1 = std::thread([&] {
      try { capture->process(buffer1, buffer2,
        tree["capture"]["device"], ip_capture, port_capture, verbose); }
      catch (const std::exception &e) {
        std::cerr << "Capture failed: " << e.what() << std::endl;
        captureFailed.store(true);
      }
      catch (...) {
        std::cerr << "Capture failed with an unknown exception" << std::endl;
        captureFailed.store(true);
      }
      captureDone.store(true);
      if (captureQueue) captureQueue->close();
    });
  };

  // setup process CPI

  // Slots in flight between the two processing stages. Two is enough for full
  // overlap: the front stage fills one while the back stage drains the other,
  // and a stage that runs ahead blocks on the free list.
  constexpr size_t kPipelineDepth = 2;
  std::vector<std::unique_ptr<blah2::CpiSlot>> slots;
  blah2::BlockingQueue<blah2::CpiSlot *> freeSlots;
  blah2::BlockingQueue<blah2::CpiSlot *> filteredSlots;
  for (size_t i = 0; i < kPipelineDepth; i++)
  {
    slots.push_back(std::make_unique<blah2::CpiSlot>(nSamples));
    freeSlots.push(slots.back().get());
  }

  // setup fftw multithread
  if (fftw_init_threads() == 0)
  {
    std::cout << "Error in FFTW multithreading." << std::endl;
    return -1;
  }

  // setup socket
  sleep(5);
  uint16_t port_map, port_detection, port_timestamp, 
    port_timing, port_iqdata, port_track;
  std::string ip;
  tree["network"]["ports"]["map"] >> port_map;
  tree["network"]["ports"]["detection"] >> port_detection;
  tree["network"]["ports"]["track"] >> port_track;
  tree["network"]["ports"]["timestamp"] >> port_timestamp;
  tree["network"]["ports"]["timing"] >> port_timing;
  tree["network"]["ports"]["iqdata"] >> port_iqdata;
  tree["network"]["ip"] >> ip;
  Socket socket_map(ip, port_map);
  Socket socket_detection(ip, port_detection);
  Socket socket_track(ip, port_track);
  Socket socket_timestamp(ip, port_timestamp);
  Socket socket_timing(ip, port_timing);
  Socket socket_iqdata(ip, port_iqdata);

  // setup process ambiguity
  int32_t delayMin, delayMax;
  int32_t dopplerMin, dopplerMax;
  bool roundHamming = true;
  tree["process"]["ambiguity"]["delayMin"] >> delayMin;
  tree["process"]["ambiguity"]["delayMax"] >> delayMax;
  tree["process"]["ambiguity"]["dopplerMin"] >> dopplerMin;
  tree["process"]["ambiguity"]["dopplerMax"] >> dopplerMax;
  // FFTW bakes the thread count into the plan, so the two stages get their own
  // share of the cores by planning with different counts. Ambiguity and the
  // spectrum analyser run in the back stage, the clutter filter in the front.
  fftw_plan_with_nthreads(blah2::kBackStageThreads);
  Ambiguity *ambiguity = new Ambiguity(delayMin, delayMax,
    dopplerMin, dopplerMax, fs, nSamples, roundHamming);

  // setup process clutter
  int32_t delayMinClutter, delayMaxClutter;
  tree["process"]["clutter"]["delayMin"] >> delayMinClutter;
  tree["process"]["clutter"]["delayMax"] >> delayMaxClutter;
  fftw_plan_with_nthreads(blah2::kFrontStageThreads);
  WienerHopf *filter = new WienerHopf(delayMinClutter, delayMaxClutter, nSamples);

  // setup process detection
  double pfa, minDoppler;
  int8_t nGuard, nTrain;
  int8_t minDelay;
  tree["process"]["detection"]["pfa"] >> pfa;
  tree["process"]["detection"]["nGuard"] >> nGuard;
  tree["process"]["detection"]["nTrain"] >> nTrain;
  tree["process"]["detection"]["minDelay"] >> minDelay;
  tree["process"]["detection"]["minDoppler"] >> minDoppler;
  CfarDetector1D *cfarDetector1D = new CfarDetector1D(pfa, nGuard, nTrain, minDelay, minDoppler);
  Interpolate *interpolate = new Interpolate(true, true);

  // setup process centroid
  uint16_t nCentroid;
  tree["process"]["detection"]["nCentroid"] >> nCentroid;
  Centroid *centroid = new Centroid(nCentroid, nCentroid, 1/tCpi);

  // setup process tracker
  uint8_t m, n, nDelete;
  double maxAcc, rangeRes, lambda;
  std::string smooth;
  tree["process"]["tracker"]["initiate"]["M"] >> m;
  tree["process"]["tracker"]["initiate"]["N"] >> n;
  tree["process"]["tracker"]["delete"] >> nDelete;
  tree["process"]["tracker"]["initiate"]["maxAcc"] >> maxAcc;
  rangeRes = (double)Constants::c/fs;
  lambda = (double)Constants::c/fc;
  Tracker *tracker = new Tracker(m, n, nDelete, ambiguity->get_cpi(), maxAcc, rangeRes, lambda);

  // setup process spectrum analyser
  double spectrumBandwidth = 2000;
  fftw_plan_with_nthreads(blah2::kBackStageThreads);
  SpectrumAnalyser *spectrumAnalyser = new SpectrumAnalyser(nSamples, spectrumBandwidth, fc, fs);

  // process options
  bool isClutter, isDetection, isTracker;
  tree["process"]["clutter"]["enable"] >> isClutter;
  tree["process"]["detection"]["enable"] >> isDetection;
  tree["process"]["tracker"]["enable"] >> isTracker;
  if (!isDetection)
  {
    isTracker = false;
  }

  // setup output data
  bool saveMap;
  tree["save"]["map"] >> saveMap;
  std::string savePath, saveMapPath;
  if (saveIq || saveMap)
  {
    char startTimeStr[16];
    struct timeval currentTime = {0, 0};
    gettimeofday(&currentTime, NULL);
    strftime(startTimeStr, 16, "%Y%m%d-%H%M%S", localtime(&currentTime.tv_sec));
    savePath = path + startTimeStr;
  }
  if (saveMap)
  {
    saveMapPath = savePath + ".map";
  }

  // Opt-in DSP preparation happens before hardware capture. Synthetic CPIs
  // never enter the pipeline, sockets, capture recording, or live timing.
  if (prepareBeforeCapture)
  {
    const auto begin = std::chrono::steady_clock::now();
    std::vector<int16_t> synthetic(size_t(nSamples) * 4);
    uint32_t seed = 0x6a09e667U;
    auto sample = [&] {
      seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
      return int16_t((seed >> 18) - 8192);
    };
    size_t preparedBytes = 0;
    for (int cpi = 0; cpi < 4; ++cpi)
    {
      auto& warm = *slots[size_t(cpi) % slots.size()];
      warm.reset();
      for (uint32_t i = 0; i < nSamples; ++i)
      {
        const int16_t re = sample(), im = sample();
        synthetic[4 * i] = re; synthetic[4 * i + 1] = im;
        synthetic[4 * i + 2] = int16_t(re / 4 + sample() / 16);
        synthetic[4 * i + 3] = int16_t(im / 4 + sample() / 16);
      }
      warm.x->assign_paired_i16(synthetic.data(), nSamples, *warm.y);
      if (isClutter && !filter->process(warm.x.get(), warm.y.get()))
        throw std::runtime_error("Pre-capture clutter preparation failed");
      spectrumAnalyser->process(warm.x.get());
      auto *warmMap = ambiguity->get_doppler_middle() == 0
        ? ambiguity->process_owned(warm.x.get(), warm.y.get())
        : ambiguity->process(warm.x.get(), warm.y.get());
      warmMap->set_metrics();
      if (isDetection)
      {
        auto first = cfarDetector1D->process(warmMap);
        auto second = centroid->process(first.get());
        auto output = interpolate->process(second.get(), warmMap);
        preparedBytes += output->to_json(0).size();
        if (isTracker) {
          auto warmTrack = tracker->process(output.get(), uint64_t(tCpi * 1000 * cpi));
          preparedBytes += warmTrack->to_json(0).size();
        }
      }
      // Exercise serialization without publishing or saving synthetic data.
      preparedBytes += warmMap->to_json_km(0, fs).size();
      preparedBytes += warm.x->to_json(0).size();
    }
    tracker->reset();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - begin).count();
    std::cout << "OWL_STARTUP_PREPARATION ms=" << elapsed
      << " synthetic_cpis=4 published=0 capture_started=0 prepared_bytes="
      << preparedBytes << std::endl;
  }

  // The first published CPI's uptime starts at live capture, excluding all
  // synthetic preparation. The counter remains zero until that live frame.
  Timing *timing = new Timing(current_time_ms());
  blah2::PipelineStop pipelineStop(
    [&] { capture->request_stop(); },
    [&] { if (captureQueue) captureQueue->cancel(); },
    [&] { freeSlots.close(); },
    [&] { filteredSlots.close(); });
  startCapture();

  // Front stage: pull a CPI out of the capture buffers and clutter filter it.
  // This is the slow half, so it sets the throughput of the whole radar.
  std::thread t2, t3;
  try
  {
  t2 = std::thread([&]{
    pipelineStop.run([&] {
      bool havePrevious = false;
      uint32_t expectedSample = 0;
      uint64_t previousEpoch = 0;
      bool pendingDiscontinuity = false;
      while (!pipelineStop.stopping())
      {
        blah2::CpiSlot *slot = freeSlots.pop();
        if (!slot || pipelineStop.stopping()) break;

        if (captureQueue)
        {
          size_t rawSlot;
          if (!captureQueue->acquire(rawSlot))
          {
            break;
          }
          try
          {
            slot->reset();
            slot->time.push_back(current_time_us());
            const auto &block = captureQueue->block(rawSlot);
            slot->firstSample = block.first;
            slot->captureEpoch = block.epoch;
            slot->pairedCapture = true;
            pendingDiscontinuity = pendingDiscontinuity ||
              (havePrevious && (block.first != expectedSample || block.epoch != previousEpoch));
            slot->captureDiscontinuity = pendingDiscontinuity;
            havePrevious = true;
            expectedSample = block.first + nSamples;
            previousEpoch = block.epoch;
            // Replace the slot in place, reusing its allocated deque blocks.
            // No second owned IQ workspace is needed.
            captureConsumer->copy(block, *slot->x, *slot->y);
          }
          catch (...)
          {
            captureQueue->release(rawSlot);
            throw;
          }
          captureQueue->release(rawSlot);
        }
        else
        {

        // Keep the legacy >CPI threshold while streaming. At finite replay
        // EOF, an exact final CPI can still be consumed before shutdown.
        bool extracted = false;
        while (!pipelineStop.stopping())
        {
          {
            std::unique_lock<IqData> lockA(*buffer1);
            std::unique_lock<IqData> lockB(*buffer2);
            const bool done = captureDone.load();
            const bool ready = blah2::fifo_frame_ready(buffer1->get_length(),
              buffer2->get_length(), nSamples, done);
            if (ready)
            {
              slot->reset();
              slot->time.push_back(current_time_us());
              for (uint32_t i = 0; i < nSamples; ++i)
              {
                slot->x->push_back(buffer1->pop_front());
                slot->y->push_back(buffer2->pop_front());
              }
              extracted = true;
            }
          }
          if (extracted || captureDone.load())
          {
            break;
          }
          // short delay to prevent tight looping
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!extracted) break;
        }
        timing_helper(slot->timingName, slot->timingTime, slot->time, "extract_buffer");

        // Latch a live retune against the CPI it applies to rather than acting
        // on it here: the tracker it resets runs in the back stage.
        slot->fcChanged = g_tuneState.fcChanged.exchange(false);
        if (slot->fcChanged)
        {
          slot->fc = g_tuneState.currentFc.load();
        }

        // clutter filter
        if (isClutter)
        {
          if (!filter->process(slot->x.get(), slot->y.get()))
          {
            // Drop the CPI as the serial loop did, but hand the retune back so
            // a failed filter cannot swallow it.
            if (slot->fcChanged)
            {
              g_tuneState.fcChanged.store(true);
            }
            if (slot->pairedCapture) pendingDiscontinuity = true;
            slot->x->clear();
            slot->y->clear();
            freeSlots.push(slot);
            continue;
          }
          timing_helper(slot->timingName, slot->timingTime, slot->time, "clutter_filter");
        }

        if (slot->pairedCapture) pendingDiscontinuity = false;
        filteredSlots.push(slot);
      }
      filteredSlots.close();
    });
    });

  // Back stage: everything downstream of the clutter filter, in capture order.
  // The tracker's state stays here, so it still sees every CPI exactly once and
  // in sequence.
  t3 = std::thread([&]{
    pipelineStop.run([&] {
      Map<std::complex<double>> *map;
      std::unique_ptr<Detection> detection;
      std::unique_ptr<Detection> detection1;
      std::unique_ptr<Detection> detection2;
      std::unique_ptr<Track> track;
      std::string mapJson, detectionJson, jsonTracker, jsonIqData, jsonTiming;
      uint64_t previousCpiEnd = 0;

      while (!pipelineStop.stopping())
      {
        blah2::CpiSlot *slot = filteredSlots.pop();
        if (!slot || pipelineStop.stopping()) break;
        std::vector<std::string> &timing_name = slot->timingName;
        std::vector<double> &timing_time = slot->timingTime;
        std::vector<uint64_t> &time = slot->time;

        // Idle time waiting on the front stage. Near zero means this stage is
        // the bottleneck; large means the clutter filter is.
        timing_helper(timing_name, timing_time, time, "pipeline_wait");

        // live retune: refresh wavelength and drop stale tracks on fc change
        if (slot->fcChanged)
        {
          fc = slot->fc;
          lambda = (double)Constants::c/fc;
          spectrumAnalyser->set_center_frequency(fc);
          tracker->set_lambda(lambda);
          tracker->reset();
        }
        else if (slot->captureDiscontinuity)
        {
          // Gaps and resets invalidate tracks even without a frequency change.
          tracker->reset();
        }

        // spectrum. Reads the reference channel, which the clutter filter only
        // reads too, so running it after the filter instead of before leaves
        // its output unchanged. It has to stay ahead of ambiguity, which drains
        // the channel it reads.
        spectrumAnalyser->process(slot->x.get());
        timing_helper(timing_name, timing_time, time, "spectrum");

        // ambiguity process
        map = slot->pairedCapture && ambiguity->get_doppler_middle() == 0
          ? ambiguity->process_owned(slot->x.get(), slot->y.get())
          : ambiguity->process(slot->x.get(), slot->y.get());
        map->set_metrics();
        timing_helper(timing_name, timing_time, time, "ambiguity_processing");

        // detection process
        if (isDetection)
        {
          detection1 = cfarDetector1D->process(map);
          detection2 = centroid->process(detection1.get());
          detection = interpolate->process(detection2.get(), map);
          timing_helper(timing_name, timing_time, time, "detector");
        }

        // tracker process
        if (isTracker)
        {
          track = tracker->process(detection.get(), time[0]/1000);
          timing_helper(timing_name, timing_time, time, "tracker");
        }

        // output IqData meta data
        jsonIqData = slot->x->to_json(time[0]/1000);
        socket_iqdata.sendData(jsonIqData);

        // output map data
        mapJson = map->to_json_km(time[0]/1000, fs);
        if (saveMap)
        {
          map->save(mapJson, saveMapPath);
        }
        socket_map.sendData(mapJson);

        // output detection data
        if (isDetection)
        {
          detectionJson = detection->to_json_km(time[0]/1000, fs);
          socket_detection.sendData(detectionJson);
        }

        // output tracker data
        if (isTracker)
        {
          jsonTracker = track->to_json(time[0]/1000);
          socket_track.sendData(jsonTracker);
        }

        // output radar data timer
        timing_helper(timing_name, timing_time, time, "output_radar_data");

        // cpi timer. Work done on this CPI across both stages, excluding the
        // queue wait, so it stays the same quantity the serial loop reported.
        double delta_ms = 0;
        for (size_t i = 0; i < timing_time.size(); i++)
        {
          if (timing_name[i] != "pipeline_wait")
          {
            delta_ms += timing_time[i];
          }
        }
        timing_name.push_back("cpi");
        timing_time.push_back(delta_ms);

        // Fraction of the incoming stream this radar actually processes.
        //
        // With the stages overlapped "cpi" is latency, not throughput, so it
        // rises even as the radar gets faster and cannot be read as a health
        // number. Wall clock between CPIs can, and expressed against the CPI
        // duration it says the thing an operator wants to know: 100% means
        // keeping up with the receiver, 50% means half the signal is going
        // unlooked at.
        //
        // Always emitted, including 0 for the first CPI where there is no
        // interval yet. A key that comes and goes leaves stale frozen traces
        // in the timing stash, which is what the previous cpi_interval did.
        uint64_t cpiEnd = current_time_us();
        double dutyCycle = 0;
        if (previousCpiEnd != 0)
        {
          const double interval_ms = (double)(cpiEnd - previousCpiEnd) / 1000;
          if (interval_ms > 0)
          {
            dutyCycle = (tCpi * 1000.0) / interval_ms * 100.0;
            // A radar cannot process more of the stream than arrives, so
            // anything above 100 is jitter in a single inter-CPI gap, not
            // information. Clamped so the figure reads as the fraction of the
            // signal actually examined.
            //
            // The cost of clamping: a geometry where the configured CPI does
            // not match what capture delivers would read a steady 100% rather
            // than an implausible 105%. That is a config fault, not a
            // throughput one, and it is not what this number is for.
            if (dutyCycle > 100.0) dutyCycle = 100.0;
          }
        }
        previousCpiEnd = cpiEnd;
        timing_name.push_back("duty_cycle");
        timing_time.push_back(dutyCycle);

        if (slot->pairedCapture)
        {
          const auto stats = captureQueue->stats();
          timing_name.push_back("capture_sample_first");
          timing_time.push_back(slot->firstSample);
          timing_name.push_back("capture_epoch");
          timing_time.push_back(slot->captureEpoch);
          timing_name.push_back("capture_frame_gap");
          timing_time.push_back(slot->captureDiscontinuity ? 1 : 0);
          timing_name.push_back("capture_published_cpis");
          timing_time.push_back(stats.published);
          timing_name.push_back("capture_taken_cpis");
          timing_time.push_back(stats.taken);
          timing_name.push_back("capture_dropped_cpis");
          timing_time.push_back(stats.dropped_cpis);
          timing_name.push_back("capture_discarded_samples");
          timing_time.push_back(stats.discarded_samples);
          timing_name.push_back("capture_discontinuities");
          timing_time.push_back(stats.discontinuities);
          timing_name.push_back("capture_pair_rejects");
          timing_time.push_back(RspDuo::rejected_callback_pairs());
          timing_name.push_back("capture_uncertain_counter_wraps");
          timing_time.push_back(RspDuo::uncertain_counter_wraps());
          timing_name.push_back("capture_queue_high_water");
          timing_time.push_back(stats.high_water);
        }

        // pipeline_wait is a stage boundary rather than a cost: the next
        // stage's delta is measured from its timestamp, so it has to be taken,
        // but idle time is not work and reporting it alongside the stages
        // invited the wrong reading.
        std::vector<std::string> reportName;
        std::vector<double> reportTime;
        for (size_t i = 0; i < timing_name.size(); i++)
        {
          if (timing_name[i] == "pipeline_wait") continue;
          reportName.push_back(timing_name[i]);
          reportTime.push_back(timing_time[i]);
        }

        if (verbose)
        {
          std::cout << "CPI time (ms): " << delta_ms << std::endl;
        }

        // output timing data
        timing->update(time[0]/1000, reportTime, reportName);
        jsonTiming = timing->to_json();
        socket_timing.sendData(jsonTiming);

        // output CPI timestamp for updating data
        std::string t0_string = std::to_string(time[0]/1000);
        socket_timestamp.sendData(t0_string);

        // Paired capture replaces the complete owned CPI in place. Keep its
        // deque blocks allocated, including the bounded ambiguity tail.
        // FIFO extraction still appends and therefore needs an empty slot.
        if (!slot->pairedCapture) {
          slot->x->clear();
          slot->y->clear();
        }
        freeSlots.push(slot);
      }
    });
    });
  }
  catch (...)
  {
    pipelineStop.fail(std::current_exception());
  }

  if (t2.joinable()) t2.join();
  if (t3.joinable()) t3.join();
  if (t1.joinable()) t1.join();
  if (captureQueue) RspDuo::set_output_queue(nullptr);

  if (auto error = pipelineStop.error())
  {
    try { std::rethrow_exception(error); }
    catch (const std::exception &e) { std::cerr << "Processing failed: " << e.what() << std::endl; }
    catch (...) { std::cerr << "Processing failed with an unknown exception" << std::endl; }
    return 1;
  }
  return captureFailed.load() ? 1 : 0;
}

void signal_callback_handler(int signum) {
  std::cout << "Caught signal " << signum << std::endl;
  if (CAPTURE_POINTER != nullptr && CAPTURE_POINTER->device)
  {
    CAPTURE_POINTER->device->kill();
  }
  else
  {
    exit(0);
  }
}

void getopt_print_help()
{
  std::cout << "--config <file.yml>: 	Set number of program\n"
               "--help:              	Show help\n";
  exit(1);
}

std::string getopt_process(int argc, char **argv)
{
  const char *const short_opts = "c:h";
  const option long_opts[] = {
      {"config", required_argument, nullptr, 'c'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, no_argument, nullptr, 0}};

  if (argc == 1)
  {
    std::cout << "Error: No arguments provided." << std::endl;
    exit(1);
  }

  std::string file;

  while (true)
  {
    const auto opt = getopt_long(argc, argv, short_opts, long_opts, nullptr);

    // handle input "-", ":", etc
    if ((argc == 2) && (-1 == opt))
    {
      std::cout << "Error: No arguments provided." << std::endl;
      exit(1);
    }

    if (-1 == opt)
      break;

    switch (opt)
    {
    case 'c':
      file = std::string(optarg);
      break;

    case 'h':
      getopt_print_help();

    // unrecognised option
    case '?':
      exit(1);

    default:
      break;
    }
  }

  return file;
}

std::string ryml_get_file(const char *filename)
{
  std::ifstream in(filename, std::ios::in | std::ios::binary);
  if (!in)
  {
    std::cerr << "could not open " << filename << std::endl;
    exit(1);
  }
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

uint64_t current_time_ms()
{
  // current time in POSIX ms
  return std::chrono::duration_cast<std::chrono::milliseconds>
  (std::chrono::system_clock::now().time_since_epoch()).count();
}

uint64_t current_time_us()
{
  // current time in POSIX us
  return std::chrono::duration_cast<std::chrono::microseconds>
  (std::chrono::system_clock::now().time_since_epoch()).count();
}

void timing_helper(std::vector<std::string>& timing_name, 
  std::vector<double>& timing_time, std::vector<uint64_t>& time_us, 
  std::string name)
{
  time_us.push_back(current_time_us());
  double delta_ms = (double)(time_us.back()-time_us[time_us.size()-2]) / 1000;
  timing_name.push_back(name);
  timing_time.push_back(delta_ms);
}
