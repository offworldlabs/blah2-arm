#include "CfarDetector1D.h"
#include "data/Map.h"

#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>

// constructor
CfarDetector1D::CfarDetector1D(double _pfa, int8_t _nGuard, int8_t _nTrain, int8_t _minDelay, double _minDoppler)
{
  // input
  pfa = _pfa;
  nGuard = _nGuard;
  nTrain = _nTrain;
  minDelay = _minDelay;
  minDoppler = _minDoppler;
}

CfarDetector1D::~CfarDetector1D()
{
}

std::unique_ptr<Detection> CfarDetector1D::process(Map<std::complex<double>> *x)
{ 
  int32_t nDelayBins = x->get_nCols();
  int32_t nDopplerBins = x->get_nRows();

  std::vector<double> mapRowSquare;

  // The training window and the false-alarm factor depend on the delay bin
  // only, not on Doppler, so they are the same for every row. Building them
  // once per CPI replaces one heap allocation and one pow() per cell, which at
  // the shipped geometry is 123,711 of each.
  //
  // The left window uses k >= 0 like the right one. The original used k > 0,
  // silently excluding bin 0 from the left window while including it on the
  // right. That asymmetry was a bug, but correcting it moves the threshold on
  // cells near the start of the delay axis, so it does change detections.
  struct TrainWindow
  {
    int leftBegin, leftEnd, rightBegin, rightEnd;
    int nCells;
    double alpha;
  };
  std::vector<TrainWindow> window(nDelayBins);
  for (int j = 0; j < nDelayBins; j++)
  {
    TrainWindow &w = window[j];
    w.leftBegin = std::max(0, j - nGuard - nTrain);
    w.leftEnd = std::min(nDelayBins, j - nGuard);
    w.rightBegin = std::max(0, j + nGuard + 1);
    w.rightEnd = std::min(nDelayBins, j + nGuard + nTrain + 1);
    if (w.leftEnd < w.leftBegin) w.leftEnd = w.leftBegin;
    if (w.rightEnd < w.rightBegin) w.rightEnd = w.rightBegin;
    w.nCells = (w.leftEnd - w.leftBegin) + (w.rightEnd - w.rightBegin);
    // nCells == 0 gave a NaN threshold, which no cell ever exceeded; such a
    // cell is skipped below instead.
    w.alpha = w.nCells > 0 ? w.nCells * (pow(pfa, -1.0 / w.nCells) - 1) : 0.0;
  }

  // store detections temporarily
  std::vector<double> delay;
  std::vector<double> doppler;
  std::vector<double> snr;

  // loop over every cell
  for (int i = 0; i < nDopplerBins; i++)
  { 
    // skip if less than min Doppler
    if (std::abs(x->doppler[i]) < minDoppler)
    {
      continue;
    } 
    // Read the row in place; get_row() returned a copy of all nDelayBins.
    const std::vector<std::complex<double>> &mapRow = x->data[i];
    mapRowSquare.resize(nDelayBins);
    for (int j = 0; j < nDelayBins; j++)
    {
      mapRowSquare[j] = (double) std::abs(mapRow[j]*mapRow[j]);
    }
    for (int j = 0; j < nDelayBins; j++)
    {
      // skip if less than min delay
      if (x->delay[j] < minDelay)
      {
        continue;
      }
      const TrainWindow &w = window[j];
      if (w.nCells == 0)
      {
        continue;
      }

      // Sum the training cells left then right, in the original order: a
      // rolling or prefix sum rounds differently and would move the threshold.
      double trainNoise = 0.0;
      for (int k = w.leftBegin; k < w.leftEnd; k++)
      {
        trainNoise += mapRowSquare[k];
      }
      for (int k = w.rightBegin; k < w.rightEnd; k++)
      {
        trainNoise += mapRowSquare[k];
      }
      trainNoise /= w.nCells;
      double threshold = w.alpha * trainNoise;

      // detection if over threshold
      if (mapRowSquare[j] > threshold)
      {
        delay.push_back(j + x->delay[0]);
        doppler.push_back(x->doppler[i]);
        // Only a detection needs the log, so it is no longer computed for
        // every cell in the map.
        snr.push_back((double)10 * std::log10(std::abs(mapRow[j])) - x->noisePower);
      }
    }
  }

  // create detection
  return std::make_unique<Detection>(delay, doppler, snr);
}
