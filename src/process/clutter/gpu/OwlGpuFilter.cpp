// Isolated OWL final-FIR offload. Low-level Vulkan/VkFFT support comes from
// the pinned public MIT helpers; no VectorWarp radar pipeline is used.
#include "hardware-vulkan-support.h"
#include "OwlGpuFilter.h"
#include <fstream>
#include <iomanip>
#include <cmath>
#include <cstring>
using namespace blah2;
namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
void barrier(VkCommandBuffer command,VkPipelineStageFlags from,VkPipelineStageFlags to,VkAccessFlags read,VkAccessFlags write){
 VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=read;b.dstAccessMask=write;
 vkCmdPipelineBarrier(command,from,to,0,1,&b,0,nullptr,0,nullptr);
}
void computeBarrier(VkCommandBuffer command){barrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);}
const char* packSource=R"(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Input {vec2 inputData[];};
layout(binding=1) writeonly buffer Work {vec2 work[];};
layout(push_constant) uniform P {uint count;uint fft;uint history;uint hop;int valid;} p;
void main(){uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;if(i>=p.count)return;
 int source=p.valid+int((i/p.fft)*p.hop+i%p.fft)-int(p.history);
 work[i]=(source<0 || source>=inputData.length())?vec2(0.0):inputData[source];
})";
const char* multiplySource=R"(#version 450
layout(local_size_x=128) in;
layout(binding=0) buffer Work {vec2 work[];};
layout(binding=1) readonly buffer Weights {vec2 weights[];};
layout(push_constant) uniform P {uint count;uint fft;uint history;uint hop;int valid;} p;
void main(){uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;if(i>=p.count)return;
 vec2 x=work[i],w=weights[i%p.fft];work[i]=vec2(x.x*w.x-x.y*w.y,x.x*w.y+x.y*w.x);
})";
const char* gatherSource=R"(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Work {vec2 work[];};
layout(binding=1) writeonly buffer Output {vec2 outputData[];};
layout(push_constant) uniform P {uint count;uint fft;uint history;uint hop;int valid;} p;
void main(){uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;if(i>=p.count)return;
 outputData[uint(p.valid)+i]=work[(i/p.hop)*p.fft+p.history+i%p.hop];
})";
}  // namespace
struct OwlGpuFilter::Impl {
 uint32_t samples,taps,fft,blocks,outputs,percent,tile;
 std::shared_ptr<Instance> owner;std::unique_ptr<Context> context;
 std::unique_ptr<Buffer> input,weights,work,output;
 std::unique_ptr<Kernel> pack,multiply,gather;std::unique_ptr<Plan> plan,weightPlan;
 VkCommandBuffer command=VK_NULL_HANDLE,referenceCommand=VK_NULL_HANDLE;
 VkFence referenceFence=VK_NULL_HANDLE;
 bool active=false,referenceActive=false,cycleOpen=false,poisoned=false,earlyReference=false;
 std::ofstream log;uint64_t frame=0;
 bool cachedReadback=false;std::vector<std::complex<float>> readback;
 Clock::time_point started,submitted,referenceStarted,referenceReturned;
 double packingMs=0,submitMs=0,referencePackingMs=0,referenceSubmitMs=0,referenceHostMs=0,correlationSolveMs=0;
 Impl(uint32_t n,uint32_t b,uint32_t k,uint32_t share):samples(n),taps(b),fft(k),percent(share){
  if(!samples||!taps||taps>fft||samples>10000000||fft>8192)throw std::runtime_error("Invalid bounded GPU FIR geometry");
  const uint32_t hop=fft-taps+1,total=(samples+hop-1)/hop;
  blocks=std::max(1u,total*percent/100);outputs=std::min(samples,blocks*hop);
  tile=blocks;  // Selected full-batch late-prefix variant.
  earlyReference=false;
  if(uint64_t(tile)*fft>32000000)throw std::runtime_error("GPU FIR workspace exceeds bounded test limit");
  owner=std::make_shared<Instance>();auto devices=enumerate(*owner);
  auto it=std::find_if(devices.begin(),devices.end(),[](const auto& d){return d.properties.vendorID==5348&&d.info.name.find("V3D")!=std::string::npos;});
  if(it==devices.end())throw std::runtime_error("Actual Pi V3D required; GPU fallback forbidden");
  context=std::make_unique<Context>(owner,*it);auto& ctx=*context;
  cachedReadback=true;
  if(cachedReadback)readback.resize(outputs);
  constexpr auto required=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  constexpr auto preferred=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  input=std::make_unique<Buffer>(ctx,uint64_t(outputs)*8,required,preferred);
  weights=std::make_unique<Buffer>(ctx,uint64_t(fft)*8,required,preferred);
  work=std::make_unique<Buffer>(ctx,uint64_t(tile)*fft*8,required,preferred);
  output=std::make_unique<Buffer>(ctx,uint64_t(outputs)*8,required,preferred);
  pack=std::make_unique<Kernel>(ctx,packSource,std::vector<Buffer*>{input.get(),work.get()});
  multiply=std::make_unique<Kernel>(ctx,multiplySource,std::vector<Buffer*>{work.get(),weights.get()});
  gather=std::make_unique<Kernel>(ctx,gatherSource,std::vector<Buffer*>{work.get(),output.get()});
  const bool lut=true;
  plan=std::make_unique<Plan>(ctx,*work,fft,tile,lut);
  weightPlan=std::make_unique<Plan>(ctx,*weights,fft,1,lut);
  VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocation.commandPool=ctx.pool;allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocation.commandBufferCount=1;
  check(vkAllocateCommandBuffers(ctx.device,&allocation,&command),"FIR command allocation");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  if(earlyReference) {
   VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   check(vkCreateFence(ctx.device,&fenceInfo,nullptr,&referenceFence),"Reference fence allocation");
   check(vkAllocateCommandBuffers(ctx.device,&allocation,&referenceCommand),"Reference command allocation");
   check(vkBeginCommandBuffer(referenceCommand,&begin),"Reference command begin");
   barrier(referenceCommand,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
   Push shape{tile*fft,fft,taps-1,hop,0};
   pack->append(referenceCommand,shape);computeBarrier(referenceCommand);
   plan->append(referenceCommand,-1);computeBarrier(referenceCommand);
   check(vkEndCommandBuffer(referenceCommand),"Reference command end");
  }
  check(vkBeginCommandBuffer(command,&begin),"FIR command begin");
  barrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
  // Same-queue execution plus this dependency makes earlier work FFT writes
  // visible to the filter tail. No CPU wait between the two submissions.
  if(earlyReference)computeBarrier(command);
  weightPlan->append(command,-1);computeBarrier(command);
  for(uint32_t first=0;first<outputs;first+=tile*hop) {
   Push shape{tile*fft,fft,taps-1,hop,int32_t(first)};
   if(!earlyReference){pack->append(command,shape);computeBarrier(command);plan->append(command,-1);computeBarrier(command);}
   multiply->append(command,shape);computeBarrier(command);plan->append(command,1);computeBarrier(command);
   shape.count=std::min(tile*hop,outputs-first);gather->append(command,shape);computeBarrier(command);
  }
  barrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
  check(vkEndCommandBuffer(command),"FIR command end");
  if(const char* path=std::getenv("OWL_GPU_FILTER_LOG")){log.open(path);if(!log)throw std::runtime_error("GPU timing log open failed");log<<std::setprecision(12);}
  std::cerr<<"OWL_GPU_FILTER device="<<it->info.name<<" fft="<<fft<<" lut="<<lut<<" warp="<<plan->app.configuration.warpSize<<" coalesced="<<plan->app.configuration.coalescedMemory<<" percent="<<percent<<" blocks="<<blocks<<" tile="<<tile<<" output_samples="<<outputs<<" properties="<<input->properties<<" early_reference="<<earlyReference<<" cached_readback="<<cachedReadback<<"\n";
 }
 ~Impl(){
  if(context){
   if(active||referenceActive||poisoned)vkDeviceWaitIdle(context->device);
   if(command)vkFreeCommandBuffers(context->device,context->pool,1,&command);
   if(referenceCommand)vkFreeCommandBuffers(context->device,context->pool,1,&referenceCommand);
   if(referenceFence)vkDestroyFence(context->device,referenceFence,nullptr);
  }
 }
 void copyReference(const std::complex<double>* x){
  auto* xp=static_cast<std::complex<float>*>(input->mapped);
  for(uint32_t i=0;i<outputs;++i)xp[i]=std::complex<float>(x[i]);
  input->flush(0,input->bytes);
 }
 void reference(const std::complex<double>* x){
  if(poisoned||cycleOpen||active||referenceActive||!x)throw std::runtime_error("Invalid GPU reference ownership");
  referenceStarted=Clock::now();referencePackingMs=referenceSubmitMs=referenceHostMs=0;
  if(earlyReference){
   copyReference(x);referencePackingMs=elapsed(referenceStarted);
   VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&referenceCommand;
   const auto beforeSubmit=Clock::now();
   const auto status=vkQueueSubmit(context->queue,1,&submit,referenceFence);
   if(status!=VK_SUCCESS){poisoned=true;check(status,"Reference submit");}
   referenceActive=true;referenceSubmitMs=elapsed(beforeSubmit);referenceHostMs=elapsed(referenceStarted);
  }
  cycleOpen=true;referenceReturned=Clock::now();
 }
 uint32_t start(const std::complex<double>* x,const std::complex<double>* w){
  if(poisoned||!cycleOpen||active||!x||!w||(earlyReference&&!referenceActive))throw std::runtime_error("Invalid GPU FIR ownership");
  correlationSolveMs=elapsed(referenceReturned);started=Clock::now();
  if(!earlyReference)copyReference(x);
  auto* wp=static_cast<std::complex<float>*>(weights->mapped);
  for(uint32_t i=0;i<taps;++i)wp[i]=std::complex<float>(w[i]);
  std::fill(wp+taps,wp+fft,std::complex<float>{});
  weights->flush(0,weights->bytes);packingMs=elapsed(started);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
  submitted=Clock::now();const auto status=vkQueueSubmit(context->queue,1,&submit,context->fence);
  if(status!=VK_SUCCESS){poisoned=true;check(status,"FIR submit");}
  submitMs=elapsed(submitted);active=true;
  return outputs;
 }
 void drain(){
  if(active){
   check(vkWaitForFences(context->device,1,&context->fence,VK_TRUE,5000000000ULL),"FIR fence");
   check(vkResetFences(context->device,1,&context->fence),"FIR fence reset");active=false;
  }
  if(referenceActive){
   check(vkWaitForFences(context->device,1,&referenceFence,VK_TRUE,5000000000ULL),"Reference fence");
   check(vkResetFences(context->device,1,&referenceFence),"Reference fence reset");referenceActive=false;
  }
  cycleOpen=false;
 }
 void abort() noexcept {
  if(!cycleOpen&&!active&&!referenceActive)return;
  try {drain();}
  catch(const std::exception& e){poisoned=true;std::cerr<<"OWL_GPU_ABORT_FAILED "<<e.what()<<'\n';}
 }
 void finish(std::complex<double>* y){
  if(poisoned||!active||!y)throw std::runtime_error("GPU FIR finish without submission");
  const auto beforeWait=Clock::now();const double cpuWindow=elapsed(submitted);
  try {drain();} catch(...){poisoned=true;throw;}
  const double waitMs=elapsed(beforeWait),gpuWindow=elapsed(submitted);
  const auto beforeRead=Clock::now();output->invalidate(0,output->bytes);
  const auto* p=static_cast<const std::complex<float>*>(output->mapped);
  if(cachedReadback){std::memcpy(readback.data(),p,output->bytes);p=readback.data();}
  for(uint32_t i=0;i<outputs;++i)y[i]-=std::complex<double>(p[i]);
  const double subtractMs=elapsed(beforeRead),tail=elapsed(started);
  // Submit-to-fence includes concurrent CPU work; it is NOT GPU execution time.
  if(log)log<<"{\"frame\":"<<++frame<<",\"gpu_samples\":"<<outputs
   <<",\"early_reference\":"<<earlyReference<<",\"reference_packing_ms\":"<<referencePackingMs
   <<",\"reference_submit_ms\":"<<referenceSubmitMs<<",\"reference_host_ms\":"<<referenceHostMs
   <<",\"correlation_solve_ms\":"<<correlationSolveMs<<",\"packing_ms\":"<<packingMs
   <<",\"submit_ms\":"<<submitMs<<",\"cpu_window_ms\":"<<cpuWindow<<",\"fence_wait_ms\":"<<waitMs
   <<",\"submit_to_fence_ms\":"<<gpuWindow<<",\"subtract_ms\":"<<subtractMs
   <<",\"filter_tail_ms\":"<<tail<<",\"reference_host_plus_tail_ms\":"<<referenceHostMs+tail
   <<",\"reference_lifetime_ms\":"<<elapsed(referenceStarted)<<"}\n";
 }

};
OwlGpuFilter::OwlGpuFilter(uint32_t samples,uint32_t taps)
    try : impl_(std::make_unique<Impl>(samples,taps,2048,50))
{ std::cerr << "OWL_GPU_FILTER status=active backend=vulkan share=50\n"; }
catch (const std::exception& error)
{ std::cerr << "OWL_GPU_FILTER status=error phase=init reason=" << error.what() << '\n'; throw; }
OwlGpuFilter::~OwlGpuFilter() = default;
void OwlGpuFilter::reference(const std::complex<double>* x)
{ try { impl_->reference(x); }
  catch (const std::exception& e) { std::cerr << "OWL_GPU_FILTER status=error phase=reference reason=" << e.what() << '\n'; throw; } }
uint32_t OwlGpuFilter::start(const std::complex<double>* x,const std::complex<double>* w)
{ try { return impl_->start(x,w); }
  catch (const std::exception& e) { std::cerr << "OWL_GPU_FILTER status=error phase=start reason=" << e.what() << '\n'; throw; } }
void OwlGpuFilter::finish(std::complex<double>* y)
{ try { impl_->finish(y); }
  catch (const std::exception& e) { std::cerr << "OWL_GPU_FILTER status=error phase=finish reason=" << e.what() << '\n'; throw; } }
void OwlGpuFilter::abort() noexcept { impl_->abort(); }
