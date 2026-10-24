// Host platform stubs for standalone test executables (BELLA_NO_GAME_CODE).
#include "platform/platform.h"

namespace plat {

bool init() { return true; }
void create_window(const std::string&, int, int) {}
void set_window_size(int, int) {}
void get_display_size(int* width, int* height) {
  if (width) *width = 640;
  if (height) *height = 480;
}
void get_window_size(int* width, int* height) {
  if (width) *width = 640;
  if (height) *height = 480;
}
void show_cursor(bool) {}
void poll_events(std::vector<InputEvent>&) {}
const uint8_t* keyboard_state() {
  static const uint8_t keys[256] = {};
  return keys;
}
MouseState take_mouse_state() { return {}; }
void mouse_pos(int* x, int* y) {
  if (x) *x = 0;
  if (y) *y = 0;
}
void set_mouse_pos(int, int) {}
bool mouse_button_down(int) { return false; }
bool has_window() { return false; }
GamepadState gamepad_state(int) { return {}; }
void gamepad_rumble(int, float, float) {}
void frame_presented(uint64_t) {}
bool gl_create_context() { return true; }
void gl_swap() {}
void gl_set_swap_interval(int) {}
bool audio_open(int, void (*)(float*, int)) { return true; }

}  // namespace plat
