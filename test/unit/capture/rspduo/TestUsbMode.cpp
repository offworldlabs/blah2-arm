#include "capture/rspduo/UsbMode.h"
#include <iostream>
int main() {
  if (owlUsbBulkMode(nullptr) || owlUsbBulkMode("isoch") || !owlUsbBulkMode("bulk")) return 1;
  unsigned cases=3;
  for (const char* value : {"", "1", "0", "BULK", "bulk ", "iso"}) {
    bool rejected=false;
    try { (void)owlUsbBulkMode(value); } catch (const std::invalid_argument&) { rejected=true; }
    if (!rejected) return 2;
    ++cases;
  }
  std::cout << "{\"pass\":true,\"cases\":" << cases << "}\n";
}
