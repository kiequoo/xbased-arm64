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
 * words, as psh.c writes them out.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "hw/xbox/hle/shader/ff.h"
#include "hw/xbox/hle/shader/ffhw.h"
#include "hw/xbox/hle/shader/psh.h"
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
    cfg[2] = (uint32_t)vk->w1_mask | (uint32_t)vk->s1_mask << 16;
    cfg[3] = vk->s1_cnt;
    cfg[4] = (vk->flat ? HLE_UBER_V_FLAT : 0u) | (vk->spec_off & 1 ? HLE_UBER_V_SPECOFF : 0u) |
             (vk->spec_off & 2 ? HLE_UBER_V_SPECA1 : 0u) | (vk->wbuf ? HLE_UBER_V_WBUF : 0u) |
             (vk->zfree ? HLE_UBER_V_ZFREE : 0u) | (vk->pt_size & HLE_VSH_PT_FIXED ? HLE_UBER_V_PTFIXED : 0u) |
             (uint32_t)vk->fog_table << 8 | (uint32_t)(vk->pt_size & 0x1FFu) << 16;
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
    "layout(location = 11) out vec3 vScr;\n"
    "layout(location = 12) out vec4 vTexP0;\n"
    "layout(location = 13) out vec4 vTexP1;\n"
    "layout(location = 14) out vec4 vTexP2;\n"
    "layout(location = 15) out vec4 vTexP3;\n"
    "layout(location = 16) out float vZs;\n"
    "layout(location = 17) out float vFogP;\n"
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
    /* an output (vsh.c out_write): R12 is oPos; oFog takes the result's x under any mask;
     * A0 the floor of x a little over (an index computed as a whole number lands on it) */
    "void wr(uint w, vec4 v) {\n"
    "  uint ty = w & 7u, ix = (w >> 3) & 255u, m = (w >> 11) & 15u;\n"
    "  bvec4 mk = bvec4((m & 8u) != 0u, (m & 4u) != 0u, (m & 2u) != 0u, (m & 1u) != 0u);\n"
    "  if (ty == 1u && m != 0u) {\n"
    "    if (ix == 12u) O[0] = mix(O[0], v, mk); else { uint r = ix %% 12u; R[r] = mix(R[r], v, mk); }\n"
    "  } else if (ty == 3u && m != 0u) {\n"
    "    if ((ix & 15u) == 5u) { O[5].x = v.x; fogw = true; } else { uint r = ix & 15u; O[r] = mix(O[r], v, mk); }\n"
    "  } else if (ty == 5u) {\n"
    "    A0 = int(floor(v.x + 1.0 / 1024.0));\n"
    "  }\n"
    "}\n";

static const char k_vs_main[] =
    "void main() {\n"
    "  uint inm = cw(0u), cmpm = cw(1u) >> 16, w1m = cw(2u) & 0xFFFFu, s1m = cw(2u) >> 16, s1c = cw(3u);\n"
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
    "    if ((s1m & bit) != 0u) {\n"           /* S1: (2x + 1) / 65535, the console's */
    "      uint n = ((s1c >> (2u * uint(i))) & 3u) + 1u;\n"
    "      for (uint j = 0u; j < n; j++) V[i][j] = (2.0 * V[i][j] + 1.0) / 65535.0;\n"
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
    "  vDiffuse = clamp(oD0, 0.0, 1.0);\n"
    "  vSpecular = (fl & %uu) != 0u ? vec4(0.0, 0.0, 0.0, 1.0) :\n"
    "              (fl & %uu) != 0u ? vec4(clamp(oD1.rgb, 0.0, 1.0), 1.0) : clamp(oD1, 0.0, 1.0);\n"
    "  vBack0 = clamp(oB0, 0.0, 1.0);\n"
    "  vBack1 = (fl & %uu) != 0u ? vec4(0.0, 0.0, 0.0, 1.0) :\n"
    "           (fl & %uu) != 0u ? vec4(clamp(oB1.rgb, 0.0, 1.0), 1.0) : clamp(oB1, 0.0, 1.0);\n"
    "  vDiffuseF = vDiffuse; vSpecularF = vSpecular; vBack0F = vBack0; vBack1F = vBack1;\n"
    "  vTex0 = O[9]; vTex1 = O[10]; vTex2 = O[11]; vTex3 = O[12];\n"
    "  vTexP0 = O[9]; vTexP1 = O[10]; vTexP2 = O[11]; vTexP3 = O[12]; vZs = oPos.z;\n"
    /* the fog (vsh.c tail): carried from 6 vertices before, the NV fog value in plain
     * floats (a stand-in's), the program's, none */
    "  uint ft = (fl >> 8) & 255u;\n"
    "  if ((ft & %uu) != 0u && !fogw) {\n"
    "    int mm = gl_VertexIndex %% 6;\n"
    "    oFog.x = mm == 0 ? pc.mvp[0].w : mm == 1 ? pc.mvp[2].w : pc.mvp[3][mm - 2];\n"
    "    fogw = true;\n"
    "  }\n"
    "  if ((ft & %uu) != 0u) {\n"
    "    float fd = (ft & %uu) != 0u ? pc.fog.z : !fogw ? 0.0 : (ft & %uu) != 0u ? abs(oFog.x) : oFog.x;\n"
    "    float fv = pc.fog.x + pc.fog.y * fd;\n"
    "    vFog = isinf(fv) || isnan(fv) || ((ft & %uu) != 0u && isinf(fd)) ? 8.0 : clamp(fv, -8.0, 8.0);\n"
    "  } else {\n"
    "    vFog = fogw ? oFog.x : ft != 0u ? 0.0 : 1.0;\n"
    "  }\n"
    "  vFogP = vFog;\n"
    "  float ptS;\n"
    "  if ((fl & %uu) != 0u) {\n"
    "    float e = float((fl >> 16) & 511u) / 8.0;\n"
    "    gl_PointSize = max(e, 1.0) * float(max((pc.flags >> 16) & 255u, 1u));\n"
    "    ptS = e;\n"
    "  } else {\n"
    "    gl_PointSize = max(oPts.x, 1.0) * float(max((pc.flags >> 16) & 255u, 1u));\n"
    "    ptS = oPts.x;\n"
    "  }\n"
    "  float w = oPos.w;\n"
    "  if (!isnan(w)) {\n"
    "    w = w > 0.0 || floatBitsToUint(w) == 0u ?\n"
    "        clamp(w, uintBitsToFloat(0x1F800000u), uintBitsToFloat(0x5F800000u)) :\n"
    "        clamp(w, uintBitsToFloat(0xDF800000u), uintBitsToFloat(0x9F800000u));\n"
    "  }\n"
    "  vec2 scr = oPos.xy;\n"
    "%s"
    "  bool ptCull = false;\n"
    "  if (pc.mvp[2].y != 0.0) {\n"
    "    ptS = floor(ptS * 8.0) / 8.0;\n"
    "    bool p2 = (floatBitsToUint(ptS) & 0x7FFFFFu) == 0u;\n"
    "    if (ptS > 1.0) gl_PointSize = ptS + (p2 ? 0.0 : 0.03125);\n"
    "    else if (ptS <= 0.0) scr += vec2(0.03125);\n"
    "    else if (ptS < 1.0) {\n"
    "      vec2 pp = floor(scr) + 0.5, dd = pp - scr;\n"
    "      float h = ptS * 0.5;\n"
    "      ptCull = any(lessThan(dd, vec2(-h))) || (p2 ? any(greaterThanEqual(dd, vec2(h))) : any(greaterThan(dd, vec2(h))));\n"
    "      scr = pp;\n"
    "    }\n"
    "  }\n"
    "  if (pc.mvp[2].y != 0.0 && pc.mvp[0].y > pc.mvp[0].x &&\n"
    "      (int(pc.mvp[0].z) & 9) == 1 && (oPos.z < pc.mvp[0].x || oPos.z > pc.mvp[0].y)) ptCull = true;\n"
    "  if (pc.mvp[2].y == 2.0) scr.x += 0.03125;\n"
    "  if (pc.mvp[2].y != 0.0 && pc.mvp[1].x >= 1.0 && gl_PointSize <= 1.0)\n"
    "    scr = ceil(scr) - 0.5;\n"
    "  vScr = vec3(scr, w);\n"
    "%s"
    "  vec2 ndc = scr * pc.vp + vec2(-1.0, 1.0);\n"
    "%s"
    "  gl_Position = vec4(ndc * w, (fl & %uu) != 0u ? 0.0 : zN * w, w);\n"
    "%s%s"
    "  if (ptCull) gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);\n"
    "  vW = w;\n"
    "}\n";

/* ---- the fixed function's vertex stage (ffhw.h) -------------------------------------
 * ffhw.c's stand-in form (plain floats, the lights as one loop) of the key in
 * words 64 - 75 (HleFfhwKey's bytes): skinning, the normal, texture coordinate
 * generation and matrices, the lights on both sides, the colours, the fog
 * coordinate, the point size, the screen position. */
static const char k_ff_lib[] =
    "const int CC = %d, CMV = %d, CIMV = %d, CTM = %d, CTG = %d, CVP = %d, CFP = %d, CEYE = %d;\n"
    "const int CAMB = %d, CEMI = %d, CPAR = %d, CPT = %d, CL = %d, CSP = %d, CBAMB = %d, CBEMI = %d;\n"
    "const int CBSP = %d, CBL = %d, LV = %d, LAMB = %d, LRNG = %d, LHALF = %d, LDIR = %d, LFALL = %d;\n"
    "const int LSPOT = %d, LPOS = %d, LATT = %d;\n"
    "uint kb(uint i) { return (cw(%uu + (i >> 2)) >> (8u * (i & 3u))) & 255u; }\n"
    "vec4 XF(vec4 v, int b) { return vec4(dot(v, c[b]), dot(v, c[b + 1]), dot(v, c[b + 2]), dot(v, c[b + 3])); }\n"
    "vec3 srgb(uint s) { return s == 1u ? V[3].rgb : s == 2u ? V[4].rgb : vec3(1.0); }\n"
    "vec4 tPos;\n"
    "vec3 tN, eyeV;\n"
    /* one side's colours (ffhw.c light_side, light_loop) */
    "void side(bool back, out vec4 d0, out vec4 d1) {\n"
    "  uint sb = back ? 44u : 12u;\n"
    "  uint s0 = kb(sb), s1 = kb(sb + 1u), s2 = kb(sb + 2u), s3 = kb(sb + 3u);\n"
    "  vec3 N = back ? -tN : tN;\n"
    "  int amb = back ? CBAMB : CAMB, emi = back ? CBEMI : CEMI, spc = back ? CBSP : CSP;\n"
    "  float a = s2 == 1u ? V[3].a : s2 == 2u ? V[4].a : c[amb].w;\n"
    "  if (s0 == 0u || s1 == 0u) {\n"
    "    d0 = vec4(c[amb].rgb, a);\n"
    "    uint add = s0 != 0u ? s0 : s1;\n"
    "    if (add != 0u) d0.rgb += srgb(add) * c[emi].rgb;\n"
    "  } else {\n"
    "    d0 = vec4(srgb(s0) + srgb(s1) * c[emi].rgb, a);\n"
    "  }\n"
    "  d1 = vec4(0.0, 0.0, 0.0, V[4].a);\n"
    "  bool le = kb(7u) != 0u;\n"
    "  [[dont_unroll]] for (int li = 0; li < 8; li++) {\n"
    "    uint t = kb(16u + uint(li)) & 3u;\n"
    "    if (t == 0u) continue;\n"
    "    int L = CL + LV * li, lc = back ? CBL + 3 * li : L + LAMB;\n"
    "    float att = 1.0, nl, nh;\n"
    "    bool lit = true;\n"
    "    if (t == 1u) {\n"
    "      vec3 Ld = normalize(c[L + LDIR].xyz);\n"
    "      nl = max(dot(N, Ld), 0.0);\n"
    "      nh = le ? max(dot(N, normalize(Ld + eyeV)), 0.0) : max(dot(N, c[L + LHALF].xyz), 0.0);\n"
    "    } else {\n"
    "      vec3 VP = c[L + LPOS].xyz - tPos.xyz / tPos.w;\n"
    "      float d = length(VP);\n"
    "      lit = d <= c[L + LRNG].x;\n"
    "      VP = normalize(VP);\n"
    "      vec3 at = c[L + LATT].xyz;\n"
    "      att = 1.0 / (at.x + at.y * d + at.z * d * d);\n"
    "      nl = max(dot(N, VP), 0.0);\n"
    "      nh = max(dot(N, normalize(VP + eyeV)), 0.0);\n"
    "      if (t == 3u) {\n"
    "        vec4 sd = c[L + LSPOT];\n"
    "        float s = dot(sd.xyz, VP) + sd.w;\n"
    "        vec3 fo = c[L + LFALL].xyz;\n"
    "        float dn = fo.y * min(s, 1.0) + fo.z;\n"
    "        if (s <= 0.0) att = 0.0;\n"
    "        else if (dn == 0.0) att *= min(s, 1.0) + fo.x > 0.0 ? " HLE_SPOT_DIV0 " : 0.0;\n"
    "        else if (s < 1.0) {\n"
    "          att *= s <= -fo.x ? 0.0 :\n"
    "                 dn > 0.0 ? clamp((s + fo.x) / dn, 0.0, 1.0) : 0.0;\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    if (lit) {\n"
    "      vec3 sc = c[spc].xyz;\n"
    "      float dn = sc.y * nh + sc.z;\n"
    "      float pf = nl <= 0.0 || nh <= -sc.x ? 0.0 : dn == 0.0 ? 10000.0 :\n"
    "                 dn > 0.0 ? clamp((nh + sc.x) / dn, 0.0, 10000.0) : 0.0;\n"
    "      d0.rgb += srgb(s1) * c[lc].rgb * att;\n"
    "      d0.rgb += srgb(s2) * c[lc + 1].rgb * (att * nl);\n"
    "      d1.rgb += srgb(s3) * c[lc + 2].rgb * (att * pf);\n"
    "    }\n"
    "  }\n"
    "}\n";

static const char k_ff_body[] =
    "  vec4 pos = V[0], n4 = vec4(V[2].xyz, 0.0);\n"
    "  uint sk = kb(4u);\n"
    "  if (sk == 0u || sk > 6u) {\n"
    "    tPos = XF(pos, CMV);\n"
    "    tN = XF(n4, CIMV).xyz;\n"
    "  } else {\n"                      /* skinning: 1 2G 2 2 3 3G ... (ffhw.c k_skin) */
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
    "  vec3 view = kb(42u) != 0u ? vec3(0.0, 0.0, 1.0) : normalize(tPos.xyz);\n"
    "  for (int s = 0; s < 4; s++) {\n"
    "    vec4 t = V[9 + s];\n"
    "    for (int j = 0; j < 4; j++) {\n"
    "      uint g = kb(24u + uint(4 * s + j));\n"
    "      int pl = CTG + 4 * s + j;\n"
    "      if (g == 1u) t[j] = dot(c[pl], tPos);\n"
    "      else if (g == 2u) t[j] = dot(c[pl], pos);\n"
    "      else if (g == 3u && j < 2) { vec3 r = reflect(view, tN); t[j] = r[j] / (2.0 * length(r + vec3(0.0, 0.0, 1.0))) + 0.5; }\n"
    "      else if (g == 4u && j < 3) t[j] = reflect(view, tN)[j];\n"
    "      else if (g == 5u && j < 3) t[j] = tN[j];\n"
    "    }\n"
    "    if (((kb(40u) >> uint(s)) & 1u) != 0u) t = XF(t, CTM + 4 * s);\n"
    "    O[9 + s] = t;\n"
    "  }\n"
    "  uint lt = kb(5u);\n"
    "  bool two = kb(43u) != 0u;\n"
    "  vec4 fD0 = V[3], fD1 = V[4], fB0 = vec4(0.0, 0.0, 0.0, 1.0), fB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "  if (lt != 0u) {\n"
    "    eyeV = kb(7u) != 0u ? normalize(c[CEYE].xyz / c[CEYE].w - tPos.xyz / tPos.w) : vec3(0.0, 0.0, -1.0);\n"
    "    side(false, fD0, fD1);\n"
    "    if (two) side(true, fB0, fB1);\n"
    "    else { fB0 = fD0; fB1 = fD1; }\n"
    "  }\n"
    /* the colours after (ffhw.c colours) */
    "  bool sep = kb(8u) != 0u;\n"
    "  if (!sep && lt != 0u) { fD0.rgb += fD1.rgb; if (two) fB0.rgb += fB1.rgb; }\n"
    "  if (kb(10u) == 0u) {\n"
    "    fD1 = vec4(0.0, 0.0, 0.0, 1.0); fB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "  } else {\n"
    "    if (!sep) { fD1 = V[4]; if (lt != 0u && two) fB1 = V[4]; }\n"
    "    if (kb(9u) == 0u) { fD1.a = 1.0; fB1.a = 1.0; }\n"
    "  }\n"
    "  O[3] = fD0; O[4] = fD1; O[7] = fB0; O[8] = fB1;\n"
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

/* the outputs (vsh.c tail) */
static void vs_tail(Buf *b)
{
    put(b, k_vs_tail, HLE_UBER_V_SPECOFF, HLE_UBER_V_SPECA1, HLE_UBER_V_SPECOFF, HLE_UBER_V_SPECA1,
        HLE_VSH_FOG_NV_CARRY, HLE_VSH_FOG_NV, HLE_VSH_FOG_NV_RAD, HLE_VSH_FOG_NV_ABS, HLE_VSH_FOG_NV_LIN,
        HLE_UBER_V_PTFIXED, HLE_SUBPIXEL_GLSL("scr"), HLE_TEXEL_EDGE_GLSL("scr"), HLE_ZNORM_GLSL("oPos.z", "zN"),
        HLE_UBER_V_WBUF | HLE_UBER_V_ZFREE, "", g_hle_glsl_zw ? HLE_ZW_NUDGE_GLSL : "");
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

size_t hle_uber_ff_vs_glsl(char *buf, size_t cap, unsigned set)
{
    Buf b = { buf, cap, 0, false };
    put(&b, "#version 450\n#extension GL_EXT_control_flow_attributes : enable\n%s", hle_push_glsl());
    put(&b, k_vs_head, set, (unsigned)(HLE_UBER_WORDS / 4));
    put(&b, k_ff_lib, HLE_FFHW_C_COMPOSITE, HLE_FFHW_C_MODELVIEW, HLE_FFHW_C_INVMODELVIEW, HLE_FFHW_C_TEXMAT,
        HLE_FFHW_C_TEXGEN, HLE_FFHW_C_VPOFF, HLE_FFHW_C_FOGPLANE, HLE_FFHW_C_EYE, HLE_FFHW_C_AMBIENT,
        HLE_FFHW_C_EMISSION, HLE_FFHW_C_PARAMS, HLE_FFHW_C_POINT, HLE_FFHW_C_LIGHTS, HLE_FFHW_C_SPEC,
        HLE_FFHW_C_BACK_AMBIENT, HLE_FFHW_C_BACK_EMISSION, HLE_FFHW_C_BACK_SPEC, HLE_FFHW_C_BACK_LIGHTS,
        HLE_FFHW_LIGHT_VEC4, HLE_FFHW_L_AMBIENT, HLE_FFHW_L_RANGE, HLE_FFHW_L_HALF, HLE_FFHW_L_DIRECTION,
        HLE_FFHW_L_FALLOFF, HLE_FFHW_L_SPOT, HLE_FFHW_L_POSITION, HLE_FFHW_L_ATTENUATION,
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
 * psh.c's stand-in form (no NVCOL, no ZWORD) for the keys hle_uber_ps_ok takes:
 * the colours as the NV2A's bytes, the fog factor by its mode, the four stages
 * by their mode with the alpha kill and the colour key, the combiners and the
 * final one on a register file (psh.c reg: 0 zero, 1 C0, 2 C1, 3 fog, 4 v0,
 * 5 v1, 8 - 11 t0 - t3, 12 r0, 13 r1, the final combiner's 14 v1 + r0 and
 * 15 E F), the byte rounding and the alpha test on bytes. */

static const char k_fs_head[] =
    "#extension GL_EXT_nonuniform_qualifier : require\n"
    "#extension GL_EXT_control_flow_attributes : enable\n"
    "%s%s"
    "layout(set = %u, binding = 0, std140) uniform UberCfg { uvec4 u[%u]; };\n"
    "layout(set = 0, binding = 0) uniform texture2D uTex2D[];\n"
    "layout(set = 0, binding = 1) uniform sampler uSmp[];\n"
    "layout(set = 0, binding = 2) uniform textureCube uTexCube[];\n"
    "layout(set = 0, binding = 3) uniform texture3D uTex3D[];\n"
    HLE_CTE_GLSL HLE_CTE3I_GLSL
    "layout(location = 0) in vec4 vDiffuse;\n"
    "layout(location = 1) in vec4 vSpecular;\n"
    "layout(location = 2) in vec4 vTex0;\n"
    "layout(location = 3) in vec4 vTex1;\n"
    "layout(location = 4) in vec4 vTex2;\n"
    "layout(location = 5) in vec4 vTex3;\n"
    "layout(location = 6) in float vFog;\n"
    "layout(location = 7) in vec4 vBack0;\n"
    "layout(location = 8) in vec4 vBack1;\n"
    "layout(location = 18) flat in vec4 vDiffuseF;\n"
    "layout(location = 19) flat in vec4 vSpecularF;\n"
    "layout(location = 20) flat in vec4 vBack0F;\n"
    "layout(location = 21) flat in vec4 vBack1F;\n"
    "%s"
    "layout(location = 0) out vec4 oColor;\n"
    HLE_DITHER_GLSL
    "uint cw(uint i) { return u[i >> 2][i & 3u]; }\n"
    "vec4 nvFC;\n"
    "float nvc8(float x) {\n"
    "  uint v = floatBitsToUint(x);\n"
    "  int e = int((v >> 23) & 0xffu);\n"
    "  if ((v >> 31) != 0u || e < 0x76) return 0.0;\n"
    "  if (e >= 0x7f) return 255.0;\n"
    "  uint fr = (((v >> 10) & 0x1fffu) | 0x2000u) << uint(e - 0x74);\n"
    "  return float(min((((fr * 0xffu) >> 23) + 1u) >> 1, 255u));\n"
    "}\n"
    "vec4 NVC8(vec4 v) { return vec4(nvc8(v.x), nvc8(v.y), nvc8(v.z), nvc8(v.w)); }\n"
    "vec4 col(uint c) { return unpackUnorm4x8(c).zyxw; }\n"
    HLE_NVPROJ_GLSL
    /* psh.c sampx as a stand-in has it (no ties, no rectangle, no border) */
    HLE_NVRECT_GLSL
    "uint gVb;\n"                       /* the key's vol (HLE_PSH_RECT, HLE_PSH_RECT4) */
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
    "vec4 samp(uint s, vec2 uv, bool bump) {\n"
    "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0x1FFu;\n"
    "  uv = rect(s, uv);\n"
    "  if ((pc.flags & (1u << s)) != 0u)\n"
    "    uv /= vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)) /\n"
    "          ((pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
    "  if (dFdx(nvFC.w) == 0.0 && dFdy(nvFC.w) == 0.0) {\n"
    "    vec2 qs = vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0));\n"
    "    vec2 qt = uv * qs, qr = floor(qt + 0.5);\n"
    "    uv = mix(uv, qr / qs, lessThan(abs(qt - qr), vec2(bump ? 1.0 / 4096.0 : 1.0 / 65536.0)));\n"
    "  }\n"
    "  uv = nvcte2i(pc.tex[s], uv, ti, dFdx(uv), dFdy(uv));\n"
    "  return texture(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv);\n"
    "}\n"
    /* the signed channels (psh.c sfun, signed_stage): converted, then filtered, as the
     * texture unit does */
    "vec4 sgn4(vec4 c) {\n"
    "  vec4 v = floor(c * 255.0 + 0.5);\n"
    "  return max(mix(v, v - 256.0, greaterThanEqual(v, vec4(128.0))) / 127.5, -1.0);\n"
    "}\n"
    "ivec4 sbi(vec4 c, bool fl) { ivec4 v = ivec4(floor(c * 255.0 + 0.5)); return fl ? v - 128 : v - 256 * (v >> 7); }\n"
    "float ibil(ivec4 c, ivec2 q) {\n"
    "  int t = (c.w * (256 - q.x) + c.z * q.x + 128) >> 8;\n"
    "  int b = (c.x * (256 - q.x) + c.y * q.x + 128) >> 8;\n"
    "  return max(float((t * (256 - q.y) + b * q.y + 128) >> 8) / 127.5, -1.0); }\n"
    "vec4 samps(uint s, vec2 uv, vec4 f, bvec4 m, bool pt, bool fl) {\n"
    "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0x1FFu;\n"
    "  uv = rect(s, uv);\n"
    "  if ((pc.flags & (1u << s)) != 0u)\n"
    "    uv /= vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)) /\n"
    "          ((pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
    "  uv = nvcte2i(pc.tex[s], uv, ti, dFdx(uv), dFdy(uv));\n"
    "  if (pt || textureQueryLevels(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)])) > 1)\n"
    "    return mix(f, fl ? clamp((f * 255.0 - 128.0) / 127.5, -1.0, 1.0) : sgn4(f), m);\n"
    "  vec2 w = fract(uv * vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)) - 0.5);\n"
    "  ivec2 q = ivec2(floor(w * 256.0));\n"
    "  ivec4 r = sbi(textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 0), fl);\n"
    "  ivec4 g = sbi(textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 1), fl);\n"
    "  ivec4 b = sbi(textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 2), fl);\n"
    "  ivec4 a = sbi(textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 3), fl);\n"
    "  return mix(f, vec4(ibil(r, q), ibil(g, q), ibil(b, q), ibil(a, q)), m);\n"
    "}\n"
    /* PSDotMapping of a texel to a vector (psh.c dmap; HILO_1 as the halves) */
    "vec3 dmap(uint m, vec4 c) {\n"
    "  if (m == 0u) return c.rgb;\n"
    "  if (m == 4u) { uvec4 i = uvec4(c * 255.0 + 0.5);\n"
    "    return vec3(float(i.a << 8 | i.r), float(i.g << 8 | i.b), 65535.0) / 65535.0; }\n"
    "  if (m == 3u) { vec3 v = floor(c.rgb * 255.0 + 0.5);\n"
    "    return mix(v, v - 256.0, greaterThanEqual(v, vec3(128.0))) / 127.0; }\n"
    "  if (m >= 5u) { uvec4 i = uvec4(c * 255.0 + 0.5);\n"
    "    ivec2 hs = ivec2(int(i.a << 8 | i.r), int(i.g << 8 | i.b));\n"
    "    hs = hs - ivec2(greaterThanEqual(hs, ivec2(32768))) * 65536;\n"
    "    vec2 h = clamp(vec2(hs) / 32767.0, -1.0, 1.0);\n"
    "    return vec3(h, sqrt(max(1.0 - dot(h, h), 0.0))); }\n"
    "  vec3 b = floor(c.rgb * 255.0 + 0.5);\n"
    "  if (m == 1u) return (b - 128.0) / 127.0;\n"
    "  return mix((b + 0.5) / 127.5, (b - 255.5) / 127.5, greaterThanEqual(b, vec3(128.0)));\n"
    "}\n"
    /* bytes as two's complement over 128 (bump offsets) */
    "vec2 sgn8(vec2 c) {\n"
    "  vec2 v = floor(c * 255.0 + 0.5);\n"
    "  return mix(v, v - 256.0, greaterThanEqual(v, vec2(128.0))) / 128.0;\n"
    "}\n"
    "vec4 samp3(uint s, vec3 uvw) {\n"
    "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0x1FFu;\n"
    "  return texture(sampler3D(uTex3D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), nvcte3i(pc.tex[s], uvw, ti, dFdx(uvw), dFdy(uvw)));\n"
    "}\n"
    "vec4 sampc(uint s, vec3 dir) {\n"
    "  if (dir == vec3(0.0)) return vec4(0.0, 0.0, 0.0, 1.0);\n"
    "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0x1FFu;\n"
    "  return texture(samplerCube(uTexCube[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), dir);\n"
    "}\n"
    /* psh.c's: a float depth word; the other kind of texture than a mode fetches (HLE_PSH_TKIND) */
    "float nvfw16(float z) {\n"
    "  if (!(z > 0.0)) return 0.0;\n"
    "  if (z >= 511.9375) return 65535.0;\n"
    "  uint b = floatBitsToUint(z);\n"
    "  int e = int(b >> 23) - 120;\n"
    "  return float(e >= 1 ? (uint(e) << 12) | ((b >> 11) & 0xfffu) : 0u);\n"
    "}\n"
    "float nvfw24(float z) {\n"
    "  if (!(z > 0.0)) return 0.0;\n"
    "  return float(min(floatBitsToUint(z) >> 7, 0xfeffffu));\n"
    "}\n"
    "vec2 cube2d(vec3 d) {\n"
    "  vec3 a = abs(d);\n"
    "  if (a.x > a.y && a.x > a.z) return vec2(d.x > 0.0 ? -d.z : d.z, d.y) / a.x;\n"
    "  if (a.y > a.x && a.y > a.z) return vec2(d.x, d.y > 0.0 ? -d.z : d.z) / a.y;\n"
    "  return vec2(d.z > 0.0 ? d.x : -d.x, d.y) / a.z;\n"
    "}\n"
    "vec4 samp2(uint s, vec2 uv) {\n"
    "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0x1FFu;\n"
    "  return texture(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv);\n"
    "}\n"
    /* a shadow buffer (psh.c shadow_sample): the stored depth against r by the function,
     * the four results weighted (or one texel's, point filtered); shw: key's shadow,
     * shadow_fmt << 8; pf: the alpha kill's high bits (a 24-bit float buffer) */
    "vec4 shsamp(uint s, vec4 tc, bool p3, uint shw, uint pf, bool point) {\n"
    "  uint fn = (shw >> 4) & 7u, fmt = (shw >> (8u + 2u * s)) & 3u;\n"
    "  if (fn == 0u || fn == 7u) return vec4(fn != 0u ? 1.0 : 0.0);\n"
    "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0x1FFu;\n"
    "  vec3 pq = nvproj(tc);\n"
    "  vec2 uv = pq.xy;\n"
    "  vec2 sz = vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0));\n"
    "  if ((pc.flags & (1u << s)) != 0u)\n"
    "    uv /= sz / ((pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
    "  bool f24 = ((pf >> (16u + s)) & 1u) != 0u;\n"
    "  float m = fmt == 1u ? 16777215.0 : fmt == 2u ? 65535.0 :\n"
    "            fmt == 3u ? (f24 ? uintBitsToFloat(0x7f7fffffu) : 511.9375) : 1.0;\n"
    "  float r = p3 ? clamp(pq.z, 0.0, m) : 0.0;\n"
    "  if (fmt == 1u || fmt == 2u) r = floor(r);\n"
    "  else if (fmt == 3u) r = f24 ? nvfw24(r) : nvfw16(r);\n"
    "  vec4 tx = vec4(0.0);\n"
    "  if (point) {\n"
    "    vec2 uq = uv * sz, ur = floor(uq + 0.5);\n"
    "    if (dFdx(nvFC.w) == 0.0 && dFdy(nvFC.w) == 0.0)\n"
    "      uq = mix(uq, ur, lessThan(abs(uq - ur), vec2(1.0 / 4096.0)));\n"
    "    tx = texelFetch(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), clamp(ivec2(floor(uq)), ivec2(0), ivec2(sz) - 1), 0);\n"
    "  }\n"
    "  bool wd = (shw & 0x80u) != 0u;\n"
    "  vec4 d;\n"
    "  if (!wd || fmt == 2u || (fmt == 3u && !f24)) {\n"
    "    vec4 c0 = point ? vec4(tx.r) : textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 0);\n"
    "    d = !wd ? (fmt != 3u ? c0 * m : round(c0 * (f24 ? 16777216.0 : 65536.0))) : round(c0 * 65535.0);\n"
    "  } else {\n"
    "    vec4 c1 = point ? vec4(tx.g) : textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 1);\n"
    "    vec4 c2 = point ? vec4(tx.b) : textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 2);\n"
    "    vec4 c3 = point ? vec4(tx.a) : textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 3);\n"
    "    d = round(c3 * 255.0) * 65536.0 + round(c2 * 255.0) * 256.0 + round(c1 * 255.0);\n"
    "  }\n"
    "  vec4 rr = vec4(r);\n"
    "  vec4 q = vec4(fn == 1u ? lessThan(d, rr) : fn == 2u ? equal(d, rr) : fn == 3u ? lessThanEqual(d, rr) :\n"
    "                fn == 4u ? greaterThan(d, rr) : fn == 5u ? notEqual(d, rr) : greaterThanEqual(d, rr));\n"
    "  if (point) return vec4(q.x);\n"
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
    "}\n"
    "%s";

static const char k_fs_main[] =
    "void main() {\n"
    "  nvFC = HLE_REAL_FC_;\n"
    "  bool aak = false;\n"
    "  if ((pc.flags & 4096u) != 0u && pc.mvp[1].x >= 1.0) {\n"
    "    ivec2 ms = ivec2(pc.mvp[1].xy), mp = ivec2(nvFC.xy);\n"
    "    aak = mp - (mp / ms) * ms != ivec2(pc.mvp[1].zw);\n"
    "    nvFC.xy = vec2(mp / ms) + 0.5;\n"
    "  }\n"
    "  uint pf = cw(51u), cnt = cw(42u), tm = cw(43u), cmp = cw(46u), vb = pf >> 20, shw = cw(52u);\n"
    "  uint dm = cw(44u), itx = cw(45u), tsg = dm >> 16, sgm = 0u;\n"
    "  gVb = vb;\n"
    "  bool fl = (pf & 0x800u) != 0u, bk = (vb & %uu) != 0u && !gl_FrontFacing;\n"
    "  vec4 v0 = fl ? (bk ? vBack0F : vDiffuseF) : (bk ? vBack0 : vDiffuse);\n"
    "  vec4 v1 = fl ? (bk ? vBack1F : vSpecularF) : (bk ? vBack1 : vSpecular);\n"
    "  v0 = NVC8(v0) / 255.0; v1 = NVC8(v1) / 255.0;\n"
    "%s"
    /* the fog factor (psh.c: 0 none, 1 the vertex's, 2 EXP, 3 EXP2, 4 LINEAR, 5 - 8 the
     * NV2A's table on the vertex stage's value) */
    "  uint fm = (pf >> 4) & 15u;\n"
    "  float fd = (pf & 0x100u) != 0u ? abs(vFog) : vFog;\n"
    "  float fogf = 1.0;\n"
    "  if (fm == 1u) fogf = isnan(vFog) ? 1.0 : clamp(vFog, 0.0, 1.0);\n"
    "  else if (fm == 2u) fogf = pc.fog.x + exp(-fd * pc.fog.z);\n"
    "  else if (fm == 3u) { float e = fd * pc.fog.z; fogf = pc.fog.x + exp(-e * e); }\n"
    "  else if (fm == 4u) fogf = (pc.fog.y - fd) / (pc.fog.y - pc.fog.x);\n"
    "  else if (fm == 5u) fogf = vFog <= 1.0 ? 0.0 : vFog >= 2.0 ? 1.0 :\n"
    "      float((int(floor((vFog - 1.0) * 4096.0 + 0.01)) * 255 + 256) >> 12) / 255.0;\n"
    "  else if (fm >= 6u) {\n"
    "    fogf = vFog >= 2.0 && fm == 6u ? 1.0 : 0.0;\n"
    "    if (vFog > 1.0 && vFog < 2.0) {\n"
    "      int ff = int(floor((vFog - 1.0) * 4096.0 + 0.01)), fw = ff & 63, fi = (ff >> 6) + 65 * int(fm - 6u);\n"
    "      fogf = float((fogk[fi] * (64 - fw) + fogk[fi + 1] * fw + 32) >> 6) / 255.0;\n"
    "    }\n"
    "  }\n"
    "  if (fm >= 2u && fm <= 4u) {\n"
    "    bool one = fm == 4u || (fm == 2u && (pf & 0x100u) == 0u);\n"
    "    fogf = isinf(vFog) || isnan(fogf) ? (one ? 1.0 : 0.0) : clamp(fogf, 0.0, 1.0);\n"
    "  }\n"
    "  if (fm == 1u || fm == 4u) fogf = floor(fogf * 255.0 + 1.0 / 1024.0) / 255.0;\n"
    /* the stages by their mode (psh.c: NONE, PROJECT2D, PROJECT3D, CUBEMAP, PASSTHRU,
     * CLIPPLANE), each fetch's alpha kill and colour key after it */

    "  float dt1 = 0.0, dt2 = 0.0, dt3 = 0.0;\n"
    "  [[dont_unroll]] for (uint s = 0u; s < 4u; s++) {\n"
    "    uint m = (tm >> (5u * s)) & 31u;\n"
    "    vec4 tc = s == 0u ? vTex0 : s == 1u ? vTex1 : s == 2u ? vTex2 : vTex3, t = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    bool tk = ((cmp >> (16u + s)) & 1u) != 0u;\n"
    "    uint ins = s == 3u ? (itx >> 20) & 3u : s == 2u ? (itx >> 16) & 1u : 0u;\n"
    "#if UB_FULL\n"
    /* the dot-product stages: the coordinates dotted with the input stage's texel through
     * PSDotMapping (psh.c dotmode); a reflection's diffuse stage 2 takes stage 3's first */
    "    if (s != 0u && (m == 17u || (m >= 9u && m <= 14u) || m == 18u)) {\n"
    "      float dv = dot(tc.xyz, dmap((dm >> (4u * (s - 1u))) & 7u, gTR(ins)));\n"
    "      if (s == 1u) dt1 = dv; else if (s == 2u) dt2 = dv; else dt3 = dv;\n"
    "    }\n"
    "    if (s == 2u && m == 11u) dt3 = dot(vTex3.xyz, dmap((dm >> 8) & 7u, gTR((itx >> 20) & 3u)));\n"
    "#endif\n"
    "#if UB_FULL\n"
    "    if (((shw >> s) & 1u) != 0u && (m == 1u || m == 2u)) t = shsamp(s, tc, m == 2u, shw, pf, (cmp & %uu) != 0u); else\n"
    "#endif\n"
    "    if (m == 1u && tk) { vec2 q = nvproj(tc).xy; t = sampc(s, normalize(vec3(1.0, q.y, -q.x))); }\n"
    "    else if (m == 1u) t = ((vb >> s) & 1u) != 0u ? samp3(s, vec3(nvproj(tc).xy, 0.0)) : samp(s, nvproj(tc).xy, false);\n"
    "    else if (m == 2u) t = samp3(s, nvproj(tc));\n"
    "    else if (m == 3u) t = tk ? samp2(s, cube2d(tc.xyz)) : sampc(s, tc.xyz);\n"
    "    else if (m == 4u) {\n"
    "      t = clamp(tc, 0.0, 1.0);\n"
    "      t = floor((floor(t * 4096.0 + 1.0 / 1024.0) + 1.0) * 255.0 / 4096.0) / 255.0;\n"
    "    } else if (m == 5u) {\n"
    "      t = vec4(0.0);\n"
    "      for (uint c = 0u; c < 4u; c++) {\n"
    "        bool ge = ((cmp >> (4u * s + c)) & 1u) != 0u;\n"
    "        if (ge ? tc[c] >= 0.0 : tc[c] < 0.0) discard;\n"
    "      }\n"
    "    }\n"
    "#if UB_FULL\n"
    "    else if (m == 6u || m == 7u) {\n"
    /* bump-env (stages 1 - 3): the input stage's (b, g) through the matrix, LUM's scale
     * by its r (psh.c BUMPENVMAP, bump_in) */
    "      uint mi = ((sgm >> ins) & 1u) != 0u ? (tsg >> (4u * ins)) & 12u : 0u;\n"
    "      vec4 ti = gT(ins), tri = gTR(ins);\n"
    "      vec2 sb = mix(ti.bg * 0.99609375, vec2(-1.0), lessThanEqual(ti.bg, vec2(-1.0)));\n"
    "      vec2 bi = mi == 0u ? sgn8(ti.bg) : mi == 12u ? sb : mix(sgn8(tri.bg), sb, bvec2((mi & 8u) != 0u, (mi & 4u) != 0u));\n"
    "      uvec2 mt = s == 1u ? pc.bump.xy : s == 2u ? pc.bump.zw : pc.bump3;\n"
    /* (BUMPENVMAP's values' rest in its bumpl word: psh.c bmat) */
    "      vec4 bh = vec4(unpackHalf2x16(mt.x), unpackHalf2x16(mt.y));\n"
    "      if (m == 6u) {\n"
    "        uint br = pc.bumpl[s - 1u]; ivec4 be; frexp(bh, be);\n"
    "        ivec4 bq = ivec4(int(br << 24u) >> 24, int(br << 16u) >> 24, int(br << 8u) >> 24, int(br) >> 24);\n"
    "        bh += vec4(bq) / 256.0 * ldexp(vec4(1.0), max(be, ivec4(-13)) - 11);\n"
    "      }\n"
    "      vec2 d = mat2(bh.xy, bh.zw) * bi;\n"
    "      t = ((vb >> s) & 1u) != 0u ? samp3(s, vec3(nvproj(tc).xy + d, tc.z)) : samp(s, nvproj(tc).xy + d, true);\n"
    "      if (m == 7u) {\n"
    "        vec2 l = vec2(bitfieldExtract(int(pc.bumpl[s - 1u]), 0, 16), bitfieldExtract(int(pc.bumpl[s - 1u]), 16, 16)) / 4096.0;\n"
    "        float lr = tri.r;\n"
    "        t.rgb = floor(t.rgb * max(l.x * lr + l.y, 0.0) * 255.0 + 1.0 / 1024.0) / 255.0;\n"
    "      }\n"
    "    }\n"
    /* BRDF: the volume at (theta eye, theta light, phi light - phi eye), the two stages
     * before read as HILO (psh.c HLE_PST_BRDF) */
    "    else if (m == 8u && s >= 2u) {\n"
    "      uvec4 ie = uvec4(gT(s - 2u) * 255.0 + 0.5), il = uvec4(gT(s - 1u) * 255.0 + 0.5);\n"
    "      vec2 he = vec2(float(ie.a << 8u | ie.r), float(ie.g << 8u | ie.b)) / 65535.0;\n"
    "      vec2 hl = vec2(float(il.a << 8u | il.r), float(il.g << 8u | il.b)) / 65535.0;\n"
    "      t = samp3(s, vec3(he.x, hl.x, hl.y - he.y));\n"
    "    }\n"
    /* the dot-product and dependent modes (psh.c: DOT_ZW as PROJECT2D) */
    "    else if (m == 9u) t = samp(s, s == 2u ? vec2(dt1, dt2) : vec2(dt2, dt3), false);\n"
    "    else if (m == 11u || m == 14u) {\n"
    "      vec3 nv = vec3(dt1, dt2, dt3);\n"
    "      t = tk ? samp2(s, cube2d(nv)) : sampc(s, nv);\n"
    "    } else if (m == 12u || m == 18u) {\n"
    "      vec3 nv = vec3(dt1, dt2, dt3), e = m == 18u ? uintBitsToFloat(pc.bump.xyz) : vec3(vTex1.w, vTex2.w, vTex3.w);\n"
    "      vec3 rv = 2.0 * nv * dot(nv, e) / max(dot(nv, nv), 1e-8) - e;\n"
    "      t = tk ? samp2(s, cube2d(rv)) : sampc(s, rv);\n"
    "    } else if (m == 13u) t = tk ? samp2(s, vec2(dt1, dt2)) : samp3(s, vec3(dt1, dt2, dt3));\n"
    "    else if (m == 15u || m == 16u) {\n"
    "      vec4 ti = gTR(ins);\n"
    "      uint xi = pc.tex[s] & 0xFFFFu, xs = (pc.tex[s] >> 16) & 0x1FFu;\n"
    "      t = texture(sampler2D(uTex2D[nonuniformEXT(xi)], uSmp[nonuniformEXT(xs)]), nvcte2i(pc.tex[s], m == 15u ? ti.ar : ti.gb, "
    "xi, dFdx(m == 15u ? ti.ar : ti.gb), dFdy(m == 15u ? ti.ar : ti.gb)));\n"
    "    } else if (m == 17u) t = vec4(0.0);\n"
    "    else if (m == 8u || m == 10u) t = samp(s, nvproj(tc).xy, false);\n"
    /* a signed stage: its texel as sampled kept (tr), the channels the texture unit gives
     * signed converted (psh.c signed_stage) */
    "#endif\n"
    "    vec4 tr = t;\n"
    "#if UB_FULL\n"
    "    uint sg = (tsg >> (4u * s)) & 15u;\n"
    "    if (sg != 0u && m != 0u && m != 4u && m != 5u && m != 17u && !(((shw >> s) & 1u) != 0u && (m == 1u || m == 2u))) {\n"
    "      sgm |= 1u << s;\n"
    "      bvec4 mk = bvec4((sg & 2u) != 0u, (sg & 4u) != 0u, (sg & 8u) != 0u, (sg & 1u) != 0u);\n"
    "      bool fl = ((itx >> s) & 1u) != 0u, pl = m == 1u && ((vb >> s) & 1u) == 0u, pt = ((dm >> (12u + s)) & 1u) != 0u;\n"
    "      if (fl) tr = mix(t, mod(floor(t * 255.0 + 0.5) + 128.0, 256.0) / 255.0, mk);\n"
    "      if (pl) t = samps(s, nvproj(tc).xy, t, mk, pt, fl);\n"
    "      else t = mix(t, fl ? clamp((t * 255.0 - 128.0) / 127.5, -1.0, 1.0) : sgn4(t), mk);\n"
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
    /* the bias's half: 0x80 / 0xFF (psh.c) */
    "  uint n = min(cnt & 0xFFu, 8u);\n"
    "  hb = 128.0 / 255.0;\n"
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
    /* (the registers' steps of 1 / 255, halves up: psh.c stage) */
    "    ab = floor(ab * 255.0 + (0.5 + 1.0 / 1024.0)) / 255.0; cd = floor(cd * 255.0 + (0.5 + 1.0 / 1024.0)) / 255.0;\n"
    "    ms = floor(ms * 255.0 + (0.5 + 1.0 / 1024.0)) / 255.0;\n"
    "    aab = floor(aab * 255.0 + (0.5 + 1.0 / 1024.0)) / 255.0; acd = floor(acd * 255.0 + (0.5 + 1.0 / 1024.0)) / 255.0;\n"
    "    ams = floor(ams * 255.0 + (0.5 + 1.0 / 1024.0)) / 255.0;\n"
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
    "    vec3 cv1 = clamp(rV1.rgb, 0.0, 1.0), cr0 = clamp(rR0.rgb, 0.0, 1.0);\n"
    "    rVR = vec4(((fe & 0x40u) != 0u ? 1.0 - cv1 : cv1) + ((fe & 0x20u) != 0u ? 1.0 - cr0 : cr0), 0.0);\n"
    "    if ((fe & 0x80u) != 0u) rVR = clamp(rVR, 0.0, 1.0);\n"
    "    float su = (fe & 0x80u) != 0u ? 1.0 : 2.0;\n"
    "    vec3 A = clamp(inr(fa >> 24, true), 0.0, ((fa >> 24) & 15u) == 14u ? su : 1.0);\n"
    "    vec3 B = clamp(inr(fa >> 16, true), 0.0, ((fa >> 16) & 15u) == 14u ? su : 1.0);\n"
    "    vec3 C = clamp(inr(fa >> 8, true), 0.0, ((fa >> 8) & 15u) == 14u ? su : 1.0);\n"
    "    vec3 D = clamp(inr(fa, true), 0.0, (fa & 15u) == 14u ? su : 1.0);\n"
    "    float Gf = clamp(ina(fe >> 8, true), 0.0, 1.0);\n"
    "    outc = vec4(A * B + (1.0 - A) * C + D, Gf);\n"
    "  }\n"
    "  outc = clamp(outc, 0.0, 1.0);\n"
    "  outc = floor(outc * 255.0 + 0.5) / 255.0;\n"
    "  if ((pf & 1u) != 0u) {\n"
    "    uint af = (pf >> 1) & 7u;\n"
    "    float a = floor(outc.a * 255.0 + 0.5), r = floor(pc.alpha_ref * 255.0 + 0.5);\n"
    "    bool ok = af == 1u ? a < r : af == 2u ? a == r : af == 3u ? a <= r : af == 4u ? a > r :\n"
    "              af == 5u ? a != r : af == 6u ? a >= r : af == 7u;\n"
    "    if (!ok) discard;\n"
    "  }\n"
    "  oColor = (pf & 0x200u) != 0u ? outc.bgra : outc;\n"
    "  uint dth = cw(53u);\n"
    "  if (dth != 0u) oColor.rgb = nvdither(oColor.rgb, vec3(1.0 / 31.0, dth == 1u ? 1.0 / 63.0 : 1.0 / 31.0, 1.0 / 31.0));\n"
    "%s"
    "  if (aak) discard;\n"
    "}\n";

size_t hle_uber_fs_glsl(char *buf, size_t cap, unsigned set, unsigned depth, bool full)
{
    /* the NV2A's fog tables (psh.c fogk): EXP, EXP_ABS, EXP2, 65 points each */
    char tab[1400];
    size_t t = (size_t)snprintf(tab, sizeof(tab), "const int fogk[195] = int[195](");
    for (int m = 6; m <= 8; m++) {
        for (int i = 0; i <= 64; i++) {
            const double d = (64.0 * i - 2048.0) / 256.0;
            const double g = m == 6 ? (d < 0.0 ? exp2(d) : 1.0) : m == 7 ? exp2(-fabs(d)) : exp2(-d * d / 8.0);
            t += (size_t)snprintf(tab + t, t < sizeof(tab) ? sizeof(tab) - t : 0, "%d%s", (int)(g * 255.0 + 0.5),
                                  m == 8 && i == 64 ? ");\n" : ", ");
        }
    }
    if (t >= sizeof(tab)) {
        return 0;
    }
    static const char *const k_depth[5] = {
        "",
        /* HLE_PSH_ZTRUNC: cut to the buffer's steps, the offset before the cut */
        "  { float zm = 1.0 / pc.zscale - 1.0;\n"
        "    float zo = uintBitsToFloat(pc.psc[18]) + uintBitsToFloat(pc.psc[19]) *\n"
        "        max(abs(dFdx(nvFC.z)), abs(dFdy(nvFC.z))) * zm;\n"
        "    gl_FragDepth = clamp(floor(nvFC.z * zm + zo) / zm, 0.0, 1.0); }\n",
        /* HLE_PSH_ZF16: no word under 2^-6 */
        "  gl_FragDepth = nvFC.z < 0.015625 * pc.zscale ? 0.0 : nvFC.z;\n",
        /* W-buffering (psh.c: the interpolated w, or the corners' by their barycentrics) */
        HLE_WBUF_STORE_GLSL,
        HLE_WBUF_STORE_GLSL,
    };
    const unsigned dv = depth < 5 ? depth : 0;
    Buf b = { buf, cap, 0, false };
    put(&b, "#version 450\n#define UB_FULL %d\n", full ? 1 : 0);
    put(&b, k_fs_head, dv == 4 ? HLE_WBUF_BARY_EXT_GLSL : "", hle_push_glsl(), set, (unsigned)(HLE_UBER_WORDS / 4),
        dv == 4 ? HLE_WBUF_IN_BARY_GLSL : dv == 3 ? HLE_WBUF_IN_GLSL : "", tab);
    put(&b, k_fs_main, HLE_PSH_TWO_SIDE, dv == 4 ? HLE_WBUF_CALC_BARY_GLSL : dv == 3 ? HLE_WBUF_CALC_GLSL : "",
        HLE_PSH_SHADOW_POINT, k_depth[dv]);
    if (b.overflow) {
        return 0;
    }
    /* (as psh.c: every gl_FragCoord the pixel's position as the pixel stage reads it) */
    for (char *f = buf; (f = strstr(f, "gl_FragCoord")) != NULL; ) {
        memcpy(f, "nvFC        ", 12);
    }
    char *f = strstr(buf, "HLE_REAL_FC_");
    if (f) {
        memcpy(f, "gl_FragCoord", 12);
    }
    return b.len;
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
    return k->wbuf ? (k->wbuf == 2 ? 4u : 3u) : k->vol & HLE_PSH_ZTRUNC ? 1u : k->compare & HLE_PSH_ZF16 ? 2u : 0u;
}

/* ---- what the interpreters draw ----------------------------------------------- */

bool hle_uber_vs_ok(const HleVshKey *k, const uint32_t *tokens, unsigned nslots)
{
    if (!tokens || !nslots || hle_ffhw_is(tokens, nslots) || (k->zclamp && g_hle_glsl_zclamp)) {
        return false;                   /* (the fixed function's key: not yet) */
    }
    /* a lighting key after the final slot (ffhw.h): the fixed function's lights, not yet */
    unsigned fin = 0;
    while (fin < nslots && fin < HLE_VSH_MAX_SLOTS && !(tokens[4 * fin + 3] & 1)) {
        fin++;
    }
    return fin < nslots && fin < HLE_VSH_MAX_SLOTS &&
           !(fin + 1 + HLE_FFHW_SLOTS <= nslots && tokens[4 * (fin + 1)] == HLE_FFHW_LT_MAGIC);
}

bool hle_uber_ps_ok(const HlePshKey *k)
{
    /* the stand-in's own text past this subset: borders, coordinates without perspective,
     * lines, the target's pixel, signed blending, stipples, debug views */
    if (k->compare & ~(0xFFFFu | 0xF0000u | HLE_PSH_SHADOW_POINT | HLE_PSH_ZF16)) {
        return false;
    }
    /* (nor 16-bit or YUY2 stages' words and texels, nor the colour-space conversion:
     * psh.c tex16, yuv_stage, HLE_PSH_CSC) */
    if (k->fbfetch || k->sblend || k->stipple || k->linez || k->wbuf > 2 || k->point_sprite ||
        k->debug || k->tex16 || (k->input_tex & (HLE_PSH_CSC | HLE_PSH_CUBE3D | HLE_PSH_CYLWRAP)) ||
        k->fog > 8 || (k->alpha_test && k->alpha_func >= 8)) {
        return false;
    }
    for (unsigned s = 0; s < 4; s++) {
        /* (stage 0 fetches by its own coordinates alone; a DOT_ST stage 2 or 3, as the
         * stand-in builds it) */
        const unsigned m = (k->tex_modes >> (5 * s)) & 0x1F;
        if (m > HLE_PST_DOT_RFLCT_SPEC_CONST || (m >= HLE_PST_BUMPENVMAP && !s) ||
            (m == HLE_PST_DOT_ST && s < 2)) {
            return false;
        }
    }
    return true;
}
