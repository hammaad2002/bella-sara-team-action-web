// Standalone mirror of the startup D3D8 COM nargs self-check (see device.cpp D3d8NargsSelfCheck).
// Link with bella_runtime when added to CMake; until then the live check runs at process init.
#include "d3d8/d3d8.h"
#include "d3d8/d3d8_sdk_nargs.h"

int main() {
  (void)d3d8::iface_d3d8_device();
  return 0;
}
