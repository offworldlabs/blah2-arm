#include "Capture.h"
#include "rspduo/RspDuo.h"
#include "process/utility/TuneState.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <cstdio>
#include <atomic>
#include <httplib.h>

// constants
const std::string Capture::VALID_TYPE[1] = {"RspDuo"};

// How many times one retune generation is attempted before being given up
// on. A pending retune is re-attempted on every poll until it succeeds, so
// some cap is needed; a few attempts still absorb a transient failure
// (device momentarily busy) without holding the radio hostage to a request
// it will never accept.
static const int MAX_RETUNE_ATTEMPTS = 3;

// constructor
Capture::Capture(std::string _type, uint32_t _fs, uint32_t _fc, std::string _path)
{
  type = _type;
  fs = _fs;
  fc = _fc;
  path = _path;
  replay = false;
  saveIq = false;
}

void Capture::process(IqData *buffer1, IqData *buffer2, c4::yml::NodeRef config,
  std::string ip_capture, uint16_t port_capture, bool verbose)
{
  std::cout << "Setting up device " + type << std::endl;

  device = factory_source(type, config, verbose);
  if (!device) throw std::runtime_error("Capture source unavailable");
  activeDevice.store(device.get());
  if (stopRequested.load())
  {
    activeDevice.store(nullptr);
    return;
  }
  std::atomic<bool> running{true};

  // capture status thread
  std::thread t1, tRetune, tPeakStatus;
  try
  {
  t1 = std::thread([&]{
    while (running.load())
    {
      httplib::Client cli("http://" + ip_capture + ":" 
        + std::to_string(port_capture));
      cli.set_connection_timeout(1, 0);
      cli.set_read_timeout(1, 0);
      cli.set_write_timeout(1, 0);
      httplib::Result res = cli.Get("/capture");

      // if capture status changed
      if (res && res->status == 200 && (res->body == "true") != saveIq)
      {
        saveIq = res->body == "true";
        if (saveIq)
        {
          device->open_file();
        }
        else
        {
          device->close_file();
        }
      }
      sleep(1);
    }
  });

  // retune poll thread: applies live fc/gain changes requested via the API
  // and reports per-tuner RF overload state back to it
  tRetune = std::thread([&]{
    bool lastOverloadA = false;
    bool lastOverloadB = false;
    unsigned long lastOverloadCountA = 0;
    unsigned long lastOverloadCountB = 0;
    bool overloadReported = false;
    auto lastRfStatusPost = std::chrono::steady_clock::now();

    while (running.load())
    {
      httplib::Client cli("http://" + ip_capture + ":"
        + std::to_string(port_capture));
      cli.set_connection_timeout(1, 0);
      cli.set_read_timeout(1, 0);
      cli.set_write_timeout(1, 0);

      // check for a pending retune request
      httplib::Result res = cli.Get("/capture/retune");
      if (res && res->status == 200)
      {
        long generation = 0;
        unsigned int newFc = 0;
        int newGainA = 0, newGainB = 0, newLnaState = 0;
        if (sscanf(res->body.c_str(), "%ld,%u,%d,%d,%d",
              &generation, &newFc, &newGainA, &newGainB, &newLnaState) == 5
            && generation > 0 && generation > lastAppliedRetuneGeneration)
        {
          if (generation != attemptedRetuneGeneration)
          {
            attemptedRetuneGeneration = generation;
            retuneAttempts = 0;
          }
          ++retuneAttempts;

          bool fcChanged = false;
          if (device->retune(newFc, newGainA, newGainB, newLnaState, fcChanged))
          {
            lastAppliedRetuneGeneration = generation;
            retuneAttempts = 0;
            fc = newFc;
            if (fcChanged)
            {
              g_tuneState.currentFc.store(newFc);
              g_tuneState.fcChanged.store(true);
            }
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count();
            std::string ack = std::to_string(generation) + ","
              + std::to_string(newFc) + ","
              + std::to_string(newGainA) + ","
              + std::to_string(newGainB) + ","
              + std::to_string(newLnaState) + ","
              + std::to_string(now);
            cli.Post("/capture/retune/ack", ack, "text/plain");
          }
          else
          {
            std::cerr << "[Capture] Retune generation " << generation
              << " rejected (attempt " << retuneAttempts << " of "
              << MAX_RETUNE_ATTEMPTS << ")" << std::endl;

            if (retuneAttempts >= MAX_RETUNE_ATTEMPTS)
            {
              // Stop here rather than re-attempting on every poll forever.
              // A retune the device will not take is usually one it cannot
              // take - most often the requested tuning saturates the front
              // end, which makes the SDRplay API stop answering control
              // calls, so retrying the same values cannot recover it and
              // only holds the device down. Marking it applied locally
              // retires it for this process; the POST retires it in the API
              // too, without which a restarted blah2 would start its own
              // attempt count from zero and pick the same request up again.
              // No ack is sent: the requester still correctly sees this
              // generation as never applied.
              lastAppliedRetuneGeneration = generation;
              std::cerr << "[Capture] Abandoning retune generation "
                << generation << " after " << retuneAttempts
                << " failed attempts" << std::endl;
              cli.Post("/capture/retune/reject",
                std::to_string(generation) + "," + std::to_string(retuneAttempts),
                "text/plain");
            }
          }
        }
      }

      // report overload state on change, with a periodic heartbeat so the
      // consumer can detect staleness
      bool overloadA = false, overloadB = false;
      if (device->get_overload(overloadA, overloadB))
      {
        // Onset counts ride along with the level. The level alone is not
        // enough for a consumer deciding whether an operating point clips:
        // this loop only samples every 250ms and the device recovers faster
        // than that, so a whole overload episode can begin and end with the
        // level reading false at both ends. The counts are incremented in
        // the driver's event callback, so they capture every episode
        // regardless of when anyone looks.
        unsigned long countA = 0, countB = 0;
        device->get_overload_counts(countA, countB);

        auto sinceLastPost = std::chrono::steady_clock::now() - lastRfStatusPost;
        if (!overloadReported || overloadA != lastOverloadA
            || overloadB != lastOverloadB
            // A changed count with an unchanged level is precisely the
            // missed-episode case, and must still be published promptly.
            || countA != lastOverloadCountA || countB != lastOverloadCountB
            || sinceLastPost > std::chrono::seconds(2))
        {
          auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
          std::string body = std::string(overloadA ? "1" : "0") + ","
            + (overloadB ? "1" : "0") + "," + std::to_string(now) + ","
            + std::to_string(countA) + "," + std::to_string(countB);
          if (cli.Post("/capture/overload-status", body, "text/plain"))
          {
            lastOverloadA = overloadA;
            lastOverloadB = overloadB;
            lastOverloadCountA = countA;
            lastOverloadCountB = countB;
            overloadReported = true;
            lastRfStatusPost = std::chrono::steady_clock::now();
          }
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  });

  // peak dBFS reporting thread: independent of the capture-status thread
  // above (one thread per concern) — reports live per-tuner signal level
  // to blah2_api every second, no on-change/heartbeat logic needed since
  // it's a continuous value, not a rare state transition
  tPeakStatus = std::thread([&]{
    while (running.load())
    {
      httplib::Client cli("http://" + ip_capture + ":"
        + std::to_string(port_capture));
      cli.set_connection_timeout(1, 0);
      cli.set_read_timeout(1, 0);
      cli.set_write_timeout(1, 0);

      double dbfsA = 0, dbfsB = 0;
      if (device->get_peak_dbfs(dbfsA, dbfsB))
      {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
        std::string body = std::to_string(dbfsA) + ","
          + std::to_string(dbfsB) + "," + std::to_string(now);
        cli.Post("/capture/rf-status", body, "text/plain");
      }

      sleep(1);
    }
  });
  }
  catch (...)
  {
    running.store(false);
    if (t1.joinable()) t1.join();
    if (tRetune.joinable()) tRetune.join();
    if (tPeakStatus.joinable()) tPeakStatus.join();
    activeDevice.store(nullptr);
    throw;
  }

  auto stopStatusThreads = [&] {
    running.store(false);
    if (t1.joinable()) t1.join();
    if (tRetune.joinable()) tRetune.join();
    if (tPeakStatus.joinable()) tPeakStatus.join();
  };
  bool deviceStarted = false;
  try
  {
    if (!replay)
    {
      device->start();
      deviceStarted = true;
      device->process(buffer1, buffer2);
      stopStatusThreads();
      device->stop(); // Stop callbacks before the paired queue can be destroyed.
      deviceStarted = false;
    }
    else
    {
      device->replay(buffer1, buffer2, file, loop);
    }
  }
  catch (...)
  {
    stopStatusThreads();
    if (deviceStarted) device->stop();
    activeDevice.store(nullptr);
    throw;
  }
  stopStatusThreads();
  activeDevice.store(nullptr);
}

void Capture::request_stop() noexcept
{
  stopRequested.store(true);
  Source *source = activeDevice.load();
  if (source) source->request_stop();
}

std::unique_ptr<Source> Capture::factory_source(const std::string& type, c4::yml::NodeRef config, bool verbose)
{
    // SDRplay RSPduo
    if (type == VALID_TYPE[0])
    {
        int agcSetPoint = 0, bandwidthNumber = 0, gainReductionA = 0, gainReductionB = 0, lnaState = 0;
        bool dabNotch = false, rfNotch = false;
        config["agcSetPoint"] >> agcSetPoint;
        config["bandwidthNumber"] >> bandwidthNumber;
        config["gainReduction"][0] >> gainReductionA;
        config["gainReduction"][1] >> gainReductionB;
        config["lnaState"] >> lnaState;
        config["dabNotch"] >> dabNotch;
        config["rfNotch"] >> rfNotch;
        return std::make_unique<RspDuo>(type, fc, fs, path, &saveIq,
          agcSetPoint, bandwidthNumber, gainReductionA, gainReductionB, lnaState,
          dabNotch, rfNotch, verbose);
    }

    // handle unknown type
    std::cerr << "Error: Source type does not exist." << std::endl;
    return nullptr;
}

void Capture::set_replay(bool _loop, std::string _file)
{
  replay = true;
  loop = _loop;
  file = _file;
}
