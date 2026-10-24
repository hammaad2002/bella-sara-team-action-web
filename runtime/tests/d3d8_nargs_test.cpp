#include <cstdio>
#include "core/rt.h"
#include "d3d8/d3d8.h"
#include "d3d8/d3d8_sdk_nargs.h"

extern "C" {
const GuestFnEntry recomp_functions[1] = {{0, nullptr}};
const uint32_t recomp_function_count = 0;

typedef void* mspace;
mspace create_mspace_with_base(void*, size_t, int) { return nullptr; }
void* mspace_malloc(mspace, size_t) { return nullptr; }
void* mspace_calloc(mspace, size_t, size_t) { return nullptr; }
void* mspace_realloc(mspace, void*, size_t) { return nullptr; }
void mspace_free(mspace, void*) {}
void mspace_mallinfo(mspace) {}  // stats only (rt::mem_log_stats); never called in tests
}

int main() {
  using d3d8::sdk_nargs::kIDirect3D8;
  if (kIDirect3D8[6].nargs != 1 || kIDirect3D8[7].nargs != 3 || kIDirect3D8[11].nargs != 5) {
    std::puts("d3d8_nargs_test FAIL: IDirect3D8 D3D8 arg counts");
    return 1;
  }
  d3d8::d3d8_verify_registered_nargs();
  std::puts("d3d8_nargs_test PASS");
  return 0;
}
