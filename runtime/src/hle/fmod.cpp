// fmod.dll HLE: real semantics for all 53 FSOUND_*/FMUSIC_* stdcall exports the game imports
// (fmod.dll 3.75 ships on the disc; its resources say FileVersion/ProductVersion "3.75").
//
// Wave 2: playback runs through the software mixer (audio/mixer.h) and the shipped banks decode
// through audio/ffs.h ("FFS " container -> FSB3.1 subsongs -> PCM16 / FMOD3 stereo IMA ADPCM;
// the IMA layout is settled by disassembling the shipped fmod.dll - see work/AUDIO-NOTES.md).
//
// Implemented semantics:
//  - FSOUND_Init opens the mixer; Close stops every channel and forgets all handles. The mixer
//    is locked at its own rate (matching FMOD's legacy default and the banks), so Init's mixrate
//    is recorded but not honoured yet; maxchannels sizes the channel table (capped at 64, the
//    FMOD 3 software limit).
//  - Handles: FSOUND_SAMPLE*/FSOUND_STREAM*/FMUSIC_MODULE* are opaque u32 keys in the registries
//    below (the game only passes them back into FMOD functions, never dereferences them).
//    FSOUND_Sample_GetName hands back a guest-visible heap copy of the sample name.
//  - Loading: FSOUND_LOADMEMORY decodes an in-guest-memory FSB3 blob (sized from its own
//    0x18-byte main header: 0x18 + headers + data); otherwise a (path, offset, length) triple
//    decodes a sub-range of a mapped host file - exactly how the game addresses the .ffs banks
//    (both FMUSIC_LoadSongEx call sites resolve to a 0x50-header single-sample FSB inside
//    music.ffs) - and a whole-file FSB3 (the "Win32/Sound/Speech/%s.FSB" path when present on
//    the disc) opens with offset 0, length 0. Decodes are cached per (path, offset, length) key.
//  - Playback: every channel is an audio::Source with FMOD volume (0..255)/pan (0..511, 256
//    centre), inverse-distance 3D attenuation from the listener state and right-vector pan -
//    modelled on dsound.cpp's mixer math. PlaySoundEx/Stream_PlayEx allocate on FSOUND_FREE (-1)
//    (both PlaySoundEx call sites in the binary use FSOUND_FREE with startpaused=1). Looping is
//    driven by the FSB header loop range: [loop_start, loop_end) plays repeatedly (the music
//    tracks store 0..num_samples-64); a channel without loop points runs once and auto-stops.
//    FSOUND_SetLoopMode/FSOUND_Sample_SetMode override the loop bit per channel/sample. The SFX
//    master volume scales every channel.
//
// Conservative (wave 3 candidates): hardware channel bookkeeping (GetNumSubChannels/GetSubChannel
// report 1/the channel), EAX/DX8 FX, partial-stream decoding (music tracks decode fully on load,
// 1.6-7.2 MB each, tens of ms), scheduled FMOD sentence playback across multiple subsongs (only
// the first sentence entry plays), var_vol/var_pan/var_freq and the game's GetMemoryStats
// accounting (always 0/0).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../audio/ffs.h"
#include "../audio/mixer.h"
#include "../core/objects.h"

// Weak fallback so BellaSara-without-game-code builds (import_coverage, ffs_test) link without
// a platform audio backend; the real runtime provides the strong platform_sdl platform_web
// definition, which the linker prefers (mixing falls back to no device, and the HLE degrades to
// silent playback with identical channel semantics).
namespace plat {
__attribute__((weak)) bool audio_open(int rate, void (*render)(float*, int)) {
  (void)rate;
  (void)render;
  return false;
}
}  // namespace plat

// Include the shared platform declarations after the weak defintion shim.
#include "../platform/platform.h"

#include "com.h"

using namespace rt;

// Link anchor: forces this translation unit (and its HLE registrars) to be kept in a static link.
void hle_link_fmod() {}

namespace {

const char* kDll = "fmod.dll";

// FMOD 3.75 constants (fmod.h), only what the HLE consumes.
constexpr uint32_t kLoopOff = 0x1, kLoopNormal = 0x2, kLoopBidi = 0x4;
constexpr uint32_t k2D = 0x2000, kHW2D = 0x80000;
constexpr uint32_t kLoadMemory = 0x8000;  // "name" arg is a pointer to data in guest memory
constexpr uint32_t kOutputDefault = 2;    // FSOUND_OUTPUT_DSOUND
constexpr int kFreeChannel = -1;          // FSOUND_FREE
constexpr int kAllChannels = -3;          // FSOUND_ALL
constexpr int kChannelsMax = 64;          // FMOD 3 supports up to 64 software channels
constexpr float kFmodVersion = 3.75f;

// FMOD errors the HLE reports through FSOUND_GetError (FMOD 3 error enum order).
constexpr int kErrFileNotFound = 4;   // FMOD_ERR_FILE_NOTFOUND
constexpr int kErrFileBad = 5;        // FMOD_ERR_FILE_BAD
constexpr int kErrMemory = 6;         // FMOD_ERR_MEMORY
constexpr int kErrChannelAlloc = 11;  // FMOD_ERR_CHANNEL_ALLOC

// The load cap: no shipped FSB subsong exceeds 8 MB; bounds a corrupt pointer's damage.
constexpr uint32_t kLoadBytesCap = 64u << 20;

struct Vec3 {
  float x = 0, y = 0, z = 0;
};
float len3(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// ---- System + listener state ------------------------------------------------------------------

std::mutex g_sys_m;  // guards g_sys/g_listener writes (game calls) against mixer reads

bool g_inited = false;
int g_output = kOutputDefault;  // FSOUND_OUTPUT_*; the game selects via FSOUND_SetOutput
int g_rate = 44100;             // FSOUND_Init mixrate (informational; the mixer stays at kRate)
int g_max_channels = 32;        // FSOUND_Init maxchannels
int g_sfx_master = 255;         // FSOUND_SetSFXMasterVolume, 0..255
int g_error = 0;                // FMOD_ERR_* returned by FSOUND_GetError

Vec3 g_ls_pos, g_ls_vel;                // listener position / velocity (world units)
Vec3 g_ls_front{0.f, 0.f, 1.f}, g_ls_top{0.f, 1.f, 0.f};
float g_ls_rolloff = 1.0f, g_ls_doppler = 1.0f;

void set_error(int e) {
  std::lock_guard<std::mutex> lock(g_sys_m);
  g_error = e;
}

void set_listener(float x, float y, float z, float vx, float vy, float vz, Vec3 front, Vec3 top) {
  std::lock_guard<std::mutex> lock(g_sys_m);
  g_ls_pos = {x, y, z};
  g_ls_vel = {vx, vy, vz};
  if (len3(front) > 0) g_ls_front = front;
  if (len3(top) > 0) g_ls_top = top;
}

// Copies the listener state under the lock (doppler/rolloff for the mix gains).
void listener_state(Vec3* pos, Vec3* vel, Vec3* front, Vec3* top, float* rolloff, float* doppler) {
  std::lock_guard<std::mutex> lock(g_sys_m);
  if (pos) *pos = g_ls_pos;
  if (vel) *vel = g_ls_vel;
  if (front) *front = g_ls_front;
  if (top) *top = g_ls_top;
  if (rolloff) *rolloff = g_ls_rolloff;
  if (doppler) *doppler = g_ls_doppler;
}

int sys_master() {
  std::lock_guard<std::mutex> lock(g_sys_m);
  return g_sfx_master;
}

// ---- Objects ----------------------------------------------------------------------------------

class FmodSample : public ComObject {
 public:
  const char* type_name() const override { return "FmodSample"; }
  uint32_t handle = 0;  // registry key (the guest-visible FSOUND_SAMPLE*)
  std::vector<int16_t> pcm;  // decoded interleaved PCM at this sample's own rate
  uint32_t rate = 44100;
  uint16_t channels = 1;
  uint32_t mode = kLoopNormal;            // raw FSOUND_* mode bits from the FSB header
  uint32_t loop_start = 0, loop_end = 0;  // frames; loop_end <= loop_start means no loop range
  uint32_t def_vol = 255, def_pan = 128, def_pri = 128;
  float min_dist = 1.0f, max_dist = 1e6f;
  std::string name;
  uint32_t name_guest = 0;  // lazily heap-allocated guest-visible name (Sample_GetName)
};

class FmodStream : public ComObject {
 public:
  const char* type_name() const override { return "FmodStream"; }
  std::vector<std::shared_ptr<FmodSample>> subs;
  std::vector<uint32_t> sentence;  // SetSubStreamSentence order (first entry plays by default)
  std::shared_ptr<FmodSample> sentence_smp;  // concatenated subsongs when SetSubStreamSentence is used
  int active_ch = -1;                        // channel currently playing this stream
  uint32_t mode = 0;               // FSOUND_Stream_SetMode
};

class FmodModule : public ComObject {  // FMUSIC_MODULE: FSB songs loaded by FMUSIC_LoadSongEx
 public:
  const char* type_name() const override { return "FmodModule"; }
  std::vector<std::shared_ptr<FmodSample>> samples;
};

class FmodChannel : public ComObject, public audio::Source {
 public:
  const char* type_name() const override { return "FmodChannel"; }
  std::shared_ptr<FmodSample> smp;
  std::shared_ptr<audio::Source> self_ref;  // keeps the mixer registration alive
  std::atomic<bool> playing{false}, looping{false}, paused{false};
  uint32_t loop_start = 0, loop_end = 0;  // loop window in frames (loop_end > loop_start)
  double cursor = 0;                      // playback cursor in frames
  uint32_t freq = 0;                      // 0 => the sample's own rate
  int volume = 255;                       // 0..255
  bool volume_absolute = false;           // FSOUND_SetVolumeAbsolute bypasses master volume
  uint32_t pan = 128;                     // 0..255, 128 centre
  bool is3d = false;                      // FSOUND_3D_SetAttributes promotes the channel
  Vec3 pos3d;                             // channel source position (world units)
  float min_dist = 1.0f, max_dist = 1e6f;
  std::mutex m;

  double step() const {
    const uint32_t r = freq ? freq : (smp ? smp->rate : 44100u);
    return (double)r / audio::kRate;
  }

  // Gain math: FMOD 3 software volume is linear 0..255; pan is 0..255 (128 centre).
  // Master SFX volume scales non-absolute channels. 3D channels attenuate by inverse distance
  // from min_dist to max_dist and pan by the listener right-vector projection.
  void gains(float* gl, float* gr) {
    const int master = sys_master();
    float g = (float)std::clamp(volume, 0, 255) / 255.0f;
    if (!volume_absolute && master < 255) {
      g *= (float)std::clamp(master, 0, 255) / 255.0f;
    }
    const int p = std::clamp((int)pan, 0, 255);
    float pl = p > 128 ? (255 - p) / 127.0f : 1.0f;
    float pr = p < 128 ? (float)p / 128.0f : 1.0f;
    if (!is3d) {
      *gl = g * pl;
      *gr = g * pr;
      return;
    }
    Vec3 pos, front, top;
    float rolloff, doppler;
    listener_state(&pos, nullptr, &front, &top, &rolloff, &doppler);
    (void)doppler;  // the doppler scale is stored, not simulated (TODO(wave 3))
    const Vec3 rel{pos3d.x - pos.x, pos3d.y - pos.y, pos3d.z - pos.z};
    const float d = len3(rel);
    const float md = std::max(min_dist, 0.001f);
    const float xd = std::max(max_dist, md);
    if (d > md) {
      g *= md / (md + rolloff * (std::min(d, xd) - md));
    }
    if (d > 1e-4f) {
      const Vec3 right{top.y * front.z - top.z * front.y, top.z * front.x - top.x * front.z,
                       top.x * front.y - top.y * front.x};
      const float rl = len3(right);
      float s = rl > 0 ? (rel.x * right.x + rel.y * right.y + rel.z * right.z) / (rl * d) : 0.f;
      s = std::clamp(s, -1.0f, 1.0f);
      pl *= std::sqrt(0.5f * (1 - s)) * 1.41421f;
      pr *= std::sqrt(0.5f * (1 + s)) * 1.41421f;
    }
    *gl = g * std::min(pl, 1.0f);
    *gr = g * std::min(pr, 1.0f);
  }

  void mix(float* out, int n) override {
    if (!playing || paused) return;
    std::lock_guard<std::mutex> lock(m);
    auto s = smp;
    if (!s || s->pcm.empty()) return;
    const int chs = s->channels ? s->channels : 1;
    const uint32_t total = (uint32_t)(s->pcm.size() / chs);
    if (!total) {
      playing = false;
      return;
    }
    float gl, gr;
    gains(&gl, &gr);
    if (gl <= 0 && gr <= 0) {  // silent channel: still advance the cursor
      cursor += step() * n;
      if (looping && cursor >= (double)loop_end) cursor -= (double)(loop_end - loop_start);
      else if (cursor >= (double)total) {
        if (looping) cursor = std::fmod(cursor, (double)total);
        else {
          cursor = 0;
          playing = false;
        }
      }
      return;
    }
    const double stepv = step();
    const bool loop = looping;
    for (int i = 0; i < n; i++) {
      const uint32_t f0 = (uint32_t)cursor;
      uint32_t f1;
      if (loop && f0 + 1 >= loop_end) f1 = loop_start;  // interpolation wraps the loop seam
      else f1 = f0 + 1 < total ? f0 + 1 : (loop ? loop_start : f0);
      const float t = (float)(cursor - (double)f0);
      const int16_t* sa = s->pcm.data() + f0 * chs;
      const int16_t* sb = s->pcm.data() + f1 * chs;
      const float l = (float)(int16_t)sa[0] / 32768.0f * (1 - t) +
                      (float)(int16_t)sb[0] / 32768.0f * t;
      const float r = chs > 1 ? (float)(int16_t)sa[1] / 32768.0f * (1 - t) +
                                    (float)(int16_t)sb[1] / 32768.0f * t
                              : l;
      out[2 * i] += l * gl;
      out[2 * i + 1] += r * gr;
      cursor += stepv;
      if (loop && cursor >= (double)loop_end) {
        cursor -= (double)(loop_end - loop_start);
      } else if (cursor >= (double)total) {
        if (loop) cursor = std::fmod(cursor, (double)total);
        else {
          cursor = 0;
          playing = false;
          return;
        }
      }
    }
  }
};

// ---- Registries -------------------------------------------------------------------------------

std::mutex g_res_m;  // guards the registries and the decode cache
uint32_t g_seed = 0x1F0E0000;
std::map<uint32_t, std::shared_ptr<FmodSample>> g_samples;
std::map<uint32_t, std::shared_ptr<FmodStream>> g_streams;
std::map<uint32_t, std::shared_ptr<FmodModule>> g_modules;
std::map<std::string, std::vector<std::shared_ptr<FmodSample>>> g_cache;

std::vector<std::shared_ptr<FmodChannel>> g_channels;  // sized kChannelsMax at Init

std::shared_ptr<FmodSample> sample_shared(uint32_t h) {
  std::lock_guard<std::mutex> lock(g_res_m);
  auto it = g_samples.find(h);
  return it == g_samples.end() ? nullptr : it->second;
}

FmodSample* sample_by(uint32_t h) {
  std::lock_guard<std::mutex> lock(g_res_m);
  auto it = g_samples.find(h);
  return it == g_samples.end() ? nullptr : it->second.get();
}

FmodStream* stream_by(uint32_t h) {
  std::lock_guard<std::mutex> lock(g_res_m);
  auto it = g_streams.find(h);
  return it == g_streams.end() ? nullptr : it->second.get();
}

FmodModule* module_by(uint32_t h) {
  std::lock_guard<std::mutex> lock(g_res_m);
  auto it = g_modules.find(h);
  return it == g_modules.end() ? nullptr : it->second.get();
}

// ---- Loading ----------------------------------------------------------------------------------

uint32_t new_handle() {
  std::lock_guard<std::mutex> lock(g_res_m);
  g_seed += 2;
  return g_seed;
}

// Builds FmodSamples from one decoded FSB subsong set, each registered under a fresh handle.
std::vector<std::shared_ptr<FmodSample>> make_samples(const std::vector<audio::FfsBank::FfsSubsong>& subs) {
  std::vector<std::shared_ptr<FmodSample>> out;
  out.reserve(subs.size());
  for (const audio::FfsBank::FfsSubsong& s : subs) {
    auto sp = std::make_shared<FmodSample>();
    sp->handle = new_handle();
    {
      std::lock_guard<std::mutex> lock(g_res_m);
      g_samples[sp->handle] = sp;
    }
    sp->pcm = s.pcm;
    sp->rate = s.info.sample_rate;
    sp->channels = s.info.channels;
    sp->mode = s.info.mode;
    sp->loop_start = s.info.loop_start;
    sp->loop_end = s.info.loop_end;
    sp->def_vol = std::min<uint32_t>(s.info.def_vol, 255);
    // The shipped music banks store 0xffff as the pan default; FMOD treats that as "not set".
    sp->def_pan = s.info.def_pan == 0xffff ? 128 : std::min<uint32_t>(s.info.def_pan, 255);
    sp->def_pri = std::min<uint32_t>(s.info.def_pri, 255);
    const float md = s.info.min_dist, xd = s.info.max_dist;
    sp->min_dist = md > 0 ? md : 1.0f;
    sp->max_dist = xd > md ? xd : 1e6f;
    sp->name = s.info.name;
    out.push_back(std::move(sp));
  }
  return out;
}

// Resolves FMOD load calls into decoded subsong samples:
//  - FSOUND_LOADMEMORY: "name_or_data" is a guest pointer; the blob is sized from the FSB3
//    layout itself (0x18-byte main header: u32 count | u32 headers | u32 data).
//  - otherwise: "name_or_data" is a path; (offset, length) select the sub-range (the .ffs
//    banks) and 0/0 opens the whole file as one FSB3 (the Speech paths on the disc).
// Returns false with g_error set on failure.
bool resolve_load(uint32_t name_or_data, uint32_t offset, uint32_t length, uint32_t mode,
                  std::vector<std::shared_ptr<FmodSample>>* out) {
  std::vector<uint8_t> blob;
  std::string cache_key;
  if (mode & kLoadMemory) {
    if (!name_or_data || memcmp(host(name_or_data), "FSB3", 4) != 0) {
      set_error(kErrFileBad);
      return false;
    }
    const uint32_t headers = ld32(name_or_data + 8), data = ld32(name_or_data + 12);
    const uint32_t total = 0x18 + headers + data;
    blob.resize(total);
    memcpy(blob.data(), host(name_or_data), total);
    cache_key = "mem@" + std::to_string(name_or_data) + ":" + std::to_string(total);
  } else {
    const std::string guest = read_cstr(name_or_data);
    const std::string hostp = vfs_host_path(guest, false);
    if (hostp.empty()) {
      log(Log::Hle, "fmod: load(%s): path not mapped", guest.c_str());
      set_error(kErrFileNotFound);
      return false;
    }
    FILE* fp = fopen(hostp.c_str(), "rb");
    if (!fp) {
      set_error(kErrFileNotFound);
      return false;
    }
    fseek(fp, 0, SEEK_END);
    const long fsize = ftell(fp);
    if (offset >= (uint32_t)fsize) {
      fclose(fp);
      log(Log::Hle, "fmod: load(%s): offset %u beyond file (%ld)", guest.c_str(), offset, fsize);
      set_error(kErrFileNotFound);
      return false;
    }
    uint8_t magic[4] = {0, 0, 0, 0};
    fseek(fp, (long)offset, SEEK_SET);
    if (fread(magic, 1, 4, fp) != 4) {
      fclose(fp);
      set_error(kErrFileBad);
      return false;
    }
    uint32_t len = length;
    if (len == 0) len = (uint32_t)fsize - (uint32_t)offset;
    len = std::min<uint32_t>(len, (uint32_t)fsize - offset);
    if (len >= kLoadBytesCap) {
      fclose(fp);
      set_error(kErrMemory);
      return false;
    }
    blob.resize(len);
    fseek(fp, (long)offset, SEEK_SET);
    const bool okread = fread(blob.data(), 1, len, fp) == len;
    fclose(fp);
    if (!okread || memcmp(magic, "FSB3", 4) != 0) {
      if (memcmp(magic, "FFS ", 4) == 0) {
        log(Log::Hle, "fmod: load(%s@%u): .ffs container itself is not an FMOD loadable",
            guest.c_str(), offset);
      }
      set_error(kErrFileBad);
      return false;
    }
    cache_key = hostp + "@" + std::to_string(offset) + ":" + std::to_string(length);
  }

  // Decode-cache lookup (the shipped banks are read-only, so key hits return the same object).
  if (!cache_key.empty()) {
    std::lock_guard<std::mutex> lock(g_res_m);
    auto it = g_cache.find(cache_key);
    if (it != g_cache.end()) {
      *out = it->second;
      return true;
    }
  }

  std::vector<audio::FfsBank::FfsSubsong> subs;
  std::string err;
  if (!audio::FfsBank::decode_fsb_bytes(blob.data(), blob.size(), &subs, &err)) {
    log(Log::Warn, "fmod: load decode failed: %s", err.c_str());
    set_error(kErrFileBad);
    return false;
  }
  std::vector<std::shared_ptr<FmodSample>> samples = make_samples(subs);
  if (samples.empty()) {
    set_error(kErrFileBad);
    return false;
  }
  if (!cache_key.empty()) {
    std::lock_guard<std::mutex> lock(g_res_m);
    if (g_cache.size() > 4096) g_cache.clear();  // the game loads a bounded set of banks
    g_cache[cache_key] = samples;
  }
  *out = std::move(samples);
  return true;
}

// ---- Channel helpers --------------------------------------------------------------------------

std::shared_ptr<FmodChannel> channel_shared(int i) {
  if (i < 0 || i >= (int)g_channels.size()) return nullptr;
  return g_channels[(size_t)i];
}

int find_free_channel() {
  for (size_t i = 0; i < g_channels.size(); i++) {
    if (!g_channels[i]->playing) return (int)i;
  }
  return -1;
}

// Starts (or restarts) a channel on a sample. FMOD default state: def volume/pan from the FSB
// header, natural frequency, loop points from the sample. Returns the channel or -1 (error set).
int start_channel(const std::shared_ptr<FmodSample>& smp, int explicit_ch, bool paused_start) {
  if (!smp || smp->pcm.empty()) {
    set_error(kErrMemory);
    return -1;
  }
  int ch = explicit_ch;
  if (ch == kFreeChannel) {
    ch = find_free_channel();
  }
  auto chan = channel_shared(ch);
  if (!chan) {
    set_error(kErrChannelAlloc);
    return -1;
  }
  if (chan->playing) {  // restarting an explicit channel cuts the old source
    chan->playing = false;
  }
  {
    std::lock_guard<std::mutex> lock(chan->m);
    chan->smp = smp;
    chan->volume = (int)smp->def_vol;
    chan->volume_absolute = false;
    chan->pan = std::clamp<uint32_t>(smp->def_pan, 0, 255);
    chan->freq = 0;  // the sample's natural rate until FSOUND_SetFrequency
    chan->cursor = 0;
    chan->min_dist = smp->min_dist;
    chan->max_dist = smp->max_dist;
    chan->is3d = false;  // promoted to 3D by FSOUND_3D_SetAttributes if sample allows 3D
    chan->pos3d = Vec3{};
    const uint32_t total_frames = (uint32_t)(smp->pcm.size() / (smp->channels ? smp->channels : 1));
    chan->loop_start = smp->loop_start;
    chan->loop_end = smp->loop_end > smp->loop_start ? smp->loop_end : total_frames;
    chan->looping = (smp->mode & (kLoopNormal | kLoopBidi)) != 0;
    chan->paused = paused_start;
    chan->playing = true;
  }
  return ch;
}

void stop_channel(int ch) {
  if (ch == kAllChannels) {
    for (auto& c : g_channels) c->playing = false;
    return;
  }
  auto c = (ch == kFreeChannel) ? nullptr : channel_shared(ch);
  if (c) c->playing = false;
}

}  // namespace

// ---- HLE entry points -------------------------------------------------------------------------

namespace {
void push_float(Cpu* c, float v) {
  c->ftop = (c->ftop - 1) & 7;
  c->fpr[c->ftop] = (double)v;
}
}  // namespace

// ---- System / setup ---------------------------------------------------------------------------

HLE_STDCALL_AS(kDll, FSOUND_Init, "_FSOUND_Init@12", 3) {
  const int rate = (int)arg(c, 0), chans = (int)arg(c, 1);
  std::lock_guard<std::mutex> lock(g_sys_m);
  if (g_inited) {
    return 1;  // FMOD tolerates re-Init through Close; treat as idempotent
  }
  g_inited = true;
  g_rate = rate;
  g_max_channels = std::clamp(chans, 1, kChannelsMax);
  g_error = 0;
  g_channels.assign((size_t)g_max_channels, nullptr);
  for (int i = 0; i < g_max_channels; i++) {
    auto chan = std::make_shared<FmodChannel>();
    audio::add(chan);
    g_channels[(size_t)i] = chan;
  }
  audio::init();
  log(Log::Hle, "fmod: Init(rate=%d, channels=%d)", rate, g_max_channels);
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_Close, "_FSOUND_Close@0", 0) {
  std::lock_guard<std::mutex> lock(g_sys_m);
  if (!g_inited) return 0;
  std::lock_guard<std::mutex> rlock(g_res_m);
  for (auto& ch : g_channels) {
    ch->playing = false;
    audio::remove(ch.get());
    ch->smp.reset();
    ch->self_ref.reset();
  }
  g_channels.clear();
  g_samples.clear();
  g_streams.clear();
  g_modules.clear();
  g_cache.clear();
  g_inited = false;
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_GetVersion, "_FSOUND_GetVersion@0", 0) {
  push_float(c, kFmodVersion);
  return 0;  // the version travels in ST0, not EAX
}

HLE_STDCALL_AS(kDll, FSOUND_SetOutput, "_FSOUND_SetOutput@4", 1) {
  std::lock_guard<std::mutex> lock(g_sys_m);
  g_output = (int)arg(c, 0);
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_GetOutput, "_FSOUND_GetOutput@0", 0) {
  std::lock_guard<std::mutex> lock(g_sys_m);
  return (uint32_t)g_output;
}

HLE_STDCALL_AS(kDll, FSOUND_GetOutputHandle, "_FSOUND_GetOutputHandle@0", 0) {
  return 0;  // the software mixer has no OS device handle
}

HLE_STDCALL_AS(kDll, FSOUND_SetBufferSize, "_FSOUND_SetBufferSize@4", 1) {
  return 1;  // latency is owned by the software mixer thread
}

HLE_STDCALL_AS(kDll, FSOUND_SetMaxHardwareChannels, "_FSOUND_SetMaxHardwareChannels@4", 1) {
  return 1;  // no hardware paths in the HLE
}

HLE_STDCALL_AS(kDll, FSOUND_SetMinHardwareChannels, "_FSOUND_SetMinHardwareChannels@4", 1) {
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_SetSFXMasterVolume, "_FSOUND_SetSFXMasterVolume@4", 1) {
  std::lock_guard<std::mutex> lock(g_sys_m);
  g_sfx_master = std::clamp((int)arg(c, 0), 0, 255);
  if (trace_enabled("audio")) log(Log::Hle, "fmod: SetSFXMasterVolume %d", g_sfx_master);
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_GetSFXMasterVolume, "_FSOUND_GetSFXMasterVolume@0", 0) {
  std::lock_guard<std::mutex> lock(g_sys_m);
  return (uint32_t)g_sfx_master;
}

HLE_STDCALL_AS(kDll, FSOUND_GetMemoryStats, "_FSOUND_GetMemoryStats@8", 2) {
  if (arg(c, 0)) st32(arg(c, 0), 0);
  if (arg(c, 1)) st32(arg(c, 1), 0);  // TODO(wave 3): track decoded/reserved bytes here
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_GetError, "_FSOUND_GetError@0", 0) {
  std::lock_guard<std::mutex> lock(g_sys_m);
  return (uint32_t)g_error;
}

HLE_STDCALL_AS(kDll, FSOUND_GetMaxChannels, "_FSOUND_GetMaxChannels@0", 0) {
  std::lock_guard<std::mutex> lock(g_sys_m);
  return (uint32_t)g_max_channels;
}

HLE_STDCALL_AS(kDll, FSOUND_Update, "_FSOUND_Update@0", 0) {
  return 1;  // the mixer renders on its own thread; 3D values apply live
}

// ---- Per-sound playback -----------------------------------------------------------------------

// FSOUND_PlaySoundEx(channel, sample, dsp, startpaused). Both call sites use FSOUND_FREE with
// startpaused=1 - the game unpauses per channel via FSOUND_SetPaused.
HLE_STDCALL_AS(kDll, FSOUND_PlaySoundEx, "_FSOUND_PlaySoundEx@16", 4) {
  if (!g_inited) {
    set_error(kErrMemory);
    return (uint32_t)-1;
  }
  auto smp = sample_shared(arg(c, 1));
  if (!smp) {
    set_error(kErrMemory);
    return (uint32_t)-1;
  }
  const int ch = start_channel(smp, (int)arg(c, 0), arg(c, 3) != 0);
  if (ch >= 0 && trace_enabled("audio"))
    log(Log::Hle, "fmod: PlaySoundEx ch=%d sample=%08x (%s) mode=%x loop=%u..%u frames=%zu paused=%u",
        ch, arg(c, 1), smp->name.c_str(), smp->mode, smp->loop_start, smp->loop_end,
        smp->pcm.size() / (smp->channels ? smp->channels : 1), arg(c, 3) != 0);
  return ch < 0 ? (uint32_t)-1 : (uint32_t)ch;
}

HLE_STDCALL_AS(kDll, FSOUND_StopSound, "_FSOUND_StopSound@4", 1) {
  stop_channel((int)arg(c, 0));
  return 1;
}

// FSOUND channels hold settings on the started channel; volume 0..255 with the sample's default
// already applied by start_channel.
HLE_STDCALL_AS(kDll, FSOUND_SetVolume, "_FSOUND_SetVolume@8", 2) {
  const int c_arg = (int)arg(c, 0);
  const int vol = std::clamp((int)arg(c, 1), 0, 255);
  if (trace_enabled("audio")) {  // the game re-sends the same volume every tick: log changes only
    auto ch = c_arg >= 0 ? channel_shared(c_arg) : nullptr;
    if (ch && ch->volume != vol) log(Log::Hle, "fmod: SetVolume ch=%d vol=%d", c_arg, vol);
  }
  if (c_arg == kAllChannels) {
    for (auto& ch : g_channels) {
      if (ch) {
        ch->volume_absolute = false;
        ch->volume = vol;
      }
    }
    return 1;
  }
  auto ch = c_arg == kFreeChannel ? nullptr : channel_shared(c_arg);
  if (!ch) return 0;
  ch->volume_absolute = false;
  ch->volume = vol;
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_SetVolumeAbsolute, "_FSOUND_SetVolumeAbsolute@8", 2) {
  const int c_arg = (int)arg(c, 0);
  const int vol = std::clamp((int)arg(c, 1), 0, 255);
  if (c_arg == kAllChannels) {
    for (auto& ch : g_channels) {
      if (ch) {
        ch->volume_absolute = true;
        ch->volume = vol;
      }
    }
    return 1;
  }
  auto ch = c_arg == kFreeChannel ? nullptr : channel_shared(c_arg);
  if (!ch) return 0;
  ch->volume_absolute = true;
  ch->volume = vol;  // FSOUND 3: same 0..255 units, bypasses SFX master
  return 1;
}

// FSOUND_SetPan: 0..255, 128 centre (0 = hard left, 255 = hard right); FSOUND_STEREOPAN (-1)
// keeps a stereo sample's own balance, i.e. centre.
HLE_STDCALL_AS(kDll, FSOUND_SetPan, "_FSOUND_SetPan@8", 2) {
  const int c_arg = (int)arg(c, 0);
  const int pan = (int)arg(c, 1) == -1 ? 128 : std::clamp((int)arg(c, 1), 0, 255);
  if (trace_enabled("audio")) log(Log::Hle, "fmod: SetPan ch=%d pan=%d", c_arg, (int)arg(c, 1));
  if (c_arg == kAllChannels) {
    for (auto& ch : g_channels) if (ch) ch->pan = pan;
    return 1;
  }
  auto ch = c_arg == kFreeChannel ? nullptr : channel_shared(c_arg);
  if (!ch) return 0;
  ch->pan = pan;
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_SetFrequency, "_FSOUND_SetFrequency@8", 2) {
  const int c_arg = (int)arg(c, 0);
  const int freq = std::clamp((int)arg(c, 1), 1, 100000);  // FMOD 3: 1..100k-ish bounded
  if (c_arg == kAllChannels) {
    for (auto& ch : g_channels) if (ch) ch->freq = freq;
    return 1;
  }
  auto ch = c_arg == kFreeChannel ? nullptr : channel_shared(c_arg);
  if (!ch) return 0;
  ch->freq = freq;
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_SetPaused, "_FSOUND_SetPaused@8", 2) {
  const int c_arg = (int)arg(c, 0);
  const bool paused = arg(c, 1) != 0;
  if (c_arg == kAllChannels) {
    for (auto& ch : g_channels) if (ch) ch->paused = paused;
    if (trace_enabled("audio")) log(Log::Hle, "fmod: SetPaused ch=ALL paused=%u", paused);
    return 1;
  }
  auto ch = c_arg == kFreeChannel ? nullptr : channel_shared(c_arg);
  if (!ch) return 0;
  ch->paused = paused;
  if (trace_enabled("audio")) log(Log::Hle, "fmod: SetPaused ch=%d paused=%u", c_arg, paused);
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_GetPaused, "_FSOUND_GetPaused@4", 1) {
  auto ch = (int)arg(c, 0) == kFreeChannel ? nullptr : channel_shared((int)arg(c, 0));
  return ch && ch->paused ? 1u : 0u;
}

HLE_STDCALL_AS(kDll, FSOUND_SetLoopMode, "_FSOUND_SetLoopMode@8", 2) {
  const int c_arg = (int)arg(c, 0);
  const uint32_t mode = arg(c, 1) & 7u;
  auto apply = [&](FmodChannel* ch) {
    std::lock_guard<std::mutex> lock(ch->m);
    if (ch->smp && ch->loop_end <= ch->loop_start) {
      ch->loop_start = 0;
      ch->loop_end = (uint32_t)(ch->smp->pcm.size() / (ch->smp->channels ? ch->smp->channels : 1));
    }
    const bool points = ch->loop_end > ch->loop_start;
    ch->looping = mode == kLoopNormal || mode == kLoopBidi ? true
                  : mode == kLoopOff                      ? false
                                                          : points;
  };
  if (c_arg == kAllChannels) {
    for (auto& ch : g_channels) if (ch) apply(ch.get());
    return 1;
  }
  auto ch = c_arg == kFreeChannel ? nullptr : channel_shared(c_arg);
  if (!ch) return 0;
  apply(ch.get());
  return 1;
}

// FSOUND_SetCurrentPosition takes milliseconds.
HLE_STDCALL_AS(kDll, FSOUND_SetCurrentPosition, "_FSOUND_SetCurrentPosition@8", 2) {
  auto ch = (int)arg(c, 0) == kFreeChannel ? nullptr : channel_shared((int)arg(c, 0));
  if (!ch || !ch->smp) return 0;
  const double seconds = (double)(int)arg(c, 1) / 1000.0 * (double)ch->smp->rate;
  std::lock_guard<std::mutex> lock(ch->m);
  ch->cursor = std::clamp(seconds, 0.0, 1e9);
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_IsPlaying, "_FSOUND_IsPlaying@4", 1) {
  auto ch = (int)arg(c, 0) == kFreeChannel ? nullptr : channel_shared((int)arg(c, 0));
  return ch && ch->playing ? 1u : 0u;
}

HLE_STDCALL_AS(kDll, FSOUND_GetCurrentSample, "_FSOUND_GetCurrentSample@4", 1) {
  auto ch = (int)arg(c, 0) == kFreeChannel ? nullptr : channel_shared((int)arg(c, 0));
  if (!ch) return 0;
  std::lock_guard<std::mutex> lock(ch->m);
  auto smp = ch->smp;
  return smp ? smp->handle : 0u;
}

// FSOUND hardware channels; our channels are single software voices.
HLE_STDCALL_AS(kDll, FSOUND_GetNumSubChannels, "_FSOUND_GetNumSubChannels@4", 1) {
  auto ch = (int)arg(c, 0) == kFreeChannel ? nullptr : channel_shared((int)arg(c, 0));
  return ch && ch->smp ? 1u : 0u;
}

HLE_STDCALL_AS(kDll, FSOUND_GetSubChannel, "_FSOUND_GetSubChannel@8", 2) {
  auto ch = (int)arg(c, 0) == kFreeChannel ? nullptr : channel_shared((int)arg(c, 0));
  return ch ? (uint32_t)arg(c, 0) : 0;  // the single subchannel is the channel itself
}

// ---- Samples ----------------------------------------------------------------------------------

HLE_STDCALL_AS(kDll, FSOUND_Sample_SetMode, "_FSOUND_Sample_SetMode@8", 2) {
  auto* smp = sample_by(arg(c, 0));
  if (!smp) return 0;
  smp->mode = (smp->mode & ~7u) | (arg(c, 1) & 7u);
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_Sample_GetMode, "_FSOUND_Sample_GetMode@4", 1) {
  auto* smp = sample_by(arg(c, 0));
  return smp ? smp->mode : 0u;
}

HLE_STDCALL_AS(kDll, FSOUND_Sample_SetMinMaxDistance, "_FSOUND_Sample_SetMinMaxDistance@12", 3) {
  auto* smp = sample_by(arg(c, 0));
  if (!smp) return 0;
  smp->min_dist = bits2f32(arg(c, 1));
  smp->max_dist = bits2f32(arg(c, 2));
  return 1;
}

// FSOUND_Sample_GetDefaults(sample, deffreq, defvol, defpan, defpri): the game at 0x0053e399 passes
// &deffreq in arg(c, 1) and NULL for the other three, then divides Sample_GetLength by deffreq.
HLE_STDCALL_AS(kDll, FSOUND_Sample_GetDefaults, "_FSOUND_Sample_GetDefaults@20", 5) {
  auto* smp = sample_by(arg(c, 0));
  if (!smp) return 0;
  if (arg(c, 1)) st32(arg(c, 1), smp->rate);
  if (arg(c, 2)) st32(arg(c, 2), smp->def_vol);
  if (arg(c, 3)) st32(arg(c, 3), smp->def_pan);
  if (arg(c, 4)) st32(arg(c, 4), smp->def_pri);
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_Sample_GetLength, "_FSOUND_Sample_GetLength@4", 1) {
  auto* smp = sample_by(arg(c, 0));
  if (!smp) return 0;
  return (uint32_t)(smp->pcm.size() / (smp->channels ? smp->channels : 1));  // frames
}

HLE_STDCALL_AS(kDll, FSOUND_Sample_GetName, "_FSOUND_Sample_GetName@4", 1) {
  auto* smp = sample_by(arg(c, 0));
  if (!smp) return 0;
  if (!smp->name_guest) {  // guest-visible copy; leaked intentionally (bounded by sample count)
    const uint32_t bytes = (uint32_t)smp->name.size() + 1;
    smp->name_guest = heap_alloc(process_heap(), bytes, true);
    memcpy(host(smp->name_guest), smp->name.c_str(), bytes);
  }
  return smp->name_guest;
}

// ---- 3D audio ---------------------------------------------------------------------------------

HLE_STDCALL_AS(kDll, FSOUND_3D_SetDopplerFactor, "_FSOUND_3D_SetDopplerFactor@4", 1) {
  std::lock_guard<std::mutex> lock(g_sys_m);
  g_ls_doppler = bits2f32(arg(c, 0));
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_3D_SetAttributes, "_FSOUND_3D_SetAttributes@12", 3) {
  const int c_arg = (int)arg(c, 0);
  const uint32_t pos = arg(c, 1), vel = arg(c, 2);
  (void)vel;  // per-channel velocity is stored nowhere (doppler not simulated)
  auto apply = [&](FmodChannel* ch) {
    std::lock_guard<std::mutex> lock(ch->m);
    if (pos) {
      ch->pos3d = {bits2f32(ld32(pos)), bits2f32(ld32(pos + 4)), bits2f32(ld32(pos + 8))};
      if (!ch->smp || ((ch->smp->mode & (k2D | kHW2D)) == 0)) {
        ch->is3d = true;
      }
    }
  };
  if (c_arg == kAllChannels) {
    for (auto& ch : g_channels) if (ch) apply(ch.get());
    return 1;
  }
  auto ch = c_arg == kFreeChannel ? nullptr : channel_shared(c_arg);
  if (!ch) return 0;
  apply(ch.get());
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_3D_Listener_SetCurrent, "_FSOUND_3D_Listener_SetCurrent@8", 2) {
  // FMOD 3 supports multiple 3D drivers; the software mixer has exactly one.
  return 1;
}

// FSOUND_3D_Listener_SetAttributes(pos, vel, fx, fy, fz, tx, ty, tz): pointers plus six floats.
HLE_STDCALL_AS(kDll, FSOUND_3D_Listener_SetAttributes, "_FSOUND_3D_Listener_SetAttributes@32", 8) {
  const uint32_t pos = arg(c, 0), vel = arg(c, 1);
  Vec3 f{bits2f32(arg(c, 2)), bits2f32(arg(c, 3)), bits2f32(arg(c, 4))};
  Vec3 t{bits2f32(arg(c, 5)), bits2f32(arg(c, 6)), bits2f32(arg(c, 7))};
  Vec3 v;
  if (vel) v = {bits2f32(ld32(vel)), bits2f32(ld32(vel + 4)), bits2f32(ld32(vel + 8))};
  Vec3 p;
  if (pos) p = {bits2f32(ld32(pos)), bits2f32(ld32(pos + 4)), bits2f32(ld32(pos + 8))};
  set_listener(p.x, p.y, p.z, v.x, v.y, v.z, f, t);
  return 1;
}

// GetAttributes writes back (pos, vel, fx, fy, fz, tx, ty, tz) for every non-null slot.
HLE_STDCALL_AS(kDll, FSOUND_3D_Listener_GetAttributes, "_FSOUND_3D_Listener_GetAttributes@32", 8) {
  Vec3 pos, vel, front, top;
  float rolloff, doppler;
  listener_state(&pos, &vel, &front, &top, &rolloff, &doppler);
  if (arg(c, 0)) {
    st32(arg(c, 0), f32bits(pos.x));
    st32(arg(c, 0) + 4, f32bits(pos.y));
    st32(arg(c, 0) + 8, f32bits(pos.z));
  }
  if (arg(c, 1)) {
    st32(arg(c, 1), f32bits(vel.x));
    st32(arg(c, 1) + 4, f32bits(vel.y));
    st32(arg(c, 1) + 8, f32bits(vel.z));
  }
  if (arg(c, 2)) st32(arg(c, 2), f32bits(front.x));
  if (arg(c, 3)) st32(arg(c, 3), f32bits(front.y));
  if (arg(c, 4)) st32(arg(c, 4), f32bits(front.z));
  if (arg(c, 5)) st32(arg(c, 5), f32bits(top.x));
  if (arg(c, 6)) st32(arg(c, 6), f32bits(top.y));
  if (arg(c, 7)) st32(arg(c, 7), f32bits(top.z));
  return 1;
}

// ---- Streams ----------------------------------------------------------------------------------

// FSOUND_Stream_Open(name_or_data, mode, offset, length) - decodes immediately (no nonblocking
// open state games); GetOpenState reports 0 = opened.
HLE_STDCALL_AS(kDll, FSOUND_Stream_Open, "_FSOUND_Stream_Open@16", 4) {
  if (!g_inited) {
    set_error(kErrMemory);
    return 0;
  }
  std::vector<std::shared_ptr<FmodSample>> samples;
  if (!resolve_load(arg(c, 0), arg(c, 2), arg(c, 3), arg(c, 1), &samples)) return 0;
  const uint32_t h = new_handle();
  auto st = std::make_shared<FmodStream>();
  st->subs = std::move(samples);
  st->mode = arg(c, 1);
  std::lock_guard<std::mutex> lock(g_res_m);
  g_streams[h] = st;
  return h;
}

// FSOUND_Stream_PlayEx(channel, stream, dsp, startpaused) - same channel model as PlaySoundEx.
HLE_STDCALL_AS(kDll, FSOUND_Stream_PlayEx, "_FSOUND_Stream_PlayEx@16", 4) {
  if (!g_inited) {
    set_error(kErrMemory);
    return (uint32_t)-1;
  }
  auto* st = stream_by(arg(c, 1));
  if (!st || st->subs.empty()) {
    set_error(kErrMemory);
    return (uint32_t)-1;
  }
  auto smp = st->sentence_smp ? st->sentence_smp : st->subs[0];
  const int ch = start_channel(smp, (int)arg(c, 0), arg(c, 3) != 0);
  if (ch >= 0) {
    if ((st->mode & (kLoopNormal | kLoopBidi)) != 0) {
      if (auto chan = channel_shared(ch)) {
        std::lock_guard<std::mutex> lock(chan->m);
        const uint32_t total_frames =
            (uint32_t)(smp->pcm.size() / (smp->channels ? smp->channels : 1));
        chan->loop_start = smp->loop_start;
        chan->loop_end = smp->loop_end > smp->loop_start ? smp->loop_end : total_frames;
        chan->looping = true;
      }
    }
    st->active_ch = ch;
    if (trace_enabled("audio"))
      log(Log::Hle, "fmod: Stream_PlayEx ch=%d stream=%08x (%s) smode=%x mode=%x loop=%u..%u frames=%zu paused=%u",
          ch, arg(c, 1), smp->name.c_str(), st->mode, smp->mode, smp->loop_start, smp->loop_end,
          smp->pcm.size() / (smp->channels ? smp->channels : 1), arg(c, 3) != 0);
  }
  return ch < 0 ? (uint32_t)-1 : (uint32_t)ch;
}

HLE_STDCALL_AS(kDll, FSOUND_Stream_Stop, "_FSOUND_Stream_Stop@4", 1) {
  auto* st = stream_by(arg(c, 0));
  if (!st) return 0;
  if (st->active_ch >= 0) {
    stop_channel(st->active_ch);
    st->active_ch = -1;
  }
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_Stream_Close, "_FSOUND_Stream_Close@4", 1) {
  auto* st = stream_by(arg(c, 0));
  if (!st) return 0;
  if (st->active_ch >= 0) stop_channel(st->active_ch);
  std::lock_guard<std::mutex> lock(g_res_m);
  return g_streams.erase(arg(c, 0)) != 0 ? 1u : 0u;
}

HLE_STDCALL_AS(kDll, FSOUND_Stream_SetMode, "_FSOUND_Stream_SetMode@8", 2) {
  auto* st = stream_by(arg(c, 0));
  if (!st) return 0;
  st->mode = arg(c, 1);
  return 1;
}

HLE_STDCALL_AS(kDll, FSOUND_Stream_SetBufferSize, "_FSOUND_Stream_SetBufferSize@4", 1) {
  return 1;  // the software mixer buffers as it renders
}

// Length of the sentence-ordered playback (or the single subsong) in milliseconds.
HLE_STDCALL_AS(kDll, FSOUND_Stream_GetLengthMs, "_FSOUND_Stream_GetLengthMs@4", 1) {
  auto* st = stream_by(arg(c, 0));
  if (!st) return 0;
  uint32_t ms = 0;
  auto ms_of = [](const std::shared_ptr<FmodSample>& s) {
    if (!s || !s->rate) return 0u;
    const uint32_t frames = (uint32_t)(s->pcm.size() / (s->channels ? s->channels : 1));
    return (uint32_t)((uint64_t)frames * 1000 / s->rate);
  };
  if (st->sentence_smp) {
    ms = ms_of(st->sentence_smp);
  } else if (!st->subs.empty()) {
    ms = ms_of(st->subs[0]);
  }
  return ms;
}

HLE_STDCALL_AS(kDll, FSOUND_Stream_GetOpenState, "_FSOUND_Stream_GetOpenState@4", 1) {
  // 0 = opened; -3 (0xFFFFFFFD) = failed to open / invalid stream handle (checked at 0x005409a0).
  return (stream_by(arg(c, 0)) ? 0u : (uint32_t)-3);
}

HLE_STDCALL_AS(kDll, FSOUND_Stream_SetSubStreamSentence, "_FSOUND_Stream_SetSubStreamSentence@12",
               3) {
  auto* st = stream_by(arg(c, 0));
  if (!st) return 0;
  const uint32_t list = arg(c, 1), num = arg(c, 2);
  st->sentence.clear();
  st->sentence_smp.reset();
  st->sentence.reserve(num);
  for (uint32_t i = 0; i < num; i++) {
    st->sentence.push_back(ld32(list + 4 * i));
  }
  if (!st->sentence.empty() && !st->subs.empty()) {
    const uint32_t seed_idx = std::min(st->sentence[0], (uint32_t)st->subs.size() - 1);
    const auto& seed = st->subs[seed_idx];
    st->sentence_smp = std::make_shared<FmodSample>();
    st->sentence_smp->handle = new_handle();
    {
      std::lock_guard<std::mutex> lock(g_res_m);
      g_samples[st->sentence_smp->handle] = st->sentence_smp;
    }
    st->sentence_smp->rate = seed->rate;
    st->sentence_smp->channels = seed->channels;
    st->sentence_smp->mode = seed->mode;
    st->sentence_smp->def_vol = seed->def_vol;
    st->sentence_smp->def_pan = seed->def_pan;
    st->sentence_smp->def_pri = seed->def_pri;
    st->sentence_smp->min_dist = seed->min_dist;
    st->sentence_smp->max_dist = seed->max_dist;
    for (uint32_t idx : st->sentence) {
      const uint32_t sub = std::min(idx, (uint32_t)st->subs.size() - 1);
      const auto& pcm = st->subs[sub]->pcm;
      st->sentence_smp->pcm.insert(st->sentence_smp->pcm.end(), pcm.begin(), pcm.end());
    }
    st->sentence_smp->loop_start = 0;
    st->sentence_smp->loop_end =
        (uint32_t)(st->sentence_smp->pcm.size() /
                   (st->sentence_smp->channels ? st->sentence_smp->channels : 1));
  }
  return 1;
}

// ---- FMUSIC (module songs) --------------------------------------------------------------------

// FMUSIC_LoadSongEx(name_or_data, offset, length, mode, samplelist, samplelist_num). Both call
// sites use path+sub-range addressing (mode without FSOUND_LOADMEMORY); sample lists are unused
// (no movieGame DLS instruments in these banks).
HLE_STDCALL_AS(kDll, FMUSIC_LoadSongEx, "_FMUSIC_LoadSongEx@24", 6) {
  if (!g_inited) {
    set_error(kErrMemory);
    return 0;
  }
  std::vector<std::shared_ptr<FmodSample>> samples;
  if (!resolve_load(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), &samples)) return 0;
  const uint32_t h = new_handle();
  auto mod = std::make_shared<FmodModule>();
  mod->samples = std::move(samples);
  std::lock_guard<std::mutex> lock(g_res_m);
  g_modules[h] = mod;
  if (trace_enabled("audio"))
    log(Log::Hle, "fmod: FMUSIC_LoadSongEx handle=%08x samples=%zu", h, mod->samples.size());
  return h;
}

HLE_STDCALL_AS(kDll, FMUSIC_FreeSong, "_FMUSIC_FreeSong@4", 1) {
  std::lock_guard<std::mutex> lock(g_res_m);
  return g_modules.erase(arg(c, 0)) != 0 ? 1u : 0u;
}

HLE_STDCALL_AS(kDll, FMUSIC_GetNumSamples, "_FMUSIC_GetNumSamples@4", 1) {
  auto* mod = module_by(arg(c, 0));
  return mod ? (uint32_t)mod->samples.size() : 0u;
}

HLE_STDCALL_AS(kDll, FMUSIC_GetSample, "_FMUSIC_GetSample@8", 2) {
  auto* mod = module_by(arg(c, 0));
  if (!mod || arg(c, 1) >= mod->samples.size()) return 0;
  return mod->samples[arg(c, 1)]->handle;
}
