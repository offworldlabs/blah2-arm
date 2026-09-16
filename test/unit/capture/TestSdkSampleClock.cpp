#include "capture/rspduo/SdkSampleClock.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#undef assert
#define assert(condition) do { if (!(condition)) throw std::runtime_error("Clock check failed: " #condition); } while (false)

int main() {
  unsigned cases=0;
  // Dual-tuner device descriptors report zero on valid live runs; the ADC
  // clock is the selected device parameters' fsFreq, not descriptorHz.
  {SdkSampleClock::ScaledDuoConfig config{6000000.0,0.0,2000000,1,true,true};
   assert(SdkSampleClock::supportsRatio3(config));
   config.descriptorHz=6000000.0;
   assert(SdkSampleClock::supportsRatio3(config));
   config.adcHz=8000000.0;
   assert(!SdkSampleClock::supportsRatio3(config));
   config.adcHz=6000000.0;config.outputHz=1000000;
   assert(!SdkSampleClock::supportsRatio3(config));
   config.outputHz=2000000;config.decimation=2;
   assert(!SdkSampleClock::supportsRatio3(config));
   config.decimation=1;config.if1620=false;
   assert(!SdkSampleClock::supportsRatio3(config));
   config.if1620=true;config.dualTuner=false;
   assert(!SdkSampleClock::supportsRatio3(config));++cases;}
  // Independent hardware model: advance the ADC counter before integer
  // division; span many wraps and all three initial division remainders.
  for(uint32_t ratio: {1U,3U}) for(uint32_t phase=0;phase<ratio;++phase) {
    SdkSampleClock clock(ratio);
    uint32_t adc=252+phase,logical=adc/ratio;
    const uint32_t steps[]={252,504,1000000,1000000000,17,252,900000000};
    for(unsigned i=0;i<300;++i) {
      const uint32_t count=steps[i%7];
      auto result=clock.observe(adc/ratio,count,i==0);
      assert(result.first==logical && result.sequence_break==(i==0));
      assert(result.phases_after>=1 && result.phases_after<=ratio);
      adc=uint32_t(uint64_t(adc)+uint64_t(count)*ratio);logical+=count;
    }
    ++cases;
  }
  // Ceil division is also representable for this live packet geometry:
  // 756 ADC ticks per 252-sample callback and ADC starts divisible by four.
  // Use a separate encoder, preserving those packet-boundary constraints.
  for(uint32_t phase=0;phase<3;++phase) {
    SdkSampleClock clock(3);uint32_t adc=252+4*phase;
    uint32_t logical=uint32_t((uint64_t(adc)+2)/3);
    const uint32_t steps[]={252,504,1000000,1000000000};
    for(unsigned i=0;i<100;++i) {
      const uint32_t count=steps[i%4];
      auto x=clock.observe(uint32_t((uint64_t(adc)+2)/3),count,i==0);
      assert(x.first==logical && x.sequence_break==(i==0));
      adc=uint32_t(uint64_t(adc)+uint64_t(count)*3);logical+=count;
    }
    ++cases;
  }
  // Observed live boundary: 6 MHz ADC, 2 MHz output, 252-sample callbacks.
  for(uint32_t phase=0;phase<3;++phase) {
    uint32_t adc=uint32_t(uint64_t(1431655176U)*3+phase);
    uint32_t logical=adc/3;SdkSampleClock clock(3);unsigned wraps=0;
    for(unsigned i=0;i<10;++i) {
      auto x=clock.observe(adc/3,252,i==0);
      assert(x.first==logical && x.sequence_break==(i==0));
      wraps+=x.wrapped;adc+=756;logical+=252;
    }
    assert(wraps==1);++cases;
  }
  // A complete missing callback must remain a break at and away from wraps.
  for(uint32_t start: {84U,1431655176U}) for(uint32_t phase=0;phase<3;++phase) {
    SdkSampleClock clock(3);uint32_t adc=start*3+phase;
    clock.observe(adc/3,252,true);adc+=1512;
    assert(clock.observe(adc/3,252).sequence_break);++cases;
  }
  // Ordinary single-sample loss and explicit reset are never normalized away.
  {SdkSampleClock clock(3);clock.observe(84,252,true);
   assert(clock.observe(337,252).sequence_break);
   assert(clock.observe(589,252,true).sequence_break);++cases;}
  // Native uint32 wrap is still a continuous sequence.
  {SdkSampleClock clock;clock.observe(UINT32_MAX-3,4);
   auto x=clock.observe(0,4);assert(x.wrapped&&!x.sequence_break&&x.first==0);++cases;}
  // Initial phase uncertainty is explicit at the scaled wrap. One-sample
  // precision cannot be inferred from divided metadata without that phase.
  {SdkSampleClock clock(3);clock.observe(1431655680U,252,true);
   auto x=clock.observe(167,252);assert(x.wrapped&&!x.sequence_break);
   assert(x.first==1431655932U&&x.phases_before==3&&x.phases_after==2);++cases;}
  // Invalid configuration and impossible divided metadata fail closed.
  {bool failed=false;try{SdkSampleClock bad(2);}catch(const std::invalid_argument&){failed=true;}assert(failed);
   SdkSampleClock clock(3);failed=false;try{clock.observe(UINT32_MAX,252);}catch(const std::invalid_argument&){failed=true;}assert(failed);++cases;}
  std::cout<<"{\"counter_cases\":"<<cases<<",\"pass\":true,\"all_initial_phases\":true,\"multiple_wraps\":true,\"metadata_phase_uncertainty_reported\":true}\n";
}
