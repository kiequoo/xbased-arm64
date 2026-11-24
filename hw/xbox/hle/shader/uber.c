/*
 * xbased HLE GPU — interpreter shaders (uber.h). Pure.
 *
 * The vertex stage runs the program's steps (hle_uber_prog: each parsed on the
 * CPU once into words) on the same register files as vsh.c's text names them
 * (R0-R11, R12 = oPos, v0-v15, c[192], the outputs, A0), with the same
 * operations in plain floats (vsh.c's k_lib), the MAC and the ILU operation of
 * a step both read before either writes; then vsh.c's tail with the key's
 * switches read from the config. The pixel stage samples the four stages by
 * their mode and runs the eight combiners and the final one from the key's
 * words, as psh.c writes them out: the same arithmetic as their own shaders.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "hw/xbox/hle/shader/ff.h"
#include "hw/xbox/hle/shader/ffhw.h"
#include "hw/xbox/hle/shader/uber.h"
#include "subprojects/nv2a_vsh_cpu/src/nv2a_vsh_disassembler.h"

typedef struct Buf {
    char *p;
    size_t cap, len;
    bool overflow;
} Buf;

static void put(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->cap > b->len ? b->cap - b->len : 0, fmt, ap);
    va_end(ap);
    if (n < 0 || b->len + (size_t)n >= b->cap) {
        b->overflow = true;
        b->len = b->cap;
        return;
    }
    b->len += (size_t)n;
}

/* ---- the program's steps ---------------------------------------------------------
 * A step: word 0 the MAC opcode (bits 0-4), the ILU's (8-12); 1-3 the MAC's
 * inputs, 4 its two outputs (low and high half), 6-8 the ILU's inputs, 9 its
 * outputs. An input: register file (0-2: Nv2aVshRegisterType), index (3-10),
 * swizzle (11-18, 2 bits a component), negated (19), relative to A0 (20). An
 * output: file (0-2), index (3-10), write mask x y z w = bits 14 13 12 11. */
static uint32_t in_word(const Nv2aVshInput *in)
{
    uint32_t sw = 0;
    for (unsigned i = 0; i < 4; i++) {
        sw |= (uint32_t)(in->swizzle[i] & 3u) << (2 * i);
    }
    return ((uint32_t)in->type & 7u) | (in->index & 255u) << 3 | sw << 11 | (in->is_negated ? 1u << 19 : 0u) |
           (in->is_relative ? 1u << 20 : 0u);
}

static uint32_t out_word(const Nv2aVshOutput *o)
{
    return ((uint32_t)o->type & 7u) | (o->index & 255u) << 3 | ((uint32_t)o->writemask & 15u) << 11;
}

unsigned hle_uber_prog(const uint32_t *tokens, unsigned nslots, uint32_t *cfg)
{
    unsigned n = 0;
    for (unsigned s = 0; s < nslots && s < HLE_VSH_MAX_SLOTS; s++) {
        Nv2aVshStep st;
        memset(&st, 0, sizeof(st));
        if (nv2a_vsh_parse_step(&st, tokens + 4 * s) != NV2AVPR_SUCCESS) {
            return 0;
        }
        uint32_t *w = cfg + HLE_UBER_STEPS_AT + HLE_UBER_STEP_WORDS * n;
        memset(w, 0, HLE_UBER_STEP_WORDS * 4);
        w[0] = ((uint32_t)st.mac.opcode & 31u) | ((uint32_t)st.ilu.opcode & 31u) << 8;
        for (unsigned i = 0; i < 3; i++) {
            w[1 + i] = in_word(&st.mac.inputs[i]);
            w[6 + i] = in_word(&st.ilu.inputs[i]);
        }
        w[4] = out_word(&st.mac.outputs[0]) | out_word(&st.mac.outputs[1]) << 16;
        w[9] = out_word(&st.ilu.outputs[0]) | out_word(&st.ilu.outputs[1]) << 16;
        n++;
        if (st.is_final) {
            return n;
        }
    }
    return 0;
}

void hle_uber_keys(const HleVshKey *vk, const HlePshKey *pk, uint32_t nsteps, uint32_t *cfg)
{
    memset(cfg, 0, HLE_UBER_STEPS_AT * 4);
    cfg[0] = vk->in_mask;
    cfg[1] = (uint32_t)vk->int_mask | (uint32_t)vk->cmp_mask << 16;
    cfg[2] = (uint32_t)vk->w1_mask;
    cfg[3] = 0;
    cfg[4] = (vk->flat ? HLE_UBER_V_FLAT : 0u) | (vk->spec_off ? HLE_UBER_V_SPECOFF : 0u) |
             (vk->wbuf ? HLE_UBER_V_WBUF : 0u) | (vk->zfree ? HLE_UBER_V_ZFREE : 0u) |
             (uint32_t)vk->fog_table << 8;
    cfg[5] = nsteps;
    if (pk) {
        for (unsigned s = 0; s < 8; s++) {
            cfg[8 + s] = pk->rgb_in[s];
            cfg[16 + s] = pk->rgb_out[s];
            cfg[24 + s] = pk->alpha_in[s];
            cfg[32 + s] = pk->alpha_out[s];
        }
        cfg[40] = pk->final_abcd;
        cfg[41] = pk->final_efg;
        cfg[42] = pk->count;
        cfg[43] = pk->tex_modes;
        cfg[44] = pk->dot_map;
        cfg[45] = pk->input_tex;
        cfg[46] = pk->compare;
        for (unsigned s = 0; s < 4; s++) {
            cfg[47 + s] = pk->ckey[s];
        }
        cfg[51] = (uint32_t)pk->alpha_test | (uint32_t)pk->alpha_func << 1 | (uint32_t)pk->fog << 4 |
                  (uint32_t)pk->fog_abs << 8 | (uint32_t)pk->out_bgra << 9 | (uint32_t)pk->specular << 10 |
                  (uint32_t)pk->flat << 11 | (uint32_t)pk->alpha_kill << 12 | (uint32_t)pk->vol << 20;
        cfg[52] = (uint32_t)pk->shadow | (uint32_t)pk->shadow_fmt << 8;
        cfg[53] = pk->dither;           /* (psh.h HlePshKey.dither) */
    }
}

/* ---- the vertex stage ------------------------------------------------------------ */

static const char k_vs_head[] =
    "layout(set = 1, binding = 0, std140) uniform VshConst { vec4 c[192]; vec4 vcur[16]; };\n"
    "layout(set = %u, binding = 0, std140) uniform UberCfg { uvec4 u[%u]; };\n"
    "invariant gl_Position;\n"
    "layout(location = 0) in vec4 vin[16];\n"
    "layout(location = 0) out vec4 vDiffuse;\n"
    "layout(location = 1) out vec4 vSpecular;\n"
    "layout(location = 2) out vec4 vTex0;\n"
    "layout(location = 3) out vec4 vTex1;\n"
    "layout(location = 4) out vec4 vTex2;\n"
    "layout(location = 5) out vec4 vTex3;\n"
    "layout(location = 6) out float vFog;\n"
    "layout(location = 7) out vec4 vBack0;\n"
    "layout(location = 8) out vec4 vBack1;\n"
    "layout(location = 9) out float vW;\n"
    /* the colours again, flat: the pixel stage takes these under SHADEMODE FLAT */
    "layout(location = 18) flat out vec4 vDiffuseF;\n"
    "layout(location = 19) flat out vec4 vSpecularF;\n"
    "layout(location = 20) flat out vec4 vBack0F;\n"
    "layout(location = 21) flat out vec4 vBack1F;\n"
    "uint cw(uint i) { return u[i >> 2][i & 3u]; }\n"
    /* vsh.c's k_lib: the operations as the stand-ins do them */
    "vec4 mulz(vec4 a, vec4 b) {\n"
    "  vec4 r = a * b;\n"
    "  vec4 z = vec4(equal(a, vec4(0.0))) + vec4(equal(b, vec4(0.0)));\n"
    "  return mix(r, vec4(0.0), greaterThan(z, vec4(0.0)));\n"
    "}\n"
    "vec4 dst4(vec4 a, vec4 b) { return vec4(1.0, a.y * b.y, a.z, b.w); }\n"
    "vec4 rcp4(float x) { return vec4(x == 0.0 ? uintBitsToFloat(0x7F800000u) : 1.0 / x); }\n"
    "vec4 rcc4(float x) {\n"
    "  float r = 1.0 / x;\n"
    "  float m = clamp(abs(r), uintBitsToFloat(0x1F800000u), uintBitsToFloat(0x5F800000u));\n"
    "  return vec4((r < 0.0 || (r == 0.0 && floatBitsToUint(r) != 0u)) ? -m : m);\n"
    "}\n"
    "vec4 rsq4(float x) {\n"
    "  float a = abs(x);\n"
    "  return vec4(a == 0.0 ? uintBitsToFloat(0x7F800000u) : inversesqrt(a));\n"
    "}\n"
    "vec4 exp4(float x) {\n"
    "  float f = floor(x);\n"
    "  return vec4(exp2(f), x - f, exp2(x), 1.0);\n"
    "}\n"
    "vec4 log4(float x) {\n"
    "  float a = abs(x);\n"
    "  if (a == 0.0) return vec4(-uintBitsToFloat(0x7F800000u), 1.0,\n"
    "                            -uintBitsToFloat(0x7F800000u), 1.0);\n"
    "  float e = floor(log2(a));\n"
    "  return vec4(e, a / exp2(e), log2(a), 1.0);\n"
    "}\n"
    "vec4 lit4(vec4 s) {\n"
    "  float x = max(s.x, 0.0), y = max(s.y, 0.0);\n"
    "  float w = clamp(s.w, -127.9961, 127.9961);\n"
    "  return vec4(1.0, x, x > 0.0 ? exp2(w * log2(y)) : 0.0, 1.0);\n"
    "}\n"
    "vec4 R[12];\n"
    "vec4 O[16];\n"
    "vec4 V[16];\n"
    "int A0;\n"
    "bool fogw;\n"
    /* an input: its file, index, A0 relative, swizzle, sign (uber.c in_word) */
    "vec4 rd(uint w) {\n"
    "  uint ty = w & 7u, ix = (w >> 3) & 255u;\n"
    "  vec4 v = vec4(0.0);\n"
    "  if (ty == 1u) v = ix == 12u ? O[0] : R[min(ix, 11u)];\n"
    "  else if (ty == 2u) v = V[ix & 15u];\n"
    "  else if (ty == 4u) v = c[((w >> 20) & 1u) != 0u ? clamp(A0 + int(ix), 0, 191) : int(ix %% 192u)];\n"
    "  uint sw = (w >> 11) & 255u;\n"
    "  v = vec4(v[sw & 3u], v[(sw >> 2) & 3u], v[(sw >> 4) & 3u], v[(sw >> 6) & 3u]);\n"
    "  return ((w >> 19) & 1u) != 0u ? -v : v;\n"
    "}\n"
    /* Nv2aVshOpcode */
    "vec4 op(uint o, vec4 a, vec4 b, vec4 c3) {\n"
    "  switch (o) {\n"
    "  case 1u: return a;\n"
    "  case 2u: return mulz(a, b);\n"
    "  case 3u: return a + b;\n"
    "  case 4u: return mulz(a, b) + c3;\n"
    "  case 5u: return vec4(dot(a.xyz, b.xyz));\n"
    "  case 6u: return vec4(dot(a.xyz, b.xyz) + b.w);\n"
    "  case 7u: return vec4(dot(a, b));\n"
    "  case 8u: return dst4(a, b);\n"
    "  case 9u: return min(a, b);\n"
    "  case 10u: return max(a, b);\n"
    "  case 11u: return vec4(lessThan(a, b));\n"
    "  case 12u: return vec4(greaterThanEqual(a, b));\n"
    "  case 13u: return a;\n"
    "  case 14u: return rcp4(a.x);\n"
    "  case 15u: return rcc4(a.x);\n"
    "  case 16u: return rsq4(a.x);\n"
    "  case 17u: return exp4(a.x);\n"
    "  case 18u: return log4(a.x);\n"
    "  case 19u: return lit4(a);\n"
    "  }\n"
    "  return vec4(0.0);\n"
    "}\n"
    /* an output (vsh.c out_write): R12 is oPos; oFog takes the most significant masked component;
     * A0 the floor of x a little over (an index computed as a whole number lands on it) */
    "void wr(uint w, vec4 v) {\n"
    "  uint ty = w & 7u, ix = (w >> 3) & 255u, m = (w >> 11) & 15u;\n"
    "  bvec4 mk = bvec4((m & 8u) != 0u, (m & 4u) != 0u, (m & 2u) != 0u, (m & 1u) != 0u);\n"
    "  if (ty == 1u && m != 0u) {\n"
    "    if (ix == 12u) O[0] = mix(O[0], v, mk); else { uint r = ix %% 12u; R[r] = mix(R[r], v, mk); }\n"
    "  } else if (ty == 3u && m != 0u) {\n"
    "    if ((ix & 15u) == 5u) { O[5].x = (m & 1u) != 0u ? v.w : (m & 2u) != 0u ? v.z : (m & 4u) != 0u ? v.y : v.x;\n"
    "      fogw = true; } else { uint r = ix & 15u; O[r] = mix(O[r], v, mk); }\n"
    "  } else if (ty == 5u) {\n"
    "    A0 = int(floor(v.x + 1.0 / 1024.0));\n"
    "  }\n"
    "}\n";

static const char k_vs_main[] =
    "void main() {\n"
    "  uint inm = cw(0u), cmpm = cw(1u) >> 16, w1m = cw(2u) & 0xFFFFu;\n"
    "  uint fl = cw(4u);\n"
    "  for (int i = 0; i < 16; i++) {\n"
    "    uint bit = 1u << uint(i);\n"
    "    vec4 a = vin[i];\n"
    "    if ((inm & bit) == 0u) {\n"
    "      V[i] = vcur[i];\n"            /* not in the arrays: the register's current value */
    "    } else if ((cmpm & bit) != 0u) {\n"
    "      int p = int(a.x) | (int(a.y) << 16);\n"     /* packed 11:11:10 as two 16-bit halves */
    "      V[i] = vec4(vec3(bitfieldExtract(p, 0, 11), bitfieldExtract(p, 11, 11), bitfieldExtract(p, 22, 10)) /\n"
    "                  vec3(1023.0, 1023.0, 511.0), 1.0);\n"
    "    } else {\n"
    "      V[i] = (w1m & bit) != 0u ? vec4(a.xyz, 1.0) : a;\n"
    "    }\n"
    "    if ((inm & (inm >> 16) & bit) != 0u) V[i] = vec4(V[i].xy, 0.0, V[i].z);\n"   /* "3W" (vsh.h) */
    "  }\n"
    "  for (int i = 0; i < 12; i++) R[i] = vec4(0.0);\n"
    "  for (int i = 0; i < 16; i++) O[i] = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "  A0 = 0;\n"
    "  fogw = false;\n";

static const char k_vs_prog[] =
    "  uint ns = min(cw(5u), %uu);\n"
    "  for (uint s = 0u; s < ns; s++) {\n"
    "    uint b = %uu + s * %uu, w0 = cw(b), mo = w0 & 31u, io = (w0 >> 8) & 31u;\n"
    "    vec4 m = vec4(0.0), il = vec4(0.0);\n"
    "    if (mo != 0u) m = op(mo, rd(cw(b + 1u)), rd(cw(b + 2u)), rd(cw(b + 3u)));\n"
    "    if (io != 0u) il = op(io, rd(cw(b + 6u)), rd(cw(b + 7u)), rd(cw(b + 8u)));\n"
    "    if (mo != 0u) { uint o = cw(b + 4u); wr(o & 0xFFFFu, m); wr(o >> 16, m); }\n"
    "    if (io != 0u) { uint o = cw(b + 9u); wr(o & 0xFFFFu, il); wr(o >> 16, il); }\n"
    "  }\n";

static const char k_vs_tail[] =
    "  vec4 oPos = O[0], oD0 = O[3], oD1 = O[4], oFog = O[5], oPts = O[6], oB0 = O[7], oB1 = O[8];\n"
    "  bool so = (fl & %uu) != 0u;\n"
    "  vDiffuse = clamp(oD0, 0.0, 1.0);\n"
    "  vSpecular = so ? vec4(0.0, 0.0, 0.0, 1.0) : clamp(oD1, 0.0, 1.0);\n"
    "  vBack0 = clamp(oB0, 0.0, 1.0);\n"
    "  vBack1 = so ? vec4(0.0, 0.0, 0.0, 1.0) : clamp(oB1, 0.0, 1.0);\n"
    "  vDiffuseF = vDiffuse; vSpecularF = vSpecular; vBack0F = vBack0; vBack1F = vBack1;\n"
    "  vTex0 = O[9]; vTex1 = O[10]; vTex2 = O[11]; vTex3 = O[12];\n"
    "  uint ft = (fl >> 8) & 255u;\n"
    "  vFog = fogw ? oFog.x : ft != 0u ? 0.0 : 1.0;\n"
    "  gl_PointSize = max(oPts.x, 1.0) * float(max((pc.flags >> 16) & 255u, 1u));\n"
    "  float w = oPos.w;\n"
    "  if (w == 0.0) w = 1.0;\n"
    "  if (isinf(w)) w = w > 0.0 ? uintBitsToFloat(0x5F800000u) : uintBitsToFloat(0xDF800000u);\n"
    "  vec2 scr = oPos.xy;\n"
    "%s"
    "  vec2 ndc = scr * pc.vp + vec2(-1.0, 1.0);\n"
    "%s"
    "  gl_Position = vec4(ndc * w, (fl & %uu) != 0u ? 0.0 : zN * w, w);\n"
    "%s"
    "  vW = w;\n"
    "}\n";


/* the outputs (vsh.c tail) */
static void vs_tail(Buf *b)
{
    put(b, k_vs_tail, HLE_UBER_V_SPECOFF, HLE_SUBPIXEL_GLSL("scr") HLE_TEXEL_EDGE_GLSL("scr"),
        HLE_ZNORM_GLSL("oPos.z", "zN"), HLE_UBER_V_WBUF | HLE_UBER_V_ZFREE, g_hle_glsl_zw ? HLE_ZW_NUDGE_GLSL : "");
}

size_t hle_uber_vs_glsl(char *buf, size_t cap, unsigned set)
{
    Buf b = { buf, cap, 0, false };
    put(&b, "#version 450\n%s", hle_push_glsl());
    put(&b, k_vs_head, set, (unsigned)(HLE_UBER_WORDS / 4));
    put(&b, "%s", k_vs_main);
    put(&b, k_vs_prog, (unsigned)HLE_VSH_MAX_SLOTS, (unsigned)HLE_UBER_STEPS_AT, (unsigned)HLE_UBER_STEP_WORDS);
    vs_tail(&b);
    return b.overflow ? 0 : b.len;
}

/* ---- the fixed function's vertex stage (ffhw.h) -------------------------------------
 * ffhw.c's arithmetic, as its text has it, from the key in words 64 - 75 (HleFfhwKey's
 * bytes): skinning, the normal, texture coordinate generation and matrices, the
 * lights (one side, D3D's spot, the specular power through pow), the colours, the
 * fog coordinate, the point size, the screen position. */
static const char k_ff_lib[] =
    "const int CC = %d, CMV = %d, CIMV = %d, CTM = %d, CTG = %d, CVP = %d, CFP = %d, CEYE = %d;\n"
    "const int CAMB = %d, CEMI = %d, CPAR = %d, CPT = %d, CL = %d, LV = %d;\n"
    "const int LAMB = %d, LDIF = %d, LSPC = %d, LRNG = %d, LHALF = %d, LDIR = %d, LSPOT = %d, LPOS = %d, LATT = %d;\n"
    "uint kb(uint i) { return (cw(%uu + (i >> 2)) >> (8u * (i & 3u))) & 255u; }\n"
    "vec4 XF(vec4 v, int b) { return vec4(dot(v, c[b]), dot(v, c[b + 1]), dot(v, c[b + 2]), dot(v, c[b + 3])); }\n"
    /* ffhw.c src_rgb: the material's (the register holds it), the diffuse or the specular register */
    "vec3 srgb(uint s) { return s == 1u ? V[3].rgb : s == 2u ? V[4].rgb : vec3(1.0); }\n";

static const char k_ff_body[] =
    "  vec4 pos = V[0], n4 = vec4(V[2].xyz, 0.0), tPos;\n"
    "  vec3 tN;\n"
    "  uint sk = kb(4u);\n"
    "  if (sk == 0u || sk > 6u) {\n"          /* (ffhw.c skinning: k_skin's n 0 past 6 too) */
    "    tPos = XF(pos, CMV);\n"
    "    tN = XF(n4, CIMV).xyz;\n"
    "  } else {\n"                          /* 1 2G 2 2 3 3G 3 4G 4 (ffhw.c k_skin) */
    "    uint n = (sk + 3u) / 2u;\n"
    "    bool mw = (sk & 1u) != 0u;\n"
    "    tPos = vec4(0.0);\n"
    "    tN = vec3(0.0);\n"
    "    float wn = 1.0, wi;\n"
    "    for (uint i = 0u; i < n; i++) {\n"
    "      if (mw && i == n - 1u) wi = wn;\n"
    "      else { wi = V[1][i]; if (mw) wn -= wi; }\n"
    "      tPos += XF(pos, CMV + 4 * int(i)) * wi;\n"
    "      tN += XF(n4, CIMV + 4 * int(i)).xyz * wi;\n"
    "    }\n"
    "  }\n"
    "  if (kb(6u) != 0u) tN = normalize(tN);\n"
    /* ffhw.c texgen */
    "  for (int s = 0; s < 4; s++) {\n"
    "    vec4 t = V[9 + s];\n"
    "    for (int j = 0; j < 4; j++) {\n"
    "      uint g = kb(24u + uint(4 * s + j));\n"
    "      int pl = CTG + 4 * s + j;\n"
    "      if (g == 1u) t[j] = dot(c[pl], tPos);\n"
    "      else if (g == 2u) t[j] = dot(c[pl], pos);\n"
    "      else if (g == 3u && j < 2) { vec3 r = reflect(normalize(tPos.xyz), tN);\n"
    "        t[j] = r[j] / (2.0 * length(r + vec3(0.0, 0.0, 1.0))) + 0.5; }\n"
    "      else if (g == 4u && j < 3) t[j] = reflect(normalize(tPos.xyz), tN)[j];\n"
    "      else if (g == 5u && j < 3) t[j] = tN[j];\n"
    "    }\n"
    "    if (((kb(40u) >> uint(s)) & 1u) != 0u) t = XF(t, CTM + 4 * s);\n"
    "    O[9 + s] = t;\n"
    "  }\n"
    /* ffhw.c lighting */
    "  uint lt = kb(5u);\n"
    "  vec4 fD0, fD1;\n"
    "  if (lt == 0u) {\n"
    "    fD0 = V[3]; fD1 = V[4];\n"
    "  } else {\n"
    "    uint s0 = kb(12u), s1 = kb(13u), s2 = kb(14u), s3 = kb(15u);\n"
    "    float a = s2 == 1u ? V[3].a : s2 == 2u ? V[4].a : c[CAMB].w;\n"
    "    fD0 = vec4((s1 == 0u ? c[CAMB].rgb : srgb(s1)) * c[CEMI].rgb + (s0 == 0u ? c[CAMB].rgb : srgb(s0)), a);\n"
    "    fD1 = vec4(0.0, 0.0, 0.0, V[4].a);\n"
    "    bool le = kb(7u) != 0u;\n"
    "    vec3 eyeV = le ? normalize(c[CEYE].xyz / c[CEYE].w - tPos.xyz / tPos.w) : vec3(0.0, 0.0, -1.0);\n"
    "    [[dont_unroll]] for (int li = 0; li < 8; li++) {\n"
    "      uint t = kb(16u + uint(li));\n"
    "      if (t == 0u) continue;\n"
    "      int L = CL + LV * li;\n"
    "      float att = 1.0, nl, nh;\n"
    "      bool lit = true;\n"
    "      if (t == 1u) {\n"
    "        vec3 Ld = normalize(c[L + LDIR].xyz);\n"
    "        nl = max(dot(tN, Ld), 0.0);\n"
    "        nh = le ? max(dot(tN, normalize(Ld + eyeV)), 0.0) : max(dot(tN, c[L + LHALF].xyz), 0.0);\n"
    "      } else {\n"
    "        vec3 VP = c[L + LPOS].xyz - tPos.xyz / tPos.w;\n"
    "        float d = length(VP);\n"
    "        lit = d <= c[L + LRNG].x;\n"
    "        VP = normalize(VP);\n"
    "        vec3 at = c[L + LATT].xyz;\n"
    "        att = 1.0 / (at.x + at.y * d + at.z * d * d);\n"
    "        nl = max(dot(tN, VP), 0.0);\n"
    "        nh = max(dot(tN, normalize(VP + eyeV)), 0.0);\n"
    "        if (t == 3u) {\n"                /* D3D's spot (ffhw.c) */
    "          vec4 sd = c[L + LSPOT];\n"
    "          float inv = 1.0 / length(sd.xyz), cphi = -inv * sd.w;\n"
    "          float sdv = dot(sd.xyz, VP), rho = inv * sdv;\n"
    "          if (rho <= cphi) att = 0.0;\n"
    "          else if (rho <= inv + cphi) att *= sdv + sd.w;\n"
    "        }\n"
    "      }\n"
    "      if (lit) {\n"
    "        float pf = nl == 0.0 || nh == 0.0 ? 0.0 : pow(nh, c[CPAR].x);\n"
    "        fD0.rgb += c[L + LAMB].rgb * att;\n"
    "        fD0.rgb += srgb(s2) * c[L + LDIF].rgb * (att * nl);\n"
    "        fD1.rgb += srgb(s3) * c[L + LSPC].rgb * (att * pf);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    /* ffhw.c's colours after */
    "  vec4 fB0 = V[7], fB1 = V[8];\n"
    "  if (kb(10u) == 0u) {\n"
    "    fD1 = vec4(0.0, 0.0, 0.0, 1.0); fB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "  } else {\n"
    "    if (kb(8u) == 0u) {\n"
    "      if (lt != 0u) fD0.rgb += fD1.rgb;\n"
    "      fD1 = V[4];\n"
    "    }\n"
    "    if (kb(9u) == 0u) { fD1.a = 1.0; fB1.a = 1.0; }\n"
    "  }\n"
    "  O[3] = fD0; O[4] = fD1; O[7] = fB0; O[8] = fB1;\n"
    /* ffhw.c's fog coordinate */
    "  uint fg = kb(11u);\n"
    "  fogw = fg != 0u;\n"
    "  if (fogw) {\n"
    "    uint m = fg - 1u;\n"
    "    vec3 fE = tPos.w != 0.0 && tPos.w != 1.0 ? tPos.xyz / tPos.w : tPos.xyz;\n"
    "    O[5].x = m == 0u ? clamp(V[4].a, 0.0, 1.0) : m == 1u ? length(fE) : m == 2u ? dot(c[CFP].xyz, fE) + c[CFP].w :\n"
    "             m == 3u ? abs(dot(c[CFP].xyz, fE) + c[CFP].w) : V[5].x;\n"
    "  }\n"
    "  if (kb(41u) != 0u) {\n"
    "    float de = length(tPos.xyz);\n"
    "    float pmin = min(c[CPT + 1].w, 63.875), pmax = min(c[CPT].w + pmin, 63.875);\n"
    "    O[6].x = clamp((inversesqrt(c[CPT].x + c[CPT].y * de + c[CPT].z * de * de) + c[CPT + 1].z) * c[CPT].w +\n"
    "                   c[CPT + 1].w, pmin, pmax);\n"
    "  } else {\n"
    "    O[6].x = max(c[CPAR].z, 1.0);\n"
    "  }\n"
    "  vec4 sp = XF(sk != 0u ? tPos : pos, CC);\n"
    "  float aw = clamp(abs(sp.w), 5.421011e-20, 1.8446744e19);\n"
    "  float sw = sp.w < 0.0 ? -aw : aw;\n"
    "  O[0] = vec4(sp.xyz / sw + c[CVP].xyz, sw);\n";

size_t hle_uber_ff_vs_glsl(char *buf, size_t cap, unsigned set)
{
    Buf b = { buf, cap, 0, false };
    put(&b, "#version 450\n#extension GL_EXT_control_flow_attributes : enable\n%s", hle_push_glsl());
    put(&b, k_vs_head, set, (unsigned)(HLE_UBER_WORDS / 4));
    put(&b, k_ff_lib, HLE_FFHW_C_COMPOSITE, HLE_FFHW_C_MODELVIEW, HLE_FFHW_C_INVMODELVIEW, HLE_FFHW_C_TEXMAT,
        HLE_FFHW_C_TEXGEN, HLE_FFHW_C_VPOFF, HLE_FFHW_C_FOGPLANE, HLE_FFHW_C_EYE, HLE_FFHW_C_AMBIENT,
        HLE_FFHW_C_EMISSION, HLE_FFHW_C_PARAMS, HLE_FFHW_C_POINT, HLE_FFHW_C_LIGHTS, HLE_FFHW_LIGHT_VEC4,
        HLE_FFHW_L_AMBIENT, HLE_FFHW_L_DIFFUSE, HLE_FFHW_L_SPECULAR, HLE_FFHW_L_RANGE, HLE_FFHW_L_HALF,
        HLE_FFHW_L_DIRECTION, HLE_FFHW_L_SPOT, HLE_FFHW_L_POSITION, HLE_FFHW_L_ATTENUATION,
        (unsigned)HLE_UBER_STEPS_AT);
    put(&b, "%s", k_vs_main);
    put(&b, "%s", k_ff_body);
    vs_tail(&b);
    return b.overflow ? 0 : b.len;
}

bool hle_uber_ff(const uint32_t *tokens, unsigned nslots, uint32_t *cfg)
{
    if (!hle_ffhw_is(tokens, nslots)) {
        return false;
    }
    memcpy(cfg + HLE_UBER_STEPS_AT, tokens, sizeof(HleFfhwKey));
    return true;
}

bool hle_uber_ff_ok(const HleVshKey *k, const uint32_t *tokens, unsigned nslots)
{
    return hle_ffhw_is(tokens, nslots) && !(k->zclamp && g_hle_glsl_zclamp);
}

/* ---- the pixel stage --------------------------------------------------------------
 * psh.c's arithmetic for the keys hle_uber_ps_ok takes: the colours as the
 * vertex stage gives them, the fog factor by its mode, the four stages by
 * their mode with their signed channels, the alpha kill and the colour key,
 * the combiners and the final one on a register file (psh.c reg: 0 zero, 1 C0,
 * 2 C1, 3 fog, 4 v0, 5 v1, 8 - 11 t0 - t3, 12 r0, 13 r1, the final combiner's
 * 14 v1 + r0 and 15 E F), the alpha test. */
static const char k_fs_head[] =
    "#extension GL_EXT_nonuniform_qualifier : require\n"
    "#extension GL_EXT_control_flow_attributes : enable\n"
    "%s"
    "layout(set = %u, binding = 0, std140) uniform UberCfg { uvec4 u[%u]; };\n"
    "layout(set = 0, binding = 0) uniform texture2D uTex2D[];\n"
    "layout(set = 0, binding = 1) uniform sampler uSmp[];\n"
    "layout(set = 0, binding = 2) uniform textureCube uTexCube[];\n"
    "layout(set = 0, binding = 3) uniform texture3D uTex3D[];\n"
    "layout(location = 0) in vec4 vDiffuse;\n"
    "layout(location = 1) in vec4 vSpecular;\n"
    "layout(location = 2) in vec4 vTex0;\n"
    "layout(location = 3) in vec4 vTex1;\n"
    "layout(location = 4) in vec4 vTex2;\n"
    "layout(location = 5) in vec4 vTex3;\n"
    "layout(location = 6) in float vFog;\n"
    "layout(location = 18) flat in vec4 vDiffuseF;\n"
    "layout(location = 19) flat in vec4 vSpecularF;\n"
    "%s"
    "layout(location = 0) out vec4 oColor;\n"
    HLE_DITHER_GLSL
    "uint cw(uint i) { return u[i >> 2][i & 3u]; }\n"
    "vec4 col(uint c) { return unpackUnorm4x8(c).zyxw; }\n"
    HLE_NVPROJ_GLSL
    HLE_NVRECT_GLSL
    "uint gVb;\n"                       /* the key's vol (HLE_PSH_RECT, HLE_PSH_RECT4) */
    /* psh.c rect_code: a linear texture kept to its own size */
    "vec2 rect(uint s, vec2 uv) {\n"
    "  if ((gVb & 0x20u) != 0u) {\n"
    "    uint r = floatBitsToUint(pc.scr[s]);\n"
    "    if (r != 0u)\n"
    "      uv = nvrectr(uv, r, (pc.flags & (256u << s)) != 0u ? 0.5 / float(pc.flags >> 24) : 0.5);\n"
    "  } else if ((gVb & 0x10u) != 0u) {\n"
    "    uint r = floatBitsToUint(pc.fog.w);\n"
    "    if (r != 0u && s == ((r >> 28 & 1u) | (r >> 30 & 2u)))\n"
    "      uv = nvrect(uv, (pc.flags & (256u << s)) != 0u ? 0.5 / float(pc.flags >> 24) : 0.5);\n"
    "  }\n"
    "  return uv;\n"
    "}\n"
    /* psh.c samp: Xbox linear textures by texel coordinates */
    "vec4 samp(uint s, vec2 uv) {\n"
    "  uint ti = pc.tex[s] & 0xFFFFu, si = pc.tex[s] >> 16;\n"
    "  uv = rect(s, uv);\n"
    "  if ((pc.flags & (1u << s)) != 0u)\n"
    "    uv /= vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)) /\n"
    "          ((pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
    "  return texture(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv);\n"
    "}\n"
    /* psh.c sfun: the signed channels converted, then filtered */
    "vec4 sgn4(vec4 c) {\n"
    "  vec4 v = floor(c * 255.0 + 0.5);\n"
    "  return max(mix(v, v - 256.0, greaterThanEqual(v, vec4(128.0))) / 127.0, -1.0);\n"
    "}\n"
    "float bil(vec4 c, vec2 w) { return mix(mix(c.w, c.z, w.x), mix(c.x, c.y, w.x), w.y); }\n"
    /* psh.c sbilod: level l's four texels signed, then blended (the stage's sampler, its
     * addressing with them) */
    "vec4 sbilod(uint ti, uint si, vec2 uv, int l) {\n"
    "  vec2 sz = vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), l));\n"
    "  vec2 p = uv * sz - 0.5, i = floor(p), w = p - i;\n"
    "  float lv = float(l);\n"
    "  vec4 t00 = sgn4(textureLod(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), (i + vec2(0.5, 0.5)) / sz, lv));\n"
    "  vec4 t10 = sgn4(textureLod(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), (i + vec2(1.5, 0.5)) / sz, lv));\n"
    "  vec4 t01 = sgn4(textureLod(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), (i + vec2(0.5, 1.5)) / sz, lv));\n"
    "  vec4 t11 = sgn4(textureLod(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), (i + vec2(1.5, 1.5)) / sz, lv));\n"
    "  mat4 c = transpose(mat4(t01, t11, t10, t00));\n"
    "  return vec4(bil(c[0], w), bil(c[1], w), bil(c[2], w), bil(c[3], w));\n"
    "}\n"
    "vec4 samps(uint s, vec2 uv, vec4 f, bvec4 m, bool pt) {\n"
    "  uint ti = pc.tex[s] & 0xFFFFu, si = pc.tex[s] >> 16;\n"
    "  uv = rect(s, uv);\n"
    "  if ((pc.flags & (1u << s)) != 0u)\n"
    "    uv /= vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)) /\n"
    "          ((pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
    "  int nl = textureQueryLevels(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]));\n"
    "  if (pt)\n"
    "    return mix(f, sgn4(f), m);\n"
    "  if (nl > 1) {\n"
    "    float lx = textureQueryLod(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv).x;\n"
    "    int l0 = min(int(floor(lx)), nl - 1);\n"
    "    float lf = lx - float(l0);\n"
    "    vec4 v = sbilod(ti, si, uv, l0);\n"
    "    if (lf > 0.0 && l0 + 1 < nl) v = mix(v, sbilod(ti, si, uv, l0 + 1), lf);\n"
    "    return mix(f, v, m);\n"
    "  }\n"
    "  vec2 w = fract(uv * vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)) - 0.5);\n"
    "  vec4 r = sgn4(textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 0));\n"
    "  vec4 g = sgn4(textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 1));\n"
    "  vec4 b = sgn4(textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 2));\n"
    "  vec4 a = sgn4(textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 3));\n"
    "  return mix(f, vec4(bil(r, w), bil(g, w), bil(b, w), bil(a, w)), m);\n"
    "}\n"
    /* PSDotMapping of a texel to a vector (psh.c dmap; HILO_1 as the halves) */
    "vec3 dmap(uint m, vec4 c) {\n"
    "  if (m == 0u) return c.rgb;\n"
    "  if (m == 4u) { uvec4 i = uvec4(c * 255.0 + 0.5);\n"
    "    return vec3(float(i.a << 8 | i.r), float(i.g << 8 | i.b), 65535.0) / 65535.0; }\n"
    "  if (m == 3u) { vec3 v = floor(c.rgb * 255.0 + 0.5);\n"
    "    return mix(v, v - 256.0, greaterThanEqual(v, vec3(128.0))) / 127.0; }\n"
    "  if (m >= 4u) { vec2 h = c.rg * 2.0 - 1.0;\n"
    "    return vec3(h, sqrt(max(1.0 - dot(h, h), 0.0))); }\n"
    "  return c.rgb * 2.0 - 1.0;\n"
    "}\n"
    /* bytes as two's complement over 127 (bump offsets) */
    "vec2 sgn8(vec2 c) {\n"
    "  vec2 v = floor(c * 255.0 + 0.5);\n"
    "  return mix(v, v - 256.0, greaterThanEqual(v, vec2(128.0))) / 127.0;\n"
    "}\n"
    "vec4 samp3(uint s, vec3 uvw) {\n"
    "  uint ti = pc.tex[s] & 0xFFFFu, si = pc.tex[s] >> 16;\n"
    "  return texture(sampler3D(uTex3D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uvw);\n"
    "}\n"
    "vec4 sampc(uint s, vec3 dir) {\n"
    "  if (dir == vec3(0.0)) return vec4(0.0, 0.0, 0.0, 1.0);\n"
    "  uint ti = pc.tex[s] & 0xFFFFu, si = pc.tex[s] >> 16;\n"
    "  return texture(samplerCube(uTexCube[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), dir);\n"
    "}\n"
    /* psh.c: the other kind of texture than a mode fetches (HLE_PSH_TKIND) */
    "vec2 cube2d(vec3 d) {\n"
    "  vec3 a = abs(d);\n"
    "  if (a.x > a.y && a.x > a.z) return vec2(d.x > 0.0 ? -d.z : d.z, d.y) / a.x;\n"
    "  if (a.y > a.x && a.y > a.z) return vec2(d.x, d.y > 0.0 ? -d.z : d.z) / a.y;\n"
    "  return vec2(d.z > 0.0 ? d.x : -d.x, d.y) / a.z;\n"
    "}\n"
    "vec4 samp2(uint s, vec2 uv) {\n"
    "  uint ti = pc.tex[s] & 0xFFFFu, si = pc.tex[s] >> 16;\n"
    "  return texture(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv);\n"
    "}\n"
    /* a shadow buffer (psh.c shadow_sample): the stored depth against r by the
     * function, the four results weighted; shw: the key's shadow | shadow_fmt << 8 */
    "vec4 shsamp(uint s, vec4 tc, bool p3, uint shw) {\n"
    "  uint fn = (shw >> 4) & 7u, fmt = (shw >> (8u + 2u * s)) & 3u;\n"
    "  if (fn == 0u || fn == 7u) return vec4(fn != 0u ? 1.0 : 0.0);\n"
    "  uint ti = pc.tex[s] & 0xFFFFu, si = pc.tex[s] >> 16;\n"
    "  vec3 pq = nvproj(tc);\n"
    "  vec2 uv = pq.xy;\n"
    "  vec2 sz = vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0));\n"
    "  if ((pc.flags & (1u << s)) != 0u)\n"
    "    uv /= sz / ((pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
    "  float m = fmt == 1u ? 16777215.0 : fmt == 2u ? 65535.0 : fmt == 3u ? 511.9375 : 1.0;\n"
    "  float r = p3 ? clamp(pq.z, 0.0, m) : 0.0;\n"
    "  vec4 d = textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv) * m;\n"
    "  vec4 rr = vec4(r);\n"
    "  vec4 q = vec4(fn == 1u ? lessThan(d, rr) : fn == 2u ? equal(d, rr) : fn == 3u ? lessThanEqual(d, rr) :\n"
    "                fn == 4u ? greaterThan(d, rr) : fn == 5u ? notEqual(d, rr) : greaterThanEqual(d, rr));\n"
    "  vec2 f = fract(uv * sz - 0.5);\n"
    "  return vec4(mix(mix(q.w, q.z, f.x), mix(q.x, q.y, f.x), f.y));\n"
    "}\n"
    "bool ckey(vec4 t, uint key, uint mask) {\n"
    "  uvec4 c = uvec4(clamp(t, 0.0, 1.0) * 255.0 + 0.5);\n"
    "  return (((c.a << 24) | (c.r << 16) | (c.g << 8) | c.b) & mask) == (key & mask);\n"
    "}\n"
    /* the registers as variables, picked by a switch on the config's (uniform) words: an
     * array indexed by them went to the GPU's local memory */
    "vec4 rC0, rC1, rFog, rV0, rV1, rT0, rT1, rT2, rT3, rR0, rR1, rVR, rEF;\n"
    "vec4 tR0, tR1, tR2, tR3;\n"
    "vec4 gT(uint i) { return i == 0u ? rT0 : i == 1u ? rT1 : i == 2u ? rT2 : rT3; }\n"
    "vec4 gTR(uint i) { return i == 0u ? tR0 : i == 1u ? tR1 : i == 2u ? tR2 : tR3; }\n"
    "float hb;\n"
    /* an input of a combiner (psh.c in_expr): fin, the final combiner's (14, 15 read) */
    "float inm(uint i, float x) {\n"
    "  switch ((i >> 5) & 7u) {\n"
    "  case 0u: return max(x, 0.0);\n"
    "  case 1u: return 1.0 - clamp(x, 0.0, 1.0);\n"
    "  case 2u: return 2.0 * max(x, 0.0) - 1.0;\n"
    "  case 3u: return -2.0 * max(x, 0.0) + 1.0;\n"
    "  case 4u: return max(x, 0.0) - hb;\n"
    "  case 5u: return -max(x, 0.0) + hb;\n"
    "  case 6u: return x;\n"
    "  }\n"
    "  return -x;\n"
    "}\n"
    "vec4 grd(uint i, bool fin) {\n"
    "  switch (i & 15u) {\n"
    "  case 1u: return rC0;\n"
    "  case 2u: return rC1;\n"
    "  case 3u: return rFog;\n"
    "  case 4u: return rV0;\n"
    "  case 5u: return rV1;\n"
    "  case 8u: return rT0;\n"
    "  case 9u: return rT1;\n"
    "  case 10u: return rT2;\n"
    "  case 11u: return rT3;\n"
    "  case 12u: return rR0;\n"
    "  case 13u: return rR1;\n"
    "  case 14u: return fin ? rVR : vec4(0.0);\n"
    "  case 15u: return fin ? rEF : vec4(0.0);\n"
    "  }\n"
    "  return vec4(0.0);\n"
    "}\n"
    "vec3 inr(uint i, bool fin) {\n"
    "  vec4 g = grd(i, fin);\n"
    "  vec3 x = (i & 0x10u) != 0u ? g.aaa : g.rgb;\n"
    "  return vec3(inm(i, x.r), inm(i, x.g), inm(i, x.b));\n"
    "}\n"
    "float ina(uint i, bool fin) {\n"
    "  vec4 g = grd(i, fin);\n"
    "  return inm(i, (i & 0x10u) != 0u ? g.a : g.b);\n"
    "}\n"
    /* a stage's result through its output mapping (psh.c out_expr) */
    "vec3 outm(uint o, vec3 v) {\n"
    "  switch ((o >> 15) & 7u) {\n"
    "  case 1u: return v - hb;\n"
    "  case 2u: return v * 2.0;\n"
    "  case 3u: return (v - hb) * 2.0;\n"
    "  case 4u: return v * 4.0;\n"
    "  case 6u: return v * 0.5;\n"
    "  }\n"
    "  return v;\n"
    "}\n"
    "float outa(uint o, float v) { return outm(o, vec3(v)).x; }\n"
    /* the writable registers (psh.c dst) */
    "void wr3(uint d, vec3 v) {\n"
    "  switch (d & 15u) {\n"
    "  case 4u: rV0.rgb = v; break;\n"
    "  case 5u: rV1.rgb = v; break;\n"
    "  case 8u: rT0.rgb = v; break;\n"
    "  case 9u: rT1.rgb = v; break;\n"
    "  case 10u: rT2.rgb = v; break;\n"
    "  case 11u: rT3.rgb = v; break;\n"
    "  case 12u: rR0.rgb = v; break;\n"
    "  case 13u: rR1.rgb = v; break;\n"
    "  }\n"
    "}\n"
    "void wr1(uint d, float v) {\n"
    "  switch (d & 15u) {\n"
    "  case 4u: rV0.a = v; break;\n"
    "  case 5u: rV1.a = v; break;\n"
    "  case 8u: rT0.a = v; break;\n"
    "  case 9u: rT1.a = v; break;\n"
    "  case 10u: rT2.a = v; break;\n"
    "  case 11u: rT3.a = v; break;\n"
    "  case 12u: rR0.a = v; break;\n"
    "  case 13u: rR1.a = v; break;\n"
    "  }\n"
    "}\n";

static const char k_fs_main[] =
    "void main() {\n"
    "  uint pf = cw(51u), cnt = cw(42u), tm = cw(43u), cmp = cw(46u), vb = pf >> 20, shw = cw(52u);\n"
    "  uint dm = cw(44u), itx = cw(45u), tsg = dm >> 16, sgm = 0u;\n"
    "  gVb = vb;\n"
    "  bool fl = (pf & 0x800u) != 0u;\n"
    "  vec4 v0 = fl ? vDiffuseF : vDiffuse, v1 = fl ? vSpecularF : vSpecular;\n"
    "%s"
    /* the fog factor (psh.c: 0 none, 1 the vertex's, 2 EXP, 3 EXP2, 4 LINEAR, of the
     * signed distance or |distance| (fog_abs)) */
    "  uint fm = (pf >> 4) & 15u;\n"
    "  float fd = (pf & 0x100u) != 0u ? abs(vFog) : vFog;\n"
    "  float fogf = 1.0;\n"
    "  if (fm == 1u) fogf = isnan(vFog) ? 1.0 : clamp(vFog, 0.0, 1.0);\n"
    "  else if (fm == 2u) fogf = exp(-fd * pc.fog.z);\n"
    "  else if (fm == 3u) { float e = fd * pc.fog.z; fogf = exp(-e * e); }\n"
    "  else if (fm >= 4u) fogf = (pc.fog.y - fd) / (pc.fog.y - pc.fog.x);\n"
    "  if (fm >= 2u) fogf = isnan(fogf) ? 1.0 : clamp(fogf, 0.0, 1.0);\n"
    /* the stages by their mode (psh.c: NONE, PROJECT2D, PROJECT3D, CUBEMAP, PASSTHRU,
     * CLIPPLANE; the full form: shadow buffers, BUMPENVMAP, BUMPENVMAP_LUM, the dot-product
     * and dependent stages, signed channels), each fetch's alpha kill and colour key
     * after it */
    "  float dt1 = 0.0, dt2 = 0.0, dt3 = 0.0;\n"
    "  [[dont_unroll]] for (uint s = 0u; s < 4u; s++) {\n"
    "    uint m = (tm >> (5u * s)) & 31u;\n"
    "    vec4 tc = s == 0u ? vTex0 : s == 1u ? vTex1 : s == 2u ? vTex2 : vTex3, t = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    bool tk = ((cmp >> (16u + s)) & 1u) != 0u, vol = ((vb >> s) & 1u) != 0u;\n"
    "    bool shp = ((shw >> s) & 1u) != 0u && (m == 1u || m == 2u);\n"
    "    uint ins = s == 3u ? (itx >> 20) & 3u : s == 2u ? (itx >> 16) & 1u : 0u;\n"
    "#if UB_FULL\n"
    "    if (s != 0u && (m == 17u || (m >= 9u && m <= 14u) || m == 18u)) {\n"
    "      float dv = dot(tc.xyz, dmap((dm >> (4u * (s - 1u))) & 7u, gTR(ins)));\n"
    "      if (s == 1u) dt1 = dv; else if (s == 2u) dt2 = dv; else dt3 = dv;\n"
    "    }\n"
    "    if (s == 2u && m == 11u) dt3 = dot(vTex3.xyz, dmap((dm >> 8) & 7u, gTR((itx >> 20) & 3u)));\n"
    "    if (shp) t = shsamp(s, tc, m == 2u, shw); else\n"
    "#endif\n"
    "    if (m == 1u && tk) { vec2 q = nvproj(tc).xy; t = sampc(s, normalize(vec3(1.0, q.y, -q.x))); }\n"
    "    else if (m == 1u) t = vol ? samp3(s, vec3(nvproj(tc).xy, 0.0)) : samp(s, nvproj(tc).xy);\n"
    "    else if (m == 2u) t = samp3(s, nvproj(tc));\n"
    "    else if (m == 3u) t = tk ? samp2(s, cube2d(tc.xyz)) : sampc(s, tc.xyz);\n"
    "    else if (m == 4u) t = clamp(tc, 0.0, 1.0);\n"
    "    else if (m == 5u) {\n"
    "      t = vec4(0.0);\n"
    "      for (uint c = 0u; c < 4u; c++) {\n"
    "        bool ge = ((cmp >> (4u * s + c)) & 1u) != 0u;\n"
    "        if (ge ? tc[c] >= 0.0 : tc[c] < 0.0) discard;\n"
    "      }\n"
    "    }\n"
    "#if UB_FULL\n"
    "    else if (m == 6u || m == 7u) {\n"
    /* bump-env (stages 1 - 3): the input stage's (b, g) as signed bytes, or as the
     * texture unit gives them signed (psh.c bump_in), through the matrix; LUM's scale
     * by its r */
    "      uint mi = ((sgm >> ins) & 1u) != 0u ? (tsg >> (4u * ins)) & 12u : 0u;\n"
    "      vec4 ti = gT(ins), tri = gTR(ins);\n"
    "      vec2 bi = mi == 12u ? ti.bg : sgn8(tri.bg);\n"
    "      uvec2 mt = s == 1u ? pc.bump.xy : s == 2u ? pc.bump.zw : pc.bump3;\n"
    "      vec2 d = mat2(unpackHalf2x16(mt.x), unpackHalf2x16(mt.y)) * bi;\n"
    /* ((s, t) not divided by q: psh.c's offset texture) */
    "      t = vol ? samp3(s, vec3(tc.xy + d, tc.z)) : samp(s, tc.xy + d);\n"
    "      if (m == 7u) {\n"
    "        vec2 l = unpackHalf2x16(pc.bumpl[s - 1u]);\n"
    "        t.rgb *= clamp(l.x * tri.r + l.y, 0.0, 1.0);\n"
    "      }\n"
    "    }\n"
    "    else if (m == 9u) t = samp(s, s == 2u ? vec2(dt1, dt2) : vec2(dt2, dt3));\n"
    "    else if (m == 11u || m == 14u) {\n"
    "      vec3 nv = vec3(dt1, dt2, dt3);\n"
    "      t = tk ? samp2(s, cube2d(nv)) : sampc(s, nv);\n"
    "    } else if (m == 12u || m == 18u) {\n"
    "      vec3 nv = vec3(dt1, dt2, dt3), e = vec3(vTex1.w, vTex2.w, vTex3.w);\n"
    "      vec3 rv = 2.0 * nv * dot(nv, e) / max(dot(nv, nv), 1e-8) - e;\n"
    "      t = tk ? samp2(s, cube2d(rv)) : sampc(s, rv);\n"
    "    } else if (m == 13u) t = tk ? samp2(s, vec2(dt1, dt2)) : samp3(s, vec3(dt1, dt2, dt3));\n"
    "    else if (m == 15u || m == 16u) {\n"
    "      vec4 ti = gTR(ins);\n"
    "      uint xi = pc.tex[s] & 0xFFFFu, xs = pc.tex[s] >> 16;\n"
    "      t = texture(sampler2D(uTex2D[nonuniformEXT(xi)], uSmp[nonuniformEXT(xs)]), m == 15u ? ti.ar : ti.gb);\n"
    "    } else if (m == 17u) t = vec4(0.0);\n"
    "    else if (m == 8u || m == 10u) t = samp(s, nvproj(tc).xy);\n"
    "#endif\n"
    /* a signed stage (psh.c signed_stage): its texel as sampled kept (tr), the channels
     * the texture unit gives signed converted: flipped bytes centred on 0x80, a 2D
     * image gathered and filtered as signed, the others as sampled */
    "    vec4 tr = t;\n"
    "#if UB_FULL\n"
    "    uint sg = (tsg >> (4u * s)) & 15u;\n"
    "    if (sg != 0u && m != 0u && m != 4u && m != 5u && m != 17u && !shp) {\n"
    "      sgm |= 1u << s;\n"
    "      bvec4 mk = bvec4((sg & 2u) != 0u, (sg & 4u) != 0u, (sg & 8u) != 0u, (sg & 1u) != 0u);\n"
    "      if (((itx >> s) & 1u) != 0u) {\n"
    "        tr = mix(t, mod(floor(t * 255.0 + 0.5) + 128.0, 256.0) / 255.0, mk);\n"
    "        t = mix(t, clamp((t * 255.0 - 128.0) / 127.0, -1.0, 1.0), mk);\n"
    "      } else if (m == 1u && !vol) {\n"
    "        t = samps(s, nvproj(tc).xy, t, mk, ((dm >> (12u + s)) & 1u) != 0u);\n"
    "      } else {\n"
    "        t = mix(t, sgn4(t), mk);\n"
    "      }\n"
    "    }\n"
    "#endif\n"
    "    if (s == 0u) tR0 = tr; else if (s == 1u) tR1 = tr; else if (s == 2u) tR2 = tr; else tR3 = tr;\n"
    "    if (m != 0u && m != 4u && m != 5u && m != 17u) {\n"
    "      if (((pf >> (12u + s)) & 1u) != 0u && t.a == 0.0) discard;\n"
    "      uint ko = (tm >> (20u + 2u * s)) & 3u;\n"
    "      if (ko != 0u && ckey(t, cw(47u + s), ((tm >> (28u + s)) & 1u) != 0u ? 0x00FFFFFFu : 0xFFFFFFFFu)) {\n"
    "        if (ko == 1u) t.a = 0.0; else if (ko == 2u) t = vec4(0.0); else discard;\n"
    "      }\n"
    "    }\n"
    "    if (s == 0u) rT0 = t; else if (s == 1u) rT1 = t; else if (s == 2u) rT2 = t; else rT3 = t;\n"
    "  }\n"
    "  rFog = vec4(col(pc.fog_color).rgb, fogf); rV0 = v0; rV1 = v1;\n"
    "  rR0 = vec4(0.0, 0.0, 0.0, rT0.a); rR1 = vec4(0.0); rVR = vec4(0.0); rEF = vec4(0.0);\n"
    /* the bias's half: 0x80 / 0xFF where a MUX is on the MSB (psh.c) */
    "  uint n = min(cnt & 0xFFu, 8u);\n"
    "  hb = 0.5;\n"
    "  if ((cnt & 0x100u) != 0u)\n"
    "    for (uint s = 0u; s < n; s++) if (((cw(16u + s) | cw(32u + s)) & 0x4000u) != 0u) hb = 128.0 / 255.0;\n"
    "  [[dont_unroll]] for (uint s = 0u; s < n; s++) {\n"
    "    uint ri = cw(8u + s), ro = cw(16u + s), ai = cw(24u + s), ao = cw(32u + s);\n"
    "    rC0 = col(pc.psc[(cnt & 0x1000u) != 0u ? s : 0u]);\n"
    "    rC1 = col(pc.psc[8u + ((cnt & 0x10000u) != 0u ? s : 0u)]);\n"
    "    vec3 A = inr(ri >> 24, false), B = inr(ri >> 16, false), C = inr(ri >> 8, false), D = inr(ri, false);\n"
    "    float Aa = ina(ai >> 24, false), Ba = ina(ai >> 16, false), Ca = ina(ai >> 8, false), Da = ina(ai, false);\n"
    "    bool mx = (cnt & 0x100u) != 0u ? rR0.a > 0.5 : (uint(rR0.a * 255.0) & 1u) == 1u;\n"
    "    vec3 ab = (ro & 0x2000u) != 0u ? vec3(dot(A, B)) : A * B;\n"
    "    vec3 cd = (ro & 0x1000u) != 0u ? vec3(dot(C, D)) : C * D;\n"
    "    vec3 ms = (ro & 0x4000u) != 0u ? (mx ? cd : ab) : ab + cd;\n"
    "    float aab = Aa * Ba, acd = Ca * Da, ams = (ao & 0x4000u) != 0u ? (mx ? acd : aab) : aab + acd;\n"
    "    ab = clamp(outm(ro, ab), -1.0, 1.0); cd = clamp(outm(ro, cd), -1.0, 1.0);\n"
    "    ms = clamp(outm(ro, ms), -1.0, 1.0);\n"
    "    aab = clamp(outa(ao, aab), -1.0, 1.0); acd = clamp(outa(ao, acd), -1.0, 1.0);\n"
    "    ams = clamp(outa(ao, ams), -1.0, 1.0);\n"
    "    wr3(ro >> 4, ab);\n"
    "    wr3(ro, cd);\n"
    "    wr3(ro >> 8, ms);\n"
    "    wr1(ao >> 4, aab);\n"
    "    wr1(ao, acd);\n"
    "    wr1(ao >> 8, ams);\n"
    "    if ((ro & 0x80000u) != 0u) wr1(ro >> 4, ab.b);\n"
    "    if ((ro & 0x40000u) != 0u) wr1(ro, cd.b);\n"
    "  }\n"
    "  uint fa = cw(40u), fe = cw(41u);\n"
    "  vec4 outc;\n"
    "  if (fa == 0u && fe == 0u) {\n"
    "    vec3 b = (pf & 0x400u) != 0u ? rV1.rgb + rR0.rgb : rR0.rgb;\n"
    "    outc = vec4(mix(rFog.rgb, clamp(b, 0.0, 1.0), fogf), clamp(rR0.a, 0.0, 1.0));\n"
    "  } else {\n"
    "    rC0 = col(pc.psc[16]); rC1 = col(pc.psc[17]);\n"
    "    vec3 E = clamp(inr(fe >> 24, false), 0.0, 1.0), F = clamp(inr(fe >> 16, false), 0.0, 1.0);\n"
    "    rEF = vec4(E * F, 0.0);\n"
    "    rVR = vec4(((fe & 0x40u) != 0u ? 1.0 - rV1.rgb : rV1.rgb) +\n"
    "               ((fe & 0x20u) != 0u ? 1.0 - rR0.rgb : rR0.rgb), 0.0);\n"
    "    if ((fe & 0x80u) != 0u) rVR = clamp(rVR, 0.0, 1.0);\n"
    "    vec3 A = clamp(inr(fa >> 24, true), 0.0, 1.0), B = clamp(inr(fa >> 16, true), 0.0, 1.0);\n"
    "    vec3 C = clamp(inr(fa >> 8, true), 0.0, 1.0), D = clamp(inr(fa, true), 0.0, 1.0);\n"
    "    float Gf = clamp(ina(fe >> 8, true), 0.0, 1.0);\n"
    "    outc = vec4(A * B + (1.0 - A) * C + D, Gf);\n"
    "  }\n"
    "  outc = clamp(outc, 0.0, 1.0);\n"
    /* the alpha test (psh.c k_cmp) */
    "  if ((pf & 1u) != 0u) {\n"
    "    uint af = (pf >> 1) & 7u;\n"
    "    float a = outc.a, r = pc.alpha_ref;\n"
    "    bool ok = af == 1u ? a < r : af == 2u ? a == r : af == 3u ? a <= r : af == 4u ? a > r :\n"
    "              af == 5u ? a != r : af == 6u ? a >= r : af == 7u;\n"
    "    if (!ok) discard;\n"
    "  }\n"
    "  oColor = (pf & 0x200u) != 0u ? outc.bgra : outc;\n"
    "  uint dth = cw(53u);\n"
    "  if (dth != 0u) oColor.rgb = nvdither(oColor.rgb, vec3(1.0 / 31.0, dth == 1u ? 1.0 / 63.0 : 1.0 / 31.0, 1.0 / 31.0));\n"
    "%s"
    "}\n";

size_t hle_uber_fs_glsl(char *buf, size_t cap, unsigned set, unsigned depth, bool full)
{
    /* the depth as psh.c stores it: the rasterizer's, cut to a D24 buffer's steps
     * (1: HLE_PSH_ZCUT), or W-buffering's w (3); the others the rasterizer's here */
    const bool wb = depth == 3;
    Buf b = { buf, cap, 0, false };
    put(&b, "#version 450\n#define UB_FULL %d\n", full ? 1 : 0);
    put(&b, k_fs_head, hle_push_glsl(), set, (unsigned)(HLE_UBER_WORDS / 4), wb ? HLE_WBUF_IN_GLSL : "");
    put(&b, k_fs_main, wb ? HLE_WBUF_CALC_GLSL : "",
        wb ? HLE_WBUF_STORE_GLSL :
        depth == 1 ? "  { float zo = uintBitsToFloat(pc.psc[18]) + uintBitsToFloat(pc.psc[19]) *\n"
        "        max(abs(dFdx(gl_FragCoord.z)), abs(dFdy(gl_FragCoord.z))) * 16777215.0;\n"
        "    gl_FragDepth = clamp(floor(gl_FragCoord.z * 16777215.0 + zo) / 16777215.0, 0.0, 1.0); }\n" : "");
    return b.overflow ? 0 : b.len;
}

bool hle_uber_fs_full(const HlePshKey *k)
{
    if (HLE_PSH_TSIGN(k)) {
        return true;
    }
    for (unsigned s = 0; s < 4; s++) {
        const unsigned m = (k->tex_modes >> (5 * s)) & 0x1F;
        if (m >= HLE_PST_BUMPENVMAP ||
            (((k->shadow >> s) & 1) && (m == HLE_PST_PROJECT2D || m == HLE_PST_PROJECT3D))) {
            return true;
        }
    }
    return false;
}

unsigned hle_uber_fs_depth(const HlePshKey *k)
{
    return k->wbuf ? 3u : (k->vol & HLE_PSH_ZCUT) ? 1u : 0u;
}

/* ---- what the interpreters draw ----------------------------------------------- */

bool hle_uber_vs_ok(const HleVshKey *k, const uint32_t *tokens, unsigned nslots)
{
    if (!tokens || !nslots || hle_ffhw_is(tokens, nslots) || (k->zclamp && g_hle_glsl_zclamp)) {
        return false;                   /* (the fixed function's key: not theirs) */
    }
    unsigned fin = 0;
    while (fin < nslots && fin < HLE_VSH_MAX_SLOTS && !(tokens[4 * fin + 3] & 1)) {
        fin++;
    }
    return fin < nslots && fin < HLE_VSH_MAX_SLOTS;
}

bool hle_uber_ps_ok(const HlePshKey *k)
{
    /* psh.c's text past this subset: the target's pixel, signed blending, point
     * sprites, debug views */
    if (k->compare & ~0xFFFFFu) {
        return false;
    }
    if (k->fbfetch || k->sblend || k->wbuf > 1 || k->point_sprite || k->debug || k->fog > 4 ||
        (k->alpha_test && k->alpha_func >= 8)) {
        return false;
    }
    for (unsigned s = 0; s < 4; s++) {
        const unsigned m = (k->tex_modes >> (5 * s)) & 0x1F;
        /* (stage 0 fetches by its own coordinates alone; DOT_ST on stage 2 or 3,
         * as psh.c reads the dot before it) */
        if (m > HLE_PST_DOT_RFLCT_SPEC_CONST || (m >= HLE_PST_BUMPENVMAP && !s) ||
            (m == HLE_PST_DOT_ST && s < 2)) {
            return false;
        }
    }
    return true;
}
