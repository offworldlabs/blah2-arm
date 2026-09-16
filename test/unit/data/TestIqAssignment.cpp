#include "data/IqData.h"
#include <array>
#include <new>
#include <stdexcept>
#include "rapidjson/document.h"
int main() {
  alignas(IqData) std::array<unsigned char, sizeof(IqData)> storage;
  // 0x42 yields a large finite nonzero double on the supported IEEE-754 hosts.
  // Tiny poison values could be truncated to zero by the two-decimal writer.
  storage.fill(0x42);
  IqData* fresh = new (storage.data()) IqData(4);
  const std::string json = fresh->to_json(1234);
  fresh->~IqData();
  rapidjson::Document metadata;
  metadata.Parse(json.c_str());
  if (metadata.HasParseError() || !metadata.IsObject() ||
      !metadata.HasMember("timestamp") || !metadata["timestamp"].IsUint64() ||
      metadata["timestamp"].GetUint64() != 1234)
    throw std::runtime_error("Fresh IQ metadata is not valid JSON");
  for (const char* key : {"min", "max", "mean"})
    if (!metadata.HasMember(key) || !metadata[key].IsNumber() ||
        metadata[key].GetDouble() != 0.0)
      throw std::runtime_error("Fresh IQ statistic placeholder is not initialized");

  IqData x(4), y(4);
  const std::array<int16_t, 16> raw{{-32768,32767,0,-1,1,2,3,4,5,6,7,8,9,10,11,12}};
  x.assign_paired_i16(raw.data(),4,y);
  for (unsigned i=0;i<4;++i)
    if(x.view_data()[i]!=std::complex<double>(raw[4*i],raw[4*i+1]) ||
       y.view_data()[i]!=std::complex<double>(raw[4*i+2],raw[4*i+3]))
      throw std::runtime_error("Paired signed IQ conversion differs");
  const auto a=x.get_data(),b=y.get_data();
  for(unsigned mode=0;mode<3;++mode){
    bool rejected=false;
    try{x.assign_paired_i16(mode==0?nullptr:raw.data(),mode==1?5:4,mode==2?x:y);}
    catch(const std::invalid_argument&){rejected=true;}
    if(!rejected||x.view_data()!=a||y.view_data()!=b)throw std::runtime_error("Invalid assignment changed input");
  }
  x.assign_paired_i16(raw.data(),2,y);
  if(x.get_length()!=2||y.get_length()!=2)throw std::runtime_error("Pair shrink failed");
  x.assign_paired_i16(nullptr,0,y);
  if(x.get_length()||y.get_length())throw std::runtime_error("Pair clear failed");
}
