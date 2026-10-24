// IDirect3DDevice8 + minimal cmd:: recording (d3d9 sources are not linked in this target).
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "../d3d9/cmd.h"
#include "../d3d9/d3d9.h"
#include "../gles/executor.h"
#include "../hle/com.h"
#include "../platform/platform.h"
#if defined(__EMSCRIPTEN__)
#include "../platform/web.h"
#endif
#include "d3d8.h"
#include "d3d8/d3d8_sdk_nargs.h"

using namespace rt;

namespace d3d8 {
namespace {

// D3D8 D3DPRESENT_PARAMETERS (52 bytes, 13 dwords) — no MultiSampleQuality field (unlike D3D9).
enum : int {
  kPP8_MultiSampleType = 4,
  kPP8_Windowed = 7,
  kPP8_EnableAutoDepthStencil = 8,
  kPP8_AutoDepthStencilFormat = 9,
  kPP8_Flags = 10,
  kPP8_PresentationInterval = 12,
};

std::recursive_mutex g_api_lock;
cmd::Stream g_stream;

cmd::Stream& rec() { return g_stream; }


static uint32_t g_next_gl_id = 1;
static uint32_t new_gl_id() { return g_next_gl_id++; }

#if !defined(__EMSCRIPTEN__)
static std::thread::id g_gl_thread;
static bool g_gl_thread_set = false;

static bool on_gl_thread() {
  if (!g_gl_thread_set) {
    g_gl_thread = std::this_thread::get_id();
    g_gl_thread_set = true;
  }
  return g_gl_thread == std::this_thread::get_id();
}

void flush_stream(bool /*present*/) {
  if (!on_gl_thread()) return;
  gles::execute(g_stream.words.data(), g_stream.words.size());
  g_stream.clear();
}
#else
void flush_stream(bool present) {
  size_t cap = g_stream.words.size();
  ::web::submit(std::move(g_stream.words), present);
  g_stream.words = {};
  g_stream.words.reserve(cap);
}
#endif

class Device8;

using StreamBinding = StreamBinding8;

class Device8 : public ComObject {
 public:
  const char* type_name() const override { return "D3D8Device"; }
  uint32_t guest_ptr = 0;
  uint32_t d3d8_guest = 0;
  uint32_t hwnd = 0, behavior = 0;
  uint32_t pp[13] = {};
  d3d::State st;
  Surface8* back_buffer = nullptr;
  ComObject* render_target = nullptr;
  Surface8* depth_stencil = nullptr;
  StateBlock8* recording = nullptr;
  bool in_scene = false;
  bool lost = false;
  uint64_t frame = 0;
  uint32_t gamma[3 * 256] = {};
  uint32_t draws = 0, prims = 0;
  uint32_t bound_tex[8] = {};
  uint32_t bound_tex_guest[8] = {};
  StreamBinding streams[16] = {};
  uint32_t fvf = 0;
  uint32_t vs_handle = 0, ps_handle = 0;
  uint32_t index_gl = 0, index_fmt = d3d::FMT_INDEX16;
  uint32_t index_guest = 0, index_base = 0;
  std::array<float, 256 * 4> vs_f{};
  std::array<float, 224 * 4> ps_f{};
};

Device8* D(ComObject* o) { return static_cast<Device8*>(o); }
uint32_t Present(Cpu* c, ComObject* self);
float fl(uint32_t v) { return bits2f32(v); }

std::vector<d3d::VertexElement> fvf_elements(uint32_t fvf) {
  std::vector<d3d::VertexElement> e;
  uint16_t off = 0;
  auto add = [&](uint8_t type, uint8_t usage, uint8_t idx, uint16_t size) {
    e.push_back({0, off, type, 0, usage, idx});
    off = (uint16_t)(off + size);
  };
  uint32_t pos = fvf & 0x400e;
  uint32_t weights = 0;
  if (pos == 0x004) add(3, 9, 0, 16);
  else if (pos == 0x4002) add(3, 0, 0, 16);
  else if (pos) {
    add(2, 0, 0, 12);
    weights = pos >= 0x006 ? (pos - 0x004) / 2 : 0;
  }
  if (weights) {
    bool last_ubyte = (fvf & 0x1000) || (fvf & 0x8000);
    uint32_t n = last_ubyte ? weights - 1 : weights;
    if (n) add((uint8_t)(n - 1), 1, 0, (uint16_t)(4 * n));
    if (last_ubyte) add((fvf & 0x8000) ? 4 : 5, 2, 0, 4);
  }
  if (fvf & 0x010) add(2, 3, 0, 12);
  if (fvf & 0x020) add(0, 4, 0, 4);
  if (fvf & 0x040) add(4, 10, 0, 4);
  if (fvf & 0x080) add(4, 10, 1, 4);
  uint32_t ntex = (fvf >> 8) & 0xf;
  for (uint32_t t = 0; t < ntex; t++) {
    uint32_t sz = (fvf >> (16 + 2 * t)) & 3;
    static const uint8_t type[4] = {1, 2, 3, 0};
    static const uint16_t bytes[4] = {8, 12, 16, 4};
    add(type[sz], 5, (uint8_t)t, bytes[sz]);
  }
  return e;
}

static uint16_t d3d8_decl_type_size(uint8_t t) {
  switch (t) {
    case 0: return 4;   // FLOAT1
    case 1: return 8;   // FLOAT2
    case 2: return 12;  // FLOAT3
    case 3: return 16;  // FLOAT4
    case 4: return 4;   // D3DCOLOR
    case 5: return 4;   // UBYTE4
    case 6: return 4;   // SHORT2
    case 7: return 8;   // SHORT4
    case 8: return 4;   // UBYTE4N
    case 9: return 4;   // SHORT2N
    case 10: return 8;  // SHORT4N
    case 11: return 4;  // USHORT2N
    case 12: return 8;  // USHORT4N
    case 13: return 4;  // UDEC3
    case 14: return 4;  // DEC3N
    case 15: return 4;  // FLOAT16_2
    case 16: return 8;  // FLOAT16_4
    default: return 4;
  }
}

std::vector<d3d::VertexElement> parse_d3d8_decl(uint32_t decl_addr) {
  std::vector<d3d::VertexElement> elts;
  if (decl_addr < kNullGuardEnd) return elts;
  uint16_t cur_stream = 0;
  uint16_t cur_offset = 0;
  for (int i = 0; i < 256; i++) {
    uint32_t token = ld32(decl_addr + i * 4);
    if (token == 0xffffffffu) break;
    uint32_t tok_type = (token >> 29) & 7;
    if (tok_type == 1) {  // D3DVSD_STREAM
      cur_stream = (uint16_t)(token & 0xf);
      cur_offset = 0;
    } else if (tok_type == 2) {  // D3DVSD_STREAMDATA
      if (token & 0x10000000u) {  // D3DVSD_SKIP
        uint32_t dwords = (token >> 16) & 0x1fu;
        cur_offset += (uint16_t)(dwords * 4);
      } else {  // D3DVSD_REG
        uint8_t reg = (uint8_t)(token & 0x1fu);
        uint8_t type = (uint8_t)((token >> 16) & 0x1fu);
        d3d::VertexElement e{};
        e.stream = cur_stream;
        e.offset = cur_offset;
        e.type = type;
        e.method = 0;
        e.usage = 16;  // Register index
        e.usage_index = reg;  // v0..v15
        elts.push_back(e);
        cur_offset += d3d8_decl_type_size(type);
      }
    }
  }
  return elts;
}

void emit_layout(const std::vector<d3d::VertexElement>& els) {
  rec().u32(cmd::OpSetVertexLayout);
  rec().u32((uint32_t)els.size());
  for (auto& e : els) {
    rec().u32(e.stream);
    rec().u32(e.offset);
    rec().u32(e.type);
    rec().u32(e.usage);
    rec().u32(e.usage_index);
  }
}

void set_fvf(Device8* d, uint32_t fvf) {
  d->fvf = fvf;
  d->st.fvf = fvf;
  emit_layout(fvf_elements(fvf));
}

void set_rs(Device8* d, uint32_t s, uint32_t v) {
  if (s >= 256) return;
  d->st.rs[s] = v;
  rec().op(cmd::OpSetRenderState, {s, v});
}

void set_tss(Device8* d, uint32_t stage, uint32_t t, uint32_t v) {
  if (stage >= 8 || t >= 33) return;
  d->st.tss[stage][t] = v;
  rec().op(cmd::OpSetTextureStageState, {stage, t, v});
}

void set_material(Device8* d, const d3d::Material& m) {
  d->st.material = m;
  rec().u32(cmd::OpSetMaterial);
  rec().bytes(&m, 17 * 4);
}

void set_light(Device8* d, uint32_t i, const d3d::Light& l) {
  d->st.lights[i] = l;
  rec().u32(cmd::OpSetLight);
  rec().u32(i);
  rec().u32(l.type);
  rec().bytes(&l.diffuse, 25 * 4);
}

void light_enable(Device8* d, uint32_t i, bool e) {
  if (!d->st.lights.count(i)) set_light(d, i, d3d::Light{});
  d->st.light_enable[i] = e;
  rec().op(cmd::OpLightEnable, {i, e ? 1u : 0u});
}

void set_clip_plane(Device8* d, uint32_t i, const float* p) {
  if (i >= 6) return;
  memcpy(d->st.clip_planes[i].data(), p, 16);
  rec().op(cmd::OpSetClipPlane, {i});
  rec().bytes(p, 16);
}

void emit_shaders(Device8* d) {
  uint32_t vs = guest_shader_program(d->vs_handle, false);
  uint32_t ps = guest_shader_program(d->ps_handle, true);
  rec().op(cmd::OpSetShaders, {vs, ps});
}

void set_vsc(Device8* d, uint32_t start, const float* data, uint32_t count) {
  if (start >= 256) return;
  count = std::min(count, 256u - start);
  memcpy(&d->vs_f[start * 4], data, count * 16);
  rec().op(cmd::OpSetVSConstF, {start, count});
  rec().bytes(data, count * 16);
}

void set_psc(Device8* d, uint32_t start, const float* data, uint32_t count) {
  if (start >= 224) return;
  count = std::min(count, 224u - start);
  memcpy(&d->ps_f[start * 4], data, count * 16);
  rec().op(cmd::OpSetPSConstF, {start, count});
  rec().bytes(data, count * 16);
}

void set_indices(Device8* d, uint32_t guest_ib) {
  d->index_gl = guest_ib ? guest_index_buffer_gl(guest_ib) : 0;
  if (guest_ib) {
    auto* ib = dynamic_cast<IndexBuffer8*>(com_object(guest_ib));
    d->index_fmt = ib ? ib->format : d3d::FMT_INDEX16;
  }
  rec().op(cmd::OpSetIndices, {d->index_gl, d->index_fmt == d3d::FMT_INDEX32 ? 32u : 16u});
}

// Like D3D8, the device holds a reference on whatever is bound to it, so a bound resource survives
// the game's Release (resources free their memory and GL objects when the count reaches zero).
static void swap_bound_ref(uint32_t* slot, uint32_t ptr) {
  uint32_t old = *slot;
  if (ComObject* o = ptr ? com_object(ptr) : nullptr) o->refs++;
  *slot = ptr;
  if (ComObject* o = old ? com_object(old) : nullptr) com_Release(nullptr, o);
}

void set_texture8(Device8* d, uint32_t stage, uint32_t ptr) {
  if (ptr) {
    if (auto* tex = dynamic_cast<Texture8*>(com_object(ptr))) texture_flush_dirty(tex);
  }
  uint32_t id = ptr ? guest_texture_gl(ptr) : 0;
  if (stage < 8) {
    d->bound_tex[stage] = id;
    swap_bound_ref(&d->bound_tex_guest[stage], ptr);
  }
  rec().op(cmd::OpSetTexture, {stage, id});
}

void set_stream8(Device8* d, uint32_t i, uint32_t ptr, uint32_t stride) {
  if (i >= 16) return;
  d->streams[i].gl_buf = ptr ? guest_vertex_buffer_gl(ptr) : 0;
  swap_bound_ref(&d->streams[i].guest_vb, ptr);
  d->streams[i].offset = 0;
  d->streams[i].stride = stride;
  rec().op(cmd::OpSetStream, {i, d->streams[i].gl_buf, 0u, stride});
}

void set_indices8(Device8* d, uint32_t ptr, uint32_t base) {
  swap_bound_ref(&d->index_guest, ptr);
  d->index_base = base;
  set_indices(d, ptr);
}

struct RecOp8 {
  uint32_t kind, a, b;
  std::vector<uint32_t> data;
};
std::map<StateBlock8*, std::vector<RecOp8>> g_recorded8;

enum RecKind8 : uint32_t {
  RK8_RS, RK8_TSS, RK8_TEX, RK8_XFORM, RK8_VP, RK8_MAT, RK8_LIGHT, RK8_LIGHTEN,
  RK8_CLIP, RK8_VS, RK8_PS, RK8_VSC, RK8_PSC, RK8_STREAM, RK8_IND
};

bool recording8(Device8* d, RecOp8 op) {
  if (!d->recording) return false;
  g_recorded8[d->recording].push_back(std::move(op));
  return true;
}

uint32_t prim_vertex_count(uint32_t prim, uint32_t n) {
  switch (prim) {
    case 1: return n;
    case 2: return n * 2;
    case 3: return n + 1;
    case 4: return n * 3;
    case 5: return n + 2;
    case 6: return n + 2;
    default: return 0;
  }
}

std::vector<uint32_t> read_shader_tokens(uint32_t p) {
  std::vector<uint32_t> t;
  for (uint32_t i = 0; i < 65536; i++) {
    uint32_t tok = ld32(p + 4 * i);
    t.push_back(tok);
    if (i > 0 && (tok & 0xffff) == 0xfffe) {
      uint32_t len = (tok >> 16) & 0x7fff;
      for (uint32_t k = 0; k < len; k++) t.push_back(ld32(p + 4 * (i + 1 + k)));
      i += len;
      continue;
    }
    if (tok == 0x0000ffff) break;
  }
  return t;
}

void copy_surface_rect(Surface8* src, int sx, int sy, Surface8* dst, int dx, int dy, int w, int h) {
  src->ensure_memory();
  dst->ensure_memory();
  d3d::FormatInfo f = d3d::format_info(src->format);
  if (f.block) {
    sx /= 4;
    sy /= 4;
    dx /= 4;
    dy /= 4;
    w = (w + 3) / 4;
    h = (h + 3) / 4;
    for (int y = 0; y < h; y++)
      memcpy(host(dst->mem + (uint32_t)(dy + y) * dst->pitch + (uint32_t)dx * f.block),
             host(src->mem + (uint32_t)(sy + y) * src->pitch + (uint32_t)sx * f.block), (size_t)w * f.block);
  } else {
    uint32_t bpp = f.bpp / 8;
    for (int y = 0; y < h; y++)
      memcpy(host(dst->mem + (uint32_t)(dy + y) * dst->pitch + (uint32_t)dx * bpp),
             host(src->mem + (uint32_t)(sy + y) * src->pitch + (uint32_t)sx * bpp), (size_t)w * bpp);
  }
}

void copy_mem_rect(uint32_t src_mem, uint32_t src_pitch, uint32_t dst_mem, uint32_t dst_pitch, uint32_t fmt, int sx,
                   int sy, int dx, int dy, int w, int h) {
  d3d::FormatInfo f = d3d::format_info(fmt);
  if (f.block) {
    sx /= 4;
    sy /= 4;
    dx /= 4;
    dy /= 4;
    w = (w + 3) / 4;
    h = (h + 3) / 4;
    for (int y = 0; y < h; y++)
      memcpy(host(dst_mem + (uint32_t)(dy + y) * dst_pitch + (uint32_t)dx * f.block),
             host(src_mem + (uint32_t)(sy + y) * src_pitch + (uint32_t)sx * f.block), (size_t)w * f.block);
  } else {
    uint32_t bpp = f.bpp / 8;
    for (int y = 0; y < h; y++)
      memcpy(host(dst_mem + (uint32_t)(dy + y) * dst_pitch + (uint32_t)dx * bpp),
             host(src_mem + (uint32_t)(sy + y) * src_pitch + (uint32_t)sx * bpp), (size_t)w * bpp);
  }
}

bool resolve_surface_mem(ComObject* o, uint32_t* w, uint32_t* h, uint32_t* fmt, uint32_t* pitch, uint32_t* mem) {
  if (auto* s = dynamic_cast<Surface8*>(o)) {
    *mem = s->ensure_memory();
    *w = s->width;
    *h = s->height;
    *fmt = s->format;
    *pitch = s->pitch;
    return true;
  }
  if (auto* ts = dynamic_cast<TextureLevelSurface8*>(o)) {
    Texture8* t = ts->tex;
    if (!t || ts->level >= t->levels || (t->is_cube && ts->face >= 6)) return false;
    size_t idx = t->slot_index(ts->face, ts->level);
    if (idx >= t->level.size()) return false;
    texture_ensure_mem(t, ts->level);
    TexLevel& lv = t->level[idx];
    if (!lv.mem) {
      uint32_t rows = d3d::level_rows(t->format, lv.h);
      lv.mem = heap_alloc(process_heap(), lv.pitch * rows, true);
    }
    *mem = lv.mem;
    *w = lv.w;
    *h = lv.h;
    *fmt = t->format;
    *pitch = lv.pitch;
    return true;
  }
  return false;
}

struct SwapChain8 : public ComObject {
  const char* type_name() const override { return "D3D8SwapChain"; }
  Device8* device = nullptr;
  Surface8* back_buffer = nullptr;
};

uint32_t swapchain_Present(Cpu* c, ComObject* self) {
  return Present(c, static_cast<SwapChain8*>(self)->device);
}

uint32_t swapchain_GetBackBuffer(Cpu* c, ComObject* self) {
  Surface8* bb = static_cast<SwapChain8*>(self)->back_buffer;
  if (!bb) return d3d::D3DERR_INVALIDCALL;
  bb->refs++;
  st32(arg(c, 3), expose_d3d8_surface(bb));
  return S_OK;
}

void set_transform(Device8* d, uint32_t s, const d3d::Mat4& m) {
  d->st.transforms[s] = m;
  rec().u32(cmd::OpSetTransform);
  rec().u32(s);
  for (float f : m.m) rec().f32(f);
}

const d3d::Mat4& get_transform(Device8* d, uint32_t s) {
  auto it = d->st.transforms.find(s);
  static const d3d::Mat4 ident = d3d::Mat4::identity();
  return it == d->st.transforms.end() ? ident : it->second;
}

void sync_depth_buffer_size(Device8* d) {
  const d3d::Viewport& v = d->st.viewport;
  if (!d->depth_stencil || d->render_target != d->back_buffer || !v.w || !v.h) return;
  Surface8* zs = d->depth_stencil;
  if (zs->width == v.w && zs->height == v.h) return;
  if (zs->gl_rb) rec().op(cmd::OpDestroyRenderbuffer, {zs->gl_rb});
  zs->width = v.w;
  zs->height = v.h;
  zs->gl_rb = new_gl_id();
  rec().op(cmd::OpCreateRenderbuffer, {zs->gl_rb, v.w, v.h, zs->format});
  rec().op(cmd::OpSetDepthStencil, {3u, zs->gl_rb});
}

void set_viewport(Device8* d, const d3d::Viewport& v) {
  d->st.viewport = v;
  rec().u32(cmd::OpSetViewport);
  rec().u32(v.x);
  rec().u32(v.y);
  rec().u32(v.w);
  rec().u32(v.h);
  rec().f32(v.min_z);
  rec().f32(v.max_z);
  sync_depth_buffer_size(d);
}

d3d::Mat4 read_mat(uint32_t p) {
  d3d::Mat4 m;
  memcpy(m.m, host(p), 64);
  return m;
}

d3d::Viewport read_vp(uint32_t p) {
  return d3d::Viewport{ld32(p), ld32(p + 4), ld32(p + 8), ld32(p + 12), fl(ld32(p + 16)), fl(ld32(p + 20))};
}

void recorder_create_device8(Device8* dev) {
  dev->lost = false;
  log(Log::Info, "D3D8 back buffer %ux%u MultiSampleType=%u", dev->pp[0], dev->pp[1], dev->pp[kPP8_MultiSampleType]);
  g_stream.op(cmd::OpResetState, {});
  d3d::init_default_state(dev->st);
  dev->st.rs[7] = dev->pp[kPP8_EnableAutoDepthStencil] ? 1 : 0;
  dev->st.viewport = d3d::Viewport{0, 0, dev->pp[0], dev->pp[1], 0.0f, 1.0f};
  if (!dev->back_buffer) {
    dev->back_buffer = new Surface8();
    dev->back_buffer->device = dev;
    com_wrap(dev->back_buffer, iface_d3d8_surface());
  }
  dev->back_buffer->width = dev->pp[0];
  dev->back_buffer->height = dev->pp[1];
  dev->back_buffer->format = dev->pp[2];
  dev->back_buffer->usage = d3d::USAGE_RENDERTARGET;
  dev->render_target = dev->back_buffer;
  if (!dev->depth_stencil && dev->pp[0] && dev->pp[1]) {
    auto* ds = new Surface8();
    ds->device = dev;
    ds->width = dev->pp[0];
    ds->height = dev->pp[1];
    uint32_t zfmt = dev->pp[kPP8_EnableAutoDepthStencil] ? dev->pp[kPP8_AutoDepthStencilFormat] : 0u;
    ds->format = zfmt ? zfmt : d3d::FMT_D24S8;
    ds->usage = d3d::USAGE_DEPTHSTENCIL;
    ds->pool = d3d::POOL_DEFAULT;
    ds->gl_rb = new_gl_id();
    rec().op(cmd::OpCreateRenderbuffer, {ds->gl_rb, ds->width, ds->height, ds->format});
    dev->depth_stencil = ds;
    com_wrap(ds, iface_d3d8_surface());
  }
  if (!dev->pp[kPP8_Windowed] && dev->pp[0] && dev->pp[1]) rt::user32_fullscreen_size((int)dev->pp[0], (int)dev->pp[1]);
  g_stream.op(cmd::OpBackBuffer,
              {dev->pp[0], dev->pp[1], dev->pp[kPP8_Windowed] ? 1u : 0u, dev->pp[2], dev->pp[kPP8_MultiSampleType]});
  g_stream.op(cmd::OpSetRenderTarget, {0, 1, 0, 0, 0});
  if (dev->depth_stencil)
    g_stream.op(cmd::OpSetDepthStencil, {3u, dev->depth_stencil->gl_rb});
  g_stream.op(cmd::OpSetRenderState, {7, dev->st.rs[7]});
}

uint32_t TestCooperativeLevel(Cpu*, ComObject* self) {
  if (!D(self)->lost) return S_OK;
  return d3d::app_active() ? d3d::D3DERR_DEVICENOTRESET : d3d::D3DERR_DEVICELOST;
}

uint32_t GetAvailableTextureMem(Cpu*, ComObject*) { return 256u << 20; }

uint32_t ResourceManagerDiscardBytes(Cpu*, ComObject*) { return S_OK; }

uint32_t GetDirect3D(Cpu* c, ComObject* self) {
  uint32_t p = D(self)->d3d8_guest;
  if (ComObject* o = com_object(p)) o->refs++;
  st32(arg(c, 1), p);
  return S_OK;
}

uint32_t GetDeviceCaps(Cpu* c, ComObject*) {
  fill_device_caps(arg(c, 1));
  return S_OK;
}

uint32_t GetDisplayMode(Cpu* c, ComObject* self) {
  uint32_t out = arg(c, 1);
  st32(out, D(self)->pp[0]);
  st32(out + 4, D(self)->pp[1]);
  st32(out + 8, 60);
  st32(out + 12, D(self)->pp[2]);
  return S_OK;
}

uint32_t GetCreationParameters(Cpu* c, ComObject* self) {
  uint32_t p = arg(c, 1);
  st32(p, 0);
  st32(p + 4, 1);
  st32(p + 8, D(self)->hwnd);
  st32(p + 12, D(self)->behavior);
  return S_OK;
}

uint32_t ShowCursor(Cpu*, ComObject*) { return 0; }

uint32_t GetBackBuffer(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  d->back_buffer->refs++;
  st32(arg(c, 3), expose_d3d8_surface(d->back_buffer));
  return S_OK;
}

uint32_t GetRasterStatus(Cpu* c, ComObject*) {
  st32(arg(c, 1), 0);
  st32(arg(c, 1) + 4, 0);
  return S_OK;
}

uint32_t SetGammaRamp(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  uint32_t ramp = arg(c, 2);
  for (uint32_t i = 0; i < 3 * 256; i++) d->gamma[i] = ld16(ramp + 2 * i);
  rec().u32(cmd::OpSetGammaRamp);
  for (uint32_t i = 0; i < 3 * 256; i++) rec().u32(d->gamma[i]);
  return S_OK;
}

uint32_t GetGammaRamp(Cpu* c, ComObject* self) {
  uint32_t ramp = arg(c, 1);
  for (uint32_t i = 0; i < 3 * 256; i++) st16(ramp + 2 * i, D(self)->gamma[i]);
  return S_OK;
}

uint32_t SetRenderTarget(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  ComObject* rt_obj = arg(c, 1) ? com_object(arg(c, 1)) : nullptr;
  Surface8* zs = arg(c, 2) ? dynamic_cast<Surface8*>(com_object(arg(c, 2))) : nullptr;
  if (rt_obj) {
    rt_obj->refs++;
    d->render_target = rt_obj;
    uint32_t kind = 0, id = 0, face = 0, level = 0, rtw = 0, rth = 0;
    if (rt_obj == d->back_buffer) {
      kind = 1;
      if (d->back_buffer) rtw = d->back_buffer->width, rth = d->back_buffer->height;
    } else if (auto* s = dynamic_cast<Surface8*>(rt_obj)) {
      rtw = s->width;
      rth = s->height;
      if (s->gl_tex) {
        kind = 2;
        id = s->gl_tex;
      } else if (s->gl_rb) {
        kind = 3;
        id = s->gl_rb;
      }
    } else if (auto* ts = dynamic_cast<TextureLevelSurface8*>(rt_obj)) {
      if (ts->tex && ts->level < ts->tex->levels) {
        kind = 2;
        id = ts->tex->gl_tex;
        face = ts->face;
        level = ts->level;
        size_t idx = ts->tex->slot_index(face, level);
        rtw = ts->tex->level[idx].w;
        rth = ts->tex->level[idx].h;
      }
    }
    rec().op(cmd::OpSetRenderTarget, {0, kind, id, face, level});
    if (rtw && rth) set_viewport(d, d3d::Viewport{0, 0, rtw, rth, 0.0f, 1.0f});
  }
  if (zs) {
    zs->refs++;
    d->depth_stencil = zs;
    if (zs->gl_rb) rec().op(cmd::OpSetDepthStencil, {3u, zs->gl_rb});
  } else {
    d->depth_stencil = nullptr;
    rec().op(cmd::OpSetDepthStencil, {0u, 0u});
  }
  return S_OK;
}

uint32_t GetRenderTarget(Cpu* c, ComObject* self) {
  ComObject* s = D(self)->render_target ? D(self)->render_target : D(self)->back_buffer;
  if (!s) {
    st32(arg(c, 1), 0);
    return d3d::D3DERR_NOTFOUND;
  }
  s->refs++;
  st32(arg(c, 1), expose_d3d8_surface(s));
  return S_OK;
}

uint32_t GetDepthStencilSurface(Cpu* c, ComObject* self) {
  Surface8* s = D(self)->depth_stencil;
  if (!s) {
    st32(arg(c, 1), 0);
    return d3d::D3DERR_NOTFOUND;
  }
  s->refs++;
  st32(arg(c, 1), expose_d3d8_surface(s));
  return S_OK;
}

uint32_t BeginScene(Cpu*, ComObject* self) {
  D(self)->in_scene = true;
  return S_OK;
}

uint32_t EndScene(Cpu*, ComObject* self) {
  D(self)->in_scene = false;
  return S_OK;
}

uint32_t Clear(Cpu* c, ComObject*) {
  uint32_t n = arg(c, 1), rects = arg(c, 2);
  rec().op(cmd::OpClear, {arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), rects ? n : 0});
  if (rects)
    for (uint32_t i = 0; i < n * 4; i++) rec().u32(ld32(rects + 4 * i));
  return S_OK;
}

uint32_t SetTransform(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  uint32_t s = arg(c, 1);
  d3d::Mat4 m = read_mat(arg(c, 2));
  if (d->recording) {
    RecOp8 op{RK8_XFORM, s, 0, {}};
    op.data.resize(16);
    memcpy(op.data.data(), m.m, 64);
    g_recorded8[d->recording].push_back(std::move(op));
    return S_OK;
  }
  set_transform(d, s, m);
  return S_OK;
}

uint32_t GetTransform(Cpu* c, ComObject* self) {
  memcpy(host(arg(c, 2)), get_transform(D(self), arg(c, 1)).m, 64);
  return S_OK;
}

uint32_t SetViewport(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  const uint32_t* p = (const uint32_t*)host(arg(c, 1));
  if (d->recording) {
    RecOp8 op{RK8_VP, 0, 0, {p[0], p[1], p[2], p[3], p[4], p[5]}};
    g_recorded8[d->recording].push_back(std::move(op));
    return S_OK;
  }
  set_viewport(d, read_vp(arg(c, 1)));
  return S_OK;
}

uint32_t GetViewport(Cpu* c, ComObject* self) {
  const d3d::Viewport& v = D(self)->st.viewport;
  uint32_t p = arg(c, 1);
  st32(p, v.x);
  st32(p + 4, v.y);
  st32(p + 8, v.w);
  st32(p + 12, v.h);
  st32(p + 16, f32bits(v.min_z));
  st32(p + 20, f32bits(v.max_z));
  return S_OK;
}

uint32_t SetRenderState(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  if (recording8(d, {RK8_RS, arg(c, 1), arg(c, 2), {}})) return S_OK;
  set_rs(d, arg(c, 1), arg(c, 2));
  return S_OK;
}

uint32_t GetRenderState(Cpu* c, ComObject* self) {
  uint32_t s = arg(c, 1);
  st32(arg(c, 2), s < 256 ? D(self)->st.rs[s] : 0);
  return S_OK;
}

uint32_t DrawPrimitive(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  d->draws++;
  d->prims += arg(c, 3);
  rec().op(cmd::OpDraw, {arg(c, 1), arg(c, 2), arg(c, 3)});
  return S_OK;
}

uint32_t DrawIndexedPrimitive(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  d->draws++;
  d->prims += arg(c, 5);
  rec().op(cmd::OpDrawIndexed,
           {arg(c, 1), d->index_base, arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5)});
  return S_OK;
}

uint32_t CreateTexture(Cpu* c, ComObject*) {
  uint32_t w = arg(c, 1), h = arg(c, 2), levels = arg(c, 3), usage = arg(c, 4), fmt = arg(c, 5), pool = arg(c, 6),
           out = arg(c, 7);
  if (out < kNullGuardEnd) return d3d8::D3DERR_INVALIDCALL;
  if (!w || !h) return d3d::D3DERR_INVALIDCALL;
  if (!d3d::format_info(fmt).known) {
    log(Log::Warn, "D3D8 CreateTexture: unsupported format %08x", fmt);
    return d3d::D3DERR_INVALIDCALL;
  }
  uint32_t lv = levels;
  if (!lv) {
    lv = 1;
    for (uint32_t m = std::max(w, h); m > 1; m >>= 1) lv++;
  }
  uint32_t gl = new_gl_id();
  rec().op(cmd::OpCreateTexture, {gl, 0, w, h, lv, fmt, (usage & d3d::USAGE_RENDERTARGET) ? 1u : 0u});
  Texture8* t = create_texture8(w, h, lv, usage, fmt, pool, gl);
  st32(out, wrap_texture8(t));
  return S_OK;
}

uint32_t CreateVertexBuffer(Cpu* c, ComObject*) {
  uint32_t len = arg(c, 1), usage = arg(c, 2), fvf = arg(c, 3), pool = arg(c, 4), out = arg(c, 5);
  if (out < kNullGuardEnd || !len) return d3d8::D3DERR_INVALIDCALL;
  uint32_t gl = new_gl_id();
  rec().op(cmd::OpCreateBuffer, {gl, 0, len});
  uint32_t mem = heap_alloc(process_heap(), len, true);
  VertexBuffer8* vb = create_vertex_buffer8(len, usage, fvf, pool, gl, mem);
  st32(out, wrap_vertex_buffer8(vb));
  return S_OK;
}

uint32_t SetTexture(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  uint32_t stage = arg(c, 1), ptr = arg(c, 2);
  if (ptr && !com_object(ptr)) return d3d::D3DERR_INVALIDCALL;
  if (recording8(d, {RK8_TEX, stage, ptr, {}})) return S_OK;
  set_texture8(d, stage, ptr);
  return S_OK;
}

uint32_t SetStreamSource(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  uint32_t i = arg(c, 1), ptr = arg(c, 2), stride = arg(c, 3);
  if (ptr && !com_object(ptr)) return d3d::D3DERR_INVALIDCALL;
  if (i >= 16) return d3d::D3DERR_INVALIDCALL;
  if (recording8(d, {RK8_STREAM, i, ptr, {stride}})) return S_OK;
  set_stream8(d, i, ptr, stride);
  return S_OK;
}

uint32_t SetVertexShader(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  uint32_t token = arg(c, 1);
  if (recording8(d, {RK8_VS, token, 0, {}})) return S_OK;
  uint32_t caller = ld32(c->esp);
  auto* s = dynamic_cast<Shader8*>(com_object(token & ~0x80000000u));
  if (s && !s->pixel) {
    d->vs_handle = token;
    d->fvf = 0;
    d->st.fvf = 0;
    if (trace_enabled("shaders")) log(Log::Info, "d3d8: SetVertexShader handle=%08x vs_id=%u elts=%zu caller=%08x", token, s->program_id, s->elements.size(), caller);
    emit_shaders(d);
    emit_layout(s->elements);
    return S_OK;
  }
  d->vs_handle = 0;
  set_fvf(d, token);
  emit_shaders(d);
  return S_OK;
}

uint32_t GetVertexShader(Cpu* c, ComObject* self) {
  uint32_t out = arg(c, 1);
  if (out < kNullGuardEnd) return d3d::D3DERR_INVALIDCALL;
  st32(out, D(self)->vs_handle ? D(self)->vs_handle : D(self)->fvf);
  return S_OK;
}

uint32_t SetPixelShader(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  uint32_t handle = arg(c, 1);
  if (recording8(d, {RK8_PS, handle, 0, {}})) return S_OK;
  if (trace_enabled("shaders")) log(Log::Info, "d3d8: SetPixelShader handle=%08x", handle);
  d->ps_handle = handle;
  emit_shaders(d);
  return S_OK;
}

uint32_t GetPixelShader(Cpu* c, ComObject* self) {
  uint32_t out = arg(c, 1);
  if (out < kNullGuardEnd) return d3d::D3DERR_INVALIDCALL;
  st32(out, D(self)->ps_handle);
  return S_OK;
}

uint32_t SetVertexShaderConstant(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  uint32_t start = arg(c, 1), count = arg(c, 3);
  if (start >= 12 && trace_enabled("shaders")) log(Log::Info, "d3d8: SetVertexShaderConstant start=%u count=%u", start, count);
  if (d->recording) {
    RecOp8 op{RK8_VSC, start, count, {}};
    op.data.resize(count * 4);
    memcpy(op.data.data(), host(arg(c, 2)), count * 16);
    g_recorded8[d->recording].push_back(std::move(op));
    return S_OK;
  }
  set_vsc(d, start, (const float*)host(arg(c, 2)), count);
  return S_OK;
}

uint32_t GetVertexShaderConstant(Cpu* c, ComObject* self) {
  uint32_t start = arg(c, 1), n = arg(c, 3);
  if (start + n > 256) return d3d::D3DERR_INVALIDCALL;
  memcpy(host(arg(c, 2)), &D(self)->vs_f[start * 4], n * 16);
  return S_OK;
}

uint32_t SetPixelShaderConstant(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  uint32_t start = arg(c, 1), count = arg(c, 3);
  uint32_t data = arg(c, 2);
  if (data < kNullGuardEnd) return d3d::D3DERR_INVALIDCALL;
  if (d->recording) {
    RecOp8 op{RK8_PSC, start, count, {}};
    op.data.resize(count * 4);
    memcpy(op.data.data(), host(data), count * 16);
    g_recorded8[d->recording].push_back(std::move(op));
    return S_OK;
  }
  set_psc(d, start, (const float*)host(data), count);
  return S_OK;
}

uint32_t GetPixelShaderConstant(Cpu* c, ComObject* self) {
  uint32_t start = arg(c, 1), n = arg(c, 3);
  if (start + n > 224) return d3d::D3DERR_INVALIDCALL;
  memcpy(host(arg(c, 2)), &D(self)->ps_f[start * 4], n * 16);
  return S_OK;
}

uint32_t MultiplyTransform(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  d3d::Mat4 a = get_transform(d, arg(c, 1)), b = read_mat(arg(c, 2)), r{};
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      for (int k = 0; k < 4; k++) r.m[i * 4 + j] += b.m[i * 4 + k] * a.m[k * 4 + j];
  set_transform(d, arg(c, 1), r);
  return S_OK;
}

uint32_t SetMaterial(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  if (d->recording) {
    RecOp8 op{RK8_MAT, 0, 0, {}};
    op.data.resize(17);
    memcpy(op.data.data(), host(arg(c, 1)), 68);
    g_recorded8[d->recording].push_back(std::move(op));
    return S_OK;
  }
  d3d::Material m;
  memcpy(&m, host(arg(c, 1)), 68);
  set_material(d, m);
  return S_OK;
}

uint32_t GetMaterial(Cpu* c, ComObject* self) {
  memcpy(host(arg(c, 1)), &D(self)->st.material, 68);
  return S_OK;
}

uint32_t SetLight(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  uint32_t idx = arg(c, 1), p = arg(c, 2);
  if (d->recording) {
    RecOp8 op{RK8_LIGHT, idx, 0, {}};
    op.data.resize(26);
    op.data[0] = ld32(p);
    memcpy(op.data.data() + 1, host(p + 4), 25 * 4);
    g_recorded8[d->recording].push_back(std::move(op));
    return S_OK;
  }
  d3d::Light l;
  l.type = ld32(p);
  memcpy(&l.diffuse, host(p + 4), 25 * 4);
  set_light(d, idx, l);
  return S_OK;
}

uint32_t GetLight(Cpu* c, ComObject* self) {
  auto it = D(self)->st.lights.find(arg(c, 1));
  if (it == D(self)->st.lights.end()) return d3d::D3DERR_INVALIDCALL;
  st32(arg(c, 2), it->second.type);
  memcpy(host(arg(c, 2) + 4), &it->second.diffuse, 25 * 4);
  return S_OK;
}

uint32_t LightEnable(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  if (recording8(d, {RK8_LIGHTEN, arg(c, 1), arg(c, 2), {}})) return S_OK;
  light_enable(d, arg(c, 1), arg(c, 2) != 0);
  return S_OK;
}

uint32_t GetLightEnable(Cpu* c, ComObject* self) {
  auto& e = D(self)->st.light_enable;
  auto it = e.find(arg(c, 1));
  st32(arg(c, 2), it != e.end() && it->second ? 128 : 0);
  return S_OK;
}

uint32_t SetClipPlane(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  if (d->recording) {
    const uint32_t* p = (const uint32_t*)host(arg(c, 2));
    RecOp8 op{RK8_CLIP, arg(c, 1), 0, {p[0], p[1], p[2], p[3]}};
    g_recorded8[d->recording].push_back(std::move(op));
    return S_OK;
  }
  set_clip_plane(d, arg(c, 1), (const float*)host(arg(c, 2)));
  return S_OK;
}

uint32_t GetClipPlane(Cpu* c, ComObject* self) {
  uint32_t i = arg(c, 1);
  if (i >= 6) return d3d::D3DERR_INVALIDCALL;
  memcpy(host(arg(c, 2)), D(self)->st.clip_planes[i].data(), 16);
  return S_OK;
}

uint32_t BeginStateBlock(Cpu*, ComObject* self) {
  auto* sb = new StateBlock8();
  sb->device = self;
  sb->recorded = true;
  D(self)->recording = sb;
  g_recorded8[sb].clear();
  return S_OK;
}

uint32_t EndStateBlock(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  StateBlock8* sb = d->recording;
  if (!sb) return d3d::D3DERR_INVALIDCALL;
  d->recording = nullptr;
  st32(arg(c, 1), wrap_state_block8(sb));
  return S_OK;
}

uint32_t ApplyStateBlock(Cpu* c, ComObject*) {
  auto* sb = dynamic_cast<StateBlock8*>(com_object(arg(c, 1)));
  if (!sb) return d3d::D3DERR_INVALIDCALL;
  apply_state_block8(sb);
  return S_OK;
}

uint32_t CaptureStateBlock(Cpu* c, ComObject*) {
  auto* sb = dynamic_cast<StateBlock8*>(com_object(arg(c, 1)));
  if (!sb) return d3d::D3DERR_INVALIDCALL;
  capture_state_block8(sb);
  return S_OK;
}

uint32_t DeleteStateBlock(Cpu* c, ComObject*) {
  ComObject* o = com_object(arg(c, 1));
  if (!o) return d3d::D3DERR_INVALIDCALL;
  if (auto* sb = dynamic_cast<StateBlock8*>(o)) {
    g_recorded8.erase(sb);
  }
  return com_Release(c, o);
}

uint32_t CreateStateBlock(Cpu* c, ComObject* self) {
  auto* sb = new StateBlock8();
  sb->device = self;
  sb->recorded = false;
  capture_state_block8(sb);
  st32(arg(c, 2), wrap_state_block8(sb));
  return S_OK;
}

uint32_t ValidateDevice(Cpu* c, ComObject*) {
  if (arg(c, 1) >= kNullGuardEnd) st32(arg(c, 1), 0);
  return S_OK;
}

uint32_t GetInfo(Cpu* c, ComObject*) {
  uint32_t p = arg(c, 2), n = arg(c, 3);
  if (p >= kNullGuardEnd && n) memset(host(p), 0, n);
  return S_OK;
}

uint32_t DeleteVertexShader(Cpu* c, ComObject*) {
  ComObject* o = com_object(arg(c, 1) & ~0x80000000u);
  if (!o) return d3d::D3DERR_INVALIDCALL;
  return com_Release(c, o);
}

uint32_t GetVertexShaderDeclaration(Cpu* c, ComObject*) {
  (void)arg(c, 1);
  (void)arg(c, 2);
  if (arg(c, 3) >= kNullGuardEnd) st32(arg(c, 3), 0);
  return S_OK;
}

uint32_t GetVertexShaderFunction(Cpu* c, ComObject*) {
  (void)arg(c, 1);
  if (arg(c, 3) >= kNullGuardEnd) st32(arg(c, 3), 0);
  return S_OK;
}

uint32_t DeletePixelShader(Cpu* c, ComObject*) {
  ComObject* o = com_object(arg(c, 1));
  if (!o) return d3d::D3DERR_INVALIDCALL;
  return com_Release(c, o);
}

uint32_t GetPixelShaderFunction(Cpu* c, ComObject*) {
  (void)arg(c, 1);
  if (arg(c, 3) >= kNullGuardEnd) st32(arg(c, 3), 0);
  return S_OK;
}

uint32_t SetClipStatus(Cpu*, ComObject*) { return S_OK; }

uint32_t GetClipStatus(Cpu* c, ComObject*) {
  st32(arg(c, 1), 0);
  st32(arg(c, 1) + 4, 0);
  return S_OK;
}

uint32_t GetTexture(Cpu* c, ComObject* self) {
  uint32_t stage = arg(c, 1);
  uint32_t tex = stage < 8 ? D(self)->bound_tex_guest[stage] : 0;
  if (ComObject* o = tex ? com_object(tex) : nullptr) o->refs++;
  st32(arg(c, 2), tex);
  return S_OK;
}

uint32_t SetTextureStageState(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  if (recording8(d, {RK8_TSS, arg(c, 1), arg(c, 2), {arg(c, 3)}})) return S_OK;
  set_tss(d, arg(c, 1), arg(c, 2), arg(c, 3));
  return S_OK;
}

uint32_t GetTextureStageState(Cpu* c, ComObject* self) {
  uint32_t s = arg(c, 1), t = arg(c, 2);
  st32(arg(c, 3), s < 8 && t < 33 ? D(self)->st.tss[s][t] : 0);
  return S_OK;
}

uint32_t GetStreamSource(Cpu* c, ComObject* self) {
  uint32_t i = arg(c, 1);
  if (i >= 16) return d3d::D3DERR_INVALIDCALL;
  Device8* d = D(self);
  uint32_t pp = arg(c, 2), ps = arg(c, 3);
  uint32_t vb = d->streams[i].guest_vb;
  // PilotLight sometimes passes a VB COM pointer as ppStreamData; writing through it would clobber the vtable.
  if (pp >= kNullGuardEnd && !com_object(pp)) {
    if (vb) {
      ComObject* o = com_object(vb);
      if (o) o->refs++;
    }
    st32(pp, vb);
  }
  if (ps >= kNullGuardEnd) st32(ps, d->streams[i].stride);
  return S_OK;
}

uint32_t SetIndices(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  uint32_t ptr = arg(c, 1), base = arg(c, 2);
  if (recording8(d, {RK8_IND, ptr, base, {}})) return S_OK;
  set_indices8(d, ptr, base);
  return S_OK;
}

uint32_t GetIndices(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  if (arg(c, 1) >= kNullGuardEnd) {
    if (ComObject* o = d->index_guest ? com_object(d->index_guest) : nullptr) o->refs++;
    st32(arg(c, 1), d->index_guest);
  }
  if (arg(c, 2) >= kNullGuardEnd) st32(arg(c, 2), d->index_base);
  return S_OK;
}

uint32_t CreateIndexBuffer(Cpu* c, ComObject*) {
  uint32_t len = arg(c, 1), usage = arg(c, 2), fmt = arg(c, 3), pool = arg(c, 4), out = arg(c, 5);
  if (out < kNullGuardEnd || !len) return d3d8::D3DERR_INVALIDCALL;
  if (fmt != d3d::FMT_INDEX16 && fmt != d3d::FMT_INDEX32) return d3d::D3DERR_INVALIDCALL;
  uint32_t gl = new_gl_id();
  rec().op(cmd::OpCreateBuffer, {gl, 1, len});
  uint32_t mem = heap_alloc(process_heap(), len, true);
  IndexBuffer8* ib = create_index_buffer8(len, usage, fmt, pool, gl, mem);
  st32(out, wrap_index_buffer8(ib));
  return S_OK;
}

uint32_t CreateRenderTarget(Cpu* c, ComObject* self) {
  uint32_t w = arg(c, 1), h = arg(c, 2), fmt = arg(c, 3), out = arg(c, 6);
  if (out < kNullGuardEnd || !w || !h) return d3d8::D3DERR_INVALIDCALL;
  auto* s = new Surface8();
  s->device = D(self);
  s->width = w;
  s->height = h;
  s->format = fmt;
  s->usage = d3d::USAGE_RENDERTARGET;
  s->pool = d3d::POOL_DEFAULT;
  s->gl_tex = new_gl_id();
  rec().op(cmd::OpCreateTexture, {s->gl_tex, 0, w, h, 1, fmt, 1u});
  st32(out, expose_d3d8_surface(s));
  return S_OK;
}

uint32_t CreateDepthStencilSurface(Cpu* c, ComObject* self) {
  uint32_t w = arg(c, 1), h = arg(c, 2), fmt = arg(c, 3), out = arg(c, 5);
  if (out < kNullGuardEnd || !w || !h) return d3d8::D3DERR_INVALIDCALL;
  auto* s = new Surface8();
  s->device = D(self);
  s->width = w;
  s->height = h;
  s->format = fmt;
  s->usage = d3d::USAGE_DEPTHSTENCIL;
  s->pool = d3d::POOL_DEFAULT;
  s->gl_rb = new_gl_id();
  rec().op(cmd::OpCreateRenderbuffer, {s->gl_rb, w, h, fmt});
  st32(out, expose_d3d8_surface(s));
  return S_OK;
}

uint32_t CreateImageSurface(Cpu* c, ComObject* self) {
  uint32_t w = arg(c, 1), h = arg(c, 2), fmt = arg(c, 3), out = arg(c, 4);
  if (out < kNullGuardEnd || !w || !h) return d3d8::D3DERR_INVALIDCALL;
  auto* s = new Surface8();
  s->device = D(self);
  s->width = w;
  s->height = h;
  s->format = fmt;
  s->pool = d3d::POOL_SYSTEMMEM;
  st32(out, expose_d3d8_surface(s));
  return S_OK;
}

// GPU-resident surface (back buffer, RT texture level, RT surface) -> executor Target {kind,id,face,level}.
static bool gpu_target(Device8* d, ComObject* o, uint32_t t[4], bool need_rt) {
  t[0] = t[1] = t[2] = t[3] = 0;
  if (o == d->back_buffer) return t[0] = 1, true;
  if (auto* s = dynamic_cast<Surface8*>(o)) {
    if (s->gl_tex) return t[0] = 2, t[1] = s->gl_tex, true;
    return false;
  }
  if (auto* ts = dynamic_cast<TextureLevelSurface8*>(o)) {
    if (!ts->tex || !ts->tex->gl_tex || ts->level >= ts->tex->levels || (need_rt && !(ts->tex->usage & d3d::USAGE_RENDERTARGET))) return false;
    t[0] = 2, t[1] = ts->tex->gl_tex, t[2] = ts->face, t[3] = ts->level;
    return true;
  }
  return false;
}

uint32_t CopyRects(Cpu* c, ComObject* self) {
  ComObject* src_obj = com_object(arg(c, 1));
  uint32_t rects = arg(c, 2);
  (void)arg(c, 3);  // cRects (only first rect used when non-null)
  ComObject* dst_obj = com_object(arg(c, 4));
  uint32_t points = arg(c, 5);
  if (!src_obj || !dst_obj) return d3d::D3DERR_INVALIDCALL;
  // Source lives on the GPU (e.g. bloom copies the back buffer into a texture): copy there, CPU memory is stale.
  uint32_t gs[4], gd[4];
  Device8* dev = D(self);
  if (gpu_target(dev, src_obj, gs, true) && gpu_target(dev, dst_obj, gd, false)) {
    uint32_t sw = 0, sh = 0;
    if (gs[0] == 1) sw = dev->back_buffer->width, sh = dev->back_buffer->height;
    else if (auto* ss = dynamic_cast<Surface8*>(src_obj)) sw = ss->width, sh = ss->height;
    else {
      auto* ts = static_cast<TextureLevelSurface8*>(src_obj);
      auto& lv = ts->tex->level[ts->tex->slot_index(ts->face, ts->level)];
      sw = lv.w, sh = lv.h;
    }
    uint32_t r[4] = {0, 0, sw, sh}, px = 0, py = 0;
    if (rects) r[0] = ld32(rects), r[1] = ld32(rects + 4), r[2] = ld32(rects + 8), r[3] = ld32(rects + 12);
    if (rects && points) px = ld32(points), py = ld32(points + 4);
    rec().op(cmd::OpStretchRect, {gs[0], gs[1], gs[2], gs[3], r[0], r[1], r[2], r[3], gd[0], gd[1], gd[2], gd[3], px, py, px + r[2] - r[0], py + r[3] - r[1], 1u});
    return S_OK;
  }
  uint32_t sw = 0, sh = 0, sfmt = 0, spitch = 0, smem = 0;
  uint32_t dw = 0, dh = 0, dfmt = 0, dpitch = 0, dmem = 0;
  if (!resolve_surface_mem(src_obj, &sw, &sh, &sfmt, &spitch, &smem) ||
      !resolve_surface_mem(dst_obj, &dw, &dh, &dfmt, &dpitch, &dmem))
    return d3d::D3DERR_INVALIDCALL;
  if (!rects) {
    copy_mem_rect(smem, spitch, dmem, dpitch, sfmt, 0, 0, 0, 0, (int)sw, (int)sh);
  } else {
    copy_mem_rect(smem, spitch, dmem, dpitch, sfmt, (int)ld32(rects), (int)ld32(rects + 4),
                  points ? (int)ld32(points) : 0, points ? (int)ld32(points + 4) : 0,
                  (int)ld32(rects + 8) - (int)ld32(rects), (int)ld32(rects + 12) - (int)ld32(rects + 4));
  }
  if (auto* dst_s = dynamic_cast<Surface8*>(dst_obj)) {
    if (dst_s->gl_tex) upload_surface8(dst_s);
  } else if (auto* dst_ts = dynamic_cast<TextureLevelSurface8*>(dst_obj)) {
    if (dst_ts->tex) upload_tex_face_level(dst_ts->tex, dst_ts->face, dst_ts->level);
  }
  return S_OK;
}

uint32_t UpdateTexture(Cpu* c, ComObject*) {
  Texture8* src = dynamic_cast<Texture8*>(com_object(arg(c, 1)));
  Texture8* dst = dynamic_cast<Texture8*>(com_object(arg(c, 2)));
  if (!src || !dst) return d3d::D3DERR_INVALIDCALL;
  texture_copy_levels(src, dst);
  return S_OK;
}

uint32_t GetFrontBuffer(Cpu* c, ComObject* self) {
  Surface8* dst = static_cast<Surface8*>(com_object(arg(c, 1)));
  Surface8* src = D(self)->back_buffer;
  if (!dst || !src) return d3d::D3DERR_INVALIDCALL;
  copy_surface_rect(src, 0, 0, dst, 0, 0, (int)src->width, (int)src->height);
  return S_OK;
}

uint32_t CreateVolumeTexture(Cpu*, ComObject*) { return d3d::D3DERR_INVALIDCALL; }

uint32_t CreateCubeTexture(Cpu* c, ComObject*) {
  uint32_t edge = arg(c, 1), levels = arg(c, 2), usage = arg(c, 3), fmt = arg(c, 4), pool = arg(c, 5), out = arg(c, 6);
  if (out < kNullGuardEnd || !edge || !d3d::format_info(fmt).known) return d3d8::D3DERR_INVALIDCALL;
  uint32_t lv = levels;
  if (!lv) {
    lv = 1;
    for (uint32_t m = edge; m > 1; m >>= 1) lv++;
  }
  uint32_t gl = new_gl_id();
  rec().op(cmd::OpCreateTexture, {gl, 1u, edge, edge, lv, fmt, (usage & d3d::USAGE_RENDERTARGET) ? 1u : 0u});
  Texture8* t = create_cube_texture8(edge, lv, usage, fmt, pool, gl);
  st32(out, wrap_cube_texture8(t));
  return S_OK;
}

uint32_t CreateAdditionalSwapChain(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  uint32_t pp = arg(c, 1), out = arg(c, 2);
  if (out < kNullGuardEnd || pp < kNullGuardEnd) return d3d8::D3DERR_INVALIDCALL;
  uint32_t w = ld32(pp), h = ld32(pp + 4), fmt = ld32(pp + 8);
  if (!w) w = d->pp[0];
  if (!h) h = d->pp[1];
  if (!fmt) fmt = d->pp[2];
  st32(pp, w);
  st32(pp + 4, h);
  st32(pp + 8, fmt);
  auto* sc = new SwapChain8();
  sc->device = d;
  sc->back_buffer = new Surface8();
  sc->back_buffer->device = d;
  sc->back_buffer->width = w;
  sc->back_buffer->height = h;
  sc->back_buffer->format = fmt;
  sc->back_buffer->usage = d3d::USAGE_RENDERTARGET;
  sc->back_buffer->pool = d3d::POOL_DEFAULT;
  sc->back_buffer->gl_tex = new_gl_id();
  rec().op(cmd::OpCreateTexture, {sc->back_buffer->gl_tex, 0u, w, h, 1u, fmt, 1u});
  com_wrap(sc->back_buffer, iface_d3d8_surface());
  st32(out, com_wrap(sc, iface_d3d8_swap_chain()));
  return S_OK;
}

uint32_t DrawPrimitiveUP(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  uint32_t prim = arg(c, 1), n = arg(c, 2), data = arg(c, 3), stride = arg(c, 4);
  uint32_t bytes = prim_vertex_count(prim, n) * stride;
  d->draws++;
  d->prims += n;
  rec().op(cmd::OpDrawUP, {prim, n, stride, bytes});
  rec().bytes(host(data), bytes);
  return S_OK;
}

uint32_t DrawIndexedPrimitiveUP(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  uint32_t prim = arg(c, 1), minv = arg(c, 2), nverts = arg(c, 3), n = arg(c, 4), idx = arg(c, 5), fmt = arg(c, 6),
           data = arg(c, 7), stride = arg(c, 8);
  if (!stride) stride = d->streams[0].stride ? d->streams[0].stride : 32;
  uint32_t isize = fmt == d3d::FMT_INDEX32 ? 4 : 2;
  uint32_t ibytes = prim_vertex_count(prim, n) * isize;
  uint32_t vbytes = (minv + nverts) * stride;
  d->draws++;
  d->prims += n;
  rec().op(cmd::OpDrawIndexedUP, {prim, minv, nverts, n, isize * 8, stride, vbytes, ibytes});
  rec().bytes(host(data), vbytes);
  rec().bytes(host(idx), ibytes);
  return S_OK;
}

uint32_t ProcessVertices(Cpu*, ComObject*) { return d3d::D3DERR_INVALIDCALL; }

uint32_t CreateVertexShader(Cpu* c, ComObject*) {
  uint32_t decl = arg(c, 1), func = arg(c, 2), out = arg(c, 3);
  (void)arg(c, 4);  // Usage
  if (out < kNullGuardEnd) return d3d8::D3DERR_INVALIDCALL;
  auto* s = new Shader8();
  s->pixel = false;
  s->tokens = read_shader_tokens(func);
  s->elements = parse_d3d8_decl(decl);
  s->program_id = new_gl_id();
  rec().op(cmd::OpCreateShader, {s->program_id, 0u, (uint32_t)s->tokens.size()});
  rec().bytes(s->tokens.data(), (uint32_t)s->tokens.size() * 4);
  uint32_t wrapped = wrap_shader8(s);
  if (trace_enabled("shaders")) log(Log::Info, "d3d8: CreateVertexShader id=%u elts=%zu tokens=%zu wrapped=%08x caller=%08x",
      s->program_id, s->elements.size(), s->tokens.size(), wrapped, ld32(c->esp));
  st32(out, wrapped);
  return S_OK;
}

uint32_t CreatePixelShader(Cpu* c, ComObject*) {
  uint32_t tokens = arg(c, 1), out = arg(c, 2);
  if (out < kNullGuardEnd) return d3d8::D3DERR_INVALIDCALL;
  auto* s = new Shader8();
  s->pixel = true;
  s->tokens = read_shader_tokens(tokens);
  s->program_id = new_gl_id();
  rec().op(cmd::OpCreateShader, {s->program_id, 1u, (uint32_t)s->tokens.size()});
  rec().bytes(s->tokens.data(), (uint32_t)s->tokens.size() * 4);
  uint32_t wrapped = wrap_shader8(s);
  if (trace_enabled("shaders")) log(Log::Info, "d3d8: CreatePixelShader id=%u tokens=%zu wrapped=%08x", s->program_id, s->tokens.size(), wrapped);
  st32(out, wrapped);
  return S_OK;
}

uint32_t SetPaletteEntries(Cpu*, ComObject*) { return S_OK; }
uint32_t GetPaletteEntries(Cpu*, ComObject*) { return S_OK; }
uint32_t SetCurrentTexturePalette(Cpu*, ComObject*) { return S_OK; }
uint32_t GetCurrentTexturePalette(Cpu* c, ComObject*) {
  st32(arg(c, 1), 0);
  return S_OK;
}

uint32_t DrawRectPatch(Cpu*, ComObject*) { return d3d::D3DERR_INVALIDCALL; }
uint32_t DrawTriPatch(Cpu*, ComObject*) { return d3d::D3DERR_INVALIDCALL; }
uint32_t DeletePatch(Cpu*, ComObject*) { return d3d::D3DERR_INVALIDCALL; }

uint32_t ok3(Cpu*, ComObject*) { return S_OK; }

uint32_t Reset(Cpu* c, ComObject* self) {
  Device8* d = D(self);
  uint32_t pp = arg(c, 1);
  if (pp >= kNullGuardEnd) {
    for (int i = 0; i < 13; i++) d->pp[i] = ld32(pp + 4 * (uint32_t)i);
    if (!d->pp[0] || !d->pp[1]) {
      int w, h;
      plat::get_window_size(&w, &h);
      if (!w || !h) {
        w = 800;
        h = 600;
      }
      d->pp[0] = (uint32_t)w;
      d->pp[1] = (uint32_t)h;
      st32(pp, d->pp[0]);
      st32(pp + 4, d->pp[1]);
    }
    recorder_create_device8(d);
  }
  d->lost = false;
  return S_OK;
}

uint32_t Present(Cpu* c, ComObject* self) {
  rt::user32_pump();
  Device8* d = D(self);
  if (d->lost) {
    flush_stream(false);
    return d3d::D3DERR_DEVICELOST;
  }
  rec().op(cmd::OpPresent, {(uint32_t)d->frame});
  d->frame++;
  static auto t_last = std::chrono::steady_clock::now();
  static uint64_t f_last = 0;
  if (trace_enabled("frames") || d->frame <= 3 || d->frame % 600 == 0) {
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - t_last).count();
    log(Log::Info, "D3D8 Present #%llu (%u draws, %u primitives, %.1f fps)", (unsigned long long)d->frame, d->draws,
        d->prims, secs > 0 ? (double)(d->frame - f_last) / secs : 0.0);
    t_last = now;
    f_last = d->frame;
    if (trace_enabled("mem")) mem_log_stats();
  }
  d->draws = d->prims = 0;
  flush_stream(true);
  return S_OK;
}

}  // namespace

uint32_t create_d3d8_device(Cpu* c, ComObject*) {
  d3d8_verify_registered_nargs();
  uint32_t adapter = arg(c, 1), type = arg(c, 2), hwnd = arg(c, 3), behavior = arg(c, 4), pp = arg(c, 5), out = arg(c, 6);
  if (adapter != 0) {
    log(Log::Warn, "D3D8 CreateDevice: invalid adapter %u", adapter);
    return d3d8::D3DERR_INVALIDCALL;
  }
  if (type != 1) {
    log(Log::Warn, "D3D8 CreateDevice: unsupported device type %u", type);
    return d3d::D3DERR_NOTAVAILABLE;
  }
  if (out < kNullGuardEnd || pp < kNullGuardEnd) {
    log(Log::Warn, "D3D8 CreateDevice: null presentation parameters or out pointer");
    return d3d8::D3DERR_INVALIDCALL;
  }
  auto* dev = new Device8();
  dev->hwnd = hwnd;
  dev->behavior = behavior;
  for (int i = 0; i < 13; i++) dev->pp[i] = ld32(pp + 4 * (uint32_t)i);
  if (!dev->pp[0] || !dev->pp[1]) {
    int w, h;
    plat::get_window_size(&w, &h);
    if (!w || !h) {
      w = 800;
      h = 600;
    }
    dev->pp[0] = (uint32_t)w;
    dev->pp[1] = (uint32_t)h;
    st32(pp, dev->pp[0]);
    st32(pp + 4, dev->pp[1]);
  }
  if (dev->pp[2] == d3d::FMT_UNKNOWN) {
    dev->pp[2] = d3d::FMT_X8R8G8B8;
    st32(pp + 8, dev->pp[2]);
  }
  log(Log::Info,
      "D3D8 CreateDevice(adapter=%u, type=%u, hwnd=%08x, behavior=%08x): %ux%u, backbuffer fmt=%08x, windowed=%u, "
      "autodepth=%u (%s), interval=%08x",
      adapter, type, hwnd, behavior, dev->pp[0], dev->pp[1], dev->pp[2], dev->pp[kPP8_Windowed],
      dev->pp[kPP8_EnableAutoDepthStencil], d3d::format_name(dev->pp[kPP8_AutoDepthStencilFormat]),
      dev->pp[kPP8_PresentationInterval]);
  if (!(behavior & 0x2)) c->fcw = (c->fcw & ~0x300u);
  dev->d3d8_guest = arg(c, 0);
  recorder_create_device8(dev);
  dev->guest_ptr = com_wrap(dev, iface_d3d8_device());
  st32(out, dev->guest_ptr);
  return S_OK;
}

static uint32_t dev_Release(Cpu* c, ComObject* self) {
  int32_t r = --self->refs;
  if (r <= 0) {
    auto* dev = D(self);
    if (dev && dev->guest_ptr && dev->guest_ptr == ld32(0x7653b0u)) {
      self->refs = 1;
      return 1;
    }
    delete self;
  }
  return (uint32_t)std::max<int32_t>(r, 0);
}

rt::ComInterface& iface_d3d8_device() {
  static ComInterface i("IDirect3DDevice8",
                        {{"QueryInterface", 2, com_QueryInterface_none},
                         {"AddRef", 0, com_AddRef},
                         {"Release", 0, dev_Release},
                         {"TestCooperativeLevel", 0, TestCooperativeLevel},
                         {"GetAvailableTextureMem", 0, GetAvailableTextureMem},
                         {"ResourceManagerDiscardBytes", 1, ResourceManagerDiscardBytes},
                         {"GetDirect3D", 1, GetDirect3D},
                         {"GetDeviceCaps", 1, GetDeviceCaps},
                         {"GetDisplayMode", 1, GetDisplayMode},
                         {"GetCreationParameters", 1, GetCreationParameters},
                         {"SetCursorProperties", 3, ok3},
                         {"SetCursorPosition", 3, ok3},
                         {"ShowCursor", 1, ShowCursor},
                         {"CreateAdditionalSwapChain", 2, CreateAdditionalSwapChain},
                         {"Reset", 1, Reset},
                         {"Present", 4, Present},
                         {"GetBackBuffer", 3, GetBackBuffer},
                         {"GetRasterStatus", 1, GetRasterStatus},
                         {"SetGammaRamp", 2, SetGammaRamp},
                         {"GetGammaRamp", 1, GetGammaRamp},
                         {"CreateTexture", 7, CreateTexture},
                         {"CreateVolumeTexture", 8, CreateVolumeTexture},
                         {"CreateCubeTexture", 6, CreateCubeTexture},
                         {"CreateVertexBuffer", 5, CreateVertexBuffer},
                         {"CreateIndexBuffer", 5, CreateIndexBuffer},
                         {"CreateRenderTarget", 6, CreateRenderTarget},
                         {"CreateDepthStencilSurface", 5, CreateDepthStencilSurface},
                         {"CreateImageSurface", 4, CreateImageSurface},
                         {"CopyRects", 5, CopyRects},
                         {"UpdateTexture", 2, UpdateTexture},
                         {"GetFrontBuffer", 1, GetFrontBuffer},
                         {"SetRenderTarget", 2, SetRenderTarget},
                         {"GetRenderTarget", 1, GetRenderTarget},
                         {"GetDepthStencilSurface", 1, GetDepthStencilSurface},
                         {"BeginScene", 0, BeginScene},
                         {"EndScene", 0, EndScene},
                         {"Clear", 6, Clear},
                         {"SetTransform", 2, SetTransform},
                         {"GetTransform", 2, GetTransform},
                         {"MultiplyTransform", 2, MultiplyTransform},
                         {"SetViewport", 1, SetViewport},
                         {"GetViewport", 1, GetViewport},
                         {"SetMaterial", 1, SetMaterial},
                         {"GetMaterial", 1, GetMaterial},
                         {"SetLight", 2, SetLight},
                         {"GetLight", 2, GetLight},
                         {"LightEnable", 2, LightEnable},
                         {"GetLightEnable", 2, GetLightEnable},
                         {"SetClipPlane", 2, SetClipPlane},
                         {"GetClipPlane", 2, GetClipPlane},
                         {"SetRenderState", 2, SetRenderState},
                         {"GetRenderState", 2, GetRenderState},
                         {"BeginStateBlock", 0, BeginStateBlock},
                         {"EndStateBlock", 1, EndStateBlock},
                         {"ApplyStateBlock", 1, ApplyStateBlock},
                         {"CaptureStateBlock", 1, CaptureStateBlock},
                         {"DeleteStateBlock", 1, DeleteStateBlock},
                         {"CreateStateBlock", 2, CreateStateBlock},
                         {"SetClipStatus", 1, SetClipStatus},
                         {"GetClipStatus", 1, GetClipStatus},
                         {"GetTexture", 2, GetTexture},
                         {"SetTexture", 2, SetTexture},
                         {"GetTextureStageState", 3, GetTextureStageState},
                         {"SetTextureStageState", 3, SetTextureStageState},
                         {"ValidateDevice", 1, ValidateDevice},
                         {"GetInfo", 3, GetInfo},
                         {"SetPaletteEntries", 2, SetPaletteEntries},
                         {"GetPaletteEntries", 2, GetPaletteEntries},
                         {"SetCurrentTexturePalette", 1, SetCurrentTexturePalette},
                         {"GetCurrentTexturePalette", 1, GetCurrentTexturePalette},
                         {"DrawPrimitive", 3, DrawPrimitive},
                         {"DrawIndexedPrimitive", 5, DrawIndexedPrimitive},
                         {"DrawPrimitiveUP", 4, DrawPrimitiveUP},
                         {"DrawIndexedPrimitiveUP", 8, DrawIndexedPrimitiveUP},
                         {"ProcessVertices", 5, ProcessVertices},
                         {"CreateVertexShader", 4, CreateVertexShader},
                         {"SetVertexShader", 1, SetVertexShader},
                         {"GetVertexShader", 1, GetVertexShader},
                         {"DeleteVertexShader", 1, DeleteVertexShader},
                         {"SetVertexShaderConstant", 3, SetVertexShaderConstant},
                         {"GetVertexShaderConstant", 3, GetVertexShaderConstant},
                         {"GetVertexShaderDeclaration", 3, GetVertexShaderDeclaration},
                         {"GetVertexShaderFunction", 3, GetVertexShaderFunction},
                         {"SetStreamSource", 3, SetStreamSource},
                         {"GetStreamSource", 3, GetStreamSource},
                         {"SetIndices", 2, SetIndices},
                         {"GetIndices", 2, GetIndices},
                         {"CreatePixelShader", 2, CreatePixelShader},
                         {"SetPixelShader", 1, SetPixelShader},
                         {"GetPixelShader", 1, GetPixelShader},
                         {"DeletePixelShader", 1, DeletePixelShader},
                         {"SetPixelShaderConstant", 3, SetPixelShaderConstant},
                         {"GetPixelShaderConstant", 3, GetPixelShaderConstant},
                         {"GetPixelShaderFunction", 3, GetPixelShaderFunction},
                         {"DrawRectPatch", 3, DrawRectPatch},
                         {"DrawTriPatch", 3, DrawTriPatch},
                         {"DeletePatch", 1, DeletePatch}},
                        &g_api_lock);
  return i;
}

void fill_device_caps(uint32_t p) {
  memset(host(p), 0, 212);
  st32(p + 0, 1);
  st32(p + 4, 0);
  st32(p + 8, 0x00020000);
  st32(p + 12, 0x20000000 | 0x00080000 | 0x00020000 | 0x00000020);
  st32(p + 16, 0x00000080);
  st32(p + 20, 0x80000000 | 0x0000000f);
  st32(p + 24, 0x00000001);
  st32(p + 28, 0x001bbef0);
  st32(p + 32, 0x000f2ef2);
  st32(p + 36, 0x0007a091);
  st32(p + 40, 0x000000ff);
  st32(p + 44, 0x00003fff);
  st32(p + 48, 0x00003fff);
  st32(p + 52, 0x000000ff);
  st32(p + 56, 0x000ffff8);
  st32(p + 60, 0x0001ec45);
  st32(p + 64, 0x03030700);
  st32(p + 68, 0x03030700);
  st32(p + 72, 0x03030700);
  st32(p + 76, 0x0000003f);
  st32(p + 80, 0x0000003f);
  st32(p + 84, 0x00000003);
  st32(p + 88, 4096);
  st32(p + 92, 4096);
  st32(p + 96, 256);
  st32(p + 100, 8192);
  st32(p + 104, 4096);
  st32(p + 108, 8);
  st32(p + 112, f32bits(1e10f));
  st32(p + 116, f32bits(-1e8f));
  st32(p + 120, f32bits(-1e8f));
  st32(p + 124, f32bits(1e8f));
  st32(p + 128, f32bits(1e8f));
  st32(p + 132, f32bits(1.0f));
  st32(p + 136, 0x000001ff);
  st32(p + 140, 0x0008ffff);
  st32(p + 144, 0x03ffffff);
  st32(p + 148, 8);
  st32(p + 152, 8);
  st32(p + 156, 0x0000013b);
  st32(p + 160, 8);
  st32(p + 164, 6);
  st32(p + 168, 4);
  st32(p + 172, 0);
  st32(p + 176, f32bits(64.0f));
  st32(p + 180, 0x000fffff);
  st32(p + 184, 0x000fffff);
  st32(p + 188, 16);
  st32(p + 192, 255);
  st32(p + 196, 0xfffe0101);
  st32(p + 200, 256);
  st32(p + 204, 0xffff0101);
  st32(p + 208, f32bits(8.0f));
}

cmd::Stream& d3d8_cmd_stream() { return g_stream; }

void upload_surface8(Surface8* s) {
  if (!s->mem || !s->gl_tex) return;
  uint32_t rows = d3d::level_rows(s->format, s->height);
  uint32_t bytes = s->pitch * rows;
  cmd::Stream& stream = d3d8_cmd_stream();
  stream.op(cmd::OpUploadTexture, {s->gl_tex, 0, 0, s->width, s->height, s->format, s->pitch, rows, bytes});
  stream.bytes(host(s->mem), bytes);
  s->dirty = false;
}

void apply_state_block8(StateBlock8* sb) {
  Device8* d = static_cast<Device8*>(sb->device);
  if (!d) return;
  if (sb->recorded) {
    for (const auto& op : g_recorded8[sb]) {
      switch (op.kind) {
        case RK8_RS: set_rs(d, op.a, op.b); break;
        case RK8_TSS: set_tss(d, op.a, op.b, op.data[0]); break;
        case RK8_TEX: set_texture8(d, op.a, op.b); break;
        case RK8_XFORM: {
          d3d::Mat4 m;
          memcpy(m.m, op.data.data(), 64);
          set_transform(d, op.a, m);
          break;
        }
        case RK8_VP:
          set_viewport(d, d3d::Viewport{op.data[0], op.data[1], op.data[2], op.data[3], fl(op.data[4]), fl(op.data[5])});
          break;
        case RK8_MAT: {
          d3d::Material m;
          memcpy(&m, op.data.data(), 68);
          set_material(d, m);
          break;
        }
        case RK8_LIGHT: {
          d3d::Light l;
          l.type = op.data[0];
          memcpy(&l.diffuse, op.data.data() + 1, 25 * 4);
          set_light(d, op.a, l);
          break;
        }
        case RK8_LIGHTEN: light_enable(d, op.a, op.b != 0); break;
        case RK8_CLIP: set_clip_plane(d, op.a, (const float*)op.data.data()); break;
        case RK8_VS: {
          auto* s = dynamic_cast<Shader8*>(com_object(op.a & ~0x80000000u));
          if (s && !s->pixel) {
            d->vs_handle = op.a;
            d->fvf = 0;
            d->st.fvf = 0;
            emit_shaders(d);
            emit_layout(s->elements);
          } else {
            d->vs_handle = 0;
            set_fvf(d, op.a);
            emit_shaders(d);
          }
          break;
        }
        case RK8_PS:
          d->ps_handle = op.a;
          emit_shaders(d);
          break;
        case RK8_VSC:
          set_vsc(d, op.a, (const float*)op.data.data(), op.b);
          break;
        case RK8_PSC:
          set_psc(d, op.a, (const float*)op.data.data(), op.b);
          break;
        case RK8_STREAM:
          set_stream8(d, op.a, op.b, op.data[0]);
          break;
        case RK8_IND:
          set_indices8(d, op.a, op.b);
          break;
      }
    }
    return;
  }
  for (uint32_t i = 0; i < 256; i++) {
    if (d->st.rs[i] != sb->snapshot.rs[i]) set_rs(d, i, sb->snapshot.rs[i]);
  }
  for (uint32_t t = 0; t < 8; t++)
    for (uint32_t k = 0; k < 33; k++) {
      if (d->st.tss[t][k] != sb->snapshot.tss[t][k]) set_tss(d, t, k, sb->snapshot.tss[t][k]);
    }
  for (auto& [k, m] : sb->snapshot.transforms) set_transform(d, k, m);
  set_viewport(d, sb->snapshot.viewport);
  set_material(d, sb->snapshot.material);
  for (auto& [i, l] : sb->snapshot.lights) set_light(d, i, l);
  for (auto& [i, e] : sb->snapshot.light_enable) light_enable(d, i, e);
  d->st = sb->snapshot;

  for (uint32_t stage = 0; stage < 8; stage++) {
    if (d->bound_tex[stage] != sb->bound_tex[stage] || d->bound_tex_guest[stage] != sb->bound_tex_guest[stage]) {
      set_texture8(d, stage, sb->bound_tex_guest[stage]);
    }
  }

  for (uint32_t i = 0; i < 16; i++) {
    const auto& s = sb->streams[i];
    if (d->streams[i].gl_buf != s.gl_buf || d->streams[i].guest_vb != s.guest_vb ||
        d->streams[i].offset != s.offset || d->streams[i].stride != s.stride) {
      swap_bound_ref(&d->streams[i].guest_vb, s.guest_vb);
      d->streams[i].gl_buf = s.guest_vb ? guest_vertex_buffer_gl(s.guest_vb) : 0;  // the snapshot may be stale
      d->streams[i].offset = s.offset;
      d->streams[i].stride = s.stride;
      rec().op(cmd::OpSetStream, {i, d->streams[i].gl_buf, s.offset, s.stride});
    }
  }

  if (d->index_gl != sb->index_gl || d->index_fmt != sb->index_fmt ||
      d->index_guest != sb->index_guest || d->index_base != sb->index_base) {
    swap_bound_ref(&d->index_guest, sb->index_guest);
    d->index_gl = sb->index_guest ? guest_index_buffer_gl(sb->index_guest) : 0;
    d->index_fmt = sb->index_fmt;
    d->index_base = sb->index_base;
    rec().op(cmd::OpSetIndices, {d->index_gl, d->index_fmt == d3d::FMT_INDEX32 ? 32u : 16u});
  }

  if (d->vs_f != sb->vs_f) {
    uint32_t first = 0, last = 255;
    while (first < 256 && memcmp(&d->vs_f[first * 4], &sb->vs_f[first * 4], 16) == 0) first++;
    while (last > first && memcmp(&d->vs_f[last * 4], &sb->vs_f[last * 4], 16) == 0) last--;
    if (first <= last) {
      set_vsc(d, first, &sb->vs_f[first * 4], last - first + 1);
    }
  }

  if (d->ps_f != sb->ps_f) {
    uint32_t first = 0, last = 223;
    while (first < 224 && memcmp(&d->ps_f[first * 4], &sb->ps_f[first * 4], 16) == 0) first++;
    while (last > first && memcmp(&d->ps_f[last * 4], &sb->ps_f[last * 4], 16) == 0) last--;
    if (first <= last) {
      set_psc(d, first, &sb->ps_f[first * 4], last - first + 1);
    }
  }

  bool fvf_changed = (d->fvf != sb->fvf);

  d->vs_handle = sb->vs_handle;
  d->ps_handle = sb->ps_handle;
  d->fvf = sb->fvf;
  d->st.fvf = sb->fvf;

  if (d->vs_handle) {
    auto* s = dynamic_cast<Shader8*>(com_object(d->vs_handle & ~0x80000000u));
    if (s && !s->pixel) emit_layout(s->elements);
  } else if (fvf_changed || d->fvf) {
    emit_layout(fvf_elements(d->fvf));
  }
  emit_shaders(d);
}

void capture_state_block8(StateBlock8* sb) {
  Device8* d = static_cast<Device8*>(sb->device);
  if (!d) return;
  if (!sb->recorded) {
    sb->snapshot = d->st;
    memcpy(sb->bound_tex, d->bound_tex, sizeof(d->bound_tex));
    memcpy(sb->bound_tex_guest, d->bound_tex_guest, sizeof(d->bound_tex_guest));
    memcpy(sb->streams, d->streams, sizeof(d->streams));
    sb->fvf = d->fvf;
    sb->vs_handle = d->vs_handle;
    sb->ps_handle = d->ps_handle;
    sb->index_gl = d->index_gl;
    sb->index_fmt = d->index_fmt;
    sb->index_guest = d->index_guest;
    sb->index_base = d->index_base;
    sb->vs_f = d->vs_f;
    sb->ps_f = d->ps_f;
  } else {
    for (auto& op : g_recorded8[sb]) {
      switch (op.kind) {
        case RK8_RS: op.b = op.a < 256 ? d->st.rs[op.a] : 0; break;
        case RK8_TSS: op.data[0] = (op.a < 8 && op.b < 33) ? d->st.tss[op.a][op.b] : 0; break;
        case RK8_TEX: op.b = op.a < 8 ? d->bound_tex_guest[op.a] : 0; break;
        case RK8_VS: op.a = d->vs_handle ? d->vs_handle : d->fvf; break;
        case RK8_PS: op.a = d->ps_handle; break;
        case RK8_VSC:
          if (op.a < 256) memcpy(op.data.data(), &d->vs_f[op.a * 4], std::min(op.b, 256u - op.a) * 16);
          break;
        case RK8_PSC:
          if (op.a < 224) memcpy(op.data.data(), &d->ps_f[op.a * 4], std::min(op.b, 224u - op.a) * 16);
          break;
        default: break;
      }
    }
  }
}

void verify_iface_nargs(const char* label, ComInterface& iface, const sdk_nargs::Method* exp, size_t nexp) {
  const auto& reg = iface.methods();
  if (reg.size() != nexp) fatal("D3D8 nargs self-check: %s slot count %zu != SDK %zu", label, reg.size(), nexp);
  for (size_t i = 0; i < nexp; i++) {
    if (strcmp(reg[i].name, exp[i].name) != 0)
      fatal("D3D8 nargs self-check: %s[%zu] name %s != SDK %s", label, i, reg[i].name, exp[i].name);
    if (reg[i].nargs != exp[i].nargs)
      fatal("D3D8 nargs self-check: %s::%s nargs %d != SDK %d", label, exp[i].name, reg[i].nargs, exp[i].nargs);
  }
}

void d3d8_verify_registered_nargs() {
  static bool done = false;
  if (done) return;
  done = true;
  verify_iface_nargs("IDirect3D8", iface_d3d8(), sdk_nargs::kIDirect3D8,
                     sizeof sdk_nargs::kIDirect3D8 / sizeof sdk_nargs::kIDirect3D8[0]);
  verify_iface_nargs("IDirect3DDevice8", iface_d3d8_device(), sdk_nargs::kIDirect3DDevice8,
                     sizeof sdk_nargs::kIDirect3DDevice8 / sizeof sdk_nargs::kIDirect3DDevice8[0]);
  verify_iface_nargs("IDirect3DSurface8", iface_d3d8_surface(), sdk_nargs::kIDirect3DSurface8,
                     sizeof sdk_nargs::kIDirect3DSurface8 / sizeof sdk_nargs::kIDirect3DSurface8[0]);
  verify_iface_nargs("IDirect3DTexture8", iface_d3d8_texture(), sdk_nargs::kIDirect3DTexture8,
                     sizeof sdk_nargs::kIDirect3DTexture8 / sizeof sdk_nargs::kIDirect3DTexture8[0]);
  verify_iface_nargs("IDirect3DVertexBuffer8", iface_d3d8_vertex_buffer(), sdk_nargs::kIDirect3DVertexBuffer8,
                     sizeof sdk_nargs::kIDirect3DVertexBuffer8 / sizeof sdk_nargs::kIDirect3DVertexBuffer8[0]);
  verify_iface_nargs("IDirect3DIndexBuffer8", iface_d3d8_index_buffer(), sdk_nargs::kIDirect3DIndexBuffer8,
                     sizeof sdk_nargs::kIDirect3DIndexBuffer8 / sizeof sdk_nargs::kIDirect3DIndexBuffer8[0]);
  verify_iface_nargs("IDirect3DStateBlock8", iface_d3d8_state_block(), sdk_nargs::kIDirect3DStateBlock8,
                     sizeof sdk_nargs::kIDirect3DStateBlock8 / sizeof sdk_nargs::kIDirect3DStateBlock8[0]);
  verify_iface_nargs("IDirect3DCubeTexture8", iface_d3d8_cube_texture(), sdk_nargs::kIDirect3DCubeTexture8,
                     sizeof sdk_nargs::kIDirect3DCubeTexture8 / sizeof sdk_nargs::kIDirect3DCubeTexture8[0]);
  verify_iface_nargs("IDirect3DSwapChain8", iface_d3d8_swap_chain(), sdk_nargs::kIDirect3DSwapChain8,
                     sizeof sdk_nargs::kIDirect3DSwapChain8 / sizeof sdk_nargs::kIDirect3DSwapChain8[0]);
}

rt::ComInterface& iface_d3d8_swap_chain() {
  static ComInterface i("IDirect3DSwapChain8",
                        {{"QueryInterface", 2, com_QueryInterface_none},
                         {"AddRef", 0, com_AddRef},
                         {"Release", 0, com_Release},
                         {"Present", 4, swapchain_Present},
                         {"GetBackBuffer", 3, swapchain_GetBackBuffer}});
  return i;
}

}  // namespace d3d8

namespace plat {
__attribute__((weak)) void get_display_size(int* width, int* height) {
  if (width) *width = 640;
  if (height) *height = 480;
}
__attribute__((weak)) void get_window_size(int* width, int* height) {
  get_display_size(width, height);
}
}  // namespace plat
