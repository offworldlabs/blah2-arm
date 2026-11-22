# IQ cache

blah2 keeps the raw IQ of its most recent CPIs in a shared-memory file, so that
other processes on the node can read a CPI after the fact. For example,
retina-tracker can save the CPIs around an anomaly to the SD card, or
retina-telemetry can send them to the server. Configured by `process.iqCache`
(see `config/README.md`), off by default.

| Piece | Where |
| --- | --- |
| The layout, the contract between writer and readers | `src/data/IqShmLayout.h` (version 1) |
| The writer | `IqCache` (`src/data/IqCache.{h,cpp}`), called by the front stage in `src/blah2.cpp` |
| The tuning log | `TuningLog` (`src/data/TuningLog.{h,cpp}`), mirrored into the file |
| The reader | `python/blah2_iq.py`, one file, needs only numpy. Consumers copy it. |
| Tests | `test/unit/data/TestIqCache.cpp`, `test/unit/data/TestTuningLog.cpp`, and the cross-process `test/shm/run.sh` |

Status: writer and reader built and tested. Not yet wired into any consumer, and
the retina-node changes they need (below) are not made.

## Why shared memory

The nodes are short of both RAM and compute, and more than one reader is
planned. With shared memory a reader maps the file and reads frames in place:
no copy in blah2, no socket, no server threads, and no lock the radar could wait
on. However many readers there are, they cost the radar nothing. An HTTP read
path was designed first and dropped for this reason.

`/dev/shm` is a `tmpfs`: its files are RAM, never written to the SD card. These
nodes have no swap, so it stays in RAM. The file survives a blah2 crash or
restart (useful for looking at the last seconds before a crash) and is lost on
reboot.

## What is stored, and what deliberately is not

The rule: store per CPI only what exists nowhere else.

- **retina-telemetry already sends per CPI** (`DetectionFrame`): detections,
  ADS-B tags, tracks, `seq`, `boot_id` and `config_version`, all under `t`,
  which is this cache's `timestamp` divided by 1000. None of it is repeated
  here.
- **Reproducible from the samples:** the map, the detections, the peak sample,
  and whether the clutter filter failed. The processing chain is deterministic
  (see [Replay](#replay-reproduces-the-live-output)).
- **Changes rarely:** `fc`, gain and LNA state. These go in the tuning log, one
  entry per change rather than one per CPI.

That leaves a timestamp, an id and the samples per CPI, plus a few facts per
run.

## The file, layout version 1

All little-endian. Every region starts on a 4096-byte page.

| Region | Offset | Size | Holds |
| --- | --- | --- | --- |
| Header | 0 | 256 B (page 0) | Run facts and the live counters, below |
| Slot table | `slotTableOffset` | 16 B per slot | `{cpiId, timestamp}` per slot |
| Config | `configOffset` | 64 KiB | `config.yml` as blah2 loaded it (`configBytes` long) |
| Tuning log | `tuningOffset` | 64 B × 4096 | Tuning entries, below |
| Slots | `slotsOffset` | `slotStride` per slot | Samples |

### Header

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | `magic` | `char[8]` | `BLAH2IQ\0` |
| 8 | `layoutVersion` | `uint32` | 1 |
| 12 | `fs` | `uint32` | Sample rate (Hz) |
| 16 | `runId` | `uint64` | blah2's start time (POSIX ms). 0 while the file is being initialised. |
| 24 | `cpi` | `double` | Configured CPI (s) |
| 32 | `nSamples` | `uint32` | Samples per channel per CPI |
| 36 | `nSlots` | `uint32` | CPIs held |
| 40 | `tuningCapacity` | `uint32` | 4096 |
| 44 | `tuningEntryBytes` | `uint32` | 64 |
| 48-96 | `slotStride`, `slotTableOffset`, `configOffset`, `configBytes`, `tuningOffset`, `slotsOffset`, `totalBytes` | `uint64` | Geometry |
| 104 | `nWritten` | `uint64` | Frames published this run. Frame `n` is complete once `nWritten > n`. |
| 112 | `nTuning` | `uint64` | Tuning entries published this run |
| 120 | `nInexact` | `uint64` | Frames that needed rounding or clipping. Always 0 on an RSPduo. |
| 128 | `imageTag` | `char[128]` | blah2's image, from the `BLAH2_IMAGE` environment variable. Empty if unset. |

### Per CPI: slot table entry and samples

| Field | Type | Meaning |
| --- | --- | --- |
| `cpiId` | `uint64` | CPIs written before this one in the run. `UINT64_MAX` in a slot not yet written this run, so a fresh slot never looks like frame 0. |
| `timestamp` | `uint64`, POSIX ms | `time[0] / 1000`, the value this CPI's `/api/map`, `/api/detection`, `/api/timing` and telemetry frame carry. The lookup key. |
| samples | `int16[4 * nSamples]` | Interleaved `I_ref, Q_ref, I_surv, Q_surv`, the `.iq` recorder's layout. 8,000,000 B at the fleet config, in a 8,003,584 B page-aligned slot. |

Frame `cpiId` lives in slot `cpiId % nSlots`.

The samples are the ones the front stage pulled out of the capture buffers for
this CPI, before the clutter filter: exactly the input the live processing chain
ran on, with no scaling. The timestamp is taken when the front stage pulls the
CPI. The samples are the oldest in the capture buffer at that moment, so their
window spans `cpi` seconds and ends at or up to half a CPI before `timestamp`.

int16 is lossless for the RSPduo, whose samples arrive as int16 and are
converted to double exactly (`src/capture/rspduo/RspDuo.cpp`). Other devices may
deliver values int16 cannot hold. Those are rounded half away from zero and
clipped (NaN is stored as -32768), counted in `nInexact`, and one warning is
logged per run. A value is never changed silently.

### Per change: tuning entry

The fleet runs with AGC off (`bandwidthNumber: 0`), so gain and LNA state
change only at startup and on a live retune (in practice Auto-Calibrate, which
can leave the radio on retuned values for days without writing `config.yml`).
Neither the config file nor retina-telemetry's `NodeConfig` (which has no gain
fields) can therefore be trusted for a given CPI. The log can.

| Offset | Field | Type | Kinds |
| --- | --- | --- | --- |
| 0 | `timestamp` | `uint64`, POSIX ms, the frames' clock | all |
| 8 | `kind` | `uint8`: 0 Config, 1 Retune, 2 GainEvent | all |
| 9 | `tuner` | `uint8`: 1 A (reference), 2 B (surveillance) | GainEvent |
| 12 | `fc` | `uint32`, Hz | Config, Retune |
| 16 | `generation` | `int64` | Retune |
| 24 | `gainReductionA` | `int32`, dB | Config, Retune |
| 28 | `gainReductionB` | `int32`, dB | Config, Retune |
| 32 | `lnaState` | `int32`, shared by both tuners | Config, Retune |
| 36 | `gRdB` | `uint32`, dB | GainEvent |
| 40 | `lnaGRdB` | `uint32`, dB | GainEvent |
| 48 | `systemGainDb` | `double`, dB | GainEvent |

| Kind | Written by | When |
| --- | --- | --- |
| `Config` | `blah2.cpp`, once | At startup, before capture opens the SDR, from `config.yml` |
| `Retune` | `Capture.cpp` retune thread | When blah2 applies a live retune, alongside its ack |
| `GainEvent` | `RspDuo::event_callback` | When the SDRplay API reports `sdrplay_api_GainChange` for one tuner |

Entry 0 is always the `Config` entry and is never overwritten. Entry `i >= 1`
sits at position `1 + (i - 1) % 4095`. The in-process `TuningLog` keeps the
same entries; `TuningLog::attach()`, which must come after `set_config()`,
replays them into the file and then mirrors each new one.

`GainEvent` entries are appended from the SDRplay event callback. The
callback must never wait on a lock held by a thread inside an SDRplay API call
(see `RspDuo::event_callback`), so `TuningLog`'s mutex, and the file write it
covers, is only ever held for the append itself.

`GainEvent` is what makes amplitude comparable. `lnaState` is an index from 1
to 9 whose dB depends on the frequency band, while the event reports the gain
reductions in dB and `systemGainDb`, the API's overall system gain. It also
covers any gain change, including AGC if a node ever enables it. On owl-ded9
(2026-11-20) it fires at startup, twice per tuner, so the log opens with
duplicate entries. Whether it fires after a live retune is **not yet
confirmed**; the `Config` and `Retune` entries are recorded regardless.

**A frame's tuning** is the last `Config` or `Retune` entry at or before the
frame's sample window, for `fc` and the requested gains, and the last
`GainEvent` for each tuner, for the measured gains. A gain change takes effect
on the device whenever the retune lands, often partway through a CPI, so a frame
whose window contains an entry straddles the change. That is visible from the
timestamps alone.

## Reading without locks

blah2 never waits for a reader and readers never lock anything. The writer
publishes a frame by storing `nWritten` with release order after the samples and
slot entry are written. Correctness rests on one check, done **after** the
reader has finished using a frame:

> Frame `cpiId` is intact if `nWritten < cpiId + nSlots - 1`, and `runId` is
> unchanged.

The writer only ever overwrites the slot holding frame `nWritten - nSlots`, so
this keeps a whole slot, one CPI, between a reader and the writer. Any memory
reordering lasts microseconds, so the margin is generous, and it does not need
memory barriers Python cannot issue. The cost is that the two oldest frames in
the ring are never readable: a reader has about `nSlots - 2` CPIs, 23 s at
`nCpi: 48`, to use a frame after it is written.

`python/blah2_iq.py` implements this, and so does `IqCache::index()` and
`IqCache::read()` in C++, as the reference:

```python
from blah2_iq import IqCache, to_complex

cache = IqCache()                          # /dev/shm/blah2-iq, read-only
cache.refresh()                            # remap after a blah2 restart or resize
for frame in cache.index():                # readable now, oldest first
    samples = cache.read(frame.timestamp)  # int16 (nSamples, 4), or None if lapped
    with open(f"{frame.timestamp}.iq", "wb") as f:
        if cache.save(frame.timestamp, f) is None:
            ...                            # lapped mid-write: discard the file
tuning = cache.tuning()                    # entry 0 is the config tuning
```

`save()` writes straight from shared memory to the file with no copy in the
reader, then checks. Every use must be checked: a reader that keeps a view and
uses it later has no protection.

## The file's lifecycle

- **Startup, cache on:** blah2 opens the file. If it exists at the right size it
  is reused, so a restarted blah2 never doubles the memory a still-mapped reader
  holds. Any other size is deleted and recreated: shrinking a file in place
  would kill a reader that has it mapped with SIGBUS. Every page is then
  reserved with `posix_fallocate`, so a node short of RAM fails at startup with
  an error, before the SDR is opened, rather than with SIGBUS mid-run. The
  header is written with `runId = 0` and `runId` is published last.
- **Startup, cache off:** blah2 deletes the file. tmpfs would otherwise keep it,
  and its RAM, until reboot.
- **Readers** call `refresh()`, which remaps when the file's inode or `runId`
  changes. Frames from an earlier run are never returned. After a resize a
  reader's old mapping keeps the old file's memory alive until it refreshes, so
  the RAM is briefly doubled. Resizes only happen on a config change.
- **blah2 exits:** the file stays.

## Where the cache taps in, and why there

`src/blah2.cpp`, in the front stage, right after `extract_buffer` and the
retune latch, before the clutter filter. Later is too late: the clutter filter
overwrites the surveillance channel `y`, and ambiguity drains the reference
channel `x`. The write shows up as its own `iq_cache` stage in `/api/timing`
and counts towards `cpi`. The key is present only when the cache is enabled.

## Size and RAM

At the fleet config (`fs` 2 MHz, `cpi` 0.5 s), one CPI is 1,000,000 samples per
channel, 8 MB per slot. The file is reserved whole at startup and never grows.
It shows in `/proc/meminfo` under `Shmem` and reduces MemAvailable by its size,
as the earlier heap version did.

| `nCpi` | File | Readable history at 2 CPI/s |
| --- | --- | --- |
| 24 | 192 MB | 11 s |
| 48 | 384 MB (366 MiB) | 23 s |
| 60 | 480 MB | 29 s |

`/dev/shm` is capped at half of RAM, 1006 MB on a 2 GB node, which puts the
ceiling near 120 CPIs; the RAM budget is the real limit well before that.

Measured on 2026-11-17 on three 2 GB Pi 5 nodes (owl-ded9, jonathan-node-1,
jonathan-node-2) running v0.8.0 at ±1000 Hz with no swap: MemAvailable was
715-800 MB, which leaves a working budget of about 500 MB with about 250 MB
spare. `docker stats` reads 0B on these nodes because the kernel boots with
`cgroup_disable=memory`; use per-process RSS and `/proc/meminfo`.

## Measured cost of the write

owl-ded9, 2026-11-20, A-B-A at `nCpi: 48`, `fs` 2 MHz, `cpi` 0.5 s, ±1000 Hz, with
the earlier heap-backed version of the ring (the same copy into different
memory). 290 s windows after a 120 s settle:

| | v0.8.0 | cache off | **cache on** | cache off |
| --- | --- | --- | --- | --- |
| Throughput (CPI/s) | 1.9997 | 2.0002 | **2.0008** | 2.0019 |
| blah2 RSS | 190.5 MB | 190.8 MB | **556.3 MB** | 190.4 MB |
| MemAvailable, mean / min | 842 / 813 MB | 845 / 829 MB | **448 / 428 MB** | 816 / 800 MB |
| `iq_cache` stage | | | **13.0 ± 2.3 ms** | |
| Front stage (extract, cache, clutter) | 199.3 ms | 197.9 ms | **217.6 ms** | 200.9 ms |
| blah2 CPU (% of one core) | 107.7% | 107.6% | **111.4%** | 107.8% |

Real time held throughout. RSS rose by exactly the ring and stayed flat. The
disabled path costs nothing. The shared-memory version needs the same
measurement repeated, with readers running.

## Not done yet

- **retina-node:** mount `/dev/shm:/dev/shm:ro` into each reader's container
  (containers otherwise get a private 64 MB `/dev/shm`), and set
  `BLAH2_IMAGE` for blah2 so the header carries the image tag.
- **Consumers:** retina-tracker (what triggers a capture, how much to keep, the
  SD card's 24 GB), retina-telemetry.
- **CI for the Python reader:** `test/shm/run.sh` needs numpy, which the build
  image does not have, so it is run by hand for now.
- **A live retune**, to confirm `GainEvent` fires after one.

## Replay reproduces the live output

Each CPI's map and detections depend only on that CPI's samples and the config.
No processing stage carries state between CPIs, every FFTW plan uses
`FFTW_ESTIMATE`, and the thread counts are fixed in code. So a cached frame run
offline through the same chain reproduces the live output. On the same blah2
image on a Pi 5, expect a bit-for-bit match. The header carries the config and
image tag replay needs.

Conditions and limits:

- **Hardware and build.** Off the Pi 5, or on a different build, expect very
  close output. Cells near the CFAR threshold can flip. The clutter filter's
  solve runs on OpenBLAS, which defaults to one thread per core, and
  bit-identity across thread counts is unverified.
- **Not reproduced:** retina-tracker (it keeps state across CPIs), ADS-B truth
  (a live external feed), blah2-api's max-hold (a rolling 20-frame window) and
  the timing figures. retina-telemetry sends tracks and ADS-B tags per CPI, but
  drops frames by design, so not every cached CPI has a telemetry frame.
- **The `.iq` recorder and `capture.replay` cannot stand in for this cache.**
  Replay discards samples whenever the capture buffer is full, so its output
  depends on processing speed. The recorder also stores the continuous stream
  with no CPI boundaries or timestamps.

## Measuring the cost

blah2 runs at about 100% duty and is limited by memory bandwidth. At 100% duty
the `duty_cycle` figure is capped, so it cannot show slack being used up.
Measure the `iq_cache` stage and the stages either side of it, throughput by
nCpi advance over a fixed window, blah2 RSS and MemAvailable. Run A-B-A on
owl-ded9 (no illuminator, so a safe test target), with the image swapped by hand
(never by publishing a tag) and only the `blah2` container recreated.
