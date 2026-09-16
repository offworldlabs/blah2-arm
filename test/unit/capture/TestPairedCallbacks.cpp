#include <algorithm>
#include <atomic>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "capture/Source.h"
#include "capture/PairedCpiQueue.h"
#define private public
#include "capture/rspduo/RspDuo.h"
#undef private
extern IqData *buffer1;
extern IqData *buffer2;
extern bool *capture_fg;
extern std::ofstream *saveIqFileLocal;
extern std::atomic<bool> run_fg;
#include <cstdio>
#include <iterator>
void require(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
struct Rig {
 bool save=false;IqData x{16},y{16};PairedCpiQueue queue{8,8};
 RspDuo device{"RspDuo",503000000,2000000,"",&save,0,0,20,20,0,false,false,false};
 std::ofstream recording;
 Rig(){buffer1=&x;buffer2=&y;capture_fg=&save;saveIqFileLocal=&recording;run_fg=true;RspDuo::set_output_queue(&queue);}
 ~Rig(){RspDuo::set_output_queue(nullptr);}
 void call(bool a,uint32_t first,unsigned n,bool reset=false){
  std::vector<short> i(n),q(n);for(unsigned j=0;j<n;++j){i[j]=short((first+j)*7+(a?0:2));q[j]=short((first+j)*7+(a?1:3));}
  sdrplay_api_StreamCbParamsT p{};p.firstSampleNum=first;p.numSamples=n;
  if(a)device._stream_a_callback(i.data(),q.data(),&p,n,reset,&device);
  else device._stream_b_callback(i.data(),q.data(),&p,n,reset,&device);
 }
 void pairData(uint32_t raw,unsigned n,uint32_t logical){
  std::vector<short> ai(n),aq(n),bi(n),bq(n);
  for(unsigned j=0;j<n;++j){ai[j]=short((logical+j)*7);aq[j]=short((logical+j)*7+1);bi[j]=short((logical+j)*7+2);bq[j]=short((logical+j)*7+3);}
  sdrplay_api_StreamCbParamsT p{};p.firstSampleNum=raw;p.numSamples=n;
  device._stream_a_callback(ai.data(),aq.data(),&p,n,0,&device);
  device._stream_b_callback(bi.data(),bq.data(),&p,n,0,&device);
 }
 void pair(uint32_t first,unsigned n,bool reset=false){call(true,first,n,reset);call(false,first,n,reset);}
 void check(uint32_t first,uint64_t epoch){size_t slot;require(queue.acquire(slot,false),"Missing block");auto &b=queue.block(slot);
  require(b.first==first&&b.epoch==epoch,"Sequence/epoch mismatch");
  for(unsigned j=0;j<8;++j)for(unsigned c=0;c<4;++c)require(b.iq[j*4+c]==short((first+j)*7+c),"Paired sample mismatch");
  for(bool direct:{false,true}) {
  PairedCpiConsumer consumer(8,direct);IqData a(8),b2(8);consumer.copy(b,a,b2);
  for(unsigned j=0;j<8;++j){require(a.view_data()[j]==std::complex<double>(b.iq[j*4],b.iq[j*4+1]),"Converted A mismatch");require(b2.view_data()[j]==std::complex<double>(b.iq[j*4+2],b.iq[j*4+3]),"Converted B mismatch");}
  }
  queue.release(slot);
 }
};
int main(int argc,char**argv){
 require(argc==2,"Need temporary recording path");unsetenv("OWL_SDK_COUNTER_SCALE");unsigned cases=0;
 {IqData a(8),b(8);const int16_t values[8]={-32768,32767,0,-1,1,2,3,4};
  a.assign_paired_i16(values,2,b);require(a.view_data()[0]==std::complex<double>(-32768,32767)&&b.view_data()[1]==std::complex<double>(3,4),"Direct extremes");
  auto before=a.get_data();bool rejected=false;try{a.assign_paired_i16(values,2,a);}catch(const std::invalid_argument&){rejected=true;}require(rejected&&before==a.get_data(),"Aliasing not rejected");
  rejected=false;try{a.assign_paired_i16(nullptr,1,b);}catch(const std::invalid_argument&){rejected=true;}require(rejected&&before==a.get_data(),"Null not rejected");
  rejected=false;try{a.assign_paired_i16(values,9,b);}catch(const std::invalid_argument&){rejected=true;}require(rejected&&before==a.get_data(),"Oversize not rejected");
  a.assign_paired_i16(nullptr,0,b);require(a.get_length()==0&&b.get_length()==0,"Zero clear");a.assign_paired_i16(values,2,b);a.assign_paired_i16(values,1,b);require(a.get_length()==1&&b.get_length()==1,"Short reuse");++cases;
 }

 for(unsigned chunk:{1,3,7,8,13,32}){Rig r;for(unsigned i=0;i<32;){unsigned n=std::min(chunk,32-i);r.pair(i,n);i+=n;}for(unsigned i=0;i<4;++i)r.check(i*8,0);require(!r.queue.stats().dropped_cpis,"Unexpected drop");++cases;}
 {Rig r;auto rejects=RspDuo::rejected_callback_pairs();r.call(false,0,8);r.pair(8,8);r.check(8,1);require(RspDuo::rejected_callback_pairs()==rejects+1,"Missing A rejection");++cases;}
 {Rig r;r.call(true,0,8);r.call(false,1,8);r.pair(8,8);r.check(8,1);++cases;}
 {Rig r;r.call(true,0,8);r.call(false,0,7);r.pair(8,8);r.check(8,1);++cases;}
 {Rig r;r.call(true,0,8);r.call(true,8,8);r.call(false,8,8);r.check(8,1);++cases;}
 {Rig r;r.pair(0,11);r.pair(100,8);r.check(100,1);require(r.queue.stats().discarded_samples==11,"Gap accounting");++cases;}
 {Rig r;r.pair(0,11);r.pair(11,8,true);r.check(11,1);++cases;}
 {Rig r;r.pair(0,3);r.call(true,3,0,true);r.call(false,3,0,true);r.pair(3,8);r.check(3,2);require(run_fg,"Zero reset failure");++cases;}
 {Rig r;r.call(true,0,0);r.call(false,0,0);r.pair(0,8);r.check(0,0);++cases;}
 {Rig r;uint32_t first=UINT32_MAX-3;r.pair(first,8);r.pair(first+8,8);r.check(first,0);r.check(first+8,0);++cases;}
 {Rig r;r.recording.open(argv[1],std::ios::binary|std::ios::trunc);require(bool(r.recording),"Cannot open recording");r.save=true;r.pair(0,8);r.check(0,0);r.recording.close();std::ifstream f(argv[1],std::ios::binary);std::vector<short> v(32);f.read(reinterpret_cast<char*>(v.data()),64);require(f.gcount()==64&&f.peek()==EOF,"Recording size");for(unsigned j=0;j<8;++j)for(unsigned c=0;c<4;++c)require(v[4*j+c]==short(j*7+c),"Recording bytes");std::remove(argv[1]);++cases;}
 {Rig r;std::vector<short> raw(32);for(unsigned j=0;j<8;++j)for(unsigned c=0;c<4;++c)raw[4*j+c]=short(j*7+c);
  {std::ofstream f(argv[1],std::ios::binary|std::ios::trunc);require(bool(f),"Cannot create replay fixture");f.write(reinterpret_cast<const char*>(raw.data()),64);require(bool(f),"Cannot write replay fixture");}
  r.device.replay(&r.x,&r.y,argv[1],false);
  require(r.x.get_length()==8&&r.y.get_length()==8,"Finite replay did not retain exact final CPI");
  for(unsigned j=0;j<8;++j){require(r.x.view_data()[j]==std::complex<double>(raw[4*j],raw[4*j+1]),"Replay A differs");require(r.y.view_data()[j]==std::complex<double>(raw[4*j+2],raw[4*j+3]),"Replay B differs");}
  std::remove(argv[1]);++cases;}
 {Rig r;bool failed=false;try{r.device.replay(&r.x,&r.y,std::string(argv[1])+".missing",false);}catch(const std::runtime_error&){failed=true;}
  require(failed,"Missing replay file was not reported");++cases;}
 {Rig r;r.queue.close();r.pair(0,8);require(!run_fg,"C boundary did not contain queue exception");++cases;}
 setenv("OWL_SDK_COUNTER_SCALE","3",1);
 for(uint32_t phase=0;phase<3;++phase){
  Rig r;const uint32_t logical=1431655765U-6;uint32_t adc=logical*3+phase;
  for(unsigned j=0;j<4;++j){r.pairData(adc/3,4,logical+j*4);adc+=12;}
  r.check(logical,0);r.check(logical+8,0);
  require(!r.queue.stats().discarded_samples&&!r.queue.stats().discontinuities,"Scaled wrap discarded samples");++cases;
 }
 {Rig r;r.pairData(84,4,84);r.pairData(89,8,89);r.check(89,1);
  require(r.queue.stats().discarded_samples==4,"Scaled true gap not retained");++cases;}
 unsetenv("OWL_SDK_COUNTER_SCALE");
 std::cout<<"{\"callback_cases\":"<<cases<<",\"exact_samples\":true,\"recording_exact\":true,\"c_boundary_contained\":true}\n";
}
