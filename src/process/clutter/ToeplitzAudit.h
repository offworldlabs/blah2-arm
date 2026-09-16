#pragma once
#include <array>
#include <iostream>
#include "HermitianToeplitz.h"
#ifdef OWL_TOEPLITZ_AUDIT
struct OwlToeplitzAudit {
  std::array<unsigned,6> reasons{};
  void record(owl_toeplitz::Result r) { ++reasons[static_cast<unsigned>(r.reason)]; }
  ~OwlToeplitzAudit() {
    std::cerr<<"OWL_TOEPLITZ_AUDIT {\"accepted\":"<<reasons[0]<<",\"fallback_input\":"<<reasons[1]
      <<",\"fallback_energy\":"<<reasons[2]<<",\"fallback_condition\":"<<reasons[3]
      <<",\"fallback_nonfinite\":"<<reasons[4]<<",\"fallback_residual\":"<<reasons[5]<<"}\n";
  }
};
inline thread_local OwlToeplitzAudit owlToeplitzAudit;
#endif
