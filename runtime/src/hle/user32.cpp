// user32.dll (+ gdi32 and winmm bits): window classes, one game window, and a message queue fed
// by the platform layer.
#include <chrono>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "../core/objects.h"
#include "../core/resources.h"
#include "../platform/platform.h"

using namespace rt;

// Link anchor: forces this translation unit (and its HLE registrars) to be kept in a static link.
void hle_link_user32() {}

namespace d3d {
void set_app_active(bool active);  // d3d8/state.cpp: device loss on deactivation (device: wave 2)
}

#ifndef __EMSCRIPTEN__
namespace plat {
__attribute__((weak)) void mouse_pos(int* x, int* y) {
  if (x) *x = 0;
  if (y) *y = 0;
}
__attribute__((weak)) void set_mouse_pos(int, int) {}
__attribute__((weak)) bool mouse_button_down(int) { return false; }
}  // namespace plat
#endif

namespace {
const char* kDll = "user32.dll";

enum : uint32_t {
  WM_NULL = 0x0000, WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_MOVE = 0x0003, WM_SIZE = 0x0005, WM_ACTIVATE = 0x0006,
  WM_SETFOCUS = 0x0007, WM_KILLFOCUS = 0x0008, WM_CLOSE = 0x0010, WM_QUIT = 0x0012, WM_SHOWWINDOW = 0x0018,
  WM_ACTIVATEAPP = 0x001c, WM_GETMINMAXINFO = 0x0024, WM_WINDOWPOSCHANGING = 0x0046, WM_WINDOWPOSCHANGED = 0x0047,
  WM_DISPLAYCHANGE = 0x007e, WM_NCCREATE = 0x0081, WM_NCCALCSIZE = 0x0083, WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101,
  WM_CHAR = 0x0102, WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105, WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201,
  WM_LBUTTONUP = 0x0202, WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205, WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208,
  WM_MOUSEWHEEL = 0x020a,
};

enum : uint32_t {
  SWP_NOSIZE = 0x0001, SWP_NOMOVE = 0x0002, SWP_NOZORDER = 0x0004, SWP_NOACTIVATE = 0x0010, SWP_FRAMECHANGED = 0x0020,
  SWP_SHOWWINDOW = 0x0040, SWP_HIDEWINDOW = 0x0080,
};

struct WindowClass {
  uint32_t wndproc;
  std::string name;
};
struct Msg {
  uint32_t hwnd, message, wparam, lparam;
};

std::mutex g_mutex;
std::map<std::string, WindowClass> g_classes;
uint32_t g_hwnd = 0, g_wndproc = 0;
std::string g_title;
int g_width = 640, g_height = 480;
std::deque<Msg> g_queue;
int g_cursor_count = 0;
bool g_window_visible = false;
int g_win_x = 0, g_win_y = 0;
const auto g_start = std::chrono::steady_clock::now();

// Which user32 HLE entry is on the stack (for WM trace correlation).
thread_local const char* g_hle_ctx = "";
thread_local int g_hle_depth = 0;

struct HleScope {
  const char* prev;
  int prev_depth;
  HleScope(const char* name) : prev(g_hle_ctx), prev_depth(g_hle_depth) {
    g_hle_ctx = name;
    g_hle_depth++;
  }
  ~HleScope() {
    g_hle_ctx = prev;
    g_hle_depth = prev_depth;
  }
};

std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

uint32_t resource_id_from_name(uint32_t name_ptr) {
  if (!name_ptr) return 0;
  if (name_ptr < 0x10000u) return name_ptr;
  const std::string s = read_cstr(name_ptr);
  if (s.empty()) return 0;
  char* end = nullptr;
  long v = strtol(s.c_str(), &end, 10);
  if (end && *end == '\0' && v > 0) return (uint32_t)v;
  return 0;
}

uint32_t now_ms() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count();
}

uint32_t send(Cpu* c, uint32_t msg, uint32_t wparam, uint32_t lparam) {
  if (!g_wndproc) return 0;
  static const bool trace = rt::trace_enabled("wm");
  if (trace) {
    rt::log(rt::Log::Info, "wm: send %04x w=%08x l=%08x depth=%d ctx=%s", msg, wparam, lparam, g_hle_depth,
            g_hle_ctx ? g_hle_ctx : "");
  }
  return guest_call(c, g_wndproc, {g_hwnd, msg, wparam, lparam});
}

uint32_t make_size_lparam() { return (uint32_t)g_height << 16 | (uint32_t)g_width; }

void init_minmaxinfo(uint32_t mmi) {
  memset(host(mmi), 0, 40);
  st32(mmi + 16, 0);
  st32(mmi + 20, 0);
  st32(mmi + 24, (uint32_t)g_width);
  st32(mmi + 28, (uint32_t)g_height);
  int sw = 0, sh = 0;
  plat::get_display_size(&sw, &sh);
  st32(mmi + 32, (uint32_t)sw);
  st32(mmi + 36, (uint32_t)sh);
}

// Synchronous WM_WINDOWPOS* + WM_SIZE/WM_MOVE (and optional WM_SHOWWINDOW) like user32 SetWindowPos.
void apply_window_pos(Cpu* c, int x, int y, int cx, int cy, uint32_t flags) {
  if (!g_hwnd) return;
  const bool move = !(flags & SWP_NOMOVE);
  const bool size = !(flags & SWP_NOSIZE);
  const bool show = (flags & SWP_SHOWWINDOW) != 0;
  const bool hide = (flags & SWP_HIDEWINDOW) != 0;
  if (move) {
    g_win_x = x;
    g_win_y = y;
  }
  if (size && cx > 0 && cy > 0) {
    g_width = cx;
    g_height = cy;
  }

  uint32_t wp = sys_alloc(28);
  st32(wp + 0, g_hwnd);
  st32(wp + 4, 0);
  st32(wp + 8, (uint32_t)g_win_x);
  st32(wp + 12, (uint32_t)g_win_y);
  st32(wp + 16, (uint32_t)g_width);
  st32(wp + 20, (uint32_t)g_height);
  st32(wp + 24, flags);

  send(c, WM_WINDOWPOSCHANGING, 0, wp);
  send(c, WM_WINDOWPOSCHANGED, 0, wp);

  if (show && !g_window_visible) {
    g_window_visible = true;
    send(c, WM_SHOWWINDOW, 1, 0);
  }
  if (hide && g_window_visible) {
    g_window_visible = false;
    send(c, WM_SHOWWINDOW, 0, 0);
  }
  if (size) send(c, WM_SIZE, 0, make_size_lparam());
  if (move) send(c, WM_MOVE, 0, (uint32_t)g_win_y << 16 | (uint32_t)(g_win_x & 0xffff));
}

void post(uint32_t msg, uint32_t wparam, uint32_t lparam) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_queue.push_back({g_hwnd, msg, wparam, lparam});
}

// Translate platform input into window messages.
void pump() {
  static const bool trace = rt::trace_enabled("input");
  std::vector<plat::InputEvent> evs;
  plat::poll_events(evs);
  for (auto& e : evs) {
    uint32_t keyl = 1u | (e.scancode & 0xff) << 16 | ((e.scancode & 0x100) ? 1u << 24 : 0u);
    if (trace && e.kind != plat::InputEvent::MouseMove)
      rt::log(rt::Log::Info, "input: event %d vk=0x%x scan=0x%x ch=%u button=%u", (int)e.kind, e.vk, e.scancode, e.ch, e.button);
    switch (e.kind) {
      case plat::InputEvent::KeyDown:
        post(e.vk == 0x12 ? WM_SYSKEYDOWN : WM_KEYDOWN, e.vk, keyl | (e.repeat ? 1u << 30 : 0u));
        break;
      case plat::InputEvent::KeyUp:
        post(e.vk == 0x12 ? WM_SYSKEYUP : WM_KEYUP, e.vk, keyl | 3u << 30);
        break;
      case plat::InputEvent::Char:
        post(WM_CHAR, e.ch, 1);
        break;
      case plat::InputEvent::MouseMove:
        post(WM_MOUSEMOVE, 0, (uint32_t)(e.y & 0xffff) << 16 | (uint32_t)(e.x & 0xffff));
        break;
      case plat::InputEvent::MouseDown:
      case plat::InputEvent::MouseUp: {
        static const uint32_t down[] = {WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN};
        static const uint32_t up[] = {WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP};
        if (e.button < 3)
          post(e.kind == plat::InputEvent::MouseDown ? down[e.button] : up[e.button], 0,
               (uint32_t)(e.y & 0xffff) << 16 | (uint32_t)(e.x & 0xffff));
        break;
      }
      case plat::InputEvent::MouseWheel:
        post(WM_MOUSEWHEEL, (uint32_t)(e.dy * 120) << 16, 0);
        break;
      case plat::InputEvent::FocusGained:
        d3d::set_app_active(true);
        post(WM_ACTIVATEAPP, 1, 0);
        post(WM_ACTIVATE, 1, 0);
        break;
      case plat::InputEvent::FocusLost:
        d3d::set_app_active(false);
        post(WM_ACTIVATE, 0, 0);
        post(WM_ACTIVATEAPP, 0, 0);
        break;
      case plat::InputEvent::Quit:
        post(WM_CLOSE, 0, 0);
        break;
    }
  }
}
}  // namespace

namespace rt {
void user32_pump() { pump(); }
}  // namespace rt

namespace {

HLE_STDCALL(kDll, RegisterClassExA, 1) {
  uint32_t wc = arg(c, 0);
  std::string name = read_cstr(ld32(wc + 40));
  std::lock_guard<std::mutex> lock(g_mutex);
  g_classes[lower(name)] = {ld32(wc + 8), name};
  log(Log::Info, "RegisterClassExA(\"%s\", wndproc=%08x)", name.c_str(), ld32(wc + 8));
  return 0xc000 + (uint32_t)g_classes.size();
}

static bool g_size_sent = false;

HLE_STDCALL(kDll, CreateWindowExA, 12) {
  HleScope scope("CreateWindowExA");
  std::string cls = lower(read_cstr(arg(c, 1)));
  std::string title = arg(c, 2) ? read_cstr(arg(c, 2)) : "";
  int w = (int)arg(c, 6), h = (int)arg(c, 7);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_classes.find(cls);
    if (it == g_classes.end()) {
      set_last_error(1407);  // ERROR_CANNOT_FIND_WND_CLASS
      return 0;
    }
    if (g_hwnd) fatal("the game created a second window");
    g_hwnd = 0x00010010;
    g_wndproc = it->second.wndproc;
    g_title = title;
    g_width = w > 0 && w < 10000 ? w : 640;
    g_height = h > 0 && h < 10000 ? h : 480;
  }
  log(Log::Info, "CreateWindowExA(\"%s\", \"%s\", %dx%d)", cls.c_str(), title.c_str(), g_width, g_height);
  plat::create_window(title, g_width, g_height);
  // CREATESTRUCTA for WM_NCCREATE/WM_CREATE.
  uint32_t cs = sys_alloc(48);
  st32(cs + 0, arg(c, 11));  // lpCreateParams
  st32(cs + 4, arg(c, 10));  // hInstance
  st32(cs + 8, arg(c, 9));   // hMenu
  st32(cs + 12, arg(c, 8));  // hwndParent
  st32(cs + 16, (uint32_t)g_height);
  st32(cs + 20, (uint32_t)g_width);
  st32(cs + 24, arg(c, 5));
  st32(cs + 28, arg(c, 4));
  st32(cs + 32, arg(c, 3));  // style
  st32(cs + 36, arg(c, 2));  // name
  st32(cs + 40, arg(c, 1));  // class
  st32(cs + 44, arg(c, 0));  // exstyle
  uint32_t mmi = sys_alloc(40);
  init_minmaxinfo(mmi);
  send(c, WM_GETMINMAXINFO, 0, mmi);
  if (!send(c, WM_NCCREATE, 0, cs)) log(Log::Warn, "WM_NCCREATE returned FALSE (continuing)");
  send(c, WM_NCCALCSIZE, 1, cs);  // wParam TRUE: calculate new client area on create
  if ((int32_t)send(c, WM_CREATE, 0, cs) == -1) {
    g_hwnd = 0;
    return 0;
  }
  const bool visible_at_create = (arg(c, 3) & 0x10000000u) != 0;
  g_window_visible = visible_at_create;
  if (visible_at_create) {
    send(c, WM_SHOWWINDOW, 1, 0);
    send(c, WM_SIZE, 0, make_size_lparam());
    send(c, WM_MOVE, 0, (uint32_t)g_win_y << 16 | (uint32_t)(g_win_x & 0xffff));
    g_size_sent = true;
  }
  return g_hwnd;
}

HLE_STDCALL(kDll, ShowWindow, 2) {
  HleScope scope("ShowWindow");
  uint32_t cmd = arg(c, 1);
  if (cmd == 0) {  // SW_HIDE
    if (g_window_visible) apply_window_pos(c, g_win_x, g_win_y, g_width, g_height, SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
    return 0;
  }
  uint32_t flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW;
  if (!g_size_sent) {
    flags &= ~SWP_NOSIZE;
    g_size_sent = true;
  }
  apply_window_pos(c, g_win_x, g_win_y, g_width, g_height, flags);
  return g_window_visible ? 1 : 0;
}

HLE_STDCALL(kDll, PeekMessageA, 5) {
  HleScope scope("PeekMessageA");
  uint32_t msg = arg(c, 0), remove = arg(c, 4) & 1;
  pump();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_queue.empty()) return 0;
  Msg m = g_queue.front();
  if (remove) g_queue.pop_front();
  st32(msg + 0, m.hwnd);
  st32(msg + 4, m.message);
  st32(msg + 8, m.wparam);
  st32(msg + 12, m.lparam);
  st32(msg + 16, now_ms());
  st32(msg + 20, 0);
  st32(msg + 24, 0);
  return 1;
}

HLE_STDCALL(kDll, TranslateMessage, 1) { return 0; }

HLE_STDCALL(kDll, DispatchMessageA, 1) {
  HleScope scope("DispatchMessageA");
  uint32_t m = arg(c, 0);
  if (ld32(m + 4) == WM_QUIT) return 0;
  return send(c, ld32(m + 4), ld32(m + 8), ld32(m + 12));
}

HLE_STDCALL(kDll, DefWindowProcA, 4) {
  uint32_t msg = arg(c, 1);
  switch (msg) {
    case WM_NCCREATE:
      return 1;
    case WM_CLOSE:
      send(c, WM_DESTROY, 0, 0);
      return 0;
    default:
      return 0;
  }
}

HLE_STDCALL(kDll, PostQuitMessage, 1) {
  post(WM_QUIT, arg(c, 0), 0);
  return 0;
}

HLE_STDCALL(kDll, ShowCursor, 1) {
  g_cursor_count += arg(c, 0) ? 1 : -1;
  plat::show_cursor(g_cursor_count >= 0);
  return (uint32_t)g_cursor_count;
}

HLE_STDCALL(kDll, GetSystemMetrics, 1) {
  int w = 0, h = 0;
  plat::get_display_size(&w, &h);
  switch (arg(c, 0)) {
    case 0: return (uint32_t)w;  // SM_CXSCREEN
    case 1: return (uint32_t)h;  // SM_CYSCREEN
    default: return 0;
  }
}

HLE_STDCALL(kDll, AdjustWindowRect, 3) { return 1; }

HLE_STDCALL(kDll, SystemParametersInfoA, 4) {
  uint32_t action = arg(c, 0), ui = arg(c, 1), pv = arg(c, 2);
  // Accessibility key queries (sticky/toggle/filter keys) get "feature off" structures.
  if ((action == 0x3a || action == 0x34 || action == 0x32) && pv) {
    uint32_t size = ld32(pv) ? ld32(pv) : ui;
    if (size > 4 && size < 64) {
      memset(host(pv + 4), 0, size - 4);
    }
  }
  return 1;
}

HLE_STDCALL(kDll, FindWindowExA, 4) { return 0; }
HLE_STDCALL(kDll, BringWindowToTop, 1) { return 1; }
HLE_STDCALL(kDll, SetForegroundWindow, 1) { return 1; }
HLE_STDCALL(kDll, InvalidateRect, 3) { return 1; }
HLE_STDCALL(kDll, ValidateRect, 2) { return 1; }
HLE_STDCALL(kDll, LoadIconA, 2) {
  uint32_t id = resource_id_from_name(arg(c, 1));
  if (!resources_open(nullptr)) return 0;
  uint32_t h = resource_icon_handle(id);
  return h ? h : 0;
}
HLE_STDCALL(kDll, GetActiveWindow, 0) { return g_hwnd; }

// MAPVK_VK_TO_VSC (0) and MAPVK_VSC_TO_VK (1) for the keys games use.
static uint32_t map_virtual_key(uint32_t code, uint32_t type) {
  static const uint8_t vk_to_sc[256] = {
      0,    0,    0,    0x46, 0,    0,    0,    0,    0x0e, 0x0f, 0,    0,    0x4c, 0x1c, 0,    0,     // 00
      0x2a, 0x1d, 0x38, 0,    0x3a, 0,    0,    0,    0,    0,    0,    0x01, 0,    0,    0,    0,     // 10
      0x39, 0x49, 0x51, 0x4f, 0x47, 0x4b, 0x48, 0x4d, 0x50, 0,    0,    0,    0x54, 0x52, 0x53, 0,     // 20
      0x0b, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0,    0,    0,    0,    0,    0,     // 30
      0,    0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,  // 40
      0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c, 0x5b, 0x5c, 0x5d, 0,    0,     // 50
      0x52, 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49, 0x37, 0x4e, 0,    0x4a, 0x53, 0x35,  // 60
      0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58, 0,    0,    0,    0,     // 70
      0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,     // 80
      0x45, 0x46, 0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,     // 90
      0x2a, 0x36, 0x1d, 0x1d, 0x38, 0x38, 0,    0,    0,    0,    0,    0,    0,    0,    0,    0,     // a0
      0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x27, 0x0d, 0x33, 0x0c, 0x34, 0x35,  // b0
      0x29, 0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,     // c0
      0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x1a, 0x2b, 0x1b, 0x28, 0,     // d0
      0,    0,    0x56, 0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,     // e0
      0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0};    // f0
  if (type == 0) return vk_to_sc[code & 0xff];
  if (type == 1 || type == 3) {
    for (int vk = 0; vk < 256; vk++)
      if (vk_to_sc[vk] == (code & 0xff) && vk >= 0x08) return (uint32_t)vk;
    return 0;
  }
  return 0;
}

HLE_STDCALL(kDll, MapVirtualKeyA, 2) { return map_virtual_key(arg(c, 0), arg(c, 1)); }
HLE_STDCALL(kDll, MapVirtualKeyExA, 3) { return map_virtual_key(arg(c, 0), arg(c, 1)); }

// ---- Window geometry, state and menus ------------------------------------------------------

HLE_STDCALL(kDll, IsIconic, 1) { return 0; }
HLE_STDCALL(kDll, SetWindowLongA, 3) { return 0; }  // previous value
HLE_STDCALL(kDll, GetWindowLongA, 2) { return 0; }
HLE_STDCALL(kDll, GetMenu, 1) { return 0; }
HLE_STDCALL(kDll, SetMenu, 2) { return 1; }
HLE_STDCALL(kDll, AdjustWindowRectEx, 4) { return 1; }
HLE_STDCALL(kDll, SetWindowPos, 7) {
  HleScope scope("SetWindowPos");
  if (arg(c, 0) != g_hwnd) return 0;
  apply_window_pos(c, (int)arg(c, 2), (int)arg(c, 3), (int)arg(c, 4), (int)arg(c, 5), arg(c, 6));
  return 1;
}
HLE_STDCALL(kDll, SetFocus, 1) {
  HleScope scope("SetFocus");
  if (arg(c, 0) == g_hwnd) send(c, WM_SETFOCUS, 0, 0);
  return 0;
}
HLE_STDCALL(kDll, UpdateWindow, 1) { return 1; }
HLE_STDCALL(kDll, IsWindowEnabled, 1) { return 1; }
HLE_STDCALL(kDll, IsWindowVisible, 1) { return 1; }
HLE_STDCALL(kDll, SetCursorPos, 2) {
  plat::set_mouse_pos((int)arg(c, 0), (int)arg(c, 1));
  return 1;
}

HLE_STDCALL(kDll, GetCursorPos, 1) {
  int x = 0, y = 0;
  plat::mouse_pos(&x, &y);
  uint32_t p = arg(c, 0);
  st32(p + 0, (uint32_t)x);
  st32(p + 4, (uint32_t)y);
  return 1;
}

HLE_STDCALL(kDll, ScreenToClient, 2) {
  uint32_t pt = arg(c, 1);
  st32(pt + 0, ld32(pt + 0));
  st32(pt + 4, ld32(pt + 4));
  return 1;
}

HLE_STDCALL(kDll, ClientToScreen, 2) {
  uint32_t pt = arg(c, 1);
  st32(pt + 0, ld32(pt + 0));
  st32(pt + 4, ld32(pt + 4));
  return 1;
}

HLE_STDCALL(kDll, GetClientRect, 2) {
  uint32_t r = arg(c, 1);
  st32(r + 0, 0), st32(r + 4, 0);
  st32(r + 8, (uint32_t)g_width), st32(r + 12, (uint32_t)g_height);
  return 1;
}

// The window fills the client area (the game runs fullscreen/windowed without decorations).
HLE_STDCALL(kDll, GetWindowRect, 2) {
  uint32_t r = arg(c, 1);
  st32(r + 0, 0), st32(r + 4, 0);
  st32(r + 8, (uint32_t)g_width), st32(r + 12, (uint32_t)g_height);
  return 1;
}

HLE_STDCALL(kDll, FindWindowA, 2) {
  std::string cls = arg(c, 0) ? lower(read_cstr(arg(c, 0))) : "";
  std::string title = arg(c, 1) ? read_cstr(arg(c, 1)) : "";
  if (!cls.empty()) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_classes.count(cls)) return 0;
  }
  if (!title.empty() && title != g_title) return 0;
  return g_hwnd;
}

HLE_STDCALL(kDll, LoadCursorA, 2) {
  uint32_t id = resource_id_from_name(arg(c, 1));
  if (!resources_open(nullptr)) return 0;
  uint32_t h = resource_cursor_handle(id);
  if (h) return h;
  return resource_stock_cursor_handle(id);
}

HLE_STDCALL(kDll, LoadStringA, 4) {
  uint32_t id = arg(c, 1);
  uint32_t buf = arg(c, 2);
  int maxlen = (int)arg(c, 3);
  if (maxlen < 0) return 0;
  std::string err;
  if (!resources_open(&err)) {
    static bool once = false;
    if (!once) {
      once = true;
      log(Log::Warn, "LoadStringA: resources_open failed: %s", err.c_str());
    }
    return 0;
  }
  if (maxlen == 0) return load_string(id, nullptr, 0);
  if (!buf) return 0;
  return load_string(id, (char*)host(buf), maxlen);
}

// RegisterWindowMessageA: stable ids in the 0xC000..0xFFFF application range.
HLE_STDCALL(kDll, RegisterWindowMessageA, 1) {
  uint32_t h = 0xc000;
  for (char ch : read_cstr(arg(c, 0))) h = h * 31 + (uint8_t)ch;
  return 0xc000 + (h & 0x3fff);
}

// ---- Message loop helpers ------------------------------------------------------------------

HLE_STDCALL(kDll, SendMessageA, 4) {
  HleScope scope("SendMessageA");
  return send(c, arg(c, 1), arg(c, 2), arg(c, 3));
}

HLE_STDCALL(kDll, CallWindowProcA, 5) {
  uint32_t proc = arg(c, 0) ? arg(c, 0) : g_wndproc;
  if (!proc) return 0;
  return guest_call(c, proc, {arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)});
}

HLE_STDCALL(kDll, PostThreadMessageA, 4) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_queue.push_back({0, arg(c, 1), arg(c, 2), arg(c, 3)});
  return 1;
}

HLE_STDCALL(kDll, GetQueueStatus, 1) {
  pump();
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_queue.empty() ? 0 : 0x04ff;  // QS_ALLINPUT
}

// Wait for messages too: return WAIT_OBJECT_0 + nCount when the queue has messages, else poll the
// handle array like WaitForMultipleObjects.
HLE_STDCALL(kDll, MsgWaitForMultipleObjects, 5) {
  uint32_t n = arg(c, 0), ptr = arg(c, 1), timeout = arg(c, 3);
  pump();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_queue.empty()) return n;  // WAIT_OBJECT_0 + nCount
  }
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
  for (;;) {
    for (uint32_t i = 0; i < n; i++) {
      if (auto o = handle_get(ld32(ptr + 4 * i)))
        if (o->wait(0)) return i;  // WAIT_OBJECT_0 + i
    }
    pump();
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (!g_queue.empty()) return n;
    }
    if (timeout != 0xffffffffu && std::chrono::steady_clock::now() >= deadline) return 0x102;  // WAIT_TIMEOUT
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// ---- Dialogs, DCs and simple input ---------------------------------------------------------

HLE_STDCALL(kDll, DialogBoxParamA, 5) {
  uint32_t template_id = resource_id_from_name(arg(c, 1));
  uint32_t dlg_proc = arg(c, 3);
  uint32_t init_param = arg(c, 4);
  if (!template_id || !dlg_proc) return 0xffffffffu;
  uint32_t dlg_hwnd = 0x00030000u | (template_id & 0xffffu);
  int result = dialog_box_modal(c, template_id, dlg_hwnd, dlg_proc, init_param);
  if (result < 0) return 0xffffffffu;
  return (uint32_t)result;
}

HLE_STDCALL(kDll, EndDialog, 2) { return 1; }
HLE_STDCALL(kDll, GetDlgItemTextA, 4) {
  uint32_t hwnd = arg(c, 0);
  uint32_t item = arg(c, 1);
  uint32_t buf = arg(c, 2);
  int maxlen = (int)arg(c, 3);
  if (maxlen < 0 || !buf) return 0;
  return get_dlg_item_text(hwnd, item, (char*)host(buf), maxlen);
}

HLE_STDCALL(kDll, MessageBoxA, 4) {
  std::string text = arg(c, 1) ? read_cstr(arg(c, 1)) : "";
  std::string caption = arg(c, 2) ? read_cstr(arg(c, 2)) : "";
  log(Log::Info, "MessageBoxA(\"%s\", \"%s\", %08x) -> IDOK", caption.c_str(), text.c_str(), arg(c, 3));
  return 1;  // IDOK
}

// Fake device contexts: enough for the game's GDI text/measurement paths until M4.
HLE_STDCALL(kDll, GetDC, 1) { return 0x00010020; }
HLE_STDCALL(kDll, ReleaseDC, 2) { return 1; }

HLE_STDCALL(kDll, BeginPaint, 2) {
  uint32_t ps = arg(c, 1);
  memset(host(ps), 0, 64);
  st32(ps + 0, 0x00010020);  // hdc
  st32(ps + 8, 0), st32(ps + 12, 0);                            // rcPaint.left/top
  st32(ps + 16, (uint32_t)g_width), st32(ps + 20, (uint32_t)g_height);  // rcPaint.right/bottom
  return 0x00010020;
}

HLE_STDCALL(kDll, EndPaint, 2) { return 1; }

HLE_STDCALL(kDll, GetKeyboardLayout, 1) { return 0x04090409; }  // US English

static bool key_or_mouse_down(uint32_t vk) {
  pump();
  if (vk == 1 || vk == 2 || vk == 4) return plat::mouse_button_down(vk == 1 ? 0 : vk == 2 ? 1 : 2);
  uint32_t sc = map_virtual_key(vk, 0);
  return sc && sc < 256 && (plat::keyboard_state()[sc] & 0x80);
}

HLE_STDCALL(kDll, GetAsyncKeyState, 1) { return key_or_mouse_down(arg(c, 0)) ? 0x8000u : 0; }

HLE_STDCALL(kDll, GetKeyState, 1) { return key_or_mouse_down(arg(c, 0)) ? (int32_t)0x8000 : 0; }

// gdi32, shell32 and winmm.
HLE_STDCALL("gdi32.dll", DeleteObject, 1) { return 1; }
HLE_STDCALL("gdi32.dll", GetDeviceCaps, 2) {
  int w = 640, h = 480;
  plat::get_display_size(&w, &h);
  switch (arg(c, 1)) {
    case 8: return (uint32_t)w;        // HORZRES
    case 10: return (uint32_t)h;       // VERTRES
    case 12: return 32;                // BITSPIXEL
    case 14: return 1;                 // PLANES
    case 24: return (uint32_t)-1;      // NUMCOLORS (many)
    case 88: return 0x10000;           // COLORRES (16bpp reported as 65536)
    default: return 0;
  }
}

HLE_STDCALL("shell32.dll", SHGetFolderPathA, 5) {
  // CSIDL_APPDATA/CSIDL_PERSONAL and friends map under the (virtual) profile; the game only needs
  // a writable path, so hand back the install tree.
  std::string dir = kGameDir;
  if (!dir.empty() && dir.back() == '\\') dir.pop_back();
  if (arg(c, 4)) write_cstr(arg(c, 4), dir);
  return 0;  // S_OK
}

HLE_STDCALL("winmm.dll", timeGetTime, 0) { return now_ms(); }
HLE_STDCALL("winmm.dll", timeBeginPeriod, 1) { return 0; }  // TIMERR_NOERROR
HLE_STDCALL("winmm.dll", timeEndPeriod, 1) { return 0; }
HLE_STDCALL("winmm.dll", timeSetEvent, 5) {
  // No multimedia timer callback is delivered; return a non-zero id so callers can "kill" it.
  // TODO(M5): drive the callback if the game's timing depends on it (audio/streaming).
  static uint32_t next_id = 1;
  static bool warned = false;
  if (!warned) {
    warned = true;
    log(Log::Info, "timeSetEvent: callbacks not delivered (M5)");
  }
  return next_id++;
}
HLE_STDCALL("winmm.dll", timeKillEvent, 1) { return 0; }  // TIMERR_NOERROR
}  // namespace

