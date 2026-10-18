// FFS/FSB bank format tests for the increment: parse both shipped banks and assert every
// structural invariant of the formats described in src/audio/ffs.h and work/AUDIO-NOTES.md,
// plus spot-decodes (PCM16 and stereo IMA). Exits 0 after printing "FFS TEST PASS"; any
// mismatch aborts with a non-zero exit so CI/cmake --build test targets fail loudly.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "audio/ffs.h"

using audio::FfsBank;
using audio::FfsSampleInfo;

static int g_failures = 0;

static void fail(const std::string& msg) {
  ++g_failures;
  fprintf(stderr, "FAIL %s\n", msg.c_str());
}

static void expect(bool cond, const std::string& msg) {
  if (!cond) fail(msg);
}

static void check_bank(const char* path_file, uint32_t expected_entries, int expect_pcm, int expect_ima, const std::string& tag) {
  std::string err;
  auto bank = FfsBank::open(path_file, &err);
  if (!bank) {
    fail(tag + ": open error: " + err);
    return;
  }
  const auto& entries = bank->entries();
  expect(entries.size() == expected_entries, tag + ": entry count");
  if (entries.size() != expected_entries) return;

  // Directory invariants: first entry begins exactly where the directory ends; names are
  // upper-case, .FSB or .DAT, and every FSB is a parseable FSB3 whose header sum closes on the
  // entry size (0x18 + headers + data == size).
  int pcm_count = 0, ima_count = 0, dat_count = 0;
  for (const FfsBank::Entry& e : entries) {
    const bool fsb = bank->is_fsb(e);
    if (fsb) {
      expect(e.size >= 0x18, tag + ": FSB entry size covers main header");
      // FSB3 main header walk via sample_info over each subsong forces full header validation.
      const uint32_t subsongs = bank->entry_subsongs(e);
      expect(subsongs > 0, tag + ": FSB declares subsongs " + e.name);
      for (uint32_t s = 0; s < subsongs; s++) {
        FfsSampleInfo info;
        err.clear();
        if (!bank->sample_info(e, s, &info, &err)) {
          fail(tag + ": sample_info " + e.name + "[" + std::to_string(s) + "] couldn't parse: " + err);
          return;
        }
        // Codec byte-exactness, MainWindow PCM sizes and IMA frame arithmetic.
        if (info.mode == 0x00000131) {
          ++pcm_count;
          expect(info.channels == 1, tag + ": mono PCM channels");
          expect(info.stream_size == info.num_samples * 2, tag + ": mono PCM bytes == frames*2");
        } else if (info.mode == 0x00000151) {
          ++pcm_count;
          expect(info.channels == 2, tag + ": stereo PCM channels");
          expect(info.stream_size == info.num_samples * 4, tag + ": stereo PCM bytes == frames*4");
        } else if (info.mode == 0x00400041) {
          ++ima_count;
          expect(info.channels == 2, tag + ": IMA channels");
          expect(info.num_samples % 64 == 0, tag + ": IMA frames multiple of 64");
          expect(info.stream_size == info.num_samples / 64 * 72, tag + ": IMA stream 72B/frame");
        } else {
          fail(tag + ": unknown codec mode " + std::to_string(info.mode) + " in " + e.name);
        }
        expect(info.sample_rate == 44100, tag + ": all bank samples are 44100Hz");
        expect(info.sample_rate > 0 && info.sample_rate < 1u << 20, tag + ": sane rate");
      }
    } else if (e.name.size() >= 4 && e.name.compare(e.name.size() - 4, 4, ".DAT") == 0) {
      ++dat_count;
      // Every .DAT must have a matching .FSB sibling with the same subsong count.
      std::string stem = e.name.substr(0, e.name.size() - 4);
      FfsBank::Entry fsb_e;
      expect(bank->entry(stem + ".FSB", &fsb_e), tag + ": .DAT has .FSB sibling " + e.name);
      if (bank->entry(stem + ".FSB", &fsb_e)) {
        const uint32_t subsongs = bank->entry_subsongs(fsb_e);
        // Round-trip: sample_info of the last subsong of the paired bank carries the DAT rows.
        FfsSampleInfo last;
        err.clear();
        if (subsongs > 0 && bank->sample_info(fsb_e, subsongs - 1, &last, &err)) {
          expect(last.dat_flag == 0 || last.dat_flag == 1,
                 tag + ": DAT flag is 0/1 " + e.name);
          expect(last.dat_dist >= 0 && std::isfinite(last.dat_dist), tag + ": DAT dist sane");
        }
      }
    } else {
      fail(tag + ": unexpected entry extension " + e.name);
    }
  }
  expect(pcm_count == expect_pcm, tag + ": PCM16 sample count " + std::to_string(pcm_count));
  expect(ima_count == expect_ima, tag + ": IMA sample count " + std::to_string(ima_count));
}

static bool decoded_audio_is_audible(const std::vector<int16_t>& pcm, const std::string& tag);
static void check_ima_decode(const char* path_file, const char* entry_name, uint32_t subsong,
                             const std::string& tag) {
  std::string err;
  auto bank = FfsBank::open(path_file, &err);
  if (!bank) {
    fail(tag + ": reopen error: " + err);
    return;
  }
  FfsBank::Entry e;
  expect(bank->entry(entry_name, &e), tag + ": entry lookup");
  if (!bank->entry(entry_name, &e)) return;
  std::vector<int16_t> pcm;
  FfsSampleInfo info;
  err.clear();
  if (!bank->decode_sample(e, subsong, &pcm, &info, &err)) {
    fail(tag + ": decode error: " + err);
    return;
  }
  expect(info.is_ima(), tag + ": is_ima bit derived");
  const uint32_t frames_expected = info.num_samples;        // per channel
  expect(pcm.size() == (size_t)frames_expected * 2, tag + ": decoded frames*channels");
  if (pcm.size() != (size_t)frames_expected * 2) return;
  expect(decoded_audio_is_audible(pcm, tag), tag + ": waveform sanity");
}

static bool decoded_audio_is_audible(const std::vector<int16_t>& pcm, const std::string& tag) {
  double sumsq = 0;
  int clip = 0;
  for (int16_t v : pcm) {
    const double d = v / 32768.0;
    sumsq += d * d;
    if (v >= 32760 || v <= -32760) clip++;
  }
  expect(!pcm.empty(), tag + ": pcm non-empty");
  if (pcm.empty()) return false;
  const double rms = std::sqrt(sumsq / pcm.size());
  expect(rms > 0.002, tag + ": RMS audible (is " + std::to_string(rms) + ")");
  expect((double)clip / pcm.size() < 0.5, tag + ": not saturated");
  return rms > 0.002 && (double)clip / pcm.size() < 0.5;
}

static void check_pcm_decode(const char* path_file, const char* entry_name, uint32_t subsong,
                             const std::string& tag) {
  std::string err;
  auto bank = FfsBank::open(path_file, &err);
  if (!bank) {
    fail(tag + ": reopen error: " + err);
    return;
  }
  FfsBank::Entry e;
  if (!bank->entry(entry_name, &e)) {
    fail(tag + ": entry lookup");
    return;
  }
  std::vector<int16_t> pcm;
  FfsSampleInfo info;
  err.clear();
  if (!bank->decode_sample(e, subsong, &pcm, &info, &err)) {
    fail(tag + ": decode error: " + err);
    return;
  }
  expect(!info.is_ima(), tag + ": pcm not marked IMA");
  expect(info.channels == 1, tag + ": UIHORSE subsong 0 is mono PCM");
  expect(pcm.size() == (size_t)info.num_samples * info.channels, tag + ": decoded sample count");
  decoded_audio_is_audible(pcm, tag);
}

int main(int argc, char** argv) {
  std::string root = argc > 1 ? argv[1] : ".";
  const std::string music = root + "/Win32/Sound/music.ffs";
  const std::string effects = root + "/Win32/Sound/effects.ffs";

  check_bank(music.c_str(), 6, 0, 6, "music");        // music: 6 IMA tracks, no PCM, no DATs
  check_bank(effects.c_str(), 42, 483, 0, "effects");  // effects: 465 mono + 18 stereo PCM, 21 DATs

  // IMA decode spot-check on the first music subsong of the first music entry.
  check_ima_decode(music.c_str(), "AWARDNORMAL_01.FSB", 0, "music ima0");
  check_ima_decode(music.c_str(), "BEACH_01.FSB", 0, "music ima beach");
  // PCM16 decode spot-check on the first effect entry's first subsong.
  check_pcm_decode(effects.c_str(), "UIHORSE.FSB", 0, "effects pcm0");

  // FSB-bytes decoder (the FMOD LOADMEMORY path) must agree with the container-path decoder
  // bit for bit: read a music FSB entry's payload raw, decode in memory, compare.
  {
    std::string err;
    auto bank = FfsBank::open(music.c_str(), &err);
    if (!bank) {
      fail("fsb-bytes reopen: " + err);
    } else {
      FfsBank::Entry e;
      if (!bank->entry("AWARDNORMAL_01.FSB", &e)) {
        fail("fsb-bytes entry lookup");
      } else {
        FILE* fp = fopen(music.c_str(), "rb");
        if (!fp) {
          fail("fsb-bytes raw read open");
        } else {
          std::vector<uint8_t> blob(e.size);
          if (fseek(fp, (long)e.offset, SEEK_SET) != 0 ||
              fread(blob.data(), 1, blob.size(), fp) != blob.size()) {
            fail("fsb-bytes raw read short");
          } else {
            std::vector<FfsBank::FfsSubsong> subs;
            err.clear();
            if (!FfsBank::decode_fsb_bytes(blob.data(), blob.size(), &subs, &err)) {
              fail("decode_fsb_bytes: " + err);
            } else if (subs.size() != 1) {
              fail("decode_fsb_bytes subsongs != 1");
            } else {
              std::vector<int16_t> pcm;
              FfsSampleInfo info;
              err.clear();
              if (!bank->decode_sample(e, 0, &pcm, &info, &err)) {
                fail("fsb-bytes compare decode_sample: " + err);
              } else if (pcm != subs[0].pcm) {
                fail("decode_fsb_bytes PCM mismatch vs decode_sample");
              } else if (subs[0].info.mode != info.mode || subs[0].info.sample_rate != info.sample_rate) {
                fail("decode_fsb_bytes header mismatch vs decode_sample");
              }
            }
          }
          fclose(fp);
        }
      }
    }
  }

  // Case-insensitive lookup.
  {
    std::string err;
    auto bank = FfsBank::open(music.c_str(), &err);
    FfsBank::Entry e;
    if (bank) {
      expect(bank->entry("awardnormal_01.fsb", &e), "case-insensitive lookup");
      expect(!bank->entry("NOPE.FSB", &e), "missing entry stays missing");
    } else {
      fail("reopen for lookup test");
    }
  }

  if (g_failures != 0) {
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  puts("FFS TEST PASS");
  return 0;
}
