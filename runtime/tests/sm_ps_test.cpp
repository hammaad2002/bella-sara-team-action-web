// Self-test for the ps_1_1 -> GLSL ES 3.00 pixel shader translator (sm_pixel_glsl).
//
// Hand-assembles D3D shader-model-1 token streams (d3d9types.h field layout) and checks the
// generated GLSL with string contains; each fixture mirrors one of the shipped .fx effects. A
// malformed stream must yield an empty string plus a precise error, never a crash.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "gles/programs.h"

using gles::PsInfo;

namespace {

// ---- Token builders -----------------------------------------------------------------------------
constexpr uint32_t ps11 = 0xffff0101;
constexpr uint32_t psEnd = 0x0000ffff;
constexpr uint32_t kRtemp = 0, kRinput = 1, kRconst = 2, kRtex = 3, kRcolorout = 8, kRdepthout = 9;
// Register type >= 8 arrives through the D3DSPR extended bit (token bit 11).
constexpr uint32_t reg(uint32_t type, uint32_t num) { return (type & 7) << 28 | (type >= 8 ? 1u << 11 : 0) | num; }
constexpr uint32_t dst(uint32_t type, uint32_t num, uint32_t mask = 0xf, uint32_t mod = 0) {
  return reg(type, num) | mask << 16 | mod << 20;
}
constexpr uint32_t src(uint32_t type, uint32_t num, uint32_t sw = 0xe4, uint32_t mod = 0) {
  return reg(type, num) | sw << 16 | mod << 24;
}
constexpr uint32_t kSat = 1;    // destination _sat
constexpr uint32_t kNeg = 1;    // source -b
constexpr uint32_t kBx2 = 4;    // source _bx2 (D3DSPSM_SIGN)
constexpr uint32_t kComp = 6;   // source _comp
constexpr uint32_t swAll = 0xe4;  // .xyzw
constexpr uint32_t swAa = 0xff;   // replicate a

// Opcodes used below.
constexpr uint32_t OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5, OP_RCP = 6, OP_DP3 = 8, OP_MIN = 10,
                   OP_MAX = 11, OP_SLT = 12, OP_SGE = 13, OP_EXP = 14, OP_LOG = 15, OP_LIT = 16, OP_LRP = 18, OP_FRC = 19,
                   OP_TEXCOORD = 64, OP_TEXKILL = 65, OP_TEX = 66, OP_TEXM3x2PAD = 71, OP_TEXM3x2TEX = 72,
                   OP_EXPP = 78, OP_LOGP = 79, OP_DEF = 81, OP_CMP = 88;

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

int fails = 0;

void check(bool ok, const char* what) {
  printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) fails++;
}

std::vector<uint32_t> bloom_final_pass() {  // tex t0; mul r0, t0, v0
  return {ps11, OP_TEX, dst(kRtex, 0, 0), OP_MUL, dst(kRtemp, 0), src(kRtex, 0), src(kRinput, 0), psEnd};
}

std::vector<uint32_t> shore_normalmap() {
  // tex t0, tex t1, texcoord t2, dp3 r0,t1_bx2,t2_bx2, mul r1,r0,r0, mul r0,r1,r1, add r0,t0,r0 (_sat),
  // mov r0.a, c0.a
  return {ps11,
          OP_TEX, dst(kRtex, 0, 0),
          OP_TEX, dst(kRtex, 1, 0),
          OP_TEXCOORD, dst(kRtex, 2, 0),
          OP_DP3, dst(kRtemp, 0), src(kRtex, 1, swAll, kBx2), src(kRtex, 2, swAll, kBx2),
          OP_MUL, dst(kRtemp, 1), src(kRtemp, 0), src(kRtemp, 0),
          OP_MUL, dst(kRtemp, 0), src(kRtemp, 1), src(kRtemp, 1),
          OP_ADD, dst(kRtemp, 0, 0xf, kSat), src(kRtex, 0), src(kRtemp, 0),
          OP_MOV, dst(kRtemp, 0, 0x8), src(kRconst, 0, swAa),
          psEnd};
}

std::vector<uint32_t> horse_anisotropic() {
  // tex t0, texm3x2pad t1, t0_bx2, texm3x2tex t2, t0_bx2, tex t3, mul r0,t2,t3, mul r0,r0,c0,
  // mul r0,r0,v0, mul r0,r0,c1, mul r0,r0,t3.a, mad r0,t3,v0,r0, lrp r1,t3.aaaa,c4,c3, mul r0,r0,r1.a
  return {ps11,
          OP_TEX, dst(kRtex, 0, 0),
          OP_TEXM3x2PAD, dst(kRtex, 1, 0), src(kRtex, 0, swAll, kBx2),
          OP_TEXM3x2TEX, dst(kRtex, 2, 0), src(kRtex, 0, swAll, kBx2),
          OP_TEX, dst(kRtex, 3, 0),
          OP_MUL, dst(kRtemp, 0), src(kRtex, 2), src(kRtex, 3),
          OP_MUL, dst(kRtemp, 0), src(kRtemp, 0), src(kRconst, 0),
          OP_MUL, dst(kRtemp, 0), src(kRtemp, 0), src(kRinput, 0),
          OP_MUL, dst(kRtemp, 0), src(kRtemp, 0), src(kRconst, 1),
          OP_MUL, dst(kRtemp, 0), src(kRtemp, 0), src(kRtex, 3, swAa),
          OP_MAD, dst(kRtemp, 0), src(kRtex, 3), src(kRinput, 0), src(kRtemp, 0),
          OP_LRP, dst(kRtemp, 1), src(kRtex, 3, swAa), src(kRconst, 4), src(kRconst, 3),
          OP_MUL, dst(kRtemp, 0), src(kRtemp, 0), src(kRtemp, 1, swAa),
          psEnd};
}

std::vector<uint32_t> arithmetic_ops() {
  // tex t0; min/max/sub/slt/sge/cmp/exp/log/frc/expp/logp/lit/mov chains over r0/r1 and c0..c3.
  return {ps11,
          OP_TEX, dst(kRtex, 0, 0),
          OP_MIN, dst(kRtemp, 0), src(kRconst, 0), src(kRconst, 1),
          OP_MAX, dst(kRtemp, 0), src(kRconst, 0), src(kRconst, 1),
          OP_SUB, dst(kRtemp, 0), src(kRconst, 0), src(kRconst, 1),
          OP_SLT, dst(kRtemp, 0), src(kRtemp, 1), src(kRconst, 2),
          OP_SGE, dst(kRtemp, 1), src(kRtemp, 0), src(kRconst, 2),
          OP_CMP, dst(kRtemp, 0), src(kRtemp, 1), src(kRconst, 0), src(kRconst, 1),
          OP_EXP, dst(kRtemp, 0), src(kRconst, 3),
          OP_LOG, dst(kRtemp, 0), src(kRconst, 3),
          OP_FRC, dst(kRtemp, 0), src(kRtex, 0),
          OP_MOV, dst(kRtemp, 1), src(kRtemp, 0, swAll, kNeg),
          OP_MOV, dst(kRtemp, 1), src(kRconst, 2, swAll, kComp),
          OP_EXPP, dst(kRtemp, 0), src(kRconst, 3),
          OP_LOGP, dst(kRtemp, 0), src(kRconst, 3),
          OP_LIT, dst(kRtemp, 0), src(kRconst, 3),
          OP_MOV, dst(kRcolorout, 0), src(kRtemp, 0),
          psEnd};
}

std::vector<uint32_t> def_literal() {  // def c0 = ...; mov oC0, c0
  uint32_t fd[] = {0x3f800000, 0x40000000, 0x40400000, 0x40800000};  // 1 2 3 4
  return {ps11, OP_DEF, dst(kRconst, 0, 0), fd[0], fd[1], fd[2], fd[3], OP_MOV, dst(kRcolorout, 0), src(kRconst, 0), psEnd};
}

}  // namespace

int main() {
  // 1. Bloom final pass: tex + mov-style mul colour pass.
  {
    PsInfo info{};
    std::string err;
    std::string g = sm_pixel_glsl(bloom_final_pass(), &info, &err);
    check(!g.empty() && info.ok, "bloom: translated ok");
    check(contains(g, "texture(s0, v_t[0].xy)"), "bloom: samples stage 0 with v_t[0]");
    check(contains(g, "clamp(t0 * v0, -1.0, 1.0)"), "bloom: saturating mul into r0");
    check(contains(g, "o_color = oC0;"), "bloom: writes o_color");
    check(contains(g, "in vec4 v_t[8];") && contains(g, "in vec4 v_c0;"), "bloom: declares varyings");
    check(!contains(g, "uniform vec4 pc[") && info.nconst == 0, "bloom: no constants referenced");
  }

  // 2. Shore normal-map combiner: _bx2 sources, dp3, dest _sat, component writemask.
  {
    PsInfo info{};
    std::string err;
    std::string g = sm_pixel_glsl(shore_normalmap(), &info, &err);
    check(!g.empty() && info.ok, "shore: translated ok");
    check(contains(g, "dot((((t1 - 0.5) * 2.0)).xyz, (((t2 - 0.5) * 2.0)).xyz)"), "shore: dp3 with _bx2 sources");
    check(contains(g, "clamp(t0 + r0, 0.0, 1.0)"), "shore: dest _sat clamps to [0,1]");
    check(contains(g, "r0.w = (clamp(pc[0].wwww, -1.0, 1.0)).w;"), "shore: alpha (w) writemask via swizzle");
    check(contains(g, "uniform vec4 pc[1];") && info.nconst == 1, "shore: one constant vector");
    check(!contains(g, "in vec4 v_c0;"), "shore: diffuse input unused");
    check(contains(g, "t2 = (v_t[2]);"), "shore: texcoord loads coordinates");
  }

  // 3. Horse anisotropic lighting: texm3x2pad/texm3x2tex, mad, lrp.
  {
    PsInfo info{};
    std::string err;
    std::string g = sm_pixel_glsl(horse_anisotropic(), &info, &err);
    check(!g.empty() && info.ok, "horse: translated ok");
    check(contains(g, "vec4(dot(v_t[1].xyz, (((t0 - 0.5) * 2.0)).xyz), 0.0, 0.0, 0.0)"), "horse: texm3x2pad row");
    check(contains(g, "texture(s2, vec2(t1.x, dot(v_t[2].xyz, (((t0 - 0.5) * 2.0)).xyz)))"), "horse: texm3x2tex lookup");
    check(contains(g, "texture(s3, v_t[3].xy)"), "horse: tex t3");
    check(contains(g, "t3 * v0 + r0"), "horse: mad");
    check(contains(g, "mix(pc[3], pc[4], t3.wwww)"), "horse: lrp");
    check(contains(g, "uniform vec4 pc[5];") && info.nconst == 5, "horse: constants c0..c4");
    check(contains(g, "uniform sampler2D s2;") && contains(g, "uniform sampler2D s3;"), "horse: declares stage samplers");
  }

  // 4. Wider arithmetic coverage: min/max/sub/slt/sge/cmp/exp/log/frc/expp/logp/lit.
  {
    PsInfo info{};
    std::string err;
    std::string g = sm_pixel_glsl(arithmetic_ops(), &info, &err);
    check(!g.empty() && info.ok, "arith: translated ok");
    check(contains(g, "min(pc[0], pc[1])"), "arith: min");
    check(contains(g, "max(pc[0], pc[1])"), "arith: max");
    check(contains(g, "pc[0] - pc[1]"), "arith: sub");
    check(contains(g, "vec4(lessThan(r1, pc[2]))"), "arith: slt");
    check(contains(g, "vec4(greaterThanEqual(r0, pc[2]))"), "arith: sge");
    check(contains(g, "mix(pc[1], pc[0], step(0.0, r1))"), "arith: cmp (a >= 0 ? b : c)");
    check(contains(g, "vec4(exp2((pc[3]).x))"), "arith: exp = exp2");
    check(contains(g, "vec4(log2(abs((pc[3]).x)))"), "arith: log = log2 of abs");
    check(contains(g, "fract(t0)"), "arith: frc");
    check(contains(g, "(-r0)"), "arith: negation");
    check(contains(g, "(1.0 - pc[2])"), "arith: _comp");
    check(contains(g, "pow((pc[3]).y, clamp((pc[3]).w, -127.9961, 127.9961))"), "arith: lit");
    check(contains(g, "o_color = oC0;"), "arith: oC0 destination");
    check(contains(g, "uniform vec4 pc[4];") && info.nconst == 4, "arith: constants c0..c3");
  }

  // 5. def literals are inlined and not counted as uniforms.
  {
    PsInfo info{};
    std::string err;
    std::string g = sm_pixel_glsl(def_literal(), &info, &err);
    check(!g.empty() && info.ok, "def: translated ok");
    check(contains(g, "cdef0 = vec4(1, 2, 3, 4)"), "def: literal inlined");
    check(contains(g, "oC0 = (clamp(cdef0, -1.0, 1.0));"), "def: reads cdef0 (saturating)");
    check(info.nconst == 0 && !contains(g, "pc["), "def: no pc uniform needed");
  }

  // 6. Malformed and unsupported streams: empty string + precise error, never a crash.
  {
    auto expect_fail = [](const char* name, const std::vector<uint32_t>& tokens, const char* err_has) {
      PsInfo info{};
      std::string err;
      std::string g = sm_pixel_glsl(tokens, &info, &err);
      check(g.empty() && !info.ok && contains(err, err_has), name);
    };
    expect_fail("bad: not a pixel shader (vertex version token)", {0xfffe0101, psEnd}, "not a pixel shader");
    expect_fail("bad: ps_1_2 unsupported", {0xffff0102, psEnd}, "ps_1_1");
    expect_fail("bad: empty stream", {}, "not a pixel shader");
    expect_fail("bad: truncated instruction", {ps11, OP_MAD}, "truncated");
    expect_fail("bad: unsupported opcode (rcp)", {ps11, OP_RCP, dst(kRtemp, 0), src(kRtemp, 0), psEnd}, "unsupported opcode");
    expect_fail("bad: oDepth write", {ps11, OP_MOV, dst(kRdepthout, 0), src(kRtemp, 0), psEnd}, "oDepth");
    expect_fail("bad: oC1 write", {ps11, OP_MOV, dst(kRcolorout, 1), src(kRtemp, 0), psEnd}, "oC0");
    expect_fail("bad: relative source", {ps11, OP_MOV, dst(kRtemp, 0), src(kRtemp, 0) | 0x2000, psEnd}, "relative addressing");
    expect_fail("bad: unsupported source modifier", {ps11, OP_MOV, dst(kRtemp, 0), src(kRtemp, 0, swAll, 10), psEnd},
                "source modifier");
    expect_fail("bad: constant out of range", {ps11, OP_MOV, dst(kRcolorout, 0), src(kRconst, 8), psEnd}, "exceeds ps_1_1");
    expect_fail("bad: texm3x2tex without pad", {ps11, OP_TEXM3x2TEX, dst(kRtex, 2, 0), src(kRtex, 0), psEnd},
                "texm3x2pad");
  }

  // 7. texkill discards.
  {
    PsInfo info{};
    std::string err;
    std::string g = sm_pixel_glsl({ps11, OP_TEXKILL, dst(kRtex, 0, 0), OP_MOV, dst(kRcolorout, 0), src(kRtex, 0), psEnd}, &info, &err);
    check(!g.empty() && info.ok && contains(g, "discard"), "texkill: discards");
  }

  printf(fails ? "sm_ps_test: %d FAILURES\n" : "sm_ps_test: all passed\n", fails);
  return fails ? 1 : 0;
}
