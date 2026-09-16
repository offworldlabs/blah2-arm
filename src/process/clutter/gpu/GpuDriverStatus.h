#pragma once
#include <string>

namespace blah2 {
// Diagnostic JSON only; not the geometry/worker ABI. Driver strings are bounded
// and escaped independently of the optional Vulkan module's other interfaces.
inline std::string gpuDiagnosticString(const std::string& value) {
  std::string result = "\"";
  for (size_t i = 0; i < value.size() && i < 240; ++i) {
    const unsigned char c = value[i];
    if (c == '"' || c == '\\') result += '\\';
    if (c < 32 || c >= 127) result += '?';
    else result += static_cast<char>(c);
  }
  return result + '"';
}
using GpuDriverStatus = std::string (*)(unsigned);
}
