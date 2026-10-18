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
}

int main() {
  d3d8::d3d8_verify_registered_nargs();
  std::puts("d3d8_nargs_test PASS");
  return 0;
}
