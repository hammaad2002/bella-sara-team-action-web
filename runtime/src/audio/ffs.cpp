// .ffs bank parser/decoder. Layout derived from the shipped banks themselves (see
// work/AUDIO-NOTES.md for the evidence: offset/size arithmetic that closes exactly on both
// banks, the FSB3 magic at every claimed entry offset, header sums == entry sizes, and the
// IMA/PCM16 self-consistency num_samples <=-> stream_size checks):
//
// Bank ("FFS " container, music.ffs / effects.ffs) - settled by byte-exact arithmetic over all
// 48 entries of both banks:
//   0x00  "FFS "
//   0x04  u32 entry count (music 6, effects 42)
//   ...   count entry records: { u32 name_len; name[name_len] (no NUL, upper case);
//                                u32 offset; u32 size }
//   `size` is the exact payload byte count (an FSB's 0x18+headers+data closes on it; a .DAT's
//   4+8*rows closes on it for all 21 companions), and a single 0xFF separator byte follows each
//   entry's payload as the next entry record or, after the last entry, as the final file byte.
//   A zero-`name_len` entry may appear at the directory end (seen in other FMOD-built banks);
//   length checks keep the parser robust either way.
//
// .FSB entries are FMOD FSB3.1 sample banks:
//   0x00 "FSB3" | u32 subsongs | u32 sample_headers_size | u32 sample_data_size |
//         u32 version (0x00030001 = FSB 3.1 observed) | u32 flags (0 observed)
//   0x18 variable sample headers, each: u16 stride (>= 0x50 for 3.1), name at +2 (<= 30 bytes,
//         NUL-trimmed), then: i32 num_samples +0x20, u32 stream_size +0x24, i32 loop_start
//         +0x28, i32 loop_end +0x2c, u32 mode +0x30 (FSOUND_* bits), i32 rate +0x34,
//         u16 def_vol +0x38, u16 def_pan +0x3a, u16 def_pri +0x3c, u16 channels +0x3e,
//         f32 min_dist +0x40, f32 max_dist +0x44 (FSB3.1 extras), u16 var_freq +0x48,
//         u16 var_vol +0x4c, u16 var_pan +0x4e. Sample data follows the header block at
//         0x18 + sample_headers_size, in subsong order (stream_size bytes each).
//   Observed codecs via mode bits, both self-consistent against num_samples/stream_size:
//     0x00000131 PCM16LE   mono   (stream_size == num_samples * 2)      [465 samples]
//     0x00000151 PCM16LE   stereo (stream_size == num_samples * 4)      [ 18 samples]
//     0x00400041 IMA ADPCM stereo (stream_size == (num_samples / 64) * 72) [ 6 samples]
//   The IMA layout is per 72-byte stereo frame: ch0 header (int16 amplitude, u8 step index,
//   u8 reserved) at 0..3, ch1 header at 4..7, then 8 bands of 8 bytes: band b occupies frame
//   bytes 8+8b..15+8b with ch0's dword at the first 4 bytes ("codes" read low-nibble-first,
//   j-th code = (u32 >> 4j) & 15) and ch1's dword at the last 4. Per band each channel consumes
//   min(8, 63 - 8b) codes (63 per block; the 64th nibble is padding and is skipped). Each code
//   appends one decoded sample per channel, and the header amplitude is emitted as sample 0, so
//   every block yields 64 samples per channel. The delta uses the "bit" decomposition
//   (step>>3 + bit0*step>>2 + bit1*step>>1 + bit2*step, negated by bit3), amplitude clamps to
//   [-32768, 32767], and the step index advances by IDXADJ[nib] = {-1,-1,-1,-1,2,4,6,8}[nib&7]
//   *2 (per fmod.dll 0x3d720), clamping to 0..88. This was settled by disassembling the shipped
//   fmod.dll 3.75 (stereo decoder VA 0x1698f, mono 0x16878, delta helper 0x16821; see
//   work/AUDIO-NOTES.md) and re-measured against the banks: per-block step-index evolution is
//   byte-exact (1998/1998 checks over all 6 songs), and the stored per-block amplitude is the
//   ENCODER's PCM value, so the decoded-final amplitude ends a few LSB away from the next
//   header (normal codec noise, not a border error).
//
// .DAT companions ("X.DAT" pairs "X.FSB"): u32 count == the FSB's subsong count, then
//   count { u32 flag; f32 distance }. Values observed: UI* flag=1 dist=30; HOOF*/HORSE* flag=0
//   dist=10; AMBIENT* flag=0 dist=2/4/5; DRESSAGE mostly flag=0 dist=30. Model: flag 1 = 2D-ish
//   (attenuation off), flag 0 = 3D with `distance` as the maximum hearing distance (TODO(M5)
//   confirm against EQ_SoundFFSData semantics).
#include "ffs.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace audio {
namespace {

// IMA ADPCM step table (DVI/IMA standard; 89 entries, index clamped to 0..88).
constexpr int kImaSteps[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
                               41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
                               190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
                               724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
                               2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
                               6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
                               16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kImaIndexAdj[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

uint32_t ru32(const uint8_t* p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
int32_t ri32(const uint8_t* p) { return (int32_t)ru32(p); }
uint16_t ru16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

std::string trim_nul(const uint8_t* p, size_t n) {
  size_t end = 0;
  while (end < n && p[end] >= 0x20 && p[end] < 0x7f) end++;
  return std::string((const char*)p, end);
}

// Custom exception-free failure flagging: functions below return false and fill *error.
// All sample-data reads go through cached FILE* with a bounded chunked read into scratch.

struct Scope {
  FILE* fp = nullptr;
  ~Scope() {
    if (fp) fclose(fp);
  }
};

void set(std::string* e, const std::string& path, const std::string& what,
         const std::string& extra = std::string()) {
  if (e) {
    *e = path + ": " + what;
    if (!extra.empty()) *e += " " + extra;
  }
}
void set_str(std::string* e, const std::string& path, const std::string& what, const char* extra) {
  if (e) {
    *e = path + ": " + what;
    if (extra) *e += std::string(" ") + extra;
  }
}

// One channel's IMA ADPCM running state within a block.
struct ImaSys {
  int cur = 0, idx = 0, step = 0;
};
void ima_init(ImaSys* s, const uint8_t* hdr) {
  s->cur = (int)(int16_t)(hdr[0] | (uint32_t)hdr[1] << 8);
  s->idx = std::clamp<int>(hdr[2], 0, 88);
  s->step = kImaSteps[s->idx];
}
void ima_emit(ImaSys* s, const uint8_t nib, std::vector<int16_t>* out) {
  // "bit" delta decomposition per fmod.dll 0x16821 (sign-extending arithmetic shifts).
  int diff = s->step >> 3;
  if (nib & 1) diff += s->step >> 2;
  if (nib & 2) diff += s->step >> 1;
  if (nib & 4) diff += s->step;
  if (nib & 8) diff = -diff;
  s->cur = std::clamp(s->cur + diff, -32768, 32767);
  s->idx = std::clamp(s->idx + kImaIndexAdj[nib], 0, 88);
  s->step = kImaSteps[s->idx];
  out->push_back((int16_t)s->cur);
}

// Decode one 72-byte stereo IMA ADPCM block (FMOD3 format confirmed against the shipped
// fmod.dll 3.75 disassembly - see the header comment / AUDIO-NOTES.md): per-channel 4-byte
// headers, then 8 bands of 8 bytes with each channel's dword consumed low-nibble-first, 63
// codes per channel (min(8, 63-8b) per band; the 64th nibble skipped) plus the header
// amplitude emitted as sample 0 per channel. Output is interleaved L/R pairs (128 samples).
void decode_ima_stereo_block(const uint8_t* f, std::vector<int16_t>* out) {
  ImaSys l, r;
  ima_init(&l, f);
  ima_init(&r, f + 4);
  out->reserve(out->size() + 128);
  out->push_back((int16_t)l.cur);  // header amplitude is sample 0 of each channel
  out->push_back((int16_t)r.cur);
  for (int b = 0; b < 8; b++) {
    const uint32_t c0 = ru32(f + 8 + 8 * b), c1 = ru32(f + 12 + 8 * b);
    const int cnt = std::min(8, 63 - 8 * b);
    for (int j = 0; j < cnt; j++) {
      ima_emit(&l, (uint8_t)((c0 >> (4 * j)) & 15), out);
      ima_emit(&r, (uint8_t)((c1 >> (4 * j)) & 15), out);
    }
  }
}

}  // namespace

std::unique_ptr<FfsBank> FfsBank::open(const std::string& path, std::string* error) {
  FILE* fp = fopen(path.c_str(), "rb");
  if (!fp) {
    set(error, path, "cannot open bank");
    return nullptr;
  }
  Scope closer{fp};
  uint8_t head[8];
  if (fread(head, 1, 8, fp) != 8 || memcmp(head, "FFS ", 4) != 0) {
    set(error, path, "not an FFS container (magic)");
    return nullptr;
  }
  auto bank = std::unique_ptr<FfsBank>(new FfsBank());
  bank->path_ = path;
  fseek(fp, 0, SEEK_END);
  const long file_size = ftell(fp);
  // Directory stream position: after the "FFS " magic + count pair; each record advances it by
  // its own byte length. (The bank's entries and directory share one read handle; the handle
  // ping-pongs between directory reads and separator probes below.)
  long dir_pos = 8;
  fseek(fp, 8, SEEK_SET);

  uint32_t n = ru32(head + 4);
  std::vector<Entry> dats;
  for (uint32_t i = 0; i < n; i++) {
    uint8_t buf[8];
    if (fread(buf, 1, 4, fp) != 4) {
      set(error, path, "truncated directory header");
      return nullptr;
    }
    uint32_t name_len = ru32(buf);
    if (name_len + (uint64_t)8 > (uint32_t)file_size) {
      set_str(error, path, "declared name length overruns the file",
              std::to_string(name_len).c_str());
      return nullptr;
    }
    std::vector<uint8_t> name(name_len);
    if (fread(name.data(), 1, name.size(), fp) != name.size()) {
      set(error, path, "truncated directory name");
      return nullptr;
    }
    // Entry records: { u32 name_len; name[name_len]; u32 offset; u32 size }.
    if (fread(buf, 1, 8, fp) != 8) {
      set(error, path, "truncated entry offset/size");
      return nullptr;
    }
    Entry e;
    e.name = std::string((const char*)name.data(), name_len);
    e.offset = ru32(buf);
    e.size = ru32(buf + 4);
    dir_pos += 12 + (long)name_len;  // record (4 len + name + 8 off/size) fully consumed
    // Sanity: entries live back to back with a single 0xFF separator after each entry's payload;
    // the first entry's offset marks the directory end.
    if (i == 0 && e.offset <= 8 + 12 + name_len) {
      set_str(error, path, "first entry offset too small to fit the directory", e.name.c_str());
      return nullptr;
    }
    if ((uint32_t)file_size < (uint32_t)(e.offset + e.size + 1)) {
      set_str(error, path, "entry extends beyond file", e.name.c_str());
      return nullptr;
    }
    uint8_t sep;
    if (fseek(fp, (long)((uint64_t)e.offset + e.size), SEEK_SET) != 0 || fread(&sep, 1, 1, fp) != 1 ||
        sep != 0xFF) {
      set_str(error, path, "missing 0xFF separator after entry", e.name.c_str());
      return nullptr;
    }
    // Restore the directory stream position by explicit arithmetic: the record body after the
    // name consumed 8 more bytes (offset+size), and the directory cursor after entry i's record
    // is (8 + sum of 12+name_len over records 0..i). Track it in dir_pos; never trust SEEK_CUR
    // after the separator probe seeked elsewhere.
    if (fseek(fp, dir_pos, SEEK_SET) != 0) {
      set(error, path, "directory rewind after separator failed");
      return nullptr;
    }
    bank->entries_.push_back(e);
    if (e.name.size() >= 4 && e.name.compare(e.name.size() - 4, 4, ".DAT") == 0) dats.push_back(e);
  }
  if (n == 0) {
    set(error, path, "bank declares zero entries");
    return nullptr;
  }
  if (bank->entries_.back().offset + bank->entries_.back().size + 1 != (uint32_t)file_size) {
    set(error, path, "last entry does not end at EOF-1");
    return nullptr;
  }

  // Pair every ".DAT" with its ".FSB" (same stem) and stash flag/dist per subsong for
  // sample_info() below by parsing the DAT table now (cheap reads; .DAT entries are tiny).
  for (const Entry& dat : dats) {
    if (dat.size < 5 || dat.name.size() < 5) continue;
    std::vector<uint8_t> data(dat.size);
    if (fseek(fp, (long)dat.offset, SEEK_SET) != 0 ||
        fread(data.data(), 1, data.size(), fp) != data.size()) {
      set_str(error, path, "short read of .DAT", dat.name.c_str());
      return nullptr;
    }
    const uint32_t count = ru32(data.data());
    // Payload is exactly 4 + count * 8 bytes: settles arithmetic on all 21 companions in
    // effects.ffs. A mismatch means misinterpretation - fail loudly rather than attach bogus
    // distances.
    if (4 + (uint64_t)count * 8 != data.size()) {
      set_str(error, path, "unexpected .DAT layout", dat.name.c_str());
      return nullptr;
    }
    std::vector<std::pair<int32_t, float>> rows;
    rows.reserve(count);
    for (uint32_t i = 0; 4 + 8 * (uint64_t)i + 8 <= data.size() && i < count; i++) {
      rows.emplace_back((int32_t)ru32(data.data() + 4 + 8 * i),
                        *(const float*)(const void*)(data.data() + 8 * i + 4));
    }
    bank->dat_rows_.emplace(dat.name.substr(0, dat.name.size() - 4), std::move(rows));
  }
  return bank;
}

bool FfsBank::entry(const std::string& name, Entry* out) const {
  for (const Entry& e : entries_) {
    if (e.name.size() != name.size()) continue;
    size_t i = 0;
    while (i < name.size() && toupper((unsigned char)e.name[i]) == toupper((unsigned char)name[i])) i++;
    if (i == name.size()) {
      if (out) *out = e;
      return true;
    }
  }
  return false;
}

uint32_t FfsBank::entry_subsongs(const Entry& e) const {
  FILE* fp = fopen(path_.c_str(), "rb");
  if (!fp) return 0;
  Scope closer{fp};
  fseek(fp, (long)(e.offset + 4), SEEK_SET);
  uint8_t buf[12];
  if (fread(buf, 1, 12, fp) != 12) return 0;
  return ru32(buf);  // subsongs at main header +4
}

bool FfsBank::sample_info(const Entry& e, uint32_t subsong, FfsSampleInfo* out,
                          std::string* error) const {
  if (!is_fsb(e)) {
    set(error, path_, "not a .FSB entry");
    return false;
  }
  FILE* fp = fopen(path_.c_str(), "rb");
  if (!fp) {
    set(error, path_, "cannot open bank");
    return false;
  }
  Scope closer{fp};
  fseek(fp, (long)e.offset, SEEK_SET);
  uint8_t mah[0x18];
  if (fread(mah, 1, sizeof(mah), fp) != sizeof(mah)) {
    set_str(error, path_, "short FSB3 main header", e.name.c_str());
    return false;
  }
  if (memcmp(mah, "FSB3", 4) != 0) {
    set_str(error, path_, "entry is not FSB3", e.name.c_str());
    return false;
  }
  const uint32_t subsongs = ru32(mah + 4);
  const uint32_t headers_size = ru32(mah + 8);
  const uint32_t version = ru32(mah + 12);
  // Only FSB 3.1 (0x00030001) has the 0x50-byte headers with the +0x40 distance extras; 3.0
  // headers are 0x40 bytes. Both banks use 3.1, but decode tolerates 3.0 without the extras.
  const uint32_t header_min = version == 0x00030001 ? 0x50 : 0x40;
  if (subsong >= subsongs) {
    set_str(error, path_, "subsong index out of range", e.name.c_str());
    return false;
  }
  // Walk the variable-size headers, keeping accruing stream offsets.
  uint32_t hdr_off = e.offset + 0x18;
  uint32_t data_off = e.offset + 0x18 + headers_size;
  uint32_t stream_off = data_off;
  FfsSampleInfo info;
  for (uint32_t i = 0; i <= subsong; i++) {
    uint8_t tmp[0x50];
    if (fseek(fp, (long)hdr_off, SEEK_SET) != 0 || fread(tmp, 1, sizeof(tmp), fp) != sizeof(tmp)) {
      set_str(error, path_, "short FSB sample header", e.name.c_str());
      return false;
    }
    const uint16_t stride = ru16(tmp);
    if (stride < header_min) {
      set_str(error, path_, "FSB sample header too small", e.name.c_str());
      return false;
    }
    if (i == subsong) {
      info.index = i;
      info.name = trim_nul(tmp + 2, 30);
      info.num_samples = (uint32_t)ri32(tmp + 0x20);
      info.stream_size = ru32(tmp + 0x24);
      info.loop_start = (uint32_t)ri32(tmp + 0x28);
      info.loop_end = (uint32_t)ri32(tmp + 0x2c);
      info.mode = ru32(tmp + 0x30);
      info.sample_rate = (uint32_t)ri32(tmp + 0x34);
      info.def_vol = ru16(tmp + 0x38);
      info.def_pan = ru16(tmp + 0x3a);
      info.def_pri = ru16(tmp + 0x3c);
      info.channels = ru16(tmp + 0x3e);
      if (version == 0x00030001) {
        memcpy(&info.min_dist, tmp + 0x40, 4);
        memcpy(&info.max_dist, tmp + 0x44, 4);
        info.var_freq = ru16(tmp + 0x48);
        info.var_vol = ru16(tmp + 0x4c);
        info.var_pan = ru16(tmp + 0x4e);
      }
      info.data_rel = stream_off - data_off;
    }
    stream_off += ru32(tmp + 0x24);
    hdr_off += stride;
    if (hdr_off > data_off) {
      set_str(error, path_, "FSB sample headers overrun the header block", e.name.c_str());
      return false;
    }
  }
  // .DAT companion (by the entry's stem) fills flag/dist when present.
  std::string stem = e.name.substr(0, e.name.size() - 4);
  auto it = dat_rows_.find(stem);
  if (it != dat_rows_.end() && subsong < it->second.size()) {
    info.dat_flag = it->second[subsong].first;
    info.dat_dist = it->second[subsong].second;
  }
  if (out) *out = info;
  return true;
}

// Codec decode shared with decode_fsb_bytes: `raw` holds this subsong's stream_size bytes.
bool decode_codecs(const uint8_t* raw, const uint32_t bytes, const FfsSampleInfo& info,
                   std::vector<int16_t>* pcm, const std::string& name, std::string* error) {
  const uint32_t mode = info.mode;
  if (mode == 0x00000131 || mode == 0x00000151) {  // PCM16LE, mono / stereo
    if (info.channels == 0 || (bytes % (2 * info.channels)) != 0) {
      set_str(error, name, "PCM16 stream size not frame-aligned", "");
      return false;
    }
    pcm->assign((size_t)(bytes / 2), 0);
    // memcpy in place; reinterpret little-endian (host is x86/ARM64 LE; runtime targets x86 web
    // and native x86/ARM64 - all little-endian, direct reinterpreting is fine after a static
    // assert).
    static_assert(sizeof(int16_t) == 2, "");
    memcpy(pcm->data(), raw, bytes);
    return true;
  }
  if (mode == 0x00400041) {  // IMA ADPCM stereo, 72-byte frames
    if (info.channels != 2 || bytes % 72 != 0) {
      set_str(error, name, "unexpected IMA geometry", "");
      return false;
    }
    pcm->reserve((size_t)info.num_samples * 2);
    for (uint32_t off = 0; off + 72 <= bytes; off += 72) {
      decode_ima_stereo_block(raw + off, pcm);  // appends 128 interleaved samples
    }
    return true;
  }
  set_str(error, name, "unsupported codec for the increment", "");
  return false;
}

bool FfsBank::decode_fsb_bytes(const uint8_t* blob, size_t len, std::vector<FfsSubsong>* subs,
                               std::string* error) {
  if (!subs) return false;
  subs->clear();
  // FSB3 main header: "FSB3" | u32 subsongs | u32 headers_size | u32 data_size | u32 version |
  //                   u32 flags - then the variable sample headers, then the sample data.
  if (len < 0x18 || memcmp(blob, "FSB3", 4) != 0) {
    set(error, "", "blob is not an FSB3 bank");
    return false;
  }
  const uint32_t subsongs = ru32(blob + 4);
  const uint32_t headers_size = ru32(blob + 8);
  const uint32_t version = ru32(blob + 12);
  if (subsongs == 0 || subsongs > 4096) {
    set_str(error, "", "implausible FSB3 subsong count", std::to_string(subsongs).c_str());
    return false;
  }
  if ((uint64_t)0x18 + headers_size > len) {
    set(error, "", "FSB3 sample headers overrun the blob");
    return false;
  }
  const uint32_t header_min = version == 0x00030001 ? 0x50 : 0x40;
  uint32_t hdr_off = 0x18;
  uint32_t stream_rel = 0;
  const uint32_t data_off = 0x18 + headers_size;
  for (uint32_t i = 0; i < subsongs; i++) {
    if (hdr_off + 0x20 > data_off) {
      set_str(error, "", "FSB3 sample header overruns the header block", "");
      return false;
    }
    const uint8_t* tmp = blob + hdr_off;
    const uint16_t stride = ru16(tmp);
    if (stride < header_min) {
      set_str(error, "", "FSB3 sample header too small", "");
      return false;
    }
    if (hdr_off + stride > data_off) {
      set_str(error, "", "FSB3 sample header strides overrun the header block", "");
      return false;
    }
    FfsSubsong s;
    FfsSampleInfo& info = s.info;
    info.index = i;
    info.name = trim_nul(tmp + 2, 30);
    info.num_samples = (uint32_t)ri32(tmp + 0x20);
    info.stream_size = ru32(tmp + 0x24);
    info.loop_start = (uint32_t)ri32(tmp + 0x28);
    info.loop_end = (uint32_t)ri32(tmp + 0x2c);
    info.mode = ru32(tmp + 0x30);
    info.sample_rate = (uint32_t)ri32(tmp + 0x34);
    info.def_vol = ru16(tmp + 0x38);
    info.def_pan = ru16(tmp + 0x3a);
    info.def_pri = ru16(tmp + 0x3c);
    info.channels = ru16(tmp + 0x3e);
    if (version == 0x00030001) {
      memcpy(&info.min_dist, tmp + 0x40, 4);
      memcpy(&info.max_dist, tmp + 0x44, 4);
      info.var_freq = ru16(tmp + 0x48);
      info.var_vol = ru16(tmp + 0x4c);
      info.var_pan = ru16(tmp + 0x4e);
    }
    info.data_rel = stream_rel;
    stream_rel += info.stream_size;
    if ((uint64_t)data_off + stream_rel > len) {
      set_str(error, "", "FSB3 sample data overruns the blob", info.name.c_str());
      return false;
    }
    hdr_off += stride;
    subs->push_back(std::move(s));
  }
  for (FfsSubsong& s : *subs) {
    const uint8_t* raw = blob + data_off + s.info.data_rel;
    if (!decode_codecs(raw, s.info.stream_size, s.info, &s.pcm, s.info.name, error)) {
      subs->clear();
      return false;
    }
  }
  return true;
}

bool FfsBank::decode_sample(const Entry& e, uint32_t subsong, std::vector<int16_t>* pcm,
                            FfsSampleInfo* out, std::string* error) const {
  FfsSampleInfo info;
  if (!sample_info(e, subsong, &info, error)) return false;
  if (out) *out = info;
  if (pcm) pcm->clear();

  FILE* fp = fopen(path_.c_str(), "rb");
  if (!fp) {
    set(error, path_, "cannot open bank");
    return false;
  }
  Scope closer{fp};
  // Data offset for this subsong inside the bank: header area then the accrued streams.
  uint32_t headers_size = 0;
  fseek(fp, (long)(e.offset + 8), SEEK_SET);
  uint8_t hs[4];
  if (fread(hs, 1, 4, fp) != 4) {
    set_str(error, path_, "short FSB header", e.name.c_str());
    return false;
  }
  headers_size = ru32(hs);
  const uint32_t data_off = e.offset + 0x18 + headers_size + info.data_rel;
  const uint32_t bytes = info.stream_size;
  std::vector<uint8_t> raw(bytes);
  fseek(fp, (long)data_off, SEEK_SET);
  if (fread(raw.data(), 1, bytes, fp) != bytes) {
    set_str(error, path_, "short read of sample data", e.name.c_str());
    return false;
  }

  return decode_codecs(raw.data(), bytes, info, pcm, e.name, error);
}

}  // namespace audio
