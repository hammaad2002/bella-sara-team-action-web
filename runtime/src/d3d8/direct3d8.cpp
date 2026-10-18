// Direct3D 8 HLE: IDirect3D8 factory and IDirect3DDevice8 creation.
#include <cstring>
#include <mutex>

#include "../d3d9/d3d9.h"
#include "../platform/platform.h"
#include "d3d8.h"

using namespace rt;

namespace d3d8 {

namespace {

class Direct3D8 : public ComObject {
 public:
  const char* type_name() const override { return "Direct3D8"; }
};

struct Mode {
  uint32_t w, h;
};
const Mode kModes[] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 960}, {1280, 1024}, {1600, 1200}, {1920, 1080}};

bool supported_texture_format(uint32_t f) {
  switch (f) {
    case d3d::FMT_A8R8G8B8:
    case d3d::FMT_X8R8G8B8:
    case d3d::FMT_R5G6B5:
    case d3d::FMT_X1R5G5B5:
    case d3d::FMT_A1R5G5B5:
    case d3d::FMT_A4R4G4B4:
    case d3d::FMT_A8:
    case d3d::FMT_L8:
    case d3d::FMT_A8L8:
    case d3d::FMT_DXT1:
    case d3d::FMT_DXT2:
    case d3d::FMT_DXT3:
    case d3d::FMT_DXT4:
    case d3d::FMT_DXT5:
      return true;
    default:
      return false;
  }
}

bool supported_depth_format(uint32_t f) {
  return f == d3d::FMT_D16 || f == d3d::FMT_D24S8 || f == d3d::FMT_D24X8 || f == d3d::FMT_D32 || f == d3d::FMT_D16_LOCKABLE
         || f == d3d::FMT_D15S1 || f == d3d::FMT_D24X4S4;
}

uint32_t GetAdapterCount(Cpu*, ComObject*) { return 1; }

uint32_t GetAdapterIdentifier(Cpu* c, ComObject*) {
  uint32_t p = arg(c, 3);
  memset(host(p), 0, 1100);
  write_cstr(p, "nv4_disp.dll");
  write_cstr(p + 512, "NVIDIA GeForce FX 5900 (Bella Sara web runtime)");
  write_cstr(p + 1024, "\\\\.\\DISPLAY1");
  st32(p + 1056, 0x000e0006);
  st32(p + 1060, 0x0006000e);
  st32(p + 1064, 0x10de);
  st32(p + 1068, 0x0331);
  st32(p + 1072, 0);
  st32(p + 1076, 0xa1);
  st32(p + 1096, 1);
  return S_OK;
}

uint32_t GetAdapterModeCount(Cpu* c, ComObject*) {
  uint32_t fmt = arg(c, 2);
  return (fmt == d3d::FMT_X8R8G8B8 || fmt == d3d::FMT_R5G6B5) ? (uint32_t)(sizeof kModes / sizeof kModes[0]) : 0;
}

uint32_t EnumAdapterModes(Cpu* c, ComObject*) {
  uint32_t fmt = arg(c, 2), i = arg(c, 3), out = arg(c, 4);
  if (i >= sizeof kModes / sizeof kModes[0]) return D3DERR_INVALIDCALL;
  st32(out, kModes[i].w);
  st32(out + 4, kModes[i].h);
  st32(out + 8, 60);
  st32(out + 12, fmt);
  return S_OK;
}

uint32_t GetAdapterDisplayMode(Cpu* c, ComObject*) {
  uint32_t out = arg(c, 2);
  int w, h;
  plat::get_display_size(&w, &h);
  st32(out, (uint32_t)w);
  st32(out + 4, (uint32_t)h);
  st32(out + 8, 60);
  st32(out + 12, d3d::FMT_X8R8G8B8);
  return S_OK;
}

uint32_t CheckDeviceType(Cpu* c, ComObject*) {
  uint32_t bb = arg(c, 4);
  return (bb == d3d::FMT_X8R8G8B8 || bb == d3d::FMT_A8R8G8B8 || bb == d3d::FMT_R5G6B5 || bb == d3d::FMT_UNKNOWN) ? S_OK
                                                                                                                    : d3d::D3DERR_NOTAVAILABLE;
}

uint32_t CheckDeviceFormat(Cpu* c, ComObject*) {
  uint32_t usage = arg(c, 4), rtype = arg(c, 5), fmt = arg(c, 6);
  log(Log::Hle, "CheckDeviceFormat: usage=%08x rtype=%08x fmt=%08x (%s)", usage, rtype, fmt, d3d::format_name(fmt));
  if (usage & d3d::USAGE_DEPTHSTENCIL) return supported_depth_format(fmt) ? S_OK : d3d::D3DERR_NOTAVAILABLE;
  if (usage & d3d::USAGE_RENDERTARGET)
    return (fmt == d3d::FMT_A8R8G8B8 || fmt == d3d::FMT_X8R8G8B8 || fmt == d3d::FMT_R5G6B5) ? S_OK : d3d::D3DERR_NOTAVAILABLE;
  if (rtype == d3d::RTYPE_TEXTURE || rtype == d3d::RTYPE_CUBETEXTURE || rtype == d3d::RTYPE_SURFACE)
    return supported_texture_format(fmt) ? S_OK : d3d::D3DERR_NOTAVAILABLE;
  return d3d::D3DERR_NOTAVAILABLE;
}

uint32_t CheckDeviceMultiSampleType(Cpu* c, ComObject*) {
  uint32_t ms_type = arg(c, 5);
  log(Log::Hle, "CheckDeviceMultiSampleType: ms_type=%u", ms_type);
  // Guest call sites pass 5 args (+this); do not read the omitted pQualityLevels slot.
  bool ok_ms = ms_type >= 1 && ms_type <= 16;
  return ok_ms ? S_OK : d3d::D3DERR_NOTAVAILABLE;
}

uint32_t CheckDepthStencilMatch(Cpu* c, ComObject*) {
  uint32_t fmt = arg(c, 5);
  log(Log::Hle, "CheckDepthStencilMatch: fmt=%08x (%s)", fmt, d3d::format_name(fmt));
  return supported_depth_format(fmt) ? S_OK : d3d::D3DERR_NOTAVAILABLE;
}

uint32_t GetDeviceCaps(Cpu* c, ComObject*) {
  log(Log::Hle, "GetDeviceCaps called on IDirect3D8");
  fill_device_caps(arg(c, 3));
  return S_OK;
}

uint32_t GetAdapterMonitor(Cpu*, ComObject*) { return 0x00010001; }

}  // namespace

ComInterface& iface_d3d8() {
  static std::recursive_mutex lock;
  static ComInterface i("IDirect3D8", {{"QueryInterface", 2, com_QueryInterface_none},
                                       {"AddRef", 0, com_AddRef},
                                       {"Release", 0, com_Release},
                                       {"RegisterSoftwareDevice", 1, nullptr},
                                       {"GetAdapterCount", 0, GetAdapterCount},
                                       {"GetAdapterIdentifier", 3, GetAdapterIdentifier},
                                       {"GetAdapterModeCount", 2, GetAdapterModeCount},
                                       {"EnumAdapterModes", 4, EnumAdapterModes},
                                       {"GetAdapterDisplayMode", 2, GetAdapterDisplayMode},
                                       {"CheckDeviceType", 5, CheckDeviceType},
                                       {"CheckDeviceFormat", 6, CheckDeviceFormat},
                                       {"CheckDeviceMultiSampleType", 6, CheckDeviceMultiSampleType},
                                       {"CheckDepthStencilMatch", 5, CheckDepthStencilMatch},
                                       {"GetDeviceCaps", 3, GetDeviceCaps},
                                       {"GetAdapterMonitor", 1, GetAdapterMonitor},
                                       {"CreateDevice", 6, create_d3d8_device}},
                                      &lock);
  return i;
}

}  // namespace d3d8

HLE_STDCALL("d3d8.dll", Direct3DCreate8, 1) {
  if (arg(c, 0) != d3d8::kD3d8SdkVersion)
    log(Log::Warn, "Direct3DCreate8: SDK version %08x (expected %u)", arg(c, 0), d3d8::kD3d8SdkVersion);
  else
    log(Log::Info, "Direct3DCreate8(SDK %u)", arg(c, 0));
  uint32_t p = com_wrap(new d3d8::Direct3D8(), d3d8::iface_d3d8());
  return p;
}
