// Shared declarations for the Bella Sara Direct3D 8 HLE (runtime/src/d3d8/).
#pragma once

#include <cstdint>
#include <vector>

#include "../core/rt.h"
#include "../d3d9/d3d9.h"
#include "../d3d9/cmd.h"

namespace d3d8 {

void d3d8_verify_registered_nargs();

constexpr uint32_t kD3d8SdkVersion = 220;  // the SDK summer 2002 release the game was built against
constexpr uint32_t D3DERR_INVALIDCALL = 0x8876086c;

rt::ComInterface& iface_d3d8();
rt::ComInterface& iface_d3d8_device();
rt::ComInterface& iface_d3d8_surface();
rt::ComInterface& iface_d3d8_texture();
rt::ComInterface& iface_d3d8_vertex_buffer();
rt::ComInterface& iface_d3d8_index_buffer();
rt::ComInterface& iface_d3d8_state_block();
rt::ComInterface& iface_d3d8_vertex_shader();
rt::ComInterface& iface_d3d8_pixel_shader();

struct TexLevel {
  uint32_t w = 0, h = 0, pitch = 0, mem = 0;
  bool dirty = false;
};

struct Texture8 : public rt::ComObject {
  const char* type_name() const override { return "D3D8Texture"; }
  uint32_t width = 0, height = 0, levels = 1, usage = 0, format = 0, pool = 0;
  uint32_t gl_tex = 0;
  std::vector<TexLevel> level;
  std::vector<uint32_t> level_surface;
};

struct VertexBuffer8 : public rt::ComObject {
  const char* type_name() const override { return "D3D8VertexBuffer"; }
  uint32_t length = 0, usage = 0, fvf = 0, pool = 0;
  uint32_t mem = 0;
  uint32_t gl_buf = 0;
};

struct IndexBuffer8 : public rt::ComObject {
  const char* type_name() const override { return "D3D8IndexBuffer"; }
  uint32_t length = 0, usage = 0, format = 0, pool = 0;
  uint32_t mem = 0;
  uint32_t gl_buf = 0;
};

struct Shader8 : public rt::ComObject {
  const char* type_name() const override { return pixel ? "D3D8PixelShader" : "D3D8VertexShader"; }
  bool pixel = false;
  std::vector<uint32_t> tokens;
  uint32_t program_id = 0;
};

struct StateBlock8 : public rt::ComObject {
  const char* type_name() const override { return "D3D8StateBlock"; }
  rt::ComObject* device = nullptr;
  d3d::State snapshot;
  bool recorded = false;
};

struct Surface8 : public rt::ComObject {
  const char* type_name() const override { return "D3D8Surface"; }
  rt::ComObject* device = nullptr;
  uint32_t width = 0, height = 0, format = 0, usage = 0, pool = 0;
  uint32_t mem = 0, pitch = 0;
  uint32_t gl_tex = 0, gl_rb = 0;
  bool dirty = false;
  uint32_t ensure_memory() {
    if (!mem) {
      pitch = d3d::row_pitch(format, width);
      mem = rt::heap_alloc(rt::process_heap(), pitch * d3d::level_rows(format, height), true);
    }
    return mem;
  }
};

void upload_surface8(Surface8* s);

void apply_state_block8(StateBlock8* sb);
void capture_state_block8(StateBlock8* sb);
void texture_ensure_mem(Texture8* t, uint32_t level);
void texture_copy_levels(Texture8* src, Texture8* dst);
void texture_flush_dirty(Texture8* t);
// Guest IDirect3DSurface8 pointer for a host surface (D3D8 vtable order).
uint32_t expose_d3d8_surface(rt::ComObject* surface);
uint32_t expose_d3d8_texture(rt::ComObject* texture);
uint32_t expose_d3d8_vertex_buffer(rt::ComObject* vb);

void fill_device_caps(uint32_t p);
uint32_t create_d3d8_device(Cpu* c, rt::ComObject* d3d8);
Texture8* create_texture8(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool,
                            uint32_t gl_tex);
VertexBuffer8* create_vertex_buffer8(uint32_t length, uint32_t usage, uint32_t fvf, uint32_t pool, uint32_t gl_buf,
                                     uint32_t mem);
IndexBuffer8* create_index_buffer8(uint32_t length, uint32_t usage, uint32_t format, uint32_t pool, uint32_t gl_buf,
                                   uint32_t mem);
uint32_t guest_texture_gl(uint32_t guest_ptr);
uint32_t guest_vertex_buffer_gl(uint32_t guest_ptr);
uint32_t guest_index_buffer_gl(uint32_t guest_ptr);
uint32_t guest_shader_program(uint32_t guest_handle, bool pixel);
uint32_t wrap_texture8(Texture8* t);
uint32_t wrap_vertex_buffer8(VertexBuffer8* vb);
uint32_t wrap_index_buffer8(IndexBuffer8* ib);
uint32_t wrap_shader8(Shader8* s);
uint32_t wrap_state_block8(StateBlock8* sb);

cmd::Stream& d3d8_cmd_stream();

}  // namespace d3d8
