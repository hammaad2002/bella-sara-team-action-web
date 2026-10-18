// PE resource / STRINGTABLE self-test (milestone M6 first increment).
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/objects.h"
#include "core/resources.h"
#include "recomp.h"

// Stubs so this harness can link libbella_runtime.a without translated game code or dlmalloc.
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

  if (g_failures) {
    fprintf(stderr, "RESOURCES TEST FAIL (%d)\n", g_failures);
    return 1;
  }
  fprintf(stderr, "RESOURCES TEST PASS\n");
  return 0;
}
