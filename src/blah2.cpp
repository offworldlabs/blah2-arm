/// @file blah2.cpp
/// @brief A real-time radar.
/// @author 30hours

#include "capture/Capture.h"
#include "data/IqData.h"
#include "data/Map.h"
#include "data/Detection.h"
#include "data/meta/Timing.h"
#include "process/ambiguity/Ambiguity.h"
#include "process/clutter/WienerHopf.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/Centroid.h"
#include "process/detection/Interpolate.h"
#include "process/spectrum/SpectrumAnalyser.h"
#include "process/utility/CpiPipeline.h"
#include "process/utility/Socket.h"
#include "process/utility/TuneState.h"

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

  // run capture
  std::thread t1([&]{capture->process(buffer1, buffer2,
    tree["capture"]["device"], ip_capture, port_capture, verbose);
  });

  // setup process CPI
  uint32_t nSamples = fs * tCpi;

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
    port_timing, port_iqdata;
  std::string ip;
  tree["network"]["ports"]["map"] >> port_map;
  tree["network"]["ports"]["detection"] >> port_detection;
  tree["network"]["ports"]["timestamp"] >> port_timestamp;
  tree["network"]["ports"]["timing"] >> port_timing;
  tree["network"]["ports"]["iqdata"] >> port_iqdata;
  tree["network"]["ip"] >> ip;
  Socket socket_map(ip, port_map);
  Socket socket_detection(ip, port_detection);
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
  // FFTW bakes the thread count into the plan, so each consumer gets its share
  // of the cores by planning with its own count. Ambiguity and the spectrum
  // analyser both run in the back stage; the clutter filter is the front
  // stage's only transform and chooses its own count inside its constructor.
  fftw_plan_with_nthreads(blah2::kBackStageThreads);
  Ambiguity *ambiguity = new Ambiguity(delayMin, delayMax,
    dopplerMin, dopplerMax, fs, nSamples, roundHamming);

  // setup process clutter
  int32_t delayMinClutter, delayMaxClutter;
  tree["process"]["clutter"]["delayMin"] >> delayMinClutter;
  tree["process"]["clutter"]["delayMax"] >> delayMaxClutter;
  // No planner call here: WienerHopf sets kClutterBlockThreads for its own
  // plans and restores the default afterwards, so anything set here would be
  // overwritten before it reached a plan.
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

  // setup process spectrum analyser
  double spectrumBandwidth = 2000;
  fftw_plan_with_nthreads(blah2::kBackStageThreads);
  SpectrumAnalyser *spectrumAnalyser = new SpectrumAnalyser(nSamples, spectrumBandwidth);

  // process options
  bool isClutter, isDetection;
  tree["process"]["clutter"]["enable"] >> isClutter;
  tree["process"]["detection"]["enable"] >> isDetection;

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

  // setup output timing
  uint64_t tStart = current_time_ms();
  Timing *timing = new Timing(tStart);

  // Front stage: pull a CPI out of the capture buffers and clutter filter it.
  // This is the slow half, so it sets the throughput of the whole radar.
  std::thread t2([&]{
      while (true)
      {
        blah2::CpiSlot *slot = freeSlots.pop();

        // wait for a full CPI to land in the capture buffers
        while (true)
        {
          buffer1->lock();
          buffer2->lock();
          if ((buffer1->get_length() > nSamples) && (buffer2->get_length() > nSamples))
          {
            break;
          }
          buffer1->unlock();
          buffer2->unlock();
          // short delay to prevent tight looping
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        slot->reset();
        slot->time.push_back(current_time_us());
        // extract data from buffer
        for (uint32_t i = 0; i < nSamples; i++)
        {
          slot->x->push_back(buffer1->pop_front());
          slot->y->push_back(buffer2->pop_front());
        }
        buffer1->unlock();
        buffer2->unlock();
        timing_helper(slot->timingName, slot->timingTime, slot->time, "extract_buffer");

        // Latch a live retune against the CPI it applies to rather than
        // acting on it here, so the change lands in capture order.
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
            slot->x->clear();
            slot->y->clear();
            freeSlots.push(slot);
            continue;
          }
          timing_helper(slot->timingName, slot->timingTime, slot->time, "clutter_filter");
        }

        filteredSlots.push(slot);
      }
    });

  // Back stage: everything downstream of the clutter filter, in capture order.
  std::thread t3([&]{
      Map<std::complex<double>> *map;
      std::unique_ptr<Detection> detection;
      std::unique_ptr<Detection> detection1;
      std::unique_ptr<Detection> detection2;
      std::string mapJson, detectionJson, jsonIqData, jsonTiming;
      uint64_t previousCpiEnd = 0;

      while (true)
      {
        blah2::CpiSlot *slot = filteredSlots.pop();
        std::vector<std::string> &timing_name = slot->timingName;
        std::vector<double> &timing_time = slot->timingTime;
        std::vector<uint64_t> &time = slot->time;

        // Idle time waiting on the front stage. Near zero means this stage is
        // the bottleneck; large means the clutter filter is.
        timing_helper(timing_name, timing_time, time, "pipeline_wait");

        // live retune: adopt the new centre frequency for this CPI onwards
        if (slot->fcChanged)
        {
          fc = slot->fc;
        }

        // spectrum. Reads the reference channel, which the clutter filter only
        // reads too, so running it after the filter instead of before leaves
        // its output unchanged. It has to stay ahead of ambiguity, which drains
        // the channel it reads.
        spectrumAnalyser->process(slot->x.get());
        timing_helper(timing_name, timing_time, time, "spectrum");

        // ambiguity process
        map = ambiguity->process(slot->x.get(), slot->y.get());
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

        // Ambiguity leaves a partial batch behind; clearing gives the front
        // stage the same empty queue the serial loop's eviction produced.
        slot->x->clear();
        slot->y->clear();
        freeSlots.push(slot);
      }
    });

  t2.join();
  t3.join();
  t1.join();

  return 0;
}

void signal_callback_handler(int signum) {
  std::cout << "Caught signal " << signum << std::endl;
  if (CAPTURE_POINTER != nullptr)
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
