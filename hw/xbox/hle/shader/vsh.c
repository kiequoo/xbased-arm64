/*
 * xbased HLE GPU — NV2A vertex program -> GLSL (pure). See vsh.h.
 *
 * Semantics follow NV_vertex_program1_1 plus the NV2A rules games rely on:
 * - a MAC and an ILU operation in one slot read their operands before
 *   either writes (both results are computed into temporaries first);
 * - MUL/MAD: a zero factor gives zero even against inf/NaN;
 * - RCP(0) = +inf, RCC clamps the magnitude to [2^-64, 2^64];
 * - R12 is oPos;
 * - the program leaves oPos in screen space (the XDK appends the viewport
 *   transform through c[-38]/c[-37]): the epilogue maps x/y back to NDC
 *   with the same mapping as pre-transformed vertices, divides z by the
 *   depth maximum and multiplies by w to undo the perspective divide.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "hw/xbox/hle/shader/ff.h"
#include "hw/xbox/hle/shader/xflib.h"
#include "hw/xbox/hle/shader/ltlib.h"
#include "hw/xbox/hle/shader/vsh.h"
#include "hw/xbox/hle/shader/ffhw.h"
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
    int n = vsnprintf(b->p + b->len, b->len < b->cap ? b->cap - b->len : 0, fmt, ap);
    va_end(ap);
    if (n < 0 || b->len + (size_t)n >= b->cap) {
        b->overflow = true;
        b->len = b->cap;
        return;
    }
    b->len += (size_t)n;
}

/* NV2A output register file: 0 oPos, 3 oD0, 4 oD1, 5 oFog, 6 oPts, 7 oB0,
 * 8 oB1, 9..12 oT0..oT3 (1, 2 unused). */
static const char *k_out[16] = {
    "oPos", "oUnused1", "oUnused2", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1",
    "oT0", "oT1", "oT2", "oT3", "oUnused13", "oUnused14", "oUnused15",
};

static const char k_sw[4] = { 'x', 'y', 'z', 'w' };

static void mask_str(Nv2aVshWritemask m, char *s)
{
    unsigned v = (unsigned)m, n = 0;
    if (v & 8) s[n++] = 'x';
    if (v & 4) s[n++] = 'y';
    if (v & 2) s[n++] = 'z';
    if (v & 1) s[n++] = 'w';
    s[n] = 0;
}

static void in_str(Buf *b, const Nv2aVshInput *in)
{
    char reg[48];
    switch (in->type) {
    case NV2ART_TEMPORARY:
        snprintf(reg, sizeof(reg), in->index == 12 ? "oPos" : "R%u", in->index);
        break;
    case NV2ART_INPUT:
        snprintf(reg, sizeof(reg), "v%u", in->index & 15);
        break;
    case NV2ART_CONTEXT:
        if (in->is_relative) {
            snprintf(reg, sizeof(reg), "c[clamp(A0 + %u, 0, 191)]", in->index);
        } else {
            snprintf(reg, sizeof(reg), "c[%u]", in->index % 192);
        }
        break;
    default:
        snprintf(reg, sizeof(reg), "vec4(0.0)");
        break;
    }
    put(b, "%s%s.%c%c%c%c", in->is_negated ? "-" : "", reg, k_sw[in->swizzle[0] & 3],
        k_sw[in->swizzle[1] & 3], k_sw[in->swizzle[2] & 3], k_sw[in->swizzle[3] & 3]);
}

/* Result expression of one operation (vec4), operands already swizzled.
 * xf: by the transform unit's own arithmetic (xflib.h). */
static void op_expr(Buf *b, const Nv2aVshOperation *o, bool xf)
{
#define I(n) in_str(b, &o->inputs[n])
    if (xf) {
        const char *f2 = NULL, *f1 = NULL;
        switch (o->opcode) {
        case NV2AOP_MUL: f2 = "xmul"; break;
        case NV2AOP_ADD: f2 = "xadd"; break;
        case NV2AOP_DP3: f2 = "xdp3"; break;
        case NV2AOP_DP4: f2 = "xdp4"; break;
        case NV2AOP_DPH: f2 = "xdph"; break;
        case NV2AOP_DST: f2 = "xdst"; break;
        case NV2AOP_RCP: f1 = "xrcp4"; break;
        case NV2AOP_RCC: f1 = "xrcc4"; break;
        case NV2AOP_RSQ: f1 = "xrsq4"; break;
        default: break;
        }
        if (o->opcode == NV2AOP_MAD) {
            put(b, "xadd(xmul("); I(0); put(b, ", "); I(1); put(b, "), "); I(2); put(b, ")");
            return;
        }
        if (f2) {
            put(b, "%s(", f2); I(0); put(b, ", "); I(1); put(b, ")");
            return;
        }
        if (f1) {
            put(b, "%s((", f1); I(0); put(b, ").x)");
            return;
        }
    }
    switch (o->opcode) {
    case NV2AOP_MOV: I(0); break;
    case NV2AOP_MUL: put(b, "mulz("); I(0); put(b, ", "); I(1); put(b, ")"); break;
    case NV2AOP_ADD: put(b, "("); I(0); put(b, " + "); I(1); put(b, ")"); break;
    case NV2AOP_MAD:
        put(b, "(mulz("); I(0); put(b, ", "); I(1); put(b, ") + "); I(2); put(b, ")");
        break;
    case NV2AOP_DP3: put(b, "vec4(dot(("); I(0); put(b, ").xyz, ("); I(1); put(b, ").xyz))");
        break;
    case NV2AOP_DP4: put(b, "vec4(dot("); I(0); put(b, ", "); I(1); put(b, "))"); break;
    case NV2AOP_DPH:
        put(b, "vec4(dot(("); I(0); put(b, ").xyz, ("); I(1); put(b, ").xyz) + ("); I(1);
        put(b, ").w)");
        break;
    case NV2AOP_DST:
        put(b, "dst4("); I(0); put(b, ", "); I(1); put(b, ")");
        break;
    case NV2AOP_MIN: put(b, "min("); I(0); put(b, ", "); I(1); put(b, ")"); break;
    case NV2AOP_MAX: put(b, "max("); I(0); put(b, ", "); I(1); put(b, ")"); break;
    case NV2AOP_SLT: put(b, "vec4(lessThan("); I(0); put(b, ", "); I(1); put(b, "))"); break;
    case NV2AOP_SGE:
        put(b, "vec4(greaterThanEqual("); I(0); put(b, ", "); I(1); put(b, "))");
        break;
    case NV2AOP_ARL: I(0); break;
    case NV2AOP_RCP: put(b, "rcp4(("); I(0); put(b, ").x)"); break;
    case NV2AOP_RCC: put(b, "rcc4(("); I(0); put(b, ").x)"); break;
    case NV2AOP_RSQ: put(b, "rsq4(("); I(0); put(b, ").x)"); break;
    case NV2AOP_EXP: put(b, "exp4(("); I(0); put(b, ").x)"); break;
    case NV2AOP_LOG: put(b, "log4(("); I(0); put(b, ").x)"); break;
    case NV2AOP_LIT: put(b, "lit4("); I(0); put(b, ")"); break;
    default:         put(b, "vec4(0.0)"); break;
    }
#undef I
}

/* fogw: set when the program writes oFog */
static void out_write(Buf *b, const Nv2aVshOutput *o, const char *val, bool *fogw)
{
    char m[5];
    mask_str(o->writemask, m);
    switch (o->type) {
    case NV2ART_TEMPORARY:
        if (m[0]) {
            if (o->index == 12) {
                put(b, "    oPos.%s = %s.%s;\n", m, val, m);
            } else {
                put(b, "    R%u.%s = %s.%s;\n", o->index % 12, m, val, m);
            }
        }
        break;
    case NV2ART_OUTPUT:
        if (m[0] && (o->index & 15) == 5) {
            /* oFog has one component, the fog coordinate: a write under any
             * mask puts the result's first component there (its x: the
             * source's first swizzle), not the one the mask names. Need for
             * Speed Underground writes oFog.w alone and is fogged by it on
             * the console; a write of two components (.yw of a source
             * swizzled .yw) takes the first of them, y (xemu's tests on a
             * console, its pull request 981). The last masked one was read
             * here: the same for a replicated source, another component for
             * the rest. */
            *fogw = true;
            put(b, "    oFog.x = %s.x;\n", val);
        } else if (m[0]) {
            put(b, "    %s.%s = %s.%s;\n", k_out[o->index & 15], m, val, m);
        }
        break;
    case NV2ART_ADDRESS:
        /* ARL floors, but an index the program computes as a whole number
         * (a bone's, from a byte attribute times 765: i / 255 * 765) can land
         * a rounding error below it here where the NV2A lands on it: floored,
         * the vertex took the bone's previous matrix row, and Azurik's model
         * threw a few of its vertices into the ground. A bias far above fp32's
         * error at these magnitudes, far below an intended fraction's. */
        put(b, "    A0 = int(floor(%s.x + 1.0 / 1024.0));\n", val);
        break;
    default:                            /* CONTEXT writes: state programs only */
        break;
    }
}

static const char k_lib[] =
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
    "}\n";

/* The declarations and main's start: the inputs (v0..v15, current values
 * for registers no array feeds), the outputs, the registers. */
static void head(Buf *bp, const HleVshKey *k, bool lt, bool cf)
{
    const char *lib = hle_vsh_xf_exact(k) ? k_xf_lib : NULL;
    /* (a stand-in's fog in plain floats: tail) */
    const char *lib2 = lt || ((k->fog_table & HLE_VSH_FOG_NV) && !(k->spec_off & HLE_VSH_XF_FAST)) ? k_lt_lib
                                                                                                 : NULL;
    Buf b = *bp;
    const char *interp = k->flat ? "flat " : "";
    /* (a stand-in's lights are a loop not to be unrolled: ffhw.c light_loop) */
    put(&b, "#version 450\n%s%s", k->spec_off & HLE_VSH_XF_FAST ?
        "#extension GL_EXT_control_flow_attributes : enable\n" : "", hle_push_glsl());
    put(&b, "layout(set = 1, binding = 0, std140) uniform VshConst { vec4 c[192]; "
            "vec4 vcur[16];%s };\n"
            "invariant gl_Position;\n", cf ? " vec4 cf[192];" : "");
    for (unsigned i = 0; i < 16; i++) {
        if (k->in_mask & (1u << i)) {
            put(&b, "layout(location = %u) in %s v%u_in;\n", i,
                (k->cmp_mask >> i) & 1 ? "uvec2" : (k->int_mask >> i) & 1 ? "ivec4" : "vec4", i);
        }
    }
    put(&b, "layout(location = 0) %sout vec4 vDiffuse;\n"
            "layout(location = 1) %sout vec4 vSpecular;\n"
            "layout(location = 2) out vec4 vTex0;\n"
            "layout(location = 3) out vec4 vTex1;\n"
            "layout(location = 4) out vec4 vTex2;\n"
            "layout(location = 5) out vec4 vTex3;\n"
            "layout(location = 6) out float vFog;\n"
            "layout(location = 7) %sout vec4 vBack0;\n"
            "layout(location = 8) %sout vec4 vBack1;\n"
            /* the screen position on the rasterizer's 1/16 pixels and w: the
             * pixel stage's colour weights (psh.c nvcol) */
            "layout(location = 11) out vec3 vScr;\n", interp, interp, interp, interp);
    if (k->spec_off & HLE_VSH_XF) {
        /* each corner's own texture coordinates, for the pixel stage's texel
         * ties (psh.c nvtie) */
        put(&b, "layout(location = 12) out vec4 vTexP0;\n"
                "layout(location = 13) out vec4 vTexP1;\n"
                "layout(location = 14) out vec4 vTexP2;\n"
                "layout(location = 15) out vec4 vTexP3;\n"
                /* and its depth on the screen, in the buffer's steps (psh.c nvzword) */
                "layout(location = 16) out float vZs;\n"
                "layout(location = 17) out float vFogP;\n"
                /* and the clip-space point as the GPU takes it (x, y, w) with the scale
                 * it was given (tail; psh.c nvtiep, where a corner's w is out at its
                 * ends and its screen position no longer a number to work with) */
                "layout(location = 22) out vec4 vHP;\n");
    }
    if (k->wbuf) {      /* W-buffering: perspective-correct w for the depth */
        put(&b, "layout(location = 9) out float vW;\n");
    }
    put(&b, "%s%s%s", k_lib, lib ? lib : "", lib2 ? lib2 : "");
    put(&b, "void main() {\n");
    for (unsigned i = 0; i < 16; i++) {
        if (k->in_mask & (1u << i) && (k->cmp_mask >> i) & 1) {
            /* fetched as two 16-bit halves: the dword is often only 2-byte aligned */
            put(&b, "  int v%up = int(v%u_in.x | (v%u_in.y << 16u));\n"
                    "  vec4 v%u = vec4(vec3(bitfieldExtract(v%up, 0, 11), "
                    "bitfieldExtract(v%up, 11, 11), bitfieldExtract(v%up, 22, 10)) / "
                    "vec3(1023.0, 1023.0, 511.0), 1.0);\n", i, i, i, i, i, i, i);
        } else if (k->in_mask & (1u << i) && (k->w1_mask >> i) & 1) {
            /* a three-component array through its four-component format */
            put(&b, "  vec4 v%u = vec4(vec3(v%u_in.xyz), 1.0);\n", i, i);
        } else if (k->in_mask & (1u << i)) {
            put(&b, "  vec4 v%u = vec4(v%u_in);\n", i, i);
        } else {
            /* not in the arrays: the NV2A reads the register's current value
             * (SET_VERTEX_DATA*: Halo's decals take their tint and fade from
             * SetVertexDataColor(9)) */
            put(&b, "  vec4 v%u = vcur[%u];\n", i, i);
        }
    }
    for (unsigned i = 0; i < 16; i++) {
        if (k->in_mask & k->s1_mask & (1u << i)) {
            /* S1: fetched as integers, normalized as the console does */
            static const char *const k_comp[4] = {"x", "xy", "xyz", "xyzw"};
            const char *c = k_comp[(k->s1_cnt >> (2 * i)) & 3];
            put(&b, "  v%u.%s = (2.0 * v%u.%s + 1.0) / 65535.0;\n", i, c, i, c);
        }
        if (k->in_mask & (1u << i) & (k->in_mask >> 16)) {
            /* a "3W" array's third component is w, z 0 (vsh.h in_mask) */
            put(&b, "  v%u = vec4(v%u.xy, 0.0, v%u.z);\n", i, i, i);
        }
    }
    put(&b, "  int A0 = 0;\n"
            "  vec4 R0 = vec4(0.0), R1 = vec4(0.0), R2 = vec4(0.0), R3 = vec4(0.0);\n"
            "  vec4 R4 = vec4(0.0), R5 = vec4(0.0), R6 = vec4(0.0), R7 = vec4(0.0);\n"
            "  vec4 R8 = vec4(0.0), R9 = vec4(0.0), R10 = vec4(0.0), R11 = vec4(0.0);\n"
            "  vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0), oD0 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "  vec4 oD1 = vec4(0.0, 0.0, 0.0, 1.0), oFog = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "  vec4 oPts = vec4(0.0, 0.0, 0.0, 1.0), oB0 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "  vec4 oB1 = vec4(0.0, 0.0, 0.0, 1.0), oT0 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "  vec4 oT1 = vec4(0.0, 0.0, 0.0, 1.0), oT2 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "  vec4 oT3 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "  vec4 oUnused1, oUnused2, oUnused13, oUnused14, oUnused15;\n");
    *bp = b;
}

static void tail(Buf *b, const HleVshKey *k, bool fogw, bool ff);

size_t hle_nvline_gs_glsl(const HleVshKey *k, bool affine, unsigned tri, unsigned width, bool ends,
                         char *buf, size_t cap)
{
    static const struct { const char *type, *name; bool col; } k_v[9] = {
        { "vec4", "vDiffuse", true }, { "vec4", "vSpecular", true }, { "vec4", "vTex0", false },
        { "vec4", "vTex1", false }, { "vec4", "vTex2", false }, { "vec4", "vTex3", false },
        { "float", "vFog", false }, { "vec4", "vBack0", true }, { "vec4", "vBack1", true } };
    Buf b = { buf, cap, 0, false };
    put(&b, "#version 450\n%s"
            "layout(%s) in;\n"
            "layout(triangle_strip, max_vertices = %u) out;\n", hle_push_glsl(),
        tri ? "triangles" : "lines", tri ? 12u : 4u);
    for (unsigned i = 0; i < 9; i++) {
        const char *q = k_v[i].col && k->flat ? "flat " : affine ? "noperspective " : "";
        put(&b, "layout(location = %u) %sin %s i%u[];\n"
                "layout(location = %u) %sout %s %s;\n", i, q, k_v[i].type, i, i,
            ends && i < 2 ? "flat " : q, k_v[i].type, k_v[i].name);
    }
    if (ends) {         /* (psh.c nvcol1: the start's colours at 0 and 1, the end's here) */
        put(&b, "layout(location = 14) flat out vec4 vDiffuse1;\n"
                "layout(location = 15) flat out vec4 vSpecular1;\n"
                "layout(location = 10) flat out vec2 vLineIW;\n");     /* w of the two ends */
    }
    if (k->wbuf) {      /* W-buffering: the corner's w (pervertex or interpolated, ff.h) */
        put(&b, "layout(location = 9) out float vW;\n");
    }
    if (k->spec_off & HLE_VSH_XF) {   /* (the ends' depths on the screen: psh.c nvlword) */
        put(&b, "layout(location = 16) in float iZs[];\n"
                "layout(location = 18) flat out vec2 vLineZ;\n"
                "#define NVLZ vLineZ = vec2(iZs[ia], iZs[ib]);\n");
    } else {
        put(&b, "#define NVLZ\n");
    }
    /* (each end's w as the vertex stage made it: its gl_Position may be scaled for
     * the clip, vsh.c tail, which leaves the screen position and z / w as they are) */
    put(&b, "layout(location = 11) in vec3 iScr[];\n");
    put(&b, "layout(location = 12) flat out ivec4 vLine;\n"
            "layout(location = 13) flat out int vLineW;\n"
            "const int nvl_w = %u;\n"
            /* a vertex's position as the vertex stage left it, in screen pixels */
            "vec2 nvl_scr(vec4 p) { return (p.xy / p.w - vec2(-1.0, 1.0)) / pc.vp; }\n"
            "void nvl_edge(int ia, int ib) {\n"
            "  vec4 p0 = gl_in[ia].gl_Position, p1 = gl_in[ib].gl_Position;\n"
            "  float w0 = iScr[ia].z, w1 = iScr[ib].z;\n"
            "  if (nvl_w == 0 || !(p0.w > 0.0) || !(p1.w > 0.0)) return;\n"
            /* (truncated to 1/16 pixel there) */
            "  ivec2 a = ivec2(floor(nvl_scr(p0) * 16.0 + 0.5)), e = ivec2(floor(nvl_scr(p1) * 16.0 + 0.5));\n"
            "  if (a == e) return;\n"
            "  vec2 f0 = vec2(a) / 16.0, f1 = vec2(e) / 16.0, d = f1 - f0;\n"
            "  bool xmaj = abs(d.x) >= abs(d.y);\n"
            "  vec2 m = xmaj ? vec2(sign(d.x), 0.0) : vec2(0.0, sign(d.y));\n"
            "  vec2 n = xmaj ? vec2(0.0, 1.0) : vec2(1.0, 0.0);\n"
            "  float dd = dot(d, d);\n"
            "  for (int i = 0; i < 4; i++) {\n"
            /* (around the line: a one-pixel line's margins as they were; a wider one's
             * 1/2 width across and 1/4 more for the box along at 45 degrees, and its
             * square end as far past the end point along the axis) */
            "    float pm = nvl_w == 8 ? 1.75 : 2.0 + float(nvl_w) * 3.0 / 32.0, pn = nvl_w == 8 ? 3.0 : 3.0 + float(nvl_w) * 3.0 / 32.0;\n"
            /* (its ends moved out along the line itself: moved along the axis alone the quad
             * leans toward the line and cuts a wide line's corner off) */
            "    vec2 mm = nvl_w == 8 ? m : d / max(abs(d.x), abs(d.y));\n"
            "    vec2 c = ((i & 2) == 0 ? f0 - mm * pm : f1 + mm * pm) + n * ((i & 1) == 0 ? -pn : pn);\n"
            "    float t = dot(c - f0, d) / dd;\n"
            /* 1 / w is linear on the screen; the outputs over w too */
            "    float iw = max(mix(1.0 / w0, 1.0 / w1, t), 1.0e-30), w = 1.0 / iw;\n"
            "    gl_Position = vec4((c * pc.vp + vec2(-1.0, 1.0)) * w, mix(p0.z / p0.w, p1.z / p1.w, t) * w, w);\n"
            "    vLine = ivec4(a, e);\n"
            "    vLineW = nvl_w;\n"
            "    NVLZ\n"
            "%s", width, /* (the ends' w, negative for a polygon's outline or a wide line: psh.c nvcol1) */
            !ends ? "" : affine ? (tri || width != 8 ? "    vLineIW = vec2(-1.0);\n" : "    vLineIW = vec2(1.0);\n") :
            tri || width != 8 ? "    vLineIW = -vec2(w0, w1);\n" : "    vLineIW = vec2(w0, w1);\n");
    for (unsigned i = 0; i < 9; i++) {
        if (k_v[i].col && k->flat) {
            put(&b, "    %s = i%u[0];\n", k_v[i].name, i);
        } else if (ends && i < 2) {
            put(&b, "    %s = i%u[ia];\n    %s1 = i%u[ib];\n", k_v[i].name, i, k_v[i].name, i);
        } else if (affine) {
            put(&b, "    %s = mix(i%u[ia], i%u[ib], t);\n", k_v[i].name, i, i);
        } else {
            put(&b, "    %s = mix(i%u[ia] / w0, i%u[ib] / w1, t) * w;\n", k_v[i].name, i, i);
        }
    }
    put(&b, "%s    EmitVertex();\n"
            "  }\n"
            "  EndPrimitive();\n"
            "}\n", k->wbuf ? "    vW = w;\n" : "");
    if (!tri) {
        put(&b, "void main() { nvl_edge(0, 1); }\n");
    } else if (tri == 5) {      /* the side 1 - 2 alone (the flat colours are vertex 0's) */
        put(&b, "void main() { nvl_edge(1, 2); }\n");
    } else {
        /* a triangle"s three sides, in its vertices" order; the faces culled
         * (tri - 1: 1 those wound clockwise on the screen, 2 the others, 3
         * both) are left out here: the quads have no winding of theirs. A
         * triangle of no area counts as wound counter-clockwise (nxdk_pgraph_tests'
         * Front_face LM: its collinear triangles gone where that winding is culled) */
        put(&b, "void main() {\n"
                "  vec4 p0 = gl_in[0].gl_Position, p1 = gl_in[1].gl_Position, p2 = gl_in[2].gl_Position;\n"
                "  if (!(p0.w > 0.0) || !(p1.w > 0.0) || !(p2.w > 0.0)) return;\n"
                "  vec2 s0 = nvl_scr(p0), u = nvl_scr(p1) - s0, v = nvl_scr(p2) - s0;\n"
                "  float area = u.x * v.y - u.y * v.x;\n"
                "  const int cull = %u;\n"
                "  if (cull == 3 || (cull == 1 && area > 0.0) || (cull == 2 && area <= 0.0)) return;\n"
                "  %s\n"
                "}\n", (tri & 7) - 1,
            /* (tri + 8: a list's own triangles, ff.h HLE_TOPO_NVTRIS) */
            tri & 8 ? "nvl_edge(1, 2); nvl_edge(2, 0); nvl_edge(0, 1);" : "nvl_edge(0, 1); nvl_edge(1, 2); nvl_edge(2, 0);");
    }
    return b.overflow ? 0 : b.len;
}

size_t hle_quadflat_gs_glsl(const HleVshKey *k, bool affine, char *buf, size_t cap)
{
    static const struct { const char *type, *name; bool col; } k_v[9] = {
        { "vec4", "vDiffuse", true }, { "vec4", "vSpecular", true }, { "vec4", "vTex0", false },
        { "vec4", "vTex1", false }, { "vec4", "vTex2", false }, { "vec4", "vTex3", false },
        { "float", "vFog", false }, { "vec4", "vBack0", true }, { "vec4", "vBack1", true } };
    Buf b = { buf, cap, 0, false };
    put(&b, "#version 450\n"
            "layout(lines_adjacency) in;\n"
            "layout(triangle_strip, max_vertices = 6) out;\n");
    for (unsigned i = 0; i < 9; i++) {
        const char *q = k_v[i].col && k->flat ? "flat " : affine ? "noperspective " : "";
        put(&b, "layout(location = %u) %sin %s i%u[];\n"
                "layout(location = %u) %sout %s %s;\n", i, q, k_v[i].type, i, i, q, k_v[i].type,
            k_v[i].name);
    }
    if (k->wbuf) {
        put(&b, "layout(location = 9) in float iW[];\n"
                "layout(location = 9) out float vW;\n");
    }
    /* (and each corner's own values the pixel stage reads per vertex: its screen
     * position, texture coordinates, depth, fog and clip-space point, psh.c nvcol /
     * nvtie / nvzdepth: a flat quad's texel ties and depth words as a smooth one's,
     * nxdk_pgraph_tests' Shade_model FixedTex_Quad_Flat) */
    static const struct { unsigned loc; const char *type, *name; } k_p[8] = {
        { 11, "vec3", "vScr" }, { 12, "vec4", "vTexP0" }, { 13, "vec4", "vTexP1" },
        { 14, "vec4", "vTexP2" }, { 15, "vec4", "vTexP3" }, { 16, "float", "vZs" },
        { 17, "float", "vFogP" }, { 22, "vec4", "vHP" } };
    const unsigned np = (k->spec_off & HLE_VSH_XF) ? 8 : 1;
    for (unsigned i = 0; i < np; i++) {
        put(&b, "layout(location = %u) in %s p%u[];\n"
                "layout(location = %u) out %s %s;\n",
            k_p[i].loc, k_p[i].type, i, k_p[i].loc, k_p[i].type, k_p[i].name);
    }
    /* the triangles the quad is drawn as elsewhere (0 1 2, 0 2 3: pbres.c),
     * the number of the primitive as there (psh.c reads its parity) */
    put(&b, "void main() {\n"
            "  const int order[6] = int[6](0, 1, 2, 0, 2, 3);\n"
            "  for (int t = 0; t < 6; t++) {\n"
            "    int j = order[t];\n"
            "    gl_Position = gl_in[j].gl_Position;\n"
            "    gl_PrimitiveID = 2 * gl_PrimitiveIDIn + t / 3;\n");
    for (unsigned i = 0; i < 9; i++) {
        put(&b, "    %s = i%u[%s];\n", k_v[i].name, i, k_v[i].col && k->flat ? "3" : "j");
    }
    for (unsigned i = 0; i < np; i++) {
        put(&b, "    %s = p%u[j];\n", k_p[i].name, i);
    }
    put(&b, "%s    EmitVertex();\n"
            "    if (t == 2 || t == 5) EndPrimitive();\n"
            "  }\n"
            "}\n", k->wbuf ? "    vW = iW[j];\n" : "");
    return b.overflow ? 0 : b.len;
}

size_t hle_vsh_glsl(const uint32_t *tokens, unsigned nslots, const HleVshKey *k,
                    char *buf, size_t cap)
{
    Buf b = { buf, cap, 0, false };
    const bool xf = hle_vsh_xf_exact(k);
    /* under SET_LIGHTING_ENABLE the lighting key follows the final slot (ffhw.h) */
    unsigned fin = 0;
    while (fin < nslots && fin < HLE_VSH_MAX_SLOTS && !(tokens[4 * fin + 3] & 1)) {
        fin++;
    }
    const bool ltk = fin + 1 + HLE_FFHW_SLOTS <= nslots && tokens[4 * (fin + 1)] == HLE_FFHW_LT_MAGIC;
    head(&b, k, ltk && xf, ltk);     /* (the stand-in lights in plain floats) */
    bool final = false, fogw = false;
    for (unsigned s = 0; s < nslots && s < HLE_VSH_MAX_SLOTS && !final; s++) {
        Nv2aVshStep st;
        memset(&st, 0, sizeof(st));
        if (nv2a_vsh_parse_step(&st, tokens + 4 * s) != NV2AVPR_SUCCESS) {
            return 0;
        }
        final = st.is_final;
        bool mac = st.mac.opcode != NV2AOP_NOP, ilu = st.ilu.opcode != NV2AOP_NOP;
        if (!mac && !ilu) {
            continue;
        }
        put(&b, "  { /* slot %u */\n", s);
        if (mac) {
            put(&b, "    vec4 m = ");
            op_expr(&b, &st.mac, xf);
            put(&b, ";\n");
        }
        if (ilu) {
            put(&b, "    vec4 i = ");
            op_expr(&b, &st.ilu, xf);
            put(&b, ";\n");
        }
        for (unsigned o = 0; mac && o < 2; o++) {
            out_write(&b, &st.mac.outputs[o], "m", &fogw);
        }
        for (unsigned o = 0; ilu && o < 2; o++) {
            out_write(&b, &st.ilu.outputs[o], "i", &fogw);
        }
        put(&b, "  }\n");
    }
    if (!final) {
        return 0;
    }
    if (ltk) {
        const size_t n = b.overflow ? 0 : hle_ffhw_lt_glsl(tokens + 4 * (fin + 1), xf, (k->spec_off & HLE_VSH_XF_FAST) != 0, b.p + b.len, b.cap - b.len);
        if (!n) {
            return 0;
        }
        b.len += n;
    }
    tail(&b, k, fogw, false);
    return b.overflow ? 0 : b.len;
}

size_t hle_vsh_glsl_wrap(const HleVshKey *k, bool lt, const char *body, bool fogw, char *buf, size_t cap)
{
    Buf b = { buf, cap, 0, false };
    head(&b, k, lt, false);
    put(&b, "  vec4 ffHP = vec4(0.0, 0.0, 0.0, 1.0);\n"       /* (ffhw.c: the clip-space point, */
            "  bool ffZ = false;\n");                         /* its w 0 before the clamp) */
    put(&b, "%s", body);
    tail(&b, k, fogw, true);
    return b.overflow ? 0 : b.len;
}

/* main's end: the outputs from the registers, oPos (screen space) to clip
 * space. fogw: oFog was written. */
static void tail(Buf *bp, const HleVshKey *k, bool fogw, bool ff)
{
    Buf b = *bp;
    /* specular off (NV2A SPECULAR_ENABLE clear): the pixel stage sees
     * (0, 0, 0, 1) whatever the program wrote */
    /* the specular alpha 1 unless LIGHT_CONTROL takes it from the material,
     * for a program as for fixed function (nxdk_pgraph_tests' Specular_back
     * _VS: the console's back faces opaque there) */
    const char *d1 = k->spec_off & 1 ? "vec4(0.0, 0.0, 0.0, 1.0)" :
                     k->spec_off & 2 ? "vec4(clamp(oD1.rgb, 0.0, 1.0), 1.0)" : "clamp(oD1, 0.0, 1.0)";
    const char *b1 = k->spec_off & 1 ? "vec4(0.0, 0.0, 0.0, 1.0)" :
                     k->spec_off & 2 ? "vec4(clamp(oB1.rgb, 0.0, 1.0), 1.0)" : "clamp(oB1, 0.0, 1.0)";
    put(&b, "  vDiffuse = clamp(oD0, 0.0, 1.0);\n"
            "  vSpecular = %s;\n"
            "  vBack0 = clamp(oB0, 0.0, 1.0);\n"
            "  vBack1 = %s;\n"
            "  vTex0 = oT0; vTex1 = oT1; vTex2 = oT2; vTex3 = oT3;\n", d1, b1);
    if ((k->spec_off & (HLE_VSH_XF | HLE_VSH_XF_FAST)) == (HLE_VSH_XF | HLE_VSH_XF_FAST)) {
        /* a stand-in: the pixel stage's inputs all the same, the colours' no-number rule
         * left to the exact form (each piece of a stand-in is the driver's time at the
         * draw that first needs it) */
        put(&b, "  vTexP0 = oT0; vTexP1 = oT1; vTexP2 = oT2; vTexP3 = oT3; vZs = oPos.z;\n");
    } else if (k->spec_off & HLE_VSH_XF) {
        put(&b, "  vTexP0 = oT0; vTexP1 = oT1; vTexP2 = oT2; vTexP3 = oT3; vZs = oPos.z;\n");
        /* A colour that is no number goes to the rasterizer as an infinity of its
         * sign: a whole step (the transform unit's own NaN, 0x7fffffff), or none
         * (a negative one a mov passed on); the clamp here made every one 0
         * (nxdk_pgraph_tests' Attrib_float: diffuse times a NaN constant, the
         * console's columns the plain gradient's, 0 times NaN 0 at the top; -NaN
         * to +NaN moved, black to white) */
        put(&b, "#define NVNAN1(v, o) v = mix(v, vec4(lessThan(floatBitsToUint(o), uvec4(0x80000000u))), greaterThan(floatBitsToUint(o) & 0x7fffffffu, uvec4(0x7f800000u)))\n"
                "  NVNAN1(vDiffuse, oD0); NVNAN1(vBack0, oB0);\n");
        if (!(k->spec_off & 1)) {
            put(&b, "  NVNAN1(vSpecular, oD1); NVNAN1(vBack1, oB1);\n");
        }
    }
    /* A program that leaves oFog alone: the fog the vertex 6 before it left
     * (pbdraw.c g_fogc), the draw's vertex i's at i mod 6 in the push constants
     * (vk/draw.c), as the chip's (nxdk_pgraph_tests' Fog_carryover). Halo draws
     * its Bink movies with fog enabled through such a program and they show on
     * the console, so what the places hold there is no fog: the first thing to
     * look at if they are fogged here. A fixed-function pixel stage keeps D3D's
     * fog: unfogged is factor 1 under FOGTABLEMODE NONE (vertex fog), distance 0
     * under the table modes. */
    if ((k->fog_table & HLE_VSH_FOG_NV_CARRY) && !fogw) {
        put(&b, "  { int m = gl_VertexIndex %% 6;\n"
                "    oFog.x = m == 0 ? pc.mvp[0].w : m == 1 ? pc.mvp[2].w : pc.mvp[3][m - 2]; }\n");
        fogw = true;
    }
    if ((k->fog_table & HLE_VSH_FOG_NV) && (k->spec_off & HLE_VSH_XF_FAST)) {
        /* a stand-in: K0 + K1 d in plain floats (the exact form's 22-bit arithmetic,
         * ltlib.h, was a fifth of a stand-in's build: 8 ms of the driver's) */
        put(&b, "  { float fd = %s;\n"
                "    float fv = pc.fog.x + pc.fog.y * fd;\n"
                "    vFog = isinf(fv) || isnan(fv)%s ? 8.0 : clamp(fv, -8.0, 8.0); }\n",
            k->fog_table & HLE_VSH_FOG_NV_RAD ? "pc.fog.z" :
            !fogw ? "0.0" : k->fog_table & HLE_VSH_FOG_NV_ABS ? "abs(oFog.x)" : "oFog.x",
            k->fog_table & HLE_VSH_FOG_NV_LIN ? " || isinf(fd)" : "");
    } else if (k->fog_table & HLE_VSH_FOG_NV) {
        /* the fog value: the lighting unit's K0 + K1 d on its 22-bit floats, as it
         * leaves them (13 bits of mantissa: the 12 bits of fraction are the pixel
         * stage's table index, cut there; nxdk_pgraph_tests' Fog_gen planar
         * pictures, exact with the corners' value whole and the pixel's cut,
         * a level off in whole columns with the corners cut to 12) (an infinity or no
         * number: above every table's range) */
        put(&b, "  { float fd = %s;\n"
                "    uint fv = lt_add3(lt_s(pc.fog.x), lt_mul(lt_s(pc.fog.y), lt_s(fd)), 0u);\n"
                "    vFog = ((fv >> 23) & 0xffu) == 0xffu%s ? 8.0 :\n"
                "           clamp(uintBitsToFloat(fv & 0xfffffc00u), -8.0, 8.0); }\n",
            /* (a program under SET_FOG_GEN_MODE RADIAL: the coordinate is not its oFog
             * but the radial distance the last fixed-function vertex left, whatever
             * it writes (pbdraw.c g_fog_rad, in pc.fog.z): nxdk_pgraph_tests' Fog_gen,
             * the program's radial pictures fogged alike in every quad) */
            k->fog_table & HLE_VSH_FOG_NV_RAD ? "pc.fog.z" :
            !fogw ? "0.0" : k->fog_table & HLE_VSH_FOG_NV_ABS ? "abs(oFog.x)" : "oFog.x",
            /* (the linear modes: an infinite coordinate is no fog even times a zero
             * K1, where the EXP modes keep K0: Fog_exceptional_value's INF pictures) */
            k->fog_table & HLE_VSH_FOG_NV_LIN ? " || isinf(fd)" : "");
    } else if (fogw) {
        put(&b, "  vFog = oFog.x;\n");
    } else {
        put(&b, "  vFog = %s;\n", k->fog_table ? "0.0" : "1.0");
    }
    if (k->spec_off & HLE_VSH_XF) {
        put(&b, "  vFogP = vFog;\n");        /* (the pixel stage's own perspective weights: psh.c nvtiep) */
    }
    /* A point's size is the program's oPts only under point parameters;
     * without them it is SET_POINT_SIZE, as the fixed-function stage's
     * (xemu's tests on a console, its pull request 2527). A program that
     * writes no oPts drew one-pixel points whatever the size set. */
    if (k->pt_size & HLE_VSH_PT_FIXED) {
        const unsigned e = k->pt_size & 0x1FFu;
        put(&b, "  gl_PointSize = %u.%03u * float(max((pc.flags >> 16) & 255u, 1u));\n",
            e < 8 ? 1u : e / 8, e < 8 ? 0u : (e % 8) * 125);
        put(&b, "  float ptS = %u.%03u;\n", e / 8, (e % 8) * 125);
    } else {
        put(&b, "  gl_PointSize = max(oPts.x, 1.0) * float(max((pc.flags >> 16) & 255u, 1u));\n"
                "  float ptS = oPts.x;\n");
    }
    put(&b, /* screen space -> clip space (the NV2A rasterizer's subpixel
             * placement, ff.h) */
            "  float w = oPos.w;\n"
            /* the NV2A keeps w's magnitude within 2^-64 .. 2^64, its sign
             * kept (+0 is positive, -0 negative): D3D's XYZRHW pass-through
             * takes RCP of rhw, and rhw 0 (Otogi's title art) would make w
             * +inf and drop the primitive; a w of 0 is 2^-64, the vertex far
             * off the screen along its own direction, not the vertex as it
             * stands (it was taken as 1: nxdk_pgraph_tests' W param suite, a
             * quad with w 0 drawn small in a corner where the console fills
             * half the screen with it) */
            "  if (!isnan(w)) {\n"
            "    w = w > 0.0 || floatBitsToUint(w) == 0u ?\n"
            "        clamp(w, uintBitsToFloat(0x1F800000u), uintBitsToFloat(0x5F800000u)) :\n"
            "        clamp(w, uintBitsToFloat(0xDF800000u), uintBitsToFloat(0x9F800000u));\n"
            "  }\n"
            "  vec2 scr = oPos.xy;\n"
            /* (a screen coordinate from 2^127 up is the largest float: the console fills
             * W_param's rcc_w_zero_inf_z-1.50e-36, its far corner (3.2e38, 2.6e38), at
             * 45 degrees as the saturated (3.4e38, 3.4e38) of z 0; z -0.96e-34's
             * (4.9e36, 4.0e36) at its own angle) */
            "  scr = mix(scr, sign(scr) * uintBitsToFloat(0x7F7FFFFFu), greaterThanEqual(abs(scr), vec2(uintBitsToFloat(0x7F000000u))));\n"
            HLE_SUBPIXEL_GLSL("scr"));
    if (k->spec_off & HLE_VSH_NO_POINTS) {
        put(&b, "  bool ptCull = false;\n");        /* (no points: the rules below are dead) */
    } else {
    put(&b,
            /* a point with SET_SMOOTHING_CONTROL's bit 0 clear
             * (vk/draw.c): one on a whole x lights the pixel after it, not the one
             * before (nxdk_pgraph_tests' Smoothing_control: the points of 0xFFFF0000's
             * four quarters a pixel to the right of 0xFFFF0001's) */
            /* A point by its size (nxdk_pgraph_tests' Point_size
             * and ZPass_pixel_count: points on whole pixels and on pixels' centres at
             * every size from 0 in eighths): the pixels whose centres lie within half
             * its size of it. A centre exactly on its edge counts on the left and the
             * top one, as the device's own points have it, and on the other two as
             * well unless the size is a power of two: 3 pixels wide on a whole pixel
             * is 4 across and 6 wide on a centre 7, where 1, 2, 8 and 32 are
             * themselves. (The rasterizer's weights again: the corner that takes what
             * the others leave of 1 is over 0 on its far edge where the reciprocal of
             * the quad's area is short, and that reciprocal is whole for a power of
             * two.) The device's points are never under a pixel: a narrower one is the
             * pixel whose centre it reaches, or nothing. Size 0 lights the pixel the
             * point is in. */
            "  bool ptCull = false;\n"
            "  if (pc.mvp[2].y != 0.0) {\n"
            /* (the size in eighths of a pixel, cut: a computed one, under point
             * parameters, is no finer on the console) */
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
            /* (and a point past either end of the depth clip range is dropped under
             * SET_ZMIN_MAX_CONTROL's CULL_NEAR_FAR, as a triangle with every corner
             * past it: vk/draw.c's range and mode; Point_params through a program, its
             * first row at z -5) */
            "  if (pc.mvp[2].y != 0.0 && pc.mvp[0].y > pc.mvp[0].x &&\n"
            "      (int(pc.mvp[0].z) & 9) == 1 && (oPos.z < pc.mvp[0].x || oPos.z > pc.mvp[0].y)) ptCull = true;\n"
            "  if (pc.mvp[2].y == 2.0) scr.x += 0.03125;\n"
            /* (a one-pixel point into an antialiased surface, drawn once per sample:
             * on its pixel's centre, so each pass lights that pixel's texel) */
            "  if (pc.mvp[2].y != 0.0 && pc.mvp[1].x >= 1.0 && gl_PointSize <= 1.0)\n"
            "    scr = ceil(scr) - 0.5;\n");
    }
    put(&b, "  vScr = vec3(scr, w);\n"
            HLE_TEXEL_EDGE_GLSL("scr")
            "  vec2 ndc = scr * pc.vp + vec2(-1.0, 1.0);\n");
    /* The NV2A clips on the depth it tests: under W-buffering that is w (the
     * pixel stage's), and without a depth test nothing. The program's z then
     * must not clip the primitive (Forza's sky dome past the far plane, its
     * minimap at z far below 0), so the clip z only keeps w >= 0. */
    if (!k->wbuf && !k->zfree) {
        put(&b, HLE_ZNORM_GLSL("oPos.z", "zN"));
    }
    /* A corner whose w is held at 2^64 (an infinite one) or under 2^-32 (either
     * sign): the rasterizer's point (x w, y w, z w, w) over its largest component
     * of x, y and w, the
     * coverage the same (a positive scale: the homogeneous triangle's w > 0 side)
     * and the GPU's clip in floats well conditioned where w runs from 2^-64 to
     * 2^64 (nxdk_pgraph_tests' W_param: the console fills the external triangles
     * exactly, pgraph/tools/homog2.py). The scale moves the GPU's own perspective
     * weights, so only there: the exact pixel stage takes the colours, texture
     * coordinates and fog from the corners with their w (nvcol, nvtiep), and an
     * ordinary w, or a far corner at w 1 (W_param's rcc pictures), keeps its clip
     * as it was. A program's alone (oPos the screen position as it left it): the
     * fixed-function stage's divide has its own cases still to fit (its w -0 and
     * infinite screen corners: W_param's ff_w_zero_inf, pgraph/RESULTS.md).
     * Not a large w short of that: Forza's sky dome, w 1e8 .. 1e10 under
     * W-buffering and behind the eye too, has triangles with corners on both
     * sides of 2^32. Scaled from 2^32 on, the GPU's perspective weights were off
     * between the scaled corners and the others, and the sky's texture smeared
     * into white streaks at the screen's edge; the clip is well conditioned
     * there as it is. W_param's corners are ordinary, tiny or held at 2^64. */
    put(&b, "  vec4 gp = vec4(ndc * w, %s, w);\n", k->wbuf || k->zfree ? "0.0" : "zN * w");
    put(&b, "  float gsc = 1.0;\n");
    put(&b, "  if (abs(w) < uintBitsToFloat(0x2F800000u) || abs(w) >= uintBitsToFloat(0x5F800000u)) {\n");
    if (ff) {
        /* (the fixed-function stage's clip-space point, its screen position times w
         * made with no divide: the divide's corner overflows where w is out at its
         * ends, ffhw.c. One whose w was 0 before the clamp keeps the divided
         * corner while that is a number (ZMinMaxControl's CtrlFixed pictures: a
         * corner on the eye's plane, a few pixels moved by the other); one that
         * is no number keeps it too, scale and all) */
        put(&b, "    if (!any(isnan(ffHP)) && !any(isinf(ffHP)) &&\n"
                "        (!ffZ || any(isnan(gp)) || any(isinf(gp)))) {\n"
                "      gp = vec4(ffHP.xy * pc.vp + vec2(-1.0, 1.0) * ffHP.w, %s, ffHP.w);\n",
            k->wbuf || k->zfree ? "0.0" : "ffHP.z * pc.zscale + ffHP.z * (pc.zscale == uintBitsToFloat(0x33800000u) || "
            "pc.zscale == uintBitsToFloat(0x37800000u) ? pc.zscale * pc.zscale : 0.0)");
    } else {
        put(&b, "    {\n");
    }
    put(&b, "      float gm = max(max(abs(gp.x), abs(gp.y)), abs(gp.w));\n"
            "      if (!isinf(gm) && !isnan(gm) && gm > 0.0) { gsc = 1.0 / gm; gp *= gsc; }\n"
            "    }\n"
            "  }\n");
    put(&b, "  gl_Position = gp;\n");
    if (k->spec_off & HLE_VSH_XF) {
        put(&b, "  vHP = vec4(gp.xyw, gsc);\n");
    }
    if (k->zclamp && g_hle_glsl_zclamp) {
        put(&b, HLE_ZCLAMP_GLSL);
    }
    if (g_hle_glsl_zw) {
        put(&b, HLE_ZW_NUDGE_GLSL);
    }
    /* (a point narrower than a pixel that reaches no centre, above: off the screen) */
    put(&b, "  if (ptCull) gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);\n");
    put(&b, "%s}\n", k->wbuf ? "  vW = w;\n" : "");
    *bp = b;
}
