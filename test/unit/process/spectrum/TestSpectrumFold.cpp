#include "data/IqData.h"
#include "process/spectrum/SpectrumAnalyser.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <deque>
#include <random>
#include <stdexcept>
#include <vector>
#include <rapidjson/document.h>

namespace {
using Complex = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;

rapidjson::Document json(IqData& iq) {
  rapidjson::Document result;
  result.Parse(iq.to_json(1234567890).c_str());
  if (result.HasParseError() || !result.HasMember("spectrum") || !result.HasMember("frequency"))
    throw std::runtime_error("Spectrum JSON is malformed");
  return result;
}

std::vector<Complex> direct(const std::deque<Complex>& input, uint32_t nfft, uint32_t decimation) {
  std::vector<Complex> output;
  for (uint32_t out = 0; out < nfft; out += decimation) {
    const uint32_t k = (out + nfft / 2 + 1) % nfft;
    Complex sum{};
    for (uint32_t sample = 0; sample < nfft; ++sample)
      sum += input[sample] * std::polar(1.0, -2.0 * kPi * k * sample / nfft);
    output.push_back(sum / static_cast<double>(nfft));
  }
  return output;
}

void close(double actual, double expected, const char* message) {
  // IqData JSON caps values at two decimal places. RapidJSON's bounded-decimal
  // writer is allowed to truncate rather than round, so the public contract is
  // one hundredth of a kHz, not half a rounded centi-kHz.
  if (!std::isfinite(actual) || std::abs(actual - expected) > 0.0101)
    throw std::runtime_error(message);
}

void verify(uint32_t n, double bandwidth, double center, double rate, bool directGate = true) {
  SpectrumAnalyser analyser(n, bandwidth, center, rate);
  IqData iq(n);
  std::mt19937_64 random(n * 17ULL + 3);
  std::uniform_real_distribution<double> value(-1, 1);
  for (uint32_t i = 0; i < n; ++i) iq.push_back({value(random) + .25, value(random) - .125});
  const auto raw = iq.view_data();
  const double resolution = rate / n;
  const uint32_t decimation = static_cast<uint32_t>(std::min<double>(n,
    std::max(1.0, std::ceil(bandwidth / resolution))));
  const uint32_t nfft = (n / decimation) * decimation;
  const auto expected = directGate ? direct(raw, nfft, decimation) : std::vector<Complex>{};
  analyser.process(&iq);
  if (raw != iq.view_data()) throw std::runtime_error("Spectrum changed raw IQ");
  const auto output = json(iq);
  const uint32_t expectedSize = nfft / decimation;
  if (output["spectrum"].Size() != expectedSize || output["frequency"].Size() != expectedSize)
    throw std::runtime_error("Spectrum JSON shape differs from physical geometry");
  for (rapidjson::SizeType i = 0; i < output["spectrum"].Size(); ++i) {
    if (!std::isfinite(output["spectrum"][i].GetDouble()))
      throw std::runtime_error("Serialized spectrum contains a nonfinite bin");
    if (directGate)
      close(output["spectrum"][i].GetDouble(), 10.0 * std::log10(std::abs(expected[i])),
        "Serialized normalized direct DFT differs");
    close(output["frequency"][i].GetDouble(), (center +
      (static_cast<double>(i * decimation) - nfft / 2.0) * resolution) / 1000.0,
      "Serialized frequency does not use capture geometry");
  }
}

void verify_retune() {
  SpectrumAnalyser analyser(64, 512, 204640000, 4096);
  IqData iq(64);
  for (uint32_t i = 0; i < 64; ++i) iq.push_back({std::sin(i * .17) + .3, std::cos(i * .11) + .2});
  analyser.process(&iq);
  const auto before = json(iq);
  analyser.set_center_frequency(204645000);
  analyser.process(&iq);
  const auto after = json(iq);
  if (before["spectrum"].Size() != after["spectrum"].Size())
    throw std::runtime_error("Retune changed spectrum shape");
  for (rapidjson::SizeType i = 0; i < before["spectrum"].Size(); ++i) {
    close(after["spectrum"][i].GetDouble(), before["spectrum"][i].GetDouble(), "Retune changed spectrum bins");
    close(after["frequency"][i].GetDouble() - before["frequency"][i].GetDouble(), 5.0,
      "Retune did not update only frequency metadata");
  }
}
}

int main() {
  // odd/even plus folded/non-folded geometry
  verify(63, 700, 101234500, 8192);
  verify(64, 512, 204640000, 4096);
  verify(257, 1000, 915000000, 48000);
  verify(256, 30000, 433920000, 96000);
  // At 1 Msamples / 2 MSps / 2 kHz, the selected physical definition emits
  // 1,000 bins, rather than the legacy 2,000 raw unnormalised FFT values.
  verify(1000000, 2000, 204640000, 2000000, false);
  verify_retune();
  bool rejected = false;
  try { SpectrumAnalyser invalid(0, 1, 1, 1); } catch (const std::invalid_argument&) { rejected = true; }
  if (!rejected) return 1;
  SpectrumAnalyser analyser(64, 200, 100000000, 4000);
  IqData shortInput(64);
  rejected = false;
  try { analyser.process(&shortInput); } catch (const std::invalid_argument&) { rejected = true; }
  if (!rejected) return 1;
  rejected = false;
  try { analyser.process(nullptr); } catch (const std::invalid_argument&) { rejected = true; }
  return rejected ? 0 : 1;
}
