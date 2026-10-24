// D3D ps_1_1 pixel shader bytecode -> GLSL ES 3.00 (fragment stage counterpart of sm_vs.cpp).
//
// The game's .fx effects embed ps.1.1 assembly compiled by D3DX8 inside the guest, so the token
// stream is the plain shader-model-1 encoding: a 0xFFFF0101 version token, then fixed-length
// instructions (parameter tokens counted per opcode, like the vertex side), comment blocks
// (0xFFFE with payload size), DEF literals and the 0x0000FFFF end marker. Declare-and-main is
// emitted as one snippet: inputs v0/v1 from v_c0/v_c1, coordinates from v_t[0..7], constants from
// uniform array pc[] (the vertex side owns c[]), one sampler2D per sampled stage, colour out to
// o_color. oDepth is not expressible here: TODO(M6).
//
// ps_1_x saturates every arithmetic result when it lands in a register: temps r# and oC0 clamp to
// the signed [-1, 1] printer range (8-bit signed fraction), while the _sat destination modifier
// clamps to [0, 1] instead. tex/texcoord writes are raw values and stay unclamped.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "programs.h"

namespace gles {

namespace {

// D3DSPR register files. The t# registers share the ADDR encoding in ps_1.x (distinguished by the
// instruction family that uses them).
enum Reg { R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_TEXTURE = 3, R_COLOROUT = 8, R_DEPTHOUT = 9 };

// D3DSIO opcodes for ps_1_1 (see the D3DKMDDI shader token documentation). REMOVED/DST/RCP/RSQ and
// the texm3x3 family are not part of this translator: unknown opcodes reject the shader.
enum Op { OP_NOP = 0, OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5, OP_DP3 = 8, OP_DP4 = 9, OP_MIN = 10,
          OP_MAX = 11, OP_SLT = 12, OP_SGE = 13, OP_EXP = 14, OP_LOG = 15, OP_LIT = 16, OP_LRP = 18, OP_FRC = 19,
          OP_TEXCOORD = 64, OP_TEXKILL = 65, OP_TEX = 66, OP_TEXM3x2PAD = 71, OP_TEXM3x2TEX = 72, OP_EXPP = 78, OP_LOGP = 79,
          OP_CND = 80, OP_DEF = 81, OP_CMP = 88, OP_PHASE = 0xfffd, OP_COMMENT = 0xfffe, OP_END = 0xffff };

uint32_t reg_type(uint32_t t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); }
uint32_t reg_num(uint32_t t) { return t & 0x7ff; }

// Number of parameter tokens after the opcode token for shader model 1 (SM2+ encode the length in
// the opcode token; ps_1_1 does not and cannot appear here).
int sm1_params(uint32_t op) {
  switch (op) {
    case OP_NOP: return 0;
    case OP_MOV: case OP_EXP: case OP_LOG: case OP_LIT: case OP_FRC: case OP_EXPP: case OP_LOGP: return 2;
    case OP_TEXCOORD: case OP_TEXKILL: case OP_TEX: return 1;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DP3: case OP_DP4: case OP_MIN: case OP_MAX: case OP_SLT: case OP_SGE:
      return 3;
    case OP_MAD: case OP_LRP: case OP_CND: case OP_CMP: return 4;
    case OP_TEXM3x2PAD: case OP_TEXM3x2TEX: return 2;
    case OP_DEF: return 5;
    default: return -1;
  }
}

struct Translator {
  const std::vector<uint32_t>& t;
  const ProgKey& key;
  std::string body, err;
  std::map<uint32_t, std::string> defs;  // c# -> vec4 literal (ps_1_x constants max out at c0..c7)
  uint32_t max_const = 0;
  bool temps[8] = {};    // r# referenced anywhere
  bool texin[8] = {};    // t# referenced anywhere (varying v_t[0..7] declared when set)
  bool stage[8] = {};    // stages sampled by tex/texm3x2tex (sampler2D s# declared when set)
  bool vin[2] = {};      // v0 diffuse / v1 specular colour inputs read
  bool colorout = false; // oC0 explicitly written
  int m3x2row = -1;      // t# written by the open texm3x2pad, -1 when none

  Translator(const std::vector<uint32_t>& tokens, const ProgKey& k) : t(tokens), key(k) {}

  std::string reg_name(uint32_t tok) {
    uint32_t type = reg_type(tok), n = reg_num(tok);
    switch (type) {
      case R_TEMP:
        if (n > 7) {
          err = "temporary register r" + std::to_string(n) + " out of range";
          return "vec4(0.0)";
        }
        temps[n] = true;
        return "r" + std::to_string(n);
      case R_INPUT:
        if (n > 1) {
          err = "ps_1_1 input register v" + std::to_string(n) + " out of range";
          return "vec4(0.0)";
        }
        vin[n] = true;
        return "v" + std::to_string(n);
      case R_CONST: return const_ref(n);
      case R_TEXTURE:
        if (n > 7) {
          err = "texture register t" + std::to_string(n) + " out of range";
          return "vec4(0.0)";
        }
        texin[n] = true;
        return "t" + std::to_string(n);
      case R_COLOROUT:
        err = "oC" + std::to_string(n) + " read as source (ps_1_1 outputs are writes only)";
        return "vec4(0.0)";
      case R_DEPTHOUT: err = "oDepth output is not supported: TODO(M6)"; return "vec4(0.0)";
      default: err = "unsupported register type " + std::to_string(type); return "vec4(0.0)";
    }
  }

  std::string const_ref(uint32_t n) {
    auto it = defs.find(n);
    if (it != defs.end()) return "cdef" + std::to_string(n);
    if (n > 7) {
      err = "constant c" + std::to_string(n) + " exceeds ps_1_1's c0..c7";
      return "vec4(0.0)";
    }
    max_const = std::max(max_const, n + 1);
    return "pc[" + std::to_string(n) + "]";
  }

  // Source operand with swizzle and modifier, as a vec4 expression.
  std::string src(size_t& i) {
    if (i >= t.size()) {
      err = "truncated token stream (source operand)";
      return "vec4(0.0)";
    }
    uint32_t tok = t[i++];
    if ((tok >> 13) & 1) err = "relative addressing is not supported in ps_1_1";
    uint32_t sw = (tok >> 16) & 0xff;
    std::string r = reg_name(tok);
    if (sw != 0xe4) {
      static const char comp[4] = {'x', 'y', 'z', 'w'};
      r += ".";
      for (int k = 0; k < 4; k++) r += comp[(sw >> (2 * k)) & 3];
    }
    switch ((tok >> 24) & 0xf) {
      case 0: return r;                              // none
      case 1: return "(-" + r + ")";                 // -b
      case 2: return "(" + r + " - 0.5)";            // _bias
      case 3: return "(-(" + r + " - 0.5))";         // -_bias
      case 4: return "((" + r + " - 0.5) * 2.0)";    // _bx2
      case 5: return "(-((" + r + " - 0.5) * 2.0))"; // -_bx2
      case 6: return "(1.0 - " + r + ")";            // _comp
      case 7: return "(" + r + " * 2.0)";            // _x2
      case 8: return "(-(" + r + " * 2.0))";         // -_x2
      case 9: return "(" + r + " * 0.5)";            // _d2
      case 10: return "(" + r + " * 4.0)";           // _x4
      default: err = "unsupported source modifier"; return r;
    }
  }

  struct Dst {
    std::string name, mask;
    uint32_t type = 0, index = 0, mbits = 0xf;
    bool sat = false;
    const char* scale = nullptr;  // instruction modifier _x2/_x4/_x8/_d2/_d4/_d8
  };

  Dst dst(size_t& i) {
    Dst d;
    if (i >= t.size()) {
      err = "truncated token stream (destination operand)";
      return d;
    }
    uint32_t tok = t[i++];
    if ((tok >> 13) & 1) err = "relative destination is not supported in ps_1_1";
    d.type = reg_type(tok), d.index = reg_num(tok);
    switch (d.type) {
      case R_TEMP:
        if (d.index > 7) {
          err = "temporary register r" + std::to_string(d.index) + " out of range";
          return d;
        }
        temps[d.index] = true;
        d.name = "r" + std::to_string(d.index);
        break;
      case R_COLOROUT:
        if (d.index != 0) {
          err = "ps_1_1 supports only the oC0 output";
          return d;
        }
        d.name = "oC0";
        colorout = true;
        break;
      case R_TEXTURE:
        if (d.index > 7) {
          err = "texture register t" + std::to_string(d.index) + " out of range";
          return d;
        }
        texin[d.index] = true;
        d.name = "t" + std::to_string(d.index);
        break;
      case R_DEPTHOUT: err = "oDepth output is not supported: TODO(M6)"; return d;
      default: err = "unsupported destination register type " + std::to_string(d.type); return d;
    }
    d.mbits = (tok >> 16) & 0xf;
    if (d.mbits == 0) d.mbits = 0xf;  // tex-family dests carry no writemask; the write covers all
    if (d.mbits != 0xf) {
      d.mask = ".";
      for (int k = 0; k < 4; k++)
        if (d.mbits & (1u << k)) d.mask += "xyzw"[k];
    }
    uint32_t mod = (tok >> 20) & 0xf;
    if ((mod & ~1u) != 0) err = "unsupported destination modifier bits " + std::to_string(mod);
    d.sat = (mod & 1) != 0;
    switch ((tok >> 24) & 0xf) {  // D3DSP_DSTSHIFT: signed 4-bit power of two
      case 0: break;
      case 1: d.scale = "2.0"; break;
      case 2: d.scale = "4.0"; break;
      case 3: d.scale = "8.0"; break;
      case 15: d.scale = "0.5"; break;
      case 14: d.scale = "0.25"; break;
      case 13: d.scale = "0.125"; break;
      default: err = "unsupported destination shift " + std::to_string((tok >> 24) & 0xf); break;
    }
    return d;
  }

  // Write `expr` through d's writemask. ps_1_x saturates arithmetic to the [-1, 1] temp range
  // automatically; the _sat destination modifier clamps to [0, 1]; raw writes (tex family) skip
  // saturation entirely.
  void assign(const Dst& d, const std::string& expr0, int saturate = 1) {
    if (!err.empty()) return;
    // The shift scales the result before the register's clamp.
    std::string expr = d.scale ? "((" + expr0 + ") * " + d.scale + ")" : expr0;
    std::string e = saturate > 0 ? "clamp(" + expr + ", -1.0, 1.0)" : expr;
    if (d.sat) e = "clamp(" + expr + ", 0.0, 1.0)";
    body += "  " + d.name + d.mask + " = (" + e + ")" + d.mask + ";\n";
  }

  bool run(PsInfo* info) {
    if (t.empty() || (t[0] & 0xffff0000) != 0xffff0000 || (t[0] & 0xffff) == 0xffff) {
      err = "not a pixel shader";
      return false;
    }
    uint32_t major = (t[0] >> 8) & 0xff, minor = t[0] & 0xff;
    if (major != 1 || minor > 1) {
      err = "only ps_1_0/ps_1_1 is supported, got ps." + std::to_string(major) + "." + std::to_string(minor);
      return false;
    }
    size_t i = 1;
    while (i < t.size() && err.empty()) {
      uint32_t tok = t[i++];
      uint32_t op = tok & 0xffff;
      if (op == OP_END) break;
      if (op == OP_COMMENT) {
        i += (tok >> 16) & 0x7fff;  // comment payload words
        continue;
      }
      if (op == OP_PHASE) {
        err = "phase markers only exist in ps_1_4+";
        break;
      }
      if ((tok >> 16) & 0xbfff) {
        char hex[12];
        snprintf(hex, sizeof hex, "0x%x", tok >> 16);
        err = std::string("unexpected opcode flag bits ") + hex;
        break;
      }
      int n = sm1_params(op);
      if (n < 0) {
        err = "unsupported opcode " + std::to_string(op);
        break;
      }
      if (t.size() < i + (size_t)n) {
        err = "truncated ps_1_1 token stream at opcode " + std::to_string(op);
        break;
      }
      size_t next = i + (size_t)n;
      switch (op) {
        case OP_NOP: break;
        case OP_DEF: {
          if (reg_type(t[i]) != R_CONST) {
            err = "def targets a non-constant register";
            break;
          }
          uint32_t r = reg_num(t[i]);
          float f[4];
          memcpy(f, &t[i + 1], 16);
          char buf[160];
          snprintf(buf, sizeof buf, "vec4(%.9g, %.9g, %.9g, %.9g)", f[0], f[1], f[2], f[3]);
          defs[r] = buf;
          break;
        }
        case OP_MOV: { Dst d = dst(i); assign(d, src(i)); break; }
        case OP_ADD: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, a + " + " + b); break; }
        case OP_SUB: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, a + " - " + b); break; }
        case OP_MUL: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, a + " * " + b); break; }
        case OP_MAD: {
          Dst d = dst(i);
          std::string a = src(i), b = src(i), c = src(i);
          assign(d, a + " * " + b + " + " + c);
          break;
        }
        case OP_DP3: {
          Dst d = dst(i);
          std::string a = src(i), b = src(i);
          assign(d, "vec4(dot((" + a + ").xyz, (" + b + ").xyz))");  // replicated to all components
          break;
        }
        case OP_DP4: {
          Dst d = dst(i);
          std::string a = src(i), b = src(i);
          assign(d, "vec4(dot(" + a + ", " + b + "))");
          break;
        }
        case OP_MIN: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, "min(" + a + ", " + b + ")"); break; }
        case OP_MAX: { Dst d = dst(i); std::string a = src(i), b = src(i); assign(d, "max(" + a + ", " + b + ")"); break; }
        case OP_SLT: {
          Dst d = dst(i);
          std::string a = src(i), b = src(i);
          assign(d, "vec4(lessThan(" + a + ", " + b + "))");
          break;
        }
        case OP_SGE: {
          Dst d = dst(i);
          std::string a = src(i), b = src(i);
          assign(d, "vec4(greaterThanEqual(" + a + ", " + b + "))");
          break;
        }
        case OP_CND: {  // ps_1.x: a > 0.5 ? b : c
          Dst d = dst(i);
          std::string a = src(i), b = src(i), c = src(i);
          assign(d, "mix(" + c + ", " + b + ", step(0.5, " + a + "))");
          break;
        }
        case OP_CMP: {  // a >= 0 ? b : c
          Dst d = dst(i);
          std::string a = src(i), b = src(i), c = src(i);
          assign(d, "mix(" + c + ", " + b + ", step(0.0, " + a + "))");
          break;
        }
        case OP_LRP: {
          Dst d = dst(i);
          std::string a = src(i), b = src(i), c = src(i);
          assign(d, "mix(" + c + ", " + b + ", " + a + ")");
          break;
        }
        case OP_EXP: { Dst d = dst(i); assign(d, "vec4(exp2((" + src(i) + ").x))"); break; }
        case OP_LOG: { Dst d = dst(i); assign(d, "vec4(log2(abs((" + src(i) + ").x)))"); break; }
        case OP_EXPP: {
          Dst d = dst(i);
          std::string a = src(i);
          assign(d, "vec4(exp2(floor((" + a + ").x)), fract((" + a + ").x), exp2((" + a + ").x), 1.0)");
          break;
        }
        case OP_LOGP: { Dst d = dst(i); assign(d, "vec4(log2(abs((" + src(i) + ").x)))"); break; }
        case OP_LIT: {
          Dst d = dst(i);
          std::string a = src(i);
          assign(d, "vec4(1.0, max((" + a + ").x, 0.0), ((" + a + ").x > 0.0 && (" + a + ").y > 0.0) ? pow((" + a + ").y, clamp((" + a +
                        ").w, -127.9961, 127.9961)) : 0.0, 1.0)");
          break;
        }
        case OP_FRC: { Dst d = dst(i); assign(d, "fract(" + src(i) + ")"); break; }
        case OP_TEXCOORD: {
          Dst d = dst(i);
          if (d.type != R_TEXTURE) {
            err = "texcoord must write a texture register";
            break;
          }
          assign(d, "v_t[" + std::to_string(d.index) + "]", 0);
          break;
        }
        case OP_TEX: {
          Dst d = dst(i);
          if (d.type != R_TEXTURE) {
            err = "tex must write a texture register";
            break;
          }
          stage[d.index] = true;
          assign(d, "texture(s" + std::to_string(d.index) + ", v_t[" + std::to_string(d.index) + "].xy)", 0);
          break;
        }
        case OP_TEXKILL: {
          Dst d = dst(i);
          if (d.type != R_TEXTURE) {
            err = "texkill must name a texture register";
            break;
          }
          body += "  if (any(lessThan(v_t[" + std::to_string(d.index) + "], vec4(0.0)))) discard;\n";
          break;
        }
        case OP_TEXM3x2PAD: {  // row 1: t#_xyz = dp3(coord row, src)
          Dst d = dst(i);
          std::string a = src(i);
          if (d.type != R_TEXTURE) {
            err = "texm3x2pad must write a texture register";
            break;
          }
          assign(d, "vec4(dot(v_t[" + std::to_string(d.index) + "].xyz, (" + a + ").xyz), 0.0, 0.0, 0.0)", 0);
          m3x2row = (int)d.index;
          break;
        }
        case OP_TEXM3x2TEX: {  // row 2: lookup with both rows as coordinates
          Dst d = dst(i);
          std::string a = src(i);
          if (d.type != R_TEXTURE) {
            err = "texm3x2tex must write a texture register";
            break;
          }
          if (m3x2row < 0) {
            err = "texm3x2tex without a preceding texm3x2pad";
            break;
          }
          assign(d, "texture(s" + std::to_string(d.index) + ", vec2(t" + std::to_string(m3x2row) +
                        ".x, dot(v_t[" + std::to_string(d.index) + "].xyz, (" + a + ").xyz)))",
                 0);
          stage[d.index] = true;
          m3x2row = -1;
          break;
        }
        default: err = "unsupported opcode " + std::to_string(op); break;
      }
      i = next;
    }
    if (err.empty() && m3x2row >= 0) err = "texm3x2pad without a following texm3x2tex";
    if (!err.empty()) {
      info->ok = false;
      return false;
    }
    info->nconst = max_const;
    info->ok = true;
    return true;
  }

  std::string glsl(const PsInfo& info) {
    std::string s = "#version 300 es\nprecision highp float;\nprecision highp int;\n";
    for (int k = 0; k < 8; k++)
      if (texin[k]) {
        s += "in vec4 v_t[8];\n";  // matches the vertex stage's output array
        break;
      }
    if (vin[0]) s += "in vec4 v_c0;\n";
    if (vin[1]) s += "in vec4 v_c1;\n";
    if (info.nconst) s += "uniform vec4 pc[" + std::to_string(info.nconst) + "];\n";
    for (int k = 0; k < 8; k++)
      if (stage[k]) s += "uniform sampler2D s" + std::to_string(k) + ";\n";
    if (key.alpha_func) s += "uniform float u_aref;\n";
    s += "out vec4 o_color;\n";
    s += "void main() {\n";
    for (auto& [n, lit] : defs) s += "  const vec4 cdef" + std::to_string(n) + " = " + lit + ";\n";
    if (vin[0]) s += "  vec4 v0 = v_c0;\n";
    if (vin[1]) s += "  vec4 v1 = v_c1;\n";
    for (int k = 0; k < 8; k++) {
      if (texin[k]) s += "  vec4 t" + std::to_string(k) + " = vec4(0.0);\n";
      if (temps[k]) s += "  vec4 r" + std::to_string(k) + " = vec4(0.0);\n";
    }
    s += "  vec4 oC0 = vec4(0.0);\n";
    s += body;
    std::string out_col = colorout ? "oC0" : temps[0] ? "r0" : "oC0";
    if (key.alpha_func) {
      static const char* cmp[9] = {"", "false", "a < u_aref", "a == u_aref", "a <= u_aref", "a > u_aref", "a != u_aref", "a >= u_aref", "true"};
      s += "  float a = floor(" + out_col + ".a * 255.0 + 0.5);\n";
      s += "  if (!(" + std::string(cmp[key.alpha_func < 9 ? key.alpha_func : 8]) + ")) discard;\n";
    }
    s += "  o_color = " + out_col + ";\n}\n";
    return s;
  }
};

}  // namespace

std::string sm_pixel_glsl(const std::vector<uint32_t>& tokens, const ProgKey& k, PsInfo* info, std::string* err) {
  info->ok = false;
  Translator tr(tokens, k);
  if (!tr.run(info)) {
    *err = tr.err;
    return "";
  }
  return tr.glsl(*info);
}

std::string sm_pixel_glsl(const std::vector<uint32_t>& tokens, PsInfo* info, std::string* err) {
  ProgKey k{};
  return sm_pixel_glsl(tokens, k, info, err);
}

}  // namespace gles
