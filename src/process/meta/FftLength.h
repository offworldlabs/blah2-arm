#pragma once

#include <cstdint>
#include <stdexcept>

namespace blah2 {

// The two processing stages run concurrently, so their plans have to share the
// four cores rather than each claiming all of them. The ambiguity batch
// transforms are the ones that pay for threads.
//
// Note the clutter filter no longer uses this: it plans at
// kClutterBlockThreads below. That leaves the front stage count governing the
// spectrum analyser alone, so there is room to retune it now that the stage's
// largest consumer has stopped competing for the same cores.
inline constexpr int kFrontStageThreads = 2;
inline constexpr int kBackStageThreads = 2;

// The clutter filter plans its own transforms at this instead.
//
// Its blocks are 2048 points, 32 KB, and a second thread costs more in sync
// than it recovers on a transform that small. The single-transform code it
// replaces went the other way and wanted the threads (477.5 ms on one against
// 339.9 on two), so the right comparison is each at its own best.
//
// Confirmed in situ on a live node rather than in isolation, at both of the
// two fastest block lengths:
//
//   block   1 thread   2 threads
//    2048     172.6      226.8
//    4096     184.6      195.1
//
// It also gives a core back: the front stage no longer needs two for the
// filter, which is the stage the pipeline is bottlenecked on.
inline constexpr int kClutterBlockThreads = 1;

// The block length the clutter correlations and convolution are computed at.
//
// Both halves of the filter used to run as single transforms over the whole
// CPI: four of 1,000,000 points for the correlations and three of 1,016,064
// for the convolution, the latter to apply a filter of only nBins taps. Those
// arrays are 16 MB, the Pi 5's L3 is 2 MB, so every pass went to DRAM, and
// blah2 is memory-bandwidth-bound
// (measured: it takes ~75% of the bus, and an idle board gives an external
// probe 11 GB/s against 2.8 with blah2 running).
//
// Computing the same quantities block by block keeps each transform inside L2.
// 2048 points is 32 KB against a 512 KB L2.
//
// Swept in situ on a live node, on the real binary under the real pipeline,
// reading the stage time blah2 reports for itself. That matters because the
// optimum moves with contention (8192 on an idle board) and because a
// benchmark running *alongside* blah2 contends for the bus differently from
// blah2 contending with its own second stage:
//
//   block   clutter_filter (ms, 1 thread)
//    1024       195.2
//    2048       172.6   <- minimum
//    4096       184.6
//    8192       186.1
//   16384       209.5
//
// against 491.7 ms for the single-transform code it replaces.
inline constexpr uint32_t kClutterBlockLength = 2048;

// Block length for a correlation or convolution over `nSamples` points with
// `nBins` taps.
//
// Overlap-save needs the block longer than the tap count, and there is nothing
// to gain from a block longer than the signal plus its own overlap. Both
// bounds only bite on the small geometries the tests use; at the shipped
// geometry the measured constant applies unchanged.
inline uint32_t clutterBlockLength(uint32_t nSamples, uint32_t nBins)
{
  if (!nSamples || !nBins)
    throw std::invalid_argument("Clutter block needs a non-empty CPI and tap count");

  const uint64_t floorLength = uint64_t(nBins) * 2;
  uint64_t length = kClutterBlockLength;
  if (length < floorLength)
  {
    length = 1;
    while (length < floorLength) length <<= 1;
  }

  const uint64_t cap = uint64_t(nSamples) + nBins;
  if (length > cap) length = cap;

  return static_cast<uint32_t>(length);
}

}
