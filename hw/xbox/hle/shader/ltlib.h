/*
 * The NV2A's lighting unit in GLSL: its arithmetic on 22-bit floats.
 *
 * The chip's fixed-function colours are not made by the transform engine's
 * 32-bit floats but by a second unit (LT) whose numbers are IEEE singles with
 * the low ten bits cut: a value enters it rounded to that width, a product
 * and a sum are cut toward zero, a reciprocal comes from a 64-entry table and
 * one refinement, and the colour leaves as a byte by its own rounding. The
 * arithmetic is the NV10's as envytools' nvhw models it from the hardware
 * (pgraph_celsius_xfrm.c: lt_mul, lt_add3, lts_add, lt_rcp, xfrm_f2b); the
 * NV2A's lit colours of nxdk_pgraph_tests come out of it to the byte where
 * plain floats are one off (light 0.7 on a normal along it: 179, floats 178).
 *
 * Values are uints: the bits of the float, bits 0..9 clear.
 */
#ifndef HLE_SHADER_LTLIB_H
#define HLE_SHADER_LTLIB_H

static const char k_lt_lib[] =
    "const uint lt_rcp_lut[64] = uint[64](\n"
    "  0x7fu, 0x7du, 0x7bu, 0x79u, 0x77u, 0x75u, 0x74u, 0x72u, 0x70u, 0x6fu, 0x6du, 0x6cu, 0x6bu, 0x69u, 0x68u, 0x67u,\n"
    "  0x65u, 0x64u, 0x63u, 0x62u, 0x60u, 0x5fu, 0x5eu, 0x5du, 0x5cu, 0x5bu, 0x5au, 0x59u, 0x58u, 0x57u, 0x56u, 0x55u,\n"
    "  0x54u, 0x54u, 0x53u, 0x52u, 0x51u, 0x50u, 0x4fu, 0x4fu, 0x4eu, 0x4du, 0x4cu, 0x4cu, 0x4bu, 0x4au, 0x4au, 0x49u,\n"
    "  0x48u, 0x48u, 0x47u, 0x46u, 0x46u, 0x45u, 0x45u, 0x44u, 0x43u, 0x43u, 0x42u, 0x42u, 0x41u, 0x41u, 0x40u, 0x40u);\n"
    /* into the unit: rounded to 14 bits of mantissa (the incrementer is eight
     * bits wide: no rounding where those are all ones) */
    "uint lt_s(float f) {\n"
    "  uint x = floatBitsToUint(f);\n"
    "  if (((x >> 10) & 0xffu) != 0xffu) x += 0x200u;\n"
    "  return x & 0xfffffc00u;\n"
    "}\n"
    "uvec3 lt_v(vec3 v) { return uvec3(lt_s(v.x), lt_s(v.y), lt_s(v.z)); }\n"
    "uint lt_mul(uint a, uint b) {\n"
    "  uint sg = (a ^ b) & 0x80000000u;\n"
    "  int ea = int((a >> 23) & 0xffu), eb = int((b >> 23) & 0xffu);\n"
    "  uint fa = ((a >> 10) & 0x1fffu) | 0x2000u, fb = ((b >> 10) & 0x1fffu) | 0x2000u;\n"
    "  if ((ea == 0xff && fa > 0x2000u) || (eb == 0xff && fb > 0x2000u)) return 0x7ffffc00u;\n"
    "  if (ea == 0 || eb == 0) return 0u;\n"
    "  if (ea == 0xff || eb == 0xff) return 0x7f800000u;\n"
    "  int e = ea + eb - 0x7f;\n"
    "  uint fr = (fa * fb) >> 13;\n"
    "  if (fr > 0x3fffu) { fr >>= 1; e++; }\n"
    "  if (e <= 0) return sg;\n"
    "  if (e >= 0xff) { e = 0xfe; fr = 0x1fffu; }\n"
    "  return sg | (uint(e) << 23) | ((fr & 0x1fffu) << 10);\n"
    "}\n"
    "uvec3 lt_vmul(uvec3 a, uvec3 b) { return uvec3(lt_mul(a.x, b.x), lt_mul(a.y, b.y), lt_mul(a.z, b.z)); }\n"
    /* the three-input adder: the operands lined up on the largest with five
     * bits to spare, cut; the sum cut to 14 bits */
    "uint lt_add3(uint a, uint b, uint c) {\n"
    "  uint v[3] = uint[3](a, b, c);\n"
    "  int ev[3]; uint fv[3]; int er = 0; bool pinf = false, ninf = false;\n"
    "  for (int i = 0; i < 3; i++) {\n"
    "    ev[i] = int((v[i] >> 23) & 0xffu);\n"
    "    fv[i] = v[i] & 0x7fffffu;\n"
    "    if (ev[i] == 0xff) {\n"
    "      if (fv[i] != 0u) return 0x7ffffc00u;\n"
    "      if ((v[i] >> 31) != 0u) ninf = true; else pinf = true;\n"
    "    }\n"
    "    if (ev[i] != 0) fv[i] |= 0x800000u;\n"
    "    fv[i] >>= 10;\n"
    "    er = max(er, ev[i] + 2);\n"
    "  }\n"
    "  if (pinf && ninf) return 0x7ffffc00u;\n"
    "  if (pinf || ninf) return 0x7f800000u;\n"
    "  int res = 0;\n"
    "  for (int i = 0; i < 3; i++) {\n"
    "    int sh = er - ev[i] - 7;\n"
    "    uint f = sh >= 32 ? 0u : sh >= 0 ? fv[i] >> uint(sh) : fv[i] << uint(-sh);\n"
    "    res += (v[i] >> 31) != 0u ? -int(f) : int(f);\n"
    "  }\n"
    "  if (res == 0) return 0u;\n"
    "  uint sg = res < 0 ? 0x80000000u : 0u;\n"
    "  uint r = uint(abs(res));\n"
    "  int n = 20 - findMSB(r);\n"
    "  r <<= uint(n); er -= n;\n"
    "  r >>= 7;\n"
    "  if (er >= 0xff) { er = 0xfe; r = 0x3fffu; }\n"
    "  if (er <= 0) return sg;\n"
    "  return sg | (uint(er) << 23) | ((r & 0x1fffu) << 10);\n"
    "}\n"
    "uint lt_add(uint a, uint b) { return lt_add3(a, b, 0u); }\n"
    "uvec3 lt_vadd(uvec3 a, uvec3 b) { return uvec3(lt_add(a.x, b.x), lt_add(a.y, b.y), lt_add(a.z, b.z)); }\n"
    "uint lt_dp(uvec3 a, uvec3 b) { uvec3 t = lt_vmul(a, b); return lt_add3(t.x, t.y, t.z); }\n"
    /* the multiply-accumulate units' adder: no spare bits */
    "uint lts_add(uint a, uint b) {\n"
    "  int ea = int((a >> 23) & 0xffu), eb = int((b >> 23) & 0xffu);\n"
    "  uint fa = ea != 0 ? ((a >> 10) & 0x1fffu) | 0x2000u : 0u;\n"
    "  uint fb = eb != 0 ? ((b >> 10) & 0x1fffu) | 0x2000u : 0u;\n"
    "  int er = max(ea, eb) + 1;\n"
    "  int sa = er - ea - 1, sb = er - eb - 1;\n"
    "  fa = sa >= 32 ? 0u : fa >> uint(sa);\n"
    "  fb = sb >= 32 ? 0u : fb >> uint(sb);\n"
    "  int res = ((a >> 31) != 0u ? -int(fa) : int(fa)) + ((b >> 31) != 0u ? -int(fb) : int(fb));\n"
    "  if (res == 0) return 0u;\n"
    "  uint sg = res < 0 ? 0x80000000u : 0u;\n"
    "  uint r = uint(abs(res));\n"
    "  int n = 14 - findMSB(r);\n"
    "  r <<= uint(n); er -= n;\n"
    "  r >>= 1;\n"
    "  if (er <= 0) return sg;\n"
    "  if (er >= 0xff) return sg | 0x7f800000u;\n"
    "  return sg | (uint(er) << 23) | ((r & 0x1fffu) << 10);\n"
    "}\n"
    /* the reciprocal: the table's seven bits and one step */
    "uint lt_rcp(uint x) {\n"
    "  int ex = int((x >> 23) & 0xffu);\n"
    "  if (ex == 0) return 0x7f800000u;\n"
    "  if (ex == 0xff) return 0u;\n"
    "  int er = 0xfd - ex;\n"
    "  uint fx = ((x & 0x7fffffu) + 0x800000u) >> 10;\n"
    "  uint s0 = lt_rcp_lut[(fx >> 7) & 0x3fu];\n"
    "  uint s1 = (((1u << 21) - s0 * fx) * s0) >> 14;\n"
    "  uint fr = (s1 << 11) - 0x800000u;\n"
    "  if (er <= 0) { er = 0; fr = 0u; }\n"
    "  return (x & 0x80000000u) | (uint(er) << 23) | fr;\n"
    "}\n"
    /* out of the unit: the colour's byte, over 255 */
    "float lt_f2b(uint v) {\n"
    "  if ((v >> 31) != 0u) return 0.0;\n"
    "  int e = int((v >> 23) & 0xffu);\n"
    "  if (e >= 0x7f) return 1.0;\n"
    "  if (e < 0x76) return 0.0;\n"
    "  uint fr = (((v >> 10) & 0x1fffu) | 0x2000u) << uint(e - 0x74);\n"
    "  fr = (((fr * 0xffu) >> 23) + 1u) >> 1;\n"
    "  return float(min(fr, 255u)) / 255.0;\n"
    "}\n"
    "vec3 lt_vf2b(uvec3 v) { return vec3(lt_f2b(v.x), lt_f2b(v.y), lt_f2b(v.z)); }\n";

#endif
