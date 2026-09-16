#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>
#include "data/IqData.h"

// One paired producer and one consumer. Each block contains signed16 IIQQ IQ.
// Storage is allocated at construction. No sample copies occur under the mutex.
// Overflow drops the oldest complete queued CPI, never one tuner independently.
class PairedCpiQueue {
public:
  struct Block { std::vector<int16_t> iq; uint32_t first=0; uint64_t epoch=0; };
  struct Stats { uint64_t published=0,taken=0,dropped_cpis=0,discarded_samples=0,
    discontinuities=0,producer_wait_ns=0,producer_wait_max_ns=0;size_t high_water=0; };
private:
  uint32_t samples_;
  uint64_t capacitySamples_;
  std::atomic<size_t> readyCount_{0};
  std::vector<Block> blocks_;
  std::vector<size_t> free_,ready_;
  size_t head_=0,count_=0,writing_=0,filled_=0;
  uint32_t expected_=0;uint64_t epoch_=0;
  bool sequence_=false,borrowed_=false;
  std::atomic<bool> closed_{false},cancelled_{false};
  size_t loan_=0;
  // Serializes producer-only partial-CPI state with final close. One lock per
  // callback, never one lock per sample; consumer handoff still uses mutex_.
  std::mutex producerMutex_;
  mutable std::mutex mutex_;
  std::condition_variable available_;
  Stats stats_;
  void discard_ready_locked(){
    while(count_){free_.push_back(ready_[head_]);head_=(head_+1)%ready_.size();--count_;
      ++stats_.dropped_cpis;stats_.discarded_samples+=samples_;}
    readyCount_.store(0,std::memory_order_release);
  }
  void discard_pending_producer_locked(){
    std::lock_guard<std::mutex> lock(mutex_);discard_ready_locked();
    stats_.discarded_samples+=filled_;++stats_.discontinuities;
    filled_=0;++epoch_;sequence_=false;
  }
public:
  PairedCpiQueue(uint32_t samples,size_t readyCapacity,uint64_t capacitySamples=0):samples_(samples),
    capacitySamples_(capacitySamples?capacitySamples:uint64_t(readyCapacity+1)*samples),
    blocks_(readyCapacity+2),ready_(readyCapacity){
    if(!samples||!readyCapacity||readyCapacity>1024||capacitySamples_<samples)throw std::invalid_argument("Invalid CPI queue size");
    free_.reserve(blocks_.size());
    for(size_t i=0;i<blocks_.size();++i){blocks_[i].iq.resize(size_t(samples)*4);if(i)free_.push_back(i);}
  }
  PairedCpiQueue(const PairedCpiQueue&)=delete;
  PairedCpiQueue& operator=(const PairedCpiQueue&)=delete;
  uint32_t samples()const{return samples_;}
  void discard_pending(){
    std::lock_guard<std::mutex> producerLock(producerMutex_);
    if(closed_||cancelled_)return;
    discard_pending_producer_locked();
  }
  // Producer-only. A reset or a sequence gap discards queued/partial old-epoch IQ.
  void push(const int16_t* iq,uint32_t count,uint32_t first,bool reset=false){
    std::lock_guard<std::mutex> producerLock(producerMutex_);
    if(cancelled_){
      std::lock_guard<std::mutex> lock(mutex_);
      stats_.discarded_samples+=count;
      throw std::runtime_error("Producer used stopped CPI queue");
    }
    if(closed_)throw std::runtime_error("Producer used closed CPI queue");
    if(count&&!iq)throw std::invalid_argument("Null callback IQ");
    if(reset&&!count){discard_pending_producer_locked();return;}
    if(!count)return;
    if(reset||(sequence_&&first!=expected_)){
      std::lock_guard<std::mutex> lock(mutex_);discard_ready_locked();
      stats_.discarded_samples+=filled_;++stats_.discontinuities;
      filled_=0;++epoch_;
    }
    sequence_=true;expected_=first+count;
    uint32_t offset=0;
    while(offset<count){
      if(cancelled_){
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.discarded_samples+=count-offset;
        throw std::runtime_error("Producer used stopped CPI queue");
      }
      if(!filled_){blocks_[writing_].first=first+offset;blocks_[writing_].epoch=epoch_;}
      const size_t take=std::min<size_t>(samples_-filled_,count-offset);
      // Preserve fractional-CPI capacity without holding a lock during copying.
      // Only the producer adds ready blocks; a stale count can overestimate only.
      if(uint64_t(readyCount_.load(std::memory_order_acquire))*samples_+filled_+take>capacitySamples_){
        std::lock_guard<std::mutex> lock(mutex_);
        while(count_&&uint64_t(count_)*samples_+filled_+take>capacitySamples_){
          free_.push_back(ready_[head_]);head_=(head_+1)%ready_.size();--count_;
          ++stats_.dropped_cpis;stats_.discarded_samples+=samples_;
        }
        readyCount_.store(count_,std::memory_order_release);
      }
      std::memcpy(blocks_[writing_].iq.data()+4*filled_,iq+4*size_t(offset),8*take);
      filled_+=take;offset+=take;
      if(filled_!=samples_)continue;
      {
#ifdef OWL_CAPTURE_PROFILE
        const auto lockBegin=std::chrono::steady_clock::now();
#endif
        std::lock_guard<std::mutex> lock(mutex_);
#ifdef OWL_CAPTURE_PROFILE
        const uint64_t waitNs=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-lockBegin).count();
        stats_.producer_wait_ns+=waitNs;stats_.producer_wait_max_ns=std::max(stats_.producer_wait_max_ns,waitNs);
#endif
        if(cancelled_){
          ++stats_.dropped_cpis;
          stats_.discarded_samples+=filled_+count-offset;
          filled_=0;
          throw std::runtime_error("Producer used stopped CPI queue");
        }
        if(closed_)throw std::runtime_error("Producer used closed CPI queue");
        if(count_==ready_.size()){
          free_.push_back(ready_[head_]);head_=(head_+1)%ready_.size();--count_;
          ++stats_.dropped_cpis;stats_.discarded_samples+=samples_;
        }
        ready_[(head_+count_)%ready_.size()]=writing_;++count_;++stats_.published;
        readyCount_.store(count_,std::memory_order_release);
        stats_.high_water=std::max(stats_.high_water,count_);
        if(free_.empty())throw std::logic_error("CPI queue pool invariant");
        writing_=free_.back();free_.pop_back();filled_=0;
      }
      available_.notify_one();
    }
  }
  // Any thread may close. Waits for the active producer callback to finish,
  // then accounts its partial tail; complete ready CPIs remain drainable.
  void close(){
    std::lock_guard<std::mutex> producerLock(producerMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if(closed_)return;
    closed_=true;stats_.discarded_samples+=filled_;filled_=0;
    available_.notify_all();
  }
  // Error shutdown: wake the consumer immediately and discard queued complete
  // CPIs, without touching producer-owned partial state. A later close() after
  // callbacks quiesce accounts that partial tail.
  void cancel(){
    cancelled_=true;
    std::lock_guard<std::mutex> lock(mutex_);
    discard_ready_locked();
    available_.notify_all();
  }
  bool acquire(size_t& slot,bool wait=true){
    std::unique_lock<std::mutex> lock(mutex_);
    if(borrowed_)throw std::logic_error("Only one consumer loan is allowed");
    if(wait)available_.wait(lock,[&]{return count_||closed_||cancelled_;});
    if(cancelled_||!count_)return false;
    slot=ready_[head_];head_=(head_+1)%ready_.size();--count_;
    readyCount_.store(count_,std::memory_order_release);
    loan_=slot;borrowed_=true;++stats_.taken;return true;
  }
  const Block& block(size_t slot)const{return blocks_.at(slot);}
  void release(size_t slot){std::lock_guard<std::mutex> lock(mutex_);
    if(!borrowed_||slot!=loan_)throw std::logic_error("Invalid CPI queue release");
    borrowed_=false;free_.push_back(slot);}
  Stats stats()const{std::lock_guard<std::mutex> lock(mutex_);return stats_;}
};

// Consumer-owned conversion workspaces; never accessed by the producer.
class PairedCpiConsumer {
  std::vector<std::complex<double>> x_,y_;
  uint32_t count_; bool direct_;
public:
  explicit PairedCpiConsumer(uint32_t count, bool direct=false):x_(direct?0:count),y_(direct?0:count),count_(count),direct_(direct){}
  void copy(const PairedCpiQueue::Block& block,IqData& x,IqData& y){
    if(block.iq.size()!=size_t(count_)*4)throw std::invalid_argument("CPI block size differs");
    if(direct_) { x.assign_paired_i16(block.iq.data(),count_,y); return; }
    for(size_t i=0;i<x_.size();++i){
      x_[i]={double(block.iq[4*i]),double(block.iq[4*i+1])};
      y_[i]={double(block.iq[4*i+2]),double(block.iq[4*i+3])};
    }
    x.assign_samples(x_.data(),x_.size());y.assign_samples(y_.data(),y_.size());
  }
};
