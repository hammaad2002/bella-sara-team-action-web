// Standalone mirror of the startup D3D8 COM nargs self-check (see device.cpp D3d8NargsSelfCheck).
// Link with bella_runtime when added to CMake; until then the live check runs at process init.
#include <cstdio>
#include "d3d8/d3d8.h"
#include "d3d8/d3d8_sdk_nargs.h"

int main() {
  d3d8::d3d8_verify_registered_nargs();
  std::puts("d3d8_nargs_test PASS");
  return 0;
}
