// Import-coverage self-check (milestone M3 gate, see docs/PLAN.md).
//
// Loads the supported executable, binds every import through the runtime's HLE registry and prints
// the ones that have no implementation, so milestone M3 can be judged by a number instead of by a
// crash. Built by `-DBELLA_NO_GAME_CODE=ON` as the `import_coverage` target and run as
// `import_coverage work/ref/BellaSara.exe`.
//
// The runtime is linked here as the static `bella_runtime` library, so the HLE translation units
// (which register their exports from static initialisers) are pulled in by referencing the link
// anchors below. The platform layer is not part of the library; the few services the HLE modules
// reference are stubbed: the harness never creates a window or pumps input.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "core/rt.h"
#include "hle/com.h"
#include "d3d8/d3d8.h"
#include "platform/platform.h"

// Link anchors (runtime/src/hle): force each HLE translation unit to be kept in the static link so
// its static HLE registrars run before the import table is inspected.
void hle_link_kernel32();
void hle_link_kernel32_files();
void hle_link_user32();
void hle_link_advapi32();
void hle_link_dinput8();
void hle_link_fmod();


// The static runtime library expects the recompiled function table (normally emitted by the
// translator's C backend) and the dlmalloc mspace entry points (normally the `dlmalloc` target).
// This harness only binds imports, so neither is reached; provide them here so `import_coverage`
// links against libbella_runtime.a without the game code or the allocator.
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

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: import_coverage <BellaSara.exe>\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "import_coverage: cannot open %s\n", argv[1]);
    return 2;
  }
  std::vector<uint8_t> exe((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  // Force the HLE translation units to link (their static registrars populate the registry).
  hle_link_kernel32();
  hle_link_kernel32_files();
  hle_link_user32();
  hle_link_advapi32();
  hle_link_dinput8();
  hle_link_fmod();
  (void)d3d8::iface_d3d8();  // direct3d8.cpp: registers d3d8.dll!Direct3DCreate8

  rt::memory_init();
  rt::load_image(exe);

  std::vector<rt::ImportInfo> imports = rt::import_list();
  size_t implemented = 0;
  for (auto& im : imports)
    if (im.implemented) implemented++;

  std::printf("%zu imports: %zu implemented, %zu missing\n", imports.size(), implemented, imports.size() - implemented);
  for (auto& im : imports)
    if (!im.implemented) std::printf("%s!%s\n", im.dll.c_str(), im.name.c_str());
  return implemented == imports.size() ? 0 : 1;
}
