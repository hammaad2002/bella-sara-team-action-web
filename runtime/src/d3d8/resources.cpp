// IDirect3DSurface8 / IDirect3DTexture8 / vertex & index buffers / state blocks (D3D8 vtable order).
#include <algorithm>
#include <map>
#include <vector>

#include "../d3d9/cmd.h"
#include "../d3d9/d3d9.h"
#include "d3d8.h"

using namespace rt;

namespace d3d8 {

uint32_t guest_texture_gl(uint32_t guest_ptr) {
  auto* t = dynamic_cast<Texture8*>(com_object(guest_ptr));
  return t ? t->gl_tex : 0;
}

uint32_t guest_vertex_buffer_gl(uint32_t guest_ptr) {
  auto* vb = dynamic_cast<VertexBuffer8*>(com_object(guest_ptr));
  return vb ? vb->gl_buf : 0;
}

uint32_t guest_index_buffer_gl(uint32_t guest_ptr) {
  auto* ib = dynamic_cast<IndexBuffer8*>(com_object(guest_ptr));
  return ib ? ib->gl_buf : 0;
}

uint32_t guest_shader_program(uint32_t guest_handle, bool pixel) {
  if (!guest_handle) return 0;
  auto* s = dynamic_cast<Shader8*>(com_object(guest_handle));
  if (!s || s->pixel != pixel) return 0;
  return s->program_id;
}

static uint32_t mip_level_count(uint32_t w, uint32_t h) {
  uint32_t full = 1;
  for (uint32_t m = std::max(w, h); m > 1; m >>= 1) full++;
  return full;
}

// Releasing a D3D8 resource must give back its guest-memory shadow copy and its GL object; the HLE heap is a
// fixed 640 MB mspace, so leaking these exhausts it after a few level loads.
Texture8::~Texture8() {
  for (TexLevel& lv : level)
    if (lv.mem) heap_free(process_heap(), lv.mem);
  if (gl_tex) d3d8_cmd_stream().op(cmd::OpDestroyTexture, {gl_tex});
}

VertexBuffer8::~VertexBuffer8() {
  if (gl_buf) d3d8_cmd_stream().op(cmd::OpDestroyBuffer, {gl_buf});
  if (mem) heap_free(process_heap(), mem);
}

IndexBuffer8::~IndexBuffer8() {
  if (gl_buf) d3d8_cmd_stream().op(cmd::OpDestroyBuffer, {gl_buf});
  if (mem) heap_free(process_heap(), mem);
}

Texture8* create_texture8(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool,
                          uint32_t gl_tex) {
  auto* t = new Texture8();
  t->width = w;
  t->height = h;
  uint32_t full = mip_level_count(w, h);
  t->levels = levels ? (levels > full ? full : levels) : 1;
  t->usage = usage;
  t->format = fmt;
  t->pool = pool;
  t->gl_tex = gl_tex;
  t->level.resize(t->levels);
  for (uint32_t l = 0; l < t->levels; l++) {
    t->level[l].w = std::max(1u, w >> l);
    t->level[l].h = std::max(1u, h >> l);
    t->level[l].pitch = d3d::row_pitch(fmt, t->level[l].w);
  }
  return t;
}

Texture8* create_cube_texture8(uint32_t edge, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool,
                               uint32_t gl_tex) {
  auto* t = new Texture8();
  t->is_cube = true;
  t->width = edge;
  t->height = edge;
  uint32_t full = mip_level_count(edge, edge);
  t->levels = levels ? (levels > full ? full : levels) : 1;
  t->usage = usage;
  t->format = fmt;
  t->pool = pool;
  t->gl_tex = gl_tex;
  t->level.resize(6 * t->levels);
  t->level_surface.resize(6 * t->levels);
  for (uint32_t face = 0; face < 6; face++) {
    for (uint32_t l = 0; l < t->levels; l++) {
      size_t idx = t->slot_index(face, l);
      t->level[idx].w = std::max(1u, edge >> l);
      t->level[idx].h = std::max(1u, edge >> l);
      t->level[idx].pitch = d3d::row_pitch(fmt, t->level[idx].w);
    }
  }
  return t;
}

VertexBuffer8* create_vertex_buffer8(uint32_t length, uint32_t usage, uint32_t fvf, uint32_t pool, uint32_t gl_buf,
                                     uint32_t mem) {
  auto* vb = new VertexBuffer8();
  vb->length = length;
  vb->usage = usage;
  vb->fvf = fvf;
  vb->pool = pool;
  vb->gl_buf = gl_buf;
  vb->mem = mem;
  return vb;
}

IndexBuffer8* create_index_buffer8(uint32_t length, uint32_t usage, uint32_t format, uint32_t pool, uint32_t gl_buf,
                                   uint32_t mem) {
  auto* ib = new IndexBuffer8();
  ib->length = length;
  ib->usage = usage;
  ib->format = format;
  ib->pool = pool;
  ib->gl_buf = gl_buf;
  ib->mem = mem;
  return ib;
}

uint32_t expose_d3d8_surface(ComObject* surface) {
  for (uint32_t w : surface->wrappers) {
    if (w && com_object(w)) return w;
  }
  return com_wrap(surface, iface_d3d8_surface());
}

uint32_t expose_d3d8_texture(ComObject* texture) {
  for (uint32_t w : texture->wrappers) {
    if (w && com_object(w)) return w;
  }
  return com_wrap(texture, iface_d3d8_texture());
}

uint32_t expose_d3d8_vertex_buffer(ComObject* vb) {
  for (uint32_t w : vb->wrappers) {
    if (w && com_object(w)) return w;
  }
  return com_wrap(vb, iface_d3d8_vertex_buffer());
}

uint32_t expose_d3d8_index_buffer(ComObject* ib) {
  for (uint32_t w : ib->wrappers) {
    if (w && com_object(w)) return w;
  }
  return com_wrap(ib, iface_d3d8_index_buffer());
}

uint32_t wrap_texture8(Texture8* texture) { return expose_d3d8_texture(texture); }

uint32_t expose_d3d8_cube_texture(ComObject* texture) {
  for (uint32_t w : texture->wrappers) {
    if (w && com_object(w)) return w;
  }
  return com_wrap(texture, iface_d3d8_cube_texture());
}

uint32_t wrap_cube_texture8(Texture8* texture) { return expose_d3d8_cube_texture(texture); }
uint32_t wrap_vertex_buffer8(VertexBuffer8* vb) { return expose_d3d8_vertex_buffer(vb); }
uint32_t wrap_index_buffer8(IndexBuffer8* ib) { return expose_d3d8_index_buffer(ib); }
uint32_t wrap_shader8(Shader8* s) {
  for (uint32_t w : s->wrappers) {
    if (w && com_object(w)) return w;
  }
  return com_wrap(s, s->pixel ? iface_d3d8_pixel_shader() : iface_d3d8_vertex_shader());
}
uint32_t wrap_state_block8(StateBlock8* sb) {
  for (uint32_t w : sb->wrappers) {
    if (w && com_object(w)) return w;
  }
  return com_wrap(sb, iface_d3d8_state_block());
}

namespace {

uint32_t ok(Cpu*, ComObject*) { return S_OK; }

struct VbLock {
  uint32_t off = 0, size = 0;
};
std::map<VertexBuffer8*, VbLock> g_vb_locks;
std::map<IndexBuffer8*, VbLock> g_ib_locks;

Texture8* T8(ComObject* o) { return static_cast<Texture8*>(o); }
VertexBuffer8* VB8(ComObject* o) { return static_cast<VertexBuffer8*>(o); }
IndexBuffer8* IB8(ComObject* o) { return static_cast<IndexBuffer8*>(o); }

void upload_vb(VertexBuffer8* b, uint32_t off, uint32_t size) {
  cmd::Stream& s = d3d8_cmd_stream();
  s.op(cmd::OpUploadBuffer, {b->gl_buf, off, size});
  s.bytes(host(b->mem + off), size);
}

void upload_ib(IndexBuffer8* b, uint32_t off, uint32_t size) {
  cmd::Stream& s = d3d8_cmd_stream();
  s.op(cmd::OpUploadBuffer, {b->gl_buf, off, size});
  s.bytes(host(b->mem + off), size);
}

uint32_t tex_ensure_level_mem(Texture8* t, uint32_t face, uint32_t level) {
  if (level >= t->levels || (t->is_cube && face >= 6)) return 0;
  size_t idx = t->slot_index(face, level);
  if (idx >= t->level.size()) return 0;
  TexLevel& lv = t->level[idx];
  if (!lv.mem) {
    uint32_t rows = d3d::level_rows(t->format, lv.h);
    lv.mem = heap_alloc(process_heap(), lv.pitch * rows, true);
  }
  return lv.mem;
}

uint32_t res_GetPrivateData(Cpu*, ComObject*) { return d3d::D3DERR_NOTFOUND; }

uint32_t res_SetPriority(Cpu*, ComObject*) { return S_OK; }

uint32_t res_GetPriority(Cpu*, ComObject*) { return 0; }

uint32_t res_PreLoad(Cpu*, ComObject*) { return S_OK; }

uint32_t surf_GetDevice(Cpu* c, ComObject* self) {
  (void)self;
  uint32_t out = arg(c, 1);
  if (out < kNullGuardEnd) return d3d::D3DERR_INVALIDCALL;
  uint32_t dev_ptr = ld32(0x7653b0u);
  if (auto* dev = com_object(dev_ptr)) com_AddRef(c, dev);
  st32(out, dev_ptr);
  return S_OK;
}

uint32_t surf_GetContainer(Cpu* c, ComObject*) {
  st32(arg(c, 1), 0);
  return d3d::D3DERR_INVALIDCALL;
}

uint32_t tex_SetLOD(Cpu*, ComObject*) { return S_OK; }

uint32_t tex_GetLOD(Cpu*, ComObject*) { return 0; }

uint32_t tex_GetType(Cpu*, ComObject*) { return d3d::RTYPE_TEXTURE; }

uint32_t vb_GetType(Cpu*, ComObject*) { return d3d::RTYPE_VERTEXBUFFER; }

uint32_t ib_GetType(Cpu*, ComObject*) { return d3d::RTYPE_INDEXBUFFER; }

static uint32_t lock_rect_offset(uint32_t fmt, uint32_t pitch, uint32_t rect) {
  if (!rect) return 0;
  uint32_t left = ld32(rect), top = ld32(rect + 4);
  d3d::FormatInfo f = d3d::format_info(fmt);
  return f.block ? (top / 4) * pitch + (left / 4) * f.block : top * pitch + left * f.bpp / 8;
}

uint32_t surf_GetDesc(Cpu* c, ComObject* self) {
  uint32_t p = arg(c, 1);
  if (p < kNullGuardEnd) return d3d::D3DERR_INVALIDCALL;
  uint32_t w = 800, h = 600, fmt = d3d::FMT_X8R8G8B8, usage = d3d::USAGE_RENDERTARGET, pool = d3d::POOL_DEFAULT;
  uint32_t pitch = d3d::row_pitch(fmt, w);
  if (auto* ts = dynamic_cast<TextureLevelSurface8*>(self)) {
    Texture8* t = ts->tex;
    if (t && ts->level < t->levels) {
      size_t idx = t->slot_index(ts->face, ts->level);
      w = t->level[idx].w;
      h = t->level[idx].h;
      fmt = t->format;
      usage = t->usage;
      pool = t->pool;
      pitch = t->level[idx].pitch;
    }
  } else if (auto* s = dynamic_cast<Surface8*>(self)) {
    w = s->width;
    h = s->height;
    fmt = s->format;
    usage = s->usage;
    pool = s->pool;
    pitch = s->pitch ? s->pitch : d3d::row_pitch(fmt, w);
  }
  st32(p + 0, fmt);
  st32(p + 4, d3d::RTYPE_SURFACE);
  st32(p + 8, usage);
  st32(p + 12, pool);
  st32(p + 16, pitch * d3d::level_rows(fmt, h));
  st32(p + 20, 0);  // MultiSampleType
  st32(p + 24, w);
  st32(p + 28, h);
  return S_OK;
}

uint32_t surf_LockRect(Cpu* c, ComObject* self) {
  uint32_t out = arg(c, 1), rect = arg(c, 2), flags = arg(c, 3);
  if (out < kNullGuardEnd) return d3d::D3DERR_INVALIDCALL;
  uint32_t pitch = 0, mem = 0;
  if (auto* ts = dynamic_cast<TextureLevelSurface8*>(self)) {
    Texture8* t = ts->tex;
    if (t && ts->level < t->levels) {
      size_t idx = t->slot_index(ts->face, ts->level);
      TexLevel& lv = t->level[idx];
      if (lv.dirty && lv.mem) upload_tex_face_level(t, ts->face, ts->level);
      mem = tex_ensure_level_mem(t, ts->face, ts->level);
      pitch = lv.pitch;
      uint32_t off = lock_rect_offset(t->format, pitch, rect);
      if (!(flags & 0x10)) lv.dirty = true;
      st32(out, pitch);
      st32(out + 4, mem + off);
      return S_OK;
    }
  } else if (auto* s = dynamic_cast<Surface8*>(self)) {
    mem = s->ensure_memory();
    pitch = s->pitch;
    uint32_t off = lock_rect_offset(s->format, pitch, rect);
    if (!(flags & 0x10)) s->dirty = true;
    st32(out, pitch);
    st32(out + 4, mem + off);
    return S_OK;
  }
  st32(out, pitch);
  st32(out + 4, mem);
  return S_OK;
}

uint32_t surf_UnlockRect(Cpu*, ComObject* self) {
  if (auto* ts = dynamic_cast<TextureLevelSurface8*>(self)) {
    Texture8* t = ts->tex;
    if (t && ts->level < t->levels && t->level[t->slot_index(ts->face, ts->level)].dirty)
      upload_tex_face_level(t, ts->face, ts->level);
  } else if (auto* s = dynamic_cast<Surface8*>(self)) {
    if (s->dirty && s->gl_tex) upload_surface8(s);
  }
  return S_OK;
}

uint32_t tex_GetLevelCount(Cpu*, ComObject* self) { return T8(self)->levels; }

uint32_t tex_GetLevelDesc(Cpu* c, ComObject* self) {
  Texture8* t = T8(self);
  uint32_t level = arg(c, 1), p = arg(c, 2);
  if (level >= t->levels || p < kNullGuardEnd) return d3d::D3DERR_INVALIDCALL;
  uint32_t w = t->level[level].w;
  uint32_t h = t->level[level].h;
  st32(p + 0, t->format);
  st32(p + 4, d3d::RTYPE_TEXTURE);
  st32(p + 8, t->usage);
  st32(p + 12, t->pool);
  st32(p + 16, t->level[level].pitch * d3d::level_rows(t->format, h));
  st32(p + 20, 0);
  st32(p + 24, w);
  st32(p + 28, h);
  return S_OK;
}

uint32_t tex_GetSurfaceLevel(Cpu* c, ComObject* self) {
  Texture8* t = T8(self);
  uint32_t level = arg(c, 1), out = arg(c, 2);
  if (level >= t->levels || out < kNullGuardEnd) return d3d::D3DERR_INVALIDCALL;
  if (t->level.size() <= level) t->level.resize(level + 1);
  if (!t->level[level].w) {
    t->level[level].w = std::max(1u, t->width >> level);
    t->level[level].h = std::max(1u, t->height >> level);
    t->level[level].pitch = d3d::row_pitch(t->format, t->level[level].w);
  }
  if (t->level_surface.size() <= level) t->level_surface.resize(level + 1);
  uint32_t& slot = t->level_surface[level];
  if (!slot || !com_object(slot)) {
    auto* view = new TextureLevelSurface8();
    view->tex = t;
    view->level = level;
    t->refs++;
    slot = com_wrap(view, iface_d3d8_surface());
  } else {
    ComObject* o = com_object(slot);
    if (o) o->refs++;
  }
  st32(out, slot);
  return S_OK;
}

uint32_t tex_LockRect(Cpu* c, ComObject* self) {
  Texture8* t = T8(self);
  uint32_t level = arg(c, 1), out = arg(c, 2), rect = arg(c, 3), flags = arg(c, 4);
  if (level >= t->levels) return d3d::D3DERR_INVALIDCALL;
  TexLevel& lv = t->level[level];
  if (lv.dirty && lv.mem) upload_tex_face_level(t, 0, level);
  uint32_t mem = tex_ensure_level_mem(t, 0, level);
  uint32_t off = 0;
  if (rect) {
    uint32_t left = ld32(rect), top = ld32(rect + 4);
    d3d::FormatInfo f = d3d::format_info(t->format);
    off = f.block ? (top / 4) * lv.pitch + (left / 4) * f.block : top * lv.pitch + left * f.bpp / 8;
  }
  if (out >= kNullGuardEnd) {
    st32(out, lv.pitch);
    st32(out + 4, mem + off);
  }
  if (!(flags & 0x10)) lv.dirty = true;
  return S_OK;
}

uint32_t tex_UnlockRect(Cpu* c, ComObject* self) {
  Texture8* t = T8(self);
  uint32_t level = arg(c, 1);
  if (level >= t->levels) return d3d::D3DERR_INVALIDCALL;
  if (t->level[level].dirty) upload_tex_face_level(t, 0, level);
  return S_OK;
}

uint32_t cubetex_GetType(Cpu*, ComObject*) { return d3d::RTYPE_CUBETEXTURE; }

uint32_t cubetex_GetLevelDesc(Cpu* c, ComObject* self) {
  Texture8* t = T8(self);
  uint32_t level = arg(c, 1), p = arg(c, 2);
  if (level >= t->levels || p < kNullGuardEnd) return d3d::D3DERR_INVALIDCALL;
  uint32_t w = t->level[t->slot_index(0, level)].w;
  uint32_t h = t->level[t->slot_index(0, level)].h;
  st32(p + 0, t->format);
  st32(p + 4, d3d::RTYPE_CUBETEXTURE);
  st32(p + 8, t->usage);
  st32(p + 12, t->pool);
  st32(p + 16, t->level[t->slot_index(0, level)].pitch * d3d::level_rows(t->format, h));
  st32(p + 20, 0);
  st32(p + 24, w);
  st32(p + 28, h);
  return S_OK;
}

uint32_t cubetex_GetCubeMapSurface(Cpu* c, ComObject* self) {
  Texture8* t = T8(self);
  uint32_t face = arg(c, 1), level = arg(c, 2), out = arg(c, 3);
  if (face >= 6 || level >= t->levels || out < kNullGuardEnd) return d3d::D3DERR_INVALIDCALL;
  size_t idx = t->slot_index(face, level);
  if (t->level_surface.size() <= idx) t->level_surface.resize(idx + 1);
  uint32_t& slot = t->level_surface[idx];
  if (!slot || !com_object(slot)) {
    auto* view = new TextureLevelSurface8();
    view->tex = t;
    view->face = face;
    view->level = level;
    t->refs++;
    slot = com_wrap(view, iface_d3d8_surface());
  } else {
    ComObject* o = com_object(slot);
    if (o) o->refs++;
  }
  st32(out, slot);
  return S_OK;
}

uint32_t cubetex_LockRect(Cpu* c, ComObject* self) {
  Texture8* t = T8(self);
  uint32_t face = arg(c, 1), level = arg(c, 2), out = arg(c, 3), rect = arg(c, 4), flags = arg(c, 5);
  if (face >= 6 || level >= t->levels) return d3d::D3DERR_INVALIDCALL;
  size_t idx = t->slot_index(face, level);
  TexLevel& lv = t->level[idx];
  if (lv.dirty && lv.mem) upload_tex_face_level(t, face, level);
  uint32_t mem = tex_ensure_level_mem(t, face, level);
  uint32_t off = 0;
  if (rect) {
    uint32_t left = ld32(rect), top = ld32(rect + 4);
    d3d::FormatInfo f = d3d::format_info(t->format);
    off = f.block ? (top / 4) * lv.pitch + (left / 4) * f.block : top * lv.pitch + left * f.bpp / 8;
  }
  if (out >= kNullGuardEnd) {
    st32(out, lv.pitch);
    st32(out + 4, mem + off);
  }
  if (!(flags & 0x10)) lv.dirty = true;
  return S_OK;
}

uint32_t cubetex_UnlockRect(Cpu* c, ComObject* self) {
  Texture8* t = T8(self);
  uint32_t face = arg(c, 1), level = arg(c, 2);
  if (face >= 6 || level >= t->levels) return d3d::D3DERR_INVALIDCALL;
  size_t idx = t->slot_index(face, level);
  if (t->level[idx].dirty) upload_tex_face_level(t, face, level);
  return S_OK;
}

static void texture_release_unlocked(Texture8* t) {
  for (size_t slot = 0; slot < t->level.size(); slot++) {
    if (!t->level[slot].dirty) continue;
    uint32_t face = t->is_cube ? (uint32_t)(slot / t->levels) : 0;
    uint32_t lv = t->is_cube ? (uint32_t)(slot % t->levels) : (uint32_t)slot;
    upload_tex_face_level(t, face, lv);
  }
}

uint32_t tex_Release(Cpu* c, ComObject* self) {
  if (auto* t = dynamic_cast<Texture8*>(self)) texture_release_unlocked(t);
  return com_Release(c, self);
}

uint32_t vb_Lock(Cpu* c, ComObject* self) {
  VertexBuffer8* b = VB8(self);
  uint32_t off = arg(c, 1), size = arg(c, 2), out = arg(c, 3), flags = arg(c, 4);
  if (size == 0 && off == 0) size = b->length;
  if (off > b->length || out < kNullGuardEnd) return d3d::D3DERR_INVALIDCALL;
  size = std::min(size, b->length - off);
  st32(out, b->mem + off);
  if (!(flags & 0x10)) g_vb_locks[b] = {off, size};
  return S_OK;
}

uint32_t vb_Unlock(Cpu*, ComObject* self) {
  VertexBuffer8* b = VB8(self);
  auto it = g_vb_locks.find(b);
  if (it == g_vb_locks.end()) return S_OK;
  VbLock l = it->second;
  g_vb_locks.erase(it);
  if (l.size) upload_vb(b, l.off, l.size);
  return S_OK;
}

uint32_t vb_GetDesc(Cpu* c, ComObject* self) {
  VertexBuffer8* b = VB8(self);
  uint32_t p = arg(c, 1);
  if (p < kNullGuardEnd) return d3d::D3DERR_INVALIDCALL;
  st32(p + 0, 100u);
  st32(p + 4, d3d::RTYPE_VERTEXBUFFER);
  st32(p + 8, b->usage);
  st32(p + 12, b->pool);
  st32(p + 16, b->length);
  st32(p + 20, b->fvf);
  return S_OK;
}

uint32_t ib_Lock(Cpu* c, ComObject* self) {
  IndexBuffer8* b = IB8(self);
  uint32_t off = arg(c, 1), size = arg(c, 2), out = arg(c, 3), flags = arg(c, 4);
  if (size == 0 && off == 0) size = b->length;
  if (off > b->length || out < kNullGuardEnd) return d3d::D3DERR_INVALIDCALL;
  size = std::min(size, b->length - off);
  st32(out, b->mem + off);
  if (!(flags & 0x10)) g_ib_locks[b] = {off, size};
  return S_OK;
}

uint32_t ib_Unlock(Cpu*, ComObject* self) {
  IndexBuffer8* b = IB8(self);
  auto it = g_ib_locks.find(b);
  if (it == g_ib_locks.end()) return S_OK;
  VbLock l = it->second;
  g_ib_locks.erase(it);
  if (l.size) upload_ib(b, l.off, l.size);
  return S_OK;
}

uint32_t ib_GetDesc(Cpu* c, ComObject* self) {
  IndexBuffer8* b = IB8(self);
  uint32_t p = arg(c, 1);
  if (p < kNullGuardEnd) return d3d::D3DERR_INVALIDCALL;
  st32(p + 0, b->format);
  st32(p + 4, d3d::RTYPE_INDEXBUFFER);
  st32(p + 8, b->usage);
  st32(p + 12, b->pool);
  st32(p + 16, b->length);
  return S_OK;
}

uint32_t sb_Apply(Cpu*, ComObject* self) {
  StateBlock8* sb = static_cast<StateBlock8*>(self);
  if (!sb->device) return d3d::D3DERR_INVALIDCALL;
  apply_state_block8(sb);
  return S_OK;
}

uint32_t sb_Capture(Cpu*, ComObject* self) {
  StateBlock8* sb = static_cast<StateBlock8*>(self);
  if (!sb->device) return d3d::D3DERR_INVALIDCALL;
  capture_state_block8(sb);
  return S_OK;
}

}  // namespace

void texture_ensure_mem(Texture8* t, uint32_t level) {
  if (level >= t->levels) return;
  size_t idx = t->slot_index(0, level);
  if (idx >= t->level.size()) return;
  TexLevel& lv = t->level[idx];
  if (!lv.mem) {
    uint32_t rows = d3d::level_rows(t->format, lv.h);
    lv.mem = heap_alloc(process_heap(), lv.pitch * rows, true);
  }
}

void upload_tex_face_level(Texture8* t, uint32_t face, uint32_t level) {
  if (level >= t->levels || (t->is_cube && face >= 6)) return;
  size_t idx = t->slot_index(face, level);
  if (idx >= t->level.size()) return;
  TexLevel& lv = t->level[idx];
  if (!lv.mem) return;
  uint32_t rows = d3d::level_rows(t->format, lv.h);
  uint32_t bytes = lv.pitch * rows;
  cmd::Stream& s = d3d8_cmd_stream();
  s.op(cmd::OpUploadTexture, {t->gl_tex, face, level, lv.w, lv.h, t->format, lv.pitch, rows, bytes});
  s.bytes(host(lv.mem), bytes);
  lv.dirty = false;
}

void texture_flush_dirty(Texture8* t) {
  if (!t) return;
  for (size_t slot = 0; slot < t->level.size(); slot++) {
    if (!t->level[slot].dirty) continue;
    uint32_t face = t->is_cube ? (uint32_t)(slot / t->levels) : 0;
    uint32_t lv = t->is_cube ? (uint32_t)(slot % t->levels) : (uint32_t)slot;
    upload_tex_face_level(t, face, lv);
  }
}

void texture_copy_levels(Texture8* src, Texture8* dst) {
  if (!src || !dst) return;
  uint32_t n = std::min(src->levels, dst->levels);
  for (uint32_t l = 0; l < n; l++) {
    texture_ensure_mem(src, l);
    texture_ensure_mem(dst, l);
    TexLevel& s = src->level[l];
    TexLevel& d = dst->level[l];
    uint32_t rows = d3d::level_rows(src->format, s.h);
    uint32_t bytes = std::min(s.pitch * rows, d.pitch * rows);
    memcpy(host(d.mem), host(s.mem), bytes);
    d.dirty = true;
    uint32_t up_rows = d3d::level_rows(dst->format, d.h);
    uint32_t up_bytes = d.pitch * up_rows;
    cmd::Stream& st = d3d8_cmd_stream();
    st.op(cmd::OpUploadTexture, {dst->gl_tex, 0, l, d.w, d.h, dst->format, d.pitch, up_rows, up_bytes});
    st.bytes(host(d.mem), up_bytes);
    d.dirty = false;
  }
}

rt::ComInterface& iface_d3d8_surface() {
  static ComInterface i("IDirect3DSurface8",
                        {{"QueryInterface", 2, com_QueryInterface_none},
                         {"AddRef", 0, com_AddRef},
                         {"Release", 0, com_Release},
                         {"GetDevice", 1, surf_GetDevice},
                         {"SetPrivateData", 3, ok},
                         {"GetPrivateData", 3, res_GetPrivateData},
                         {"FreePrivateData", 1, ok},
                         {"GetContainer", 2, surf_GetContainer},
                         {"GetDesc", 1, surf_GetDesc},
                         {"LockRect", 3, surf_LockRect},
                         {"UnlockRect", 0, surf_UnlockRect}});
  return i;
}

rt::ComInterface& iface_d3d8_cube_texture() {
  static ComInterface i("IDirect3DCubeTexture8",
                        {{"QueryInterface", 2, com_QueryInterface_none},
                         {"AddRef", 0, com_AddRef},
                         {"Release", 0, tex_Release},
                         {"GetDevice", 1, surf_GetDevice},
                         {"SetPrivateData", 3, ok},
                         {"GetPrivateData", 3, res_GetPrivateData},
                         {"FreePrivateData", 1, ok},
                         {"SetPriority", 1, res_SetPriority},
                         {"GetPriority", 0, res_GetPriority},
                         {"PreLoad", 0, res_PreLoad},
                         {"GetType", 0, cubetex_GetType},
                         {"SetLOD", 1, tex_SetLOD},
                         {"GetLOD", 0, tex_GetLOD},
                         {"GetLevelCount", 0, tex_GetLevelCount},
                         {"GetLevelDesc", 2, cubetex_GetLevelDesc},
                         {"GetCubeMapSurface", 3, cubetex_GetCubeMapSurface},
                         {"LockRect", 5, cubetex_LockRect},
                         {"UnlockRect", 2, cubetex_UnlockRect},
                         {"AddDirtyRect", 2, ok}});
  return i;
}

rt::ComInterface& iface_d3d8_texture() {
  static ComInterface i("IDirect3DTexture8",
                        {{"QueryInterface", 2, com_QueryInterface_none},
                         {"AddRef", 0, com_AddRef},
                         {"Release", 0, tex_Release},
                         {"GetDevice", 1, surf_GetDevice},
                         {"SetPrivateData", 3, ok},
                         {"GetPrivateData", 3, res_GetPrivateData},
                         {"FreePrivateData", 1, ok},
                         {"SetPriority", 1, res_SetPriority},
                         {"GetPriority", 0, res_GetPriority},
                         {"PreLoad", 0, res_PreLoad},
                         {"GetType", 0, tex_GetType},
                         {"SetLOD", 1, tex_SetLOD},
                         {"GetLOD", 0, tex_GetLOD},
                         {"GetLevelCount", 0, tex_GetLevelCount},
                         {"GetLevelDesc", 2, tex_GetLevelDesc},
                         {"GetSurfaceLevel", 2, tex_GetSurfaceLevel},
                         {"LockRect", 4, tex_LockRect},
                         {"UnlockRect", 1, tex_UnlockRect},
                         {"AddDirtyRect", 1, ok}});
  return i;
}

rt::ComInterface& iface_d3d8_vertex_buffer() {
  static ComInterface i("IDirect3DVertexBuffer8",
                        {{"QueryInterface", 2, com_QueryInterface_none},
                         {"AddRef", 0, com_AddRef},
                         {"Release", 0, com_Release},
                         {"GetDevice", 1, surf_GetDevice},
                         {"SetPrivateData", 3, ok},
                         {"GetPrivateData", 3, res_GetPrivateData},
                         {"FreePrivateData", 1, ok},
                         {"SetPriority", 1, res_SetPriority},
                         {"GetPriority", 0, res_GetPriority},
                         {"PreLoad", 0, res_PreLoad},
                         {"GetType", 0, vb_GetType},
                         {"Lock", 4, vb_Lock},
                         {"Unlock", 0, vb_Unlock},
                         {"GetDesc", 1, vb_GetDesc}});
  return i;
}

rt::ComInterface& iface_d3d8_index_buffer() {
  static ComInterface i("IDirect3DIndexBuffer8",
                        {{"QueryInterface", 2, com_QueryInterface_none},
                         {"AddRef", 0, com_AddRef},
                         {"Release", 0, com_Release},
                         {"GetDevice", 1, surf_GetDevice},
                         {"SetPrivateData", 3, ok},
                         {"GetPrivateData", 3, res_GetPrivateData},
                         {"FreePrivateData", 1, ok},
                         {"SetPriority", 1, res_SetPriority},
                         {"GetPriority", 0, res_GetPriority},
                         {"PreLoad", 0, res_PreLoad},
                         {"GetType", 0, ib_GetType},
                         {"Lock", 4, ib_Lock},
                         {"Unlock", 0, ib_Unlock},
                         {"GetDesc", 1, ib_GetDesc}});
  return i;
}

rt::ComInterface& iface_d3d8_vertex_shader() {
  static ComInterface i("IDirect3DVertexShader8",
                        {{"QueryInterface", 2, com_QueryInterface_none},
                         {"AddRef", 0, com_AddRef},
                         {"Release", 0, com_Release},
                         {"GetDevice", 1, surf_GetDevice}});
  return i;
}

rt::ComInterface& iface_d3d8_pixel_shader() {
  static ComInterface i("IDirect3DPixelShader8",
                        {{"QueryInterface", 2, com_QueryInterface_none},
                         {"AddRef", 0, com_AddRef},
                         {"Release", 0, com_Release},
                         {"GetDevice", 1, surf_GetDevice}});
  return i;
}

rt::ComInterface& iface_d3d8_state_block() {
  static ComInterface i("IDirect3DStateBlock8",
                        {{"QueryInterface", 2, com_QueryInterface_none},
                         {"AddRef", 0, com_AddRef},
                         {"Release", 0, com_Release},
                         {"GetDevice", 1, surf_GetDevice},
                         {"Apply", 0, sb_Apply},
                         {"Capture", 0, sb_Capture}});
  return i;
}

}  // namespace d3d8
