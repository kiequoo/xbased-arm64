/* The title fixes' table (hw/xbox/hle/fixes/fixes.c): which titles each fix is
 * on for, and the override syntax. The structure rule itself (nothing but the
 * fixes layer asks which title runs) is tools/native/check-layers.sh. */
#include "hw/xbox/hle/fixes/fixes.h"
#include "hle_test.h"
#include "xbe_fixture.h"

static uint8_t buf[0x1000];

static const FxSection k_secs[] = {
    { ".text", 0x06, 0x00011000, 0x00100000, 0x001000, 0x100000 },
};

static HleXbeInfo title_with(const FxLib *libs, unsigned n)
{
    uint32_t len = fx_build_xbe(buf, sizeof(buf), 0x00012345, false, k_secs, 1, libs, n, 0x4D530064);
    HleXbeInfo x;
    memset(&x, 0, sizeof(x));
    HT_EQ_INT(hle_xbe_parse(buf, len, &x), HLE_XBE_OK);
    return x;
}

static const uint32_t k_d3d_all = HLE_FIX_D3D_SNAP_BIAS | HLE_FIX_D3D_LAZY_TARGETS |
                                  HLE_FIX_D3D_LAZY_PUSHES | HLE_FIX_D3D_LAZY_REPORTS |
                                  HLE_FIX_D3D_LAZY_ZALIAS;

/* A title without D3D runs the base alone: not one fix is on */
static void test_no_d3d_is_the_base(void)
{
    static const FxLib libs[] = { { "XAPILIB", 1, 0, 5849, 0x4001 } };
    HleXbeInfo x = title_with(libs, 1);
    HT_EQ_U32(hle_fixes_defaults(&x), 0);
}

/* The D3D titles get the d3d_ fixes, the build-specific one only for 4928 */
static void test_d3d_titles(void)
{
    static const FxLib l5849[] = { { "D3D8", 1, 0, 5849, 0x4003 } };
    static const FxLib l4928[] = { { "D3D8", 1, 0, 4928, 0x4003 } };
    static const FxLib ltcg[] = { { "D3D8LTCG", 1, 0, 5849, 0x4003 } };
    HleXbeInfo a = title_with(l5849, 1), b = title_with(l4928, 1), c = title_with(ltcg, 1);
    HT_EQ_U32(hle_fixes_defaults(&a), k_d3d_all);
    HT_EQ_U32(hle_fixes_defaults(&b), k_d3d_all | HLE_FIX_D3D_METHOD_RUNS);
    HT_EQ_U32(hle_fixes_defaults(&c), k_d3d_all);
}

static void test_override_syntax(void)
{
    unsigned bad = 0;
    const uint32_t d = k_d3d_all;
    HT_EQ_U32(hle_fixes_apply(d, "-d3d_snap_bias", &bad), d & ~HLE_FIX_D3D_SNAP_BIAS);
    HT_EQ_U32(hle_fixes_apply(0, "d3d_lazy_pushes,+d3d_lazy_reports", &bad),
              HLE_FIX_D3D_LAZY_PUSHES | HLE_FIX_D3D_LAZY_REPORTS);
    HT_EQ_U32(hle_fixes_apply(d, "-all", &bad), 0);
    HT_EQ_U32(hle_fixes_apply(0, "+all", &bad), k_d3d_all | HLE_FIX_D3D_METHOD_RUNS);
    HT_EQ_U32(bad, 0);
    HT_EQ_U32(hle_fixes_apply(d, "-nothing", &bad), d);
    HT_EQ_U32(bad, 1);
    HT_EQ_U32(hle_fixes_apply(d, NULL, NULL), d);
}

static void test_names(void)
{
    char out[128];
    hle_fixes_names(0, out, sizeof(out));
    HT_EQ_STR(out, "-");
    hle_fixes_names(HLE_FIX_D3D_SNAP_BIAS | HLE_FIX_D3D_LAZY_PUSHES, out, sizeof(out));
    HT_EQ_STR(out, "d3d_snap_bias,d3d_lazy_pushes");
}

int main(void)
{
    HT_RUN(test_no_d3d_is_the_base);
    HT_RUN(test_d3d_titles);
    HT_RUN(test_override_syntax);
    HT_RUN(test_names);
    HT_DONE();
}
