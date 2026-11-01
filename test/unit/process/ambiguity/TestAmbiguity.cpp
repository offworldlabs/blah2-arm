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
    CHECK(ambiguity.get_nfft() == 6643);
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
    CHECK(ambiguity.get_nfft() == 6750);
}

/// @brief Test constructor at the shipped geometry, +/-1000 Hz.
/// @details The span every node runs (retina-node's default.yml, mirrored in
/// config/config.yml for standalone use). 2 Hz bins give 1001 Doppler bins and
/// nCorr 999, and the delay window must fit inside nCorr. The FFT length rounds
/// 1997 up to 2000 = 2^4 * 5^3.
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
    CHECK(ambiguity.get_n_delay_bins() < ambiguity.get_n_corr());
    CHECK(ambiguity.get_n_doppler_bins() == 1001);
    CHECK(ambiguity.get_nfft() == 2000);
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
