#pragma once
#include <cstring>
#include <stdexcept>
// ISOCH stays the existing default. This option only selects the USB
// transport before SDK Init; it never changes ADC/output sample rates.
inline bool owlUsbBulkMode(const char* value) {
  if (!value || std::strcmp(value, "isoch") == 0) return false;
  if (std::strcmp(value, "bulk") == 0) return true;
  throw std::invalid_argument("OWL_SDK_USB_MODE must be isoch or bulk");
}
