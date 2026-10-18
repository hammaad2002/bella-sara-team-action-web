// PE resource / STRINGTABLE self-test (milestone M6 first increment).
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/objects.h"
#include "core/resources.h"
#include "core/rt.h"
#include "d3d8/d3d8.h"
#include "platform/platform.h"
#include "recomp.h"

namespace plat {
void get_display_size(int* width, int* height) { *width = 640, *height = 480; }
void get_window_size(int* width, int* height) { *width = 640, *height = 480; }
}  // namespace plat

// Stubs so this harness can link libbella_runtime.a without translated game code or libdlmalloc.
extern "C" {
const GuestFnEntry recomp_functions[1] = {{0, nullptr}};
const uint32_t recomp_function_count = 0;

typedef void* mspace;
namespace {
mspace g_test_mspace = reinterpret_cast<mspace>(1);
uint32_t g_test_heap_next = rt::kHeapBase;
void* test_heap_alloc(size_t bytes) {
  uint32_t align = 16;
  uint32_t a = (g_test_heap_next + align - 1) & ~(align - 1);
  if (a + bytes > rt::kHeapEnd) return nullptr;
  g_test_heap_next = a + (uint32_t)bytes;
  return rt::host(a);
}
}  // namespace
mspace create_mspace_with_base(void*, size_t, int) { return g_test_mspace; }
void* mspace_malloc(mspace m, size_t bytes) { return m ? test_heap_alloc(bytes) : nullptr; }
void* mspace_calloc(mspace m, size_t n, size_t sz) {
  size_t bytes = n * sz;
  void* p = mspace_malloc(m, bytes);
  if (p) memset(p, 0, bytes);
  return p;
}
void* mspace_realloc(mspace m, void* mem, size_t bytes) {
  if (!mem) return mspace_malloc(m, bytes);
  return mspace_malloc(m, bytes);
}
void mspace_free(mspace, void*) {}
}

static int g_failures = 0;

static void fail(const std::string& msg) {
  ++g_failures;
  fprintf(stderr, "FAIL %s\n", msg.c_str());
}

static void expect(bool cond, const std::string& msg) {
  if (!cond) fail(msg);
}

static bool sane_utf8(const std::string& s) {
  if (s.empty()) return false;
  for (unsigned char ch : s) {
    if (ch < 0x20 && ch != '\t') return false;
  }
  return true;
}

namespace {

// IDirect3DDevice8 vtable indices (prompt "slot N" is 1-based for IUnknown+methods).
constexpr unsigned kDevGetDeviceCaps = 7;                 // slot 8
constexpr unsigned kDevCreateAdditionalSwapChain = 13;    // slot 13
constexpr unsigned kDevSetRenderTarget = 31;
constexpr unsigned kDevGetRenderTarget = 32;
constexpr unsigned kDevGetDepthStencilSurface = 33;
constexpr unsigned kDevCreateRenderTarget = 25;
constexpr unsigned kD3d8CreateDevice = 15;

static uint32_t hle_call(Cpu* c, rt::HleFn fn, std::initializer_list<uint32_t> args) {
  uint32_t saved = c->esp;
  for (auto it = std::rbegin(args); it != std::rend(args); ++it) {
    c->esp -= 4;
    st32(c->esp, *it);
  }
  c->esp -= 4;
  st32(c->esp, rt::kReturnSentinel);
  fn(c);
  c->esp = saved;
  return c->eax;
}

static uint32_t com_call(Cpu* c, uint32_t obj, unsigned slot, std::initializer_list<uint32_t> args) {
  uint32_t fn = ld32(ld32(obj) + 4u * slot);
  std::vector<uint32_t> stack{obj};
  stack.insert(stack.end(), args);
  uint32_t saved = c->esp;
  for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
    c->esp -= 4;
    st32(c->esp, *it);
  }
  c->esp -= 4;
  st32(c->esp, rt::kReturnSentinel);
  rt_dispatch(c, fn);
  c->esp = saved;
  return c->eax;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: resources_test <install-dir>\n");
    return 1;
  }
  rt::vfs_init(argv[1]);

  std::string err;
  if (!rt::resources_open(&err)) {
    fail("resources_open: " + err);
    fprintf(stderr, "RESOURCES TEST FAIL (%d)\n", g_failures);
    return 1;
  }

  uint32_t dir_rva = 0, dir_sz = 0;
  expect(rt::resources_pe_resource_directory(&dir_rva, &dir_sz), "resource directory present");
  size_t rsrc_off = 0;
  expect(rt::resources_pe_rva_to_offset(dir_rva, &rsrc_off), "resource directory RVA maps");
  uint32_t rva_back = 0;
  expect(rt::resources_pe_offset_to_rva(rsrc_off, &rva_back) && rva_back == dir_rva,
         "resource directory RVA round-trip");

  std::vector<rt::ResourcesStringTable> tables;
  rt::resources_foreach_string_table([&](const rt::ResourcesStringTable& t) { tables.push_back(t); });
  expect(!tables.empty(), "at least one RT_STRING block");

  for (const rt::ResourcesStringTable& t : tables) {
    uint32_t rva = 0;
    expect(rt::resources_pe_offset_to_rva(t.offset, &rva), "string block offset -> RVA");
    size_t off2 = 0;
    expect(rt::resources_pe_rva_to_offset(rva, &off2) && off2 == t.offset, "string block RVA -> offset");
    expect(t.size > 0 && t.size <= 65536, "string block size sane");
    expect(t.offset + t.size <= 4000000, "string block bounds within image file");
  }

  size_t string_count = 0;
  std::vector<std::pair<uint32_t, std::string>> samples;
  rt::resources_foreach_string([&](uint32_t id, const std::string& utf8) {
    ++string_count;
    expect(sane_utf8(utf8), "string id " + std::to_string(id) + " sane text");
    if (samples.size() < 8) samples.emplace_back(id, utf8);
  });
  expect(string_count >= 2, "non-trivial STRINGTABLE count");
  fprintf(stderr, "resources_test: %zu STRINGTABLE blocks, %zu strings\n", tables.size(), string_count);
  for (const auto& [id, s] : samples) fprintf(stderr, "  sample id %u: \"%s\"\n", id, s.c_str());

  // load_string semantics on real ids.
  char buf[256];
  int n = rt::load_string(117, buf, (int)sizeof(buf));
  expect(n == 10 && std::string(buf) == "Bella Sara", "load_string(117) full copy");
  expect(rt::load_string(118, buf, (int)sizeof(buf)) == 11 && std::string(buf) == "EQ_RESOURCE",
         "load_string(118)");

  expect(rt::load_string(1, buf, (int)sizeof(buf)) == 0, "absent id returns 0");
  expect(rt::load_string(117, nullptr, 0) == 10, "maxlen==0 returns length without copy");

  char tiny[5];
  int tn = rt::load_string(117, tiny, 5);
  expect(tn == 4 && std::string(tiny) == "Bell", "truncation returns maxlen-1 and NUL-terminates");

  rt::ResourceTypeCounts counts{};
  expect(rt::resources_type_counts(&counts), "resource type counts");
  expect(counts.icons == 2, "RT_ICON count");
  expect(counts.group_icons == 1, "RT_GROUP_ICON count");
  expect(counts.cursors == 0, "RT_CURSOR count");
  expect(counts.group_cursors == 0, "RT_GROUP_CURSOR count");
  expect(counts.menus == 1, "RT_MENU count");
  expect(counts.dialogs == 1, "RT_DIALOG count");
  expect(counts.string_blocks == 1, "RT_STRING block count");
  expect(counts.versions == 1, "RT_VERSION count");
  expect(rt::resource_has_group_icon(108), "group icon id 108");
  expect(rt::resource_has_menu(105), "menu id 105");
  expect(rt::resource_has_dialog(106), "dialog id 106");
  expect(rt::resource_icon_handle(108) != 0, "LoadIcon-style handle for group 108");
  expect(rt::resource_stock_cursor_handle(0x7f00) != 0, "IDC_ARROW stock cursor");
  fprintf(stderr,
          "resources_test: icons=%zu group_icons=%zu menus=%zu dialogs=%zu versions=%zu\n",
          counts.icons, counts.group_icons, counts.menus, counts.dialogs, counts.versions);

  // D3D8 device regressions (Caps2 windowed, SetRenderTarget depth unbind, swap-chain pp writeback).
  rt::memory_init();
  rt::dispatch_init();
  (void)d3d8::iface_d3d8();
  rt::GuestThread* gt = rt::create_guest_thread(0);
  Cpu* cpu = &gt->cpu;
  rt::HleFn direct3d_create8 = rt::hle_find("d3d8.dll", "Direct3DCreate8");
  expect(direct3d_create8 != nullptr, "Direct3DCreate8 registered");
  uint32_t d3d8_obj = direct3d_create8 ? hle_call(cpu, direct3d_create8, {d3d8::kD3d8SdkVersion}) : 0;
  expect(d3d8_obj != 0, "Direct3DCreate8");

  uint32_t pp = rt::heap_alloc(rt::process_heap(), 52, true);
  st32(pp + 0, 640);
  st32(pp + 4, 480);
  st32(pp + 8, d3d::FMT_X8R8G8B8);
  st32(pp + 28, 1);  // Windowed
  st32(pp + 32, 1);  // EnableAutoDepthStencil
  st32(pp + 36, d3d::FMT_D24S8);
  uint32_t dev_out = rt::heap_alloc(rt::process_heap(), 4, true);
  uint32_t dev = 0;
  if (d3d8_obj) {
    expect(com_call(cpu, d3d8_obj, kD3d8CreateDevice, {0, 1, 0x100u, 0, pp, dev_out}) == 0, "CreateDevice");
    dev = ld32(dev_out);
  }
  expect(dev != 0, "IDirect3DDevice8");

  uint32_t caps_buf = rt::heap_alloc(rt::process_heap(), 304, true);
  uint32_t caps_hr = dev ? com_call(cpu, dev, kDevGetDeviceCaps, {caps_buf}) : 1;
  expect(caps_hr == 0, "GetDeviceCaps hr");
  expect((ld32(caps_buf + 12) & 0x00080000u) != 0, "GetDeviceCaps Caps2 CANRENDERWINDOWED");

  uint32_t ds_ptr = rt::heap_alloc(rt::process_heap(), 4, true);
  uint32_t rt_ptr = rt::heap_alloc(rt::process_heap(), 4, true);
  uint32_t bb_ptr = rt::heap_alloc(rt::process_heap(), 4, true);
  uint32_t orig_ds = 0, orig_bb = 0, rt_surf = 0;
  if (dev) {
    expect(com_call(cpu, dev, kDevGetDepthStencilSurface, {ds_ptr}) == 0, "GetDepthStencilSurface orig");
    orig_ds = ld32(ds_ptr);
    expect(orig_ds != 0, "depth-stencil present");
    expect(com_call(cpu, dev, kDevGetRenderTarget, {bb_ptr}) == 0, "GetRenderTarget orig_bb");
    orig_bb = ld32(bb_ptr);
    expect(com_call(cpu, dev, kDevCreateRenderTarget, {128, 128, d3d::FMT_X8R8G8B8, 0, 0, rt_ptr}) == 0,
           "CreateRenderTarget");
    rt_surf = ld32(rt_ptr);
    expect(rt_surf != 0, "rt_surf");

    expect(com_call(cpu, dev, kDevSetRenderTarget, {rt_surf, 0}) == 0, "SetRenderTarget(tex,0)");
    st32(ds_ptr, 0);
    uint32_t ds_hr = com_call(cpu, dev, kDevGetDepthStencilSurface, {ds_ptr});
    expect(ds_hr == d3d::D3DERR_NOTFOUND, "depth unbound after SetRenderTarget(tex,0)");
    expect(ld32(ds_ptr) == 0, "GetDepthStencilSurface out zero");

    expect(com_call(cpu, dev, kDevSetRenderTarget, {0, orig_ds}) == 0, "SetRenderTarget(0,orig_ds)");
    st32(rt_ptr, 0);
    expect(com_call(cpu, dev, kDevGetRenderTarget, {rt_ptr}) == 0, "GetRenderTarget after depth-only set");
    expect(ld32(rt_ptr) == rt_surf, "color RT preserved when pRenderTarget==0");
    st32(ds_ptr, 0);
    expect(com_call(cpu, dev, kDevGetDepthStencilSurface, {ds_ptr}) == 0, "GetDepthStencilSurface restored");
    expect(ld32(ds_ptr) == orig_ds, "depth-stencil restored");

    expect(com_call(cpu, dev, kDevSetRenderTarget, {orig_bb, orig_ds}) == 0, "SetRenderTarget restore");
  }

  uint32_t sc_pp = rt::heap_alloc(rt::process_heap(), 52, true);
  memset(rt::host(sc_pp), 0, 52);
  uint32_t sc_out = rt::heap_alloc(rt::process_heap(), 4, true);
  if (dev) {
    expect(com_call(cpu, dev, kDevCreateAdditionalSwapChain, {sc_pp, sc_out}) == 0,
           "CreateAdditionalSwapChain");
    expect(ld32(sc_pp + 0) == 640, "swap chain pp BackBufferWidth");
    expect(ld32(sc_pp + 4) == 480, "swap chain pp BackBufferHeight");
    expect(ld32(sc_pp + 8) == d3d::FMT_X8R8G8B8, "swap chain pp BackBufferFormat");
  }

  if (g_failures) {
    fprintf(stderr, "RESOURCES TEST FAIL (%d)\n", g_failures);
    return 1;
  }
  fprintf(stderr, "RESOURCES TEST PASS\n");
  return 0;
}
