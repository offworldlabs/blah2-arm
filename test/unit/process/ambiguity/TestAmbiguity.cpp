/// @file TestAmbiguity.cpp
/// @brief Unit test for Ambiguity.cpp
/// @author 30hours
/// @author Dan G
/// @todo Add golden data IqData file for testing.
/// @todo Declaration match to coding style?

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "process/ambiguity/Ambiguity.h"

#include <random>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>
#include <stdexcept>

/// @brief Use random_device as RNG.
std::random_device g_rd;

/// @brief Generate random IQ data.
/// @param iqData Address of IqData object.
/// @details Have to use out ref parameter because there's no copy/move ctors.
/// @return Void.
void random_iq(IqData& iq_data) {
    std::mt19937 gen(g_rd());
    std::uniform_real_distribution<> dist(-100.0, 100.0);

    for (uint32_t i = 0; i < iq_data.get_n(); ++i) {
        iq_data.push_back({dist(gen), dist(gen)});
    }
}

/// @brief Read file to IqData buffer.
/// @param buffer1 IqData buffer reference.
/// @param buffer2 IqData buffer surveillance.
/// @param file String of file name.
/// @return Void.
void read_file(IqData& buffer1, IqData& buffer2, const std::string& file)
{
  short i1, q1, i2, q2;
  auto file_replay = fopen(file.c_str(), "rb");
  if (!file_replay) {
    return;
  }

  auto read_short = [](short& v, FILE* fid) {
    auto rv{fread(&v, 1, sizeof(short), fid)};
    return rv == sizeof(short);
  };

  while (!feof(file_replay))
  {
    if (!read_short(i1, file_replay)) break;
    if (!read_short(q1, file_replay)) break;
    if (!read_short(i2, file_replay)) break;
    if (!read_short(q2, file_replay)) break;

    buffer1.push_back({(double)i1, (double)q1});
    buffer2.push_back({(double)i2, (double)q2});

    // only read for the buffer length - this class is very poorly designed
    if (buffer1.get_length() == buffer1.get_n()) {
        break;
    }
  }

  fclose(file_replay);
}

/// @brief Test constructor.
/// @details Check constructor parameters created correctly.
TEST_CASE("Constructor", "[constructor]")
{
    int32_t delayMin{-10};
    int32_t delayMax{300};
    int32_t dopplerMin{-300};
    int32_t dopplerMax{300};

    uint32_t fs{2'000'000};
    float tCpi{0.5};
    uint32_t nSamples = tCpi * fs;    // narrow on purpose

    Ambiguity ambiguity(delayMin, delayMax, dopplerMin, 
      dopplerMax, fs, nSamples);

    CHECK_THAT(ambiguity.get_cpi(), Catch::Matchers::WithinAbs(tCpi, 0.02));
    CHECK(ambiguity.get_doppler_middle() == 0);
    CHECK(ambiguity.get_n_corr() == 3322);
    CHECK(ambiguity.get_n_delay_bins() == delayMax + std::abs(delayMin) + 1);
    CHECK(ambiguity.get_n_doppler_bins() == 301);
    // nCorr reference samples against nCorr + 310 surveillance
    CHECK(ambiguity.get_nfft() == 3632);
}

/// @brief Test constructor with rounded Hamming number FFT length.
TEST_CASE("Constructor_Round", "[constructor]")
{
    int32_t delayMin{-10};
    int32_t delayMax{300};
    int32_t dopplerMin{-300};
    int32_t dopplerMax{300};

    uint32_t fs{2'000'000};
    float tCpi{0.5};
    uint32_t nSamples = tCpi * fs;    // narrow on purpose

    Ambiguity ambiguity(delayMin, delayMax, dopplerMin, 
      dopplerMax, fs, nSamples, true);

    CHECK_THAT(ambiguity.get_cpi(), Catch::Matchers::WithinAbs(tCpi, 0.02));
    CHECK(ambiguity.get_doppler_middle() == 0);
    CHECK(ambiguity.get_n_corr() == 3322);
    CHECK(ambiguity.get_n_delay_bins() == delayMax + std::abs(delayMin) + 1);
    CHECK(ambiguity.get_n_doppler_bins() == 301);
    // 3632 rounded up to 3645 = 3^6 * 5
    CHECK(ambiguity.get_nfft() == 3645);
}

/// @brief Test constructor at the shipped geometry, +/-1000 Hz.
/// @details The span every node runs (retina-node's default.yml, mirrored in
/// config/config.yml for standalone use). 2 Hz bins give 1001 Doppler bins and
/// nCorr 999. Each batch reads 999 + 410 = 1409 surveillance samples, which
/// rounds up to 1440 = 2^5 * 3^2 * 5.
TEST_CASE("Constructor_Round_Shipped", "[constructor]")
{
    int32_t delayMin{-10};
    int32_t delayMax{400};
    int32_t dopplerMin{-1000};
    int32_t dopplerMax{1000};

    uint32_t fs{2'000'000};
    float tCpi{0.5};
    uint32_t nSamples = tCpi * fs;    // narrow on purpose

    Ambiguity ambiguity(delayMin, delayMax, dopplerMin,
      dopplerMax, fs, nSamples, true);

    CHECK_THAT(ambiguity.get_cpi(), Catch::Matchers::WithinAbs(tCpi, 0.02));
    CHECK(ambiguity.get_doppler_middle() == 0);
    CHECK(ambiguity.get_n_corr() == 999);
    CHECK(ambiguity.get_n_delay_bins() == 411);
    CHECK(ambiguity.get_n_doppler_bins() == 1001);
    CHECK(ambiguity.get_nfft() == 1440);
}

/// @brief Test simple ambiguity processing.
TEST_CASE("Process_Simple", "[process]")
{
    auto round_hamming = GENERATE(true, false);

    int32_t delayMin{-10};
    int32_t delayMax{300};
    int32_t dopplerMin{-300};
    int32_t dopplerMax{300};

    uint32_t fs{2'000'000};
    float tCpi{0.5};
    uint32_t nSamples = tCpi * fs;    // narrow on purpose

    Ambiguity ambiguity(delayMin, delayMax, dopplerMin, 
      dopplerMax, fs, nSamples, round_hamming);

    IqData x{nSamples};
    IqData y{nSamples};

    random_iq(x);
    random_iq(y);
    auto map{ambiguity.process(&x, &y)};
    map->set_metrics();
    CHECK(map->maxPower > 0.0);
    CHECK(map->noisePower > 0.0);
}

/// @brief Test processing from a file.
TEST_CASE("Process_File", "[process]")
{
    std::filesystem::path test_input_file("20231214-230611.rspduo");
    // Bail if the test file doesn't exist
    if (!std::filesystem::exists(test_input_file)) {
      SKIP("Input test file does not exist.");
    }
    
    auto round_hamming = GENERATE(true, false);

    int32_t delayMin{-10};
    int32_t delayMax{300};
    int32_t dopplerMin{-300};
    int32_t dopplerMax{300};

    uint32_t fs{2'000'000};
    float tCpi{0.5};
    uint32_t nSamples = tCpi * fs;    // narrow on purpose

    Ambiguity ambiguity(delayMin, delayMax, dopplerMin, 
      dopplerMax, fs, nSamples, round_hamming);
    IqData x{nSamples};
    IqData y{nSamples};

    read_file(x, y, "20231214-230611.rspduo");
    REQUIRE(x.get_length() == x.get_n());

    auto map{ambiguity.process(&x ,&y)};
    map->set_metrics();
    CHECK_THAT(map->maxPower, Catch::Matchers::WithinAbs(30.2816, 0.001));
    CHECK_THAT(map->noisePower, Catch::Matchers::WithinAbs(76.918, 0.001));
}

/// @brief Reference noise and a delayed, Doppler-shifted copy as surveillance.
/// @details Seeded so the window tests are repeatable.
static void delayed_echo(IqData &x, IqData &y, uint32_t fs, uint32_t delay,
  double doppler)
{
  std::mt19937 gen(3);
  std::normal_distribution<> dist;
  std::vector<std::complex<double>> s(x.get_n() + delay);
  for (auto &v : s) v = {dist(gen), dist(gen)};
  for (uint32_t i = 0; i < x.get_n(); i++)
  {
    x.push_back(s[i + delay]);
    y.push_back(s[i] * std::polar(1.0, 2.0 * M_PI * doppler * i / fs));
  }
}

/// @brief Peak cell of a map: power, row and column.
struct Peak { double power; uint32_t row, col; };
static Peak find_peak(Map<std::complex<double>> *map)
{
  Peak p{0.0, 0, 0};
  for (uint32_t i = 0; i < map->get_nRows(); i++)
    for (uint32_t j = 0; j < map->get_nCols(); j++)
      if (std::norm(map->data[i][j]) > p.power)
        p = {std::norm(map->data[i][j]), i, j};
  return p;
}

/// @brief Strongest leakage in the peak's column, at least `offset` Doppler
/// bins from the peak, in dB relative to the peak.
static double leakage_db(Map<std::complex<double>> *map, const Peak &p,
  uint32_t offset)
{
  double worst = 0.0;
  for (uint32_t i = 0; i < map->get_nRows(); i++)
    if (std::abs(int(i) - int(p.row)) >= int(offset))
      worst = std::max(worst, std::norm(map->data[i][p.col]));
  return 10 * std::log10(worst / p.power);
}

TEST_CASE("Doppler_Window_Parse", "[window]")
{
  Ambiguity::DopplerWindow w = Ambiguity::DopplerWindow::Hann;
  CHECK(Ambiguity::parse_doppler_window("none", w));
  CHECK(w == Ambiguity::DopplerWindow::None);
  CHECK(Ambiguity::parse_doppler_window("hann", w));
  CHECK(w == Ambiguity::DopplerWindow::Hann);
  CHECK_FALSE(Ambiguity::parse_doppler_window("Hann", w));
  CHECK_FALSE(Ambiguity::parse_doppler_window("", w));
}

/// @details An echo halfway between two Doppler bins is the worst case for
/// leakage. Without a window it spreads as a sinc, about -28 dB at 8 bins
/// away; Hann must push that far below.
TEST_CASE("Doppler_Window_Hann_Cuts_Leakage", "[window]")
{
  uint32_t fs{200'000};
  uint32_t nSamples{100'000};
  Ambiguity none(-10, 50, -100, 100, fs, nSamples, true);
  Ambiguity hann(-10, 50, -100, 100, fs, nSamples, true,
    Ambiguity::DopplerWindow::Hann);
  double offBin = 10.5 / none.get_cpi();

  IqData x1{nSamples}, y1{nSamples}, x2{nSamples}, y2{nSamples};
  delayed_echo(x1, y1, fs, 20, offBin);
  delayed_echo(x2, y2, fs, 20, offBin);
  Map<std::complex<double>> *mapNone = none.process(&x1, &y1);
  Peak peakNone = find_peak(mapNone);
  double leakNone = leakage_db(mapNone, peakNone, 8);
  Map<std::complex<double>> *mapHann = hann.process(&x2, &y2);
  Peak peakHann = find_peak(mapHann);
  double leakHann = leakage_db(mapHann, peakHann, 8);

  CHECK(peakHann.col == peakNone.col);
  CHECK(leakNone > -32.0);
  CHECK(leakHann < leakNone - 15.0);
}

/// @details The taper is scaled to mean 1, so an echo on a bin centre keeps
/// its peak height and only the noise floor moves.
TEST_CASE("Doppler_Window_Hann_Keeps_Peak", "[window]")
{
  uint32_t fs{200'000};
  uint32_t nSamples{100'000};
  Ambiguity none(-10, 50, -100, 100, fs, nSamples, true);
  Ambiguity hann(-10, 50, -100, 100, fs, nSamples, true,
    Ambiguity::DopplerWindow::Hann);
  double onBin = 10.0 / none.get_cpi();

  IqData x1{nSamples}, y1{nSamples}, x2{nSamples}, y2{nSamples};
  delayed_echo(x1, y1, fs, 20, onBin);
  delayed_echo(x2, y2, fs, 20, onBin);
  Peak peakNone = find_peak(none.process(&x1, &y1));
  Peak peakHann = find_peak(hann.process(&x2, &y2));

  CHECK(peakHann.row == peakNone.row);
  CHECK(peakHann.col == peakNone.col);
  CHECK(std::abs(10 * std::log10(peakHann.power / peakNone.power)) < 0.1);
}

/// @brief At a low fs there are more Doppler bins than the correlation FFT is
/// long, so the Doppler buffer must be sized to the bins, not to nfft.
/// @details 250 kHz at +/-1000 Hz: 1001 bins against nfft 250. Before the fix
/// this wrote ~12 KB past the buffer every CPI (ClickUp 123zgecjc4k). Run under
/// AddressSanitizer to see the old overflow; here, check the echo lands where
/// it should.
TEST_CASE("Low_Fs_Doppler_Buffer", "[lowfs]")
{
  uint32_t fs{250'000};
  uint32_t nSamples{125'000};
  Ambiguity ambiguity(-10, 100, -1000, 1000, fs, nSamples, true);
  REQUIRE(ambiguity.get_n_doppler_bins() > ambiguity.get_nfft());

  IqData x{nSamples}, y{nSamples};
  delayed_echo(x, y, fs, 60, 100.0);
  auto map = ambiguity.process(&x, &y);
  Peak p = find_peak(map);
  CHECK(map->delay[p.col] == 60);
  CHECK(std::abs(map->doppler[p.row] - 100.0) <= 1.0 / ambiguity.get_cpi());
}

/// @brief Reference implementation of the batches map, straight from the
/// definition: batch i correlates reference [i*nCorr, (i+1)*nCorr) against
/// surveillance at every delay in the window, zero outside the samples held,
/// then a Doppler DFT across batches in Ambiguity's output order.
static std::vector<std::vector<std::complex<double>>> brute_force_map(
  const std::vector<std::complex<double>> &x,
  const std::vector<std::complex<double>> &y, int32_t delayMin,
  int32_t delayMax, uint32_t nDoppler, uint32_t nCorr)
{
  const uint32_t nDelay = delayMax - delayMin + 1;
  std::vector<std::vector<std::complex<double>>> z(nDoppler,
    std::vector<std::complex<double>>(nDelay));
  for (uint32_t i = 0; i < nDoppler; i++)
    for (uint32_t j = 0; j < nDelay; j++)
      for (uint32_t n = 0; n < nCorr; n++)
      {
        const int64_t k = int64_t(i) * nCorr + n + delayMin + j;
        if (k >= 0 && k < int64_t(y.size()))
          z[i][j] += y[k] * std::conj(x[size_t(i) * nCorr + n]);
      }
  std::vector<std::vector<std::complex<double>>> map(nDoppler,
    std::vector<std::complex<double>>(nDelay));
  for (uint32_t row = 0; row < nDoppler; row++)
  {
    const uint32_t bin = (row + nDoppler / 2 + 1) % nDoppler;
    for (uint32_t j = 0; j < nDelay; j++)
      for (uint32_t i = 0; i < nDoppler; i++)
        map[row][j] += z[i][j] * std::polar(1.0, -2.0 * M_PI * double(i) * bin / nDoppler);
  }
  return map;
}

/// @brief The FFT path matches the definition, including delays beyond nCorr
/// and the zeroed samples before and after the CPI.
/// @details 41 Doppler bins of 48 samples, window -60..90: the window is wider
/// than a batch at both ends.
TEST_CASE("Matches_Brute_Force", "[delay]")
{
  auto round_hamming = GENERATE(true, false);
  uint32_t fs{10'000};
  uint32_t nSamples{2'000};
  int32_t delayMin{-60}, delayMax{90};
  Ambiguity ambiguity(delayMin, delayMax, -100, 100, fs, nSamples, round_hamming);
  REQUIRE(ambiguity.get_n_doppler_bins() == 41);
  REQUIRE(ambiguity.get_n_corr() == 48);

  IqData x{nSamples}, y{nSamples};
  random_iq(x);
  random_iq(y);
  std::vector<std::complex<double>> xv = x.get_data(), yv = y.get_data();
  auto expected = brute_force_map(xv, yv, delayMin, delayMax, 41, 48);
  auto map = ambiguity.process(&x, &y);

  double peak = 0.0, worst = 0.0;
  for (uint32_t i = 0; i < 41; i++)
    for (uint32_t j = 0; j < expected[i].size(); j++)
    {
      peak = std::max(peak, std::abs(expected[i][j]));
      worst = std::max(worst, std::abs(map->data[i][j] - expected[i][j]));
    }
  CHECK(worst < 1e-9 * peak);
  CHECK(x.get_length() == 0);
  CHECK(y.get_length() == 0);
}

/// @brief A target far beyond nCorr keeps its full power.
/// @details 250 kHz at +/-1000 Hz gives nCorr 124. Equal-length batches would
/// lose (124 - d) / 124 of the power and nothing at all past d = 123; with the
/// extended segment an echo at 300 bins is as strong as one at 20.
TEST_CASE("Far_Delay_Full_Power", "[delay]")
{
  uint32_t fs{250'000};
  uint32_t nSamples{125'000};
  Ambiguity ambiguity(-10, 400, -1000, 1000, fs, nSamples, true);
  REQUIRE(ambiguity.get_n_corr() == 124);
  REQUIRE(ambiguity.get_n_delay_bins() == 411);

  IqData x1{nSamples}, y1{nSamples}, x2{nSamples}, y2{nSamples};
  delayed_echo(x1, y1, fs, 20, 100.0);
  delayed_echo(x2, y2, fs, 300, 100.0);
  Peak near = find_peak(ambiguity.process(&x1, &y1));
  auto map = ambiguity.process(&x2, &y2);
  Peak far = find_peak(map);
  CHECK(map->delay[near.col] == 20);
  CHECK(map->delay[far.col] == 300);
  CHECK(std::abs(10 * std::log10(far.power / near.power)) < 0.5);
}

/// @brief Negative delays are read from before the batch.
TEST_CASE("Negative_Delay", "[delay]")
{
  uint32_t fs{250'000};
  uint32_t nSamples{125'000};
  Ambiguity ambiguity(-200, 50, -1000, 1000, fs, nSamples, true);
  // echo leads the reference by 150 samples: x[i] = s[i], y[i] = s[i + 150]
  IqData x{nSamples}, y{nSamples};
  delayed_echo(y, x, fs, 150, 0.0);
  auto map = ambiguity.process(&x, &y);
  CHECK(map->delay[find_peak(map).col] == -150);
}

/// @brief An empty window, or one longer than the CPI, is refused.
TEST_CASE("Delay_Window_Invalid_Throws", "[delay]")
{
  CHECK_THROWS_AS(Ambiguity(100, 50, -1000, 1000, 250'000, 125'000, true),
    std::invalid_argument);
  CHECK_THROWS_AS(Ambiguity(0, 2'000, -100, 100, 10'000, 2'000, true),
    std::invalid_argument);
}

/// @brief A CPI shorter than the Doppler bin count leaves batches empty.
TEST_CASE("Empty_Batch_Throws", "[lowfs]")
{
  // 2 Hz bins at +/-1000 Hz is 1001 bins, from only 500 samples
  CHECK_THROWS_AS(Ambiguity(-10, 400, -1000, 1000, 1'000, 500, true),
    std::invalid_argument);
}

/// @brief Every sample rate retina-gui offers runs the shipped window in full.
/// @details Shipped: delay -10..400, +/-1000 Hz, 0.5 s CPI. nCorr runs from 999
/// at 2 MHz down to 31 at 62.5 kHz, and the window is honoured at all of them.
TEST_CASE("Every_Gui_Sample_Rate", "[lowfs]")
{
  auto fs = GENERATE(62'500u, 125'000u, 250'000u, 500'000u, 1'000'000u,
    2'000'000u);
  uint32_t nSamples = fs / 2;
  Ambiguity ambiguity(-10, 400, -1000, 1000, fs, nSamples, true);
  CHECK(ambiguity.get_n_delay_bins() == 411);

  IqData x{nSamples}, y{nSamples};
  delayed_echo(x, y, fs, 300, 100.0);
  auto map = ambiguity.process(&x, &y);
  REQUIRE(map->get_nCols() == 411);
  CHECK(map->delay.front() == -10);
  CHECK(map->delay.back() == 400);
  CHECK(map->delay[find_peak(map).col] == 300);
}
