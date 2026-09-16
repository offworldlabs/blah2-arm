#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>

// Metadata adapter for a 32-bit ADC counter divided by an integer rate ratio.
// Never changes IQ, callback lengths, or tuner-pair validation. Track every
// possible ADC remainder until observations identify the counter phase. This
// avoids assuming that the divided counter itself wraps at 2^32, or always at
// floor(2^32 / ratio), which is wrong when the division has a remainder.
//
// The divided metadata alone has one-sample uncertainty at a wrap until the
// ADC remainder is identified. Report phase cardinality, do not conceal it.
// Resets and counters outside all predictions remain sequence breaks.
class SdkSampleClock {
public:
  struct ScaledDuoConfig {
    double adcHz;
    double descriptorHz;
    uint32_t outputHz;
    unsigned decimation;
    bool if1620;
    bool dualTuner;
  };
  static bool supportsRatio3(const ScaledDuoConfig& config) {
    // The selected-device descriptor can be zero in a valid dual-tuner run.
    // The effective ADC clock comes from devParams->fsFreq, not that field.
    return config.adcHz==6000000.0 && config.outputHz==2000000 &&
           config.decimation==1 && config.if1620 && config.dualTuner;
  }
  struct Result {
    uint32_t first;
    bool sequence_break, wrapped;
    unsigned phases_before, phases_after;
  };
private:
  uint32_t ratio_=1, logicalNext_=0, previousRaw_=0;
  std::array<uint32_t,3> adcNext_{};
  unsigned phases_=0;
  bool have_=false;
  void seed(uint32_t first, uint32_t count) {
    phases_=0;
    for(uint32_t remainder=0;remainder<ratio_;++remainder) {
      const uint64_t adc=uint64_t(first)*ratio_+remainder;
      if(adc< (uint64_t(1)<<32))
        adcNext_[phases_++]=uint32_t(adc+uint64_t(count)*ratio_);
    }
    if(!phases_)throw std::invalid_argument("SDK counter outside configured ADC ratio");
  }
public:
  explicit SdkSampleClock(uint32_t ratio=1) { configure(ratio); }
  void configure(uint32_t ratio) {
    if(ratio!=1 && ratio!=3)throw std::invalid_argument("Unvalidated SDK counter ratio");
    ratio_=ratio;clear();
  }
  void clear() {have_=false;phases_=0;logicalNext_=previousRaw_=0;}
  Result observe(uint32_t first,uint32_t count,bool reset=false) {
    const unsigned before=phases_;
    bool gap=false;
    if(have_ && !reset) {
      unsigned kept=0;
      for(unsigned i=0;i<phases_;++i)
        if(adcNext_[i]/ratio_==first)
          adcNext_[kept++]=uint32_t(uint64_t(adcNext_[i])+uint64_t(count)*ratio_);
      phases_=kept;gap=!kept;
    }
    const bool continuing=have_ && !reset && !gap;
    const uint32_t logical=continuing?logicalNext_:first;
    if(!continuing)seed(first,count);
    const bool wrapped=continuing && first<previousRaw_;
    have_=true;logicalNext_=logical+count;previousRaw_=first;
    return {logical,reset||gap,wrapped,before,phases_};
  }
};
