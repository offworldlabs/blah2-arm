# IQ cache

A RAM ring holding the raw IQ of the most recent CPIs, so that other processes
can fetch a CPI after the fact. For example, retina-tracker can write the CPIs
around an anomaly to the SD card, or retina-telemetry can send them to the
server. Configured by `process.iqCache` (see `config/README.md`), off by
default.

Two classes:

| Class | Holds | Granularity |
| --- | --- | --- |
| `IqCache` (`src/data/IqCache.{h,cpp}`) | The samples of the last `nCpi` CPIs | Per CPI |
| `TuningLog` (`src/data/TuningLog.{h,cpp}`) | Every change to `fc`, gain and LNA state since blah2 started | Per change |

Status: the write path is built. The read path (an HTTP server inside blah2,
bound to loopback) is not built yet; see [Read path](#read-path-not-built).

## What is stored, and what deliberately is not

The rule: store per CPI only what exists nowhere else.

- **retina-telemetry already sends per CPI** (`DetectionFrame`): detections,
  ADS-B tags, tracks, `seq`, `boot_id` and `config_version`, all under `t`,
  which is this cache's `timestamp` divided by 1000. None of it is repeated
  here.
- **Reproducible from the samples:** the map, the detections, the peak sample,
  and whether the clutter filter failed. The processing chain is deterministic
  (see [Replay](#replay-reproduces-the-live-output)).
- **Changes rarely:** `fc`, gain and LNA state. These go in `TuningLog`, one
  entry per change rather than one per CPI.

That leaves only a timestamp, an id and the samples per CPI.

### Per CPI: `IqCache` frame

| Field | Type | Size | Meaning |
| --- | --- | --- | --- |
| `cpiId` | `uint64` | 8 B | CPIs written before this one since blah2 started. A reader that sees it jump has missed frames that were overwritten. |
| `timestamp` | `uint64`, POSIX ms | 8 B | `time[0] / 1000`, the value this CPI's `/api/map`, `/api/detection`, `/api/timing` and telemetry frame carry. The lookup key. |
| samples | `int16[4 * nSamples]` | 8,000,000 B at `fs` 2 MHz, `cpi` 0.5 s | Interleaved `I_ref, Q_ref, I_surv, Q_surv`, the `.iq` recorder's layout, little-endian |

The samples are the ones the front stage pulled out of the capture buffers for
this CPI, before the clutter filter. They are exactly the input the live
processing chain ran on.

The timestamp is taken when the front stage pulls the CPI. The samples are the
oldest in the capture buffer at that moment, so their window ends at or up to
half a CPI before `timestamp`, and spans `cpi` seconds.

int16 is lossless for the RSPduo, whose samples arrive as int16 and are
converted to double exactly (`src/capture/rspduo/RspDuo.cpp`). Other devices may
deliver values int16 cannot hold. Those are rounded half away from zero and
clipped, counted in `IqCache::get_n_inexact()`, and one warning is logged per
run. A value is never changed silently.

### Per change: `TuningLog` entry

The fleet runs with AGC off (`bandwidthNumber: 0`), so gain and LNA state
change only at startup and on a live retune (in practice Auto-Calibrate, which
can leave the radio on retuned values for days without writing `config.yml`).
Neither the config file nor retina-telemetry's `NodeConfig` (which has no gain
fields) can therefore be trusted for a given CPI. The log can.

Every entry has a `timestamp` (POSIX ms, the same `system_clock` as the frames)
and a `kind`. Each kind carries its own fields; the rest are zero.

| Kind | Written by | When | Fields |
| --- | --- | --- | --- |
| `Config` | `blah2.cpp`, once | At startup, before capture opens the SDR | `fc`, `gainReductionA`, `gainReductionB`, `lnaState`, as in `config.yml` |
| `Retune` | `Capture.cpp` retune thread | When blah2 applies a live retune, at the same time as it acks it | `generation`, `fc`, `gainReductionA`, `gainReductionB`, `lnaState`, as requested and applied |
| `GainEvent` | `RspDuo::event_callback` | When the SDRplay API reports `sdrplay_api_GainChange` for one tuner | `tuner` (1 = A reference, 2 = B surveillance), `gRdB`, `lnaGRdB`, `systemGainDb` |

`lnaState` is one value shared by both tuners: `RspDuo` applies the same LNA
state to A and B.

`GainEvent` is what makes amplitude comparable. `lnaState` is an index from 1
to 9 whose dB depends on the frequency band, while the event reports the gain
reductions in dB and `systemGainDb`, the API's overall system gain. It also
covers any gain change, including AGC if a node ever enables it. On owl-ded9
(2026-11-20) it fires at startup, twice per tuner, so the log opens with
duplicate entries. Whether it fires after a live retune is **not yet
confirmed**; the `Config` and `Retune` entries are recorded regardless, so the
requested values are known either way.

**A frame's tuning** is the last `Config` or `Retune` entry at or before the
frame's sample window, for `fc` and the requested gains, and the last
`GainEvent` for each tuner, for the measured gains. A gain change takes effect
on the device whenever the retune lands, often partway through a CPI, so a
frame whose window contains an entry straddles the change. That is visible from
the timestamps alone, and no per-frame field could say more.

The log keeps 4096 entries. The `Config` entry is never evicted; when full, the
oldest change after it goes and `get_n_dropped()` counts it. Auto-Calibrate
produces tens of entries per run, so only a node running AGC could reach the
bound.

### Per run: in the planned `/iq/index`

Not built yet. Stated once rather than per frame:

- `fs`, `cpi`, samples per CPI, slot count, and `get_n_inexact()`
- the config file blah2 loaded, which replay needs (`NodeConfig` covers only
  part of `process.*`), and the blah2 image tag
- the `TuningLog` entries

## Where the cache taps in, and why there

`src/blah2.cpp`, in the front stage, right after `extract_buffer` and the
retune latch, before the clutter filter. Later is too late: the clutter filter
overwrites the surveillance channel `y`, and ambiguity drains the reference
channel `x`.

The write shows up as its own `iq_cache` stage in `/api/timing` and counts
towards `cpi`. The key is present only when the cache is enabled. That is fixed
at startup, so the key never comes and goes within a run.

## Size and RAM

At the fleet config (`fs` 2 MHz, `cpi` 0.5 s), one CPI is 1,000,000 samples per
channel, which is **8 MB per slot**. The whole ring is allocated and committed
at startup, so blah2's RSS rises by `nCpi * 8 MB` at once and then stays flat.
A node that cannot afford it fails at startup, before the SDR is opened, rather
than at some later CPI. The tuning log is a few hundred kB at most.

Measured on 2026-11-17 on three 2 GB Pi 5 nodes (owl-ded9, jonathan-node-1,
jonathan-node-2) running v0.8.0 at ±1000 Hz with no swap:

- MemAvailable was 715-800 MB.
- That leaves a working budget of about 500 MB, with about 250 MB spare. Most of
  "available" is page cache, and with no swap an overshoot means the OOM killer.
- `docker stats` reads 0B on these nodes because the kernel boots with
  `cgroup_disable=memory`. Use per-process RSS instead.

| `nCpi` | RAM | History at 2 CPI/s |
| --- | --- | --- |
| 24 | 192 MB | 12 s |
| 48 | 384 MB | 24 s |
| 60 | 480 MB | 30 s |

48 is the suggested starting point until a long-term memory minimum (collectd)
confirms the budget. 2 GB is the fleet floor; fairforest-b has 4 GB, but the
setting has to fit the 2 GB boards.

## Concurrency

`IqCache`: one writer (the front stage) and any number of readers.

- A write first takes its target slot out of the index under the lock. It then
  converts the samples into the slot without the lock, and finally publishes the
  slot's metadata under the lock.
- A read finds a frame by timestamp and copies the whole frame out under the
  lock.

A reader therefore never sees a half-written frame. The writer never waits
longer than one in-progress copy, which is a few ms for 8 MB. Readers never hold
the lock while sending to a client, so a slow client cannot stall the radar.
`test/unit/data/TestIqCache.cpp` races a reader against the writer and fails on
any torn frame.

`TuningLog`: appended from the retune thread and from the SDRplay event
callback, read by anyone. The SDRplay callback must never wait on a lock held
across an SDRplay API call (see `RspDuo::event_callback`); the log's mutex is
only ever held for a push or a copy, never across an API call, so that cannot
happen. `test/unit/data/TestTuningLog.cpp` races two writers and a reader.

## Replay reproduces the live output

Each CPI's map and detections depend only on that CPI's samples and the config.
No processing stage carries state between CPIs, every FFTW plan uses
`FFTW_ESTIMATE`, and the thread counts are fixed in code. So a cached frame run
offline through the same chain reproduces the live output. On the same blah2
image on a Pi 5, expect a bit-for-bit match.

Conditions and limits:

- **Hardware and build.** Off the Pi 5, or on a different build, expect very
  close output. Cells near the CFAR threshold can flip. The clutter filter's
  solve runs on OpenBLAS, which defaults to one thread per core, and
  bit-identity across thread counts is unverified.
- **Store the config and image tag with any frames kept long term.** The output
  depends on both.
- **Not reproduced:** retina-tracker (it keeps state across CPIs), ADS-B truth
  (a live external feed), blah2-api's max-hold (a rolling 20-frame window) and
  the timing figures. retina-telemetry sends tracks and ADS-B tags per CPI, but
  drops frames by design, so not every cached CPI has a telemetry frame.
- **The `.iq` recorder and `capture.replay` cannot stand in for this cache.**
  Replay discards samples whenever the capture buffer is full, so its output
  depends on processing speed. The recorder also stores the continuous stream
  with no CPI boundaries or timestamps. Below 100% duty, the live CPIs are not
  back-to-back slices of that stream.

## Read path (not built)

Planned: a small HTTP server inside blah2, bound to `127.0.0.1` only. blah2,
retina-tracker and retina-telemetry all run with host networking, and
cpp-httplib is already linked into blah2. It is not routed through blah2-api,
which is already the largest process on a node and would add a copy of each
8 MB frame.

- `GET /iq/index`: the per-run data above, and per held frame its `cpiId` and
  `timestamp`.
- `GET /iq/frame?timestamp=<ms>`: the frame as binary (a small header, then the
  int16 samples), or 404 once overwritten.

Concurrent reads will be capped (2 planned), so their copies cost a known
`2 * 8 MB`. There is no pinning: a reader must fetch a frame within
`nCpi * cpi` seconds of its CPI being processed.

## Measuring the cost

blah2 runs at about 100% duty and is limited by memory bandwidth. The write
reads 32 MB of doubles and writes 8 MB of int16 per CPI. At 100% duty the
`duty_cycle` figure is capped, so it cannot show slack being used up. Measure:

- the `iq_cache` stage time, and the `extract_buffer` and `clutter_filter` times
  either side of it (memory contention can slow neighbouring stages);
- `pipeline_wait`, the back stage's idle time waiting on the front stage. If it
  grows, the front stage has become the bottleneck;
- throughput by nCpi advance over a fixed window, not by polling;
- blah2 RSS (`VmRSS` and `VmHWM`).

Run it A-B-A on owl-ded9 (no illuminator, so a safe test target), with the cache
off, on, then off, with the image swapped by hand (never by publishing a tag).
On the same run, check the blah2 log for `[RspDuo] Gain change` lines at
startup and after a retune, to confirm the `GainEvent` source.
