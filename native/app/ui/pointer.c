/*
 * xbased front end — the mouse pointer (pointer.h): a dart drawn by its
 * distance to the edge, each pixel's cover worked out there (no image to
 * scale), in layers: a shadow, a green glow, a dark edge, the body with the
 * dashboard's green from light to the console's own and a lighter rim on
 * its upper part.
 */
#include "native/app/ui/pointer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* the dart, the tip at (0, 0), in its height's units: down the left side, in
 * to a notch, out to the right point */
static const float k_dart[][2] = {{0, 0}, {0, 0.88f}, {0.25f, 0.66f}, {0.62f, 0.62f}};
#define NPTS 4

#define BODY_H 30.0f    /* the dart's height at scale 1, in pixels (the system's arrow is about 19) */
#define EDGE 1.7f       /* the dark edge's width */
#define GLOW 3.5f       /* the glow's reach past it */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* the signed distance from (x, y) to the dart of height h: below 0 inside */
static float dart_dist(float x, float y, float h)
{
    float best = 1e9f;
    bool in = false;
    for (int i = 0, j = NPTS - 1; i < NPTS; j = i++) {
        const float ax = k_dart[j][0] * h, ay = k_dart[j][1] * h;
        const float bx = k_dart[i][0] * h, by = k_dart[i][1] * h;
        const float ex = bx - ax, ey = by - ay, wx = x - ax, wy = y - ay;
        const float t = clampf((wx * ex + wy * ey) / (ex * ex + ey * ey), 0, 1);
        const float dx = wx - ex * t, dy = wy - ey * t;
        const float d = dx * dx + dy * dy;
        if (d < best) {
            best = d;
        }
        if ((ay > y) != (by > y) && x < ax + (y - ay) * ex / ey) {
            in = !in;
        }
    }
    const float d = sqrtf(best);
    return in ? -d : d;
}

typedef struct {
    float r, g, b, a;
} Px;

/* src over dst, straight alpha */
static void over(Px *d, float r, float g, float b, float a)
{
    if (a <= 0) {
        return;
    }
    const float na = a + d->a * (1 - a);
    if (na <= 0) {
        return;
    }
    d->r = (r * a + d->r * d->a * (1 - a)) / na;
    d->g = (g * a + d->g * d->a * (1 - a)) / na;
    d->b = (b * a + d->b * d->a * (1 - a)) / na;
    d->a = na;
}

static float mixf(float a, float b, float t)
{
    return a + (b - a) * t;
}

static int margin(float s)
{
    return (int)ceilf((EDGE + GLOW) * s) + 1;
}

int ptr_art_size(float scale)
{
    const float s = scale < 1 ? 1 : scale > 4 ? 4 : scale;
    return (int)ceilf(0.88f * BODY_H * s) + 2 * margin(s);
}

void ptr_art(float scale, uint32_t *argb, int *size, int *hot)
{
    const float s = scale < 1 ? 1 : scale > 4 ? 4 : scale;
    const float h = BODY_H * s, edge = EDGE * s, glow = GLOW * s;
    const int m = margin(s), n = ptr_art_size(scale);
    /* the body's colours, top to bottom: a light lime, the dashboard's
     * green, the console's dark green (#107C10) */
    static const float top[3] = {0.80f, 0.97f, 0.50f}, mid[3] = {0.42f, 0.80f, 0.17f},
                       bot[3] = {0.063f, 0.486f, 0.063f};
    for (int py = 0; py < n; py++) {
        for (int px = 0; px < n; px++) {
            const float x = px + 0.5f - m, y = py + 0.5f - m;
            const float d = dart_dist(x, y, h);
            Px o = {0, 0, 0, 0};
            /* the shadow, down and right, soft */
            const float ds = dart_dist(x - 1.2f * s, y - 1.8f * s, h);
            over(&o, 0, 0, 0, 0.38f * clampf(1 - (ds - edge) / (2.6f * s), 0, 1));
            /* the glow round the edge */
            if (d > edge - 1) {
                over(&o, 0.42f, 0.92f, 0.18f, 0.60f * expf(-(d > edge ? d - edge : 0) / (1.2f * s)) *
                                                   clampf(1 - (d - edge) / glow, 0, 1));
            }
            /* the dark edge: the dart grown by its width */
            over(&o, 0.024f, 0.15f, 0.012f, clampf(edge + 0.5f - d, 0, 1));
            /* the body */
            const float cov = clampf(0.5f - d, 0, 1);
            if (cov > 0) {
                const float t = clampf(y / (0.88f * h), 0, 1);
                float c[3];
                for (int k = 0; k < 3; k++) {
                    c[k] = t < 0.45f ? mixf(top[k], mid[k], t / 0.45f) : mixf(mid[k], bot[k], (t - 0.45f) / 0.55f);
                }
                /* a light rim inside the edge, fading down the dart */
                const float rim = clampf(1 - (-d - 0.6f * s) / (1.4f * s), 0, 1) * (1 - t) * 0.55f;
                /* a gloss near the tip, as on the console's green jewel */
                const float gx = x - 0.13f * h, gy = y - 0.27f * h;
                const float gloss = 0.45f * clampf(1 - sqrtf(gx * gx * 2.2f + gy * gy) / (0.24f * h), 0, 1);
                const float lit = rim > gloss ? rim : gloss;
                for (int k = 0; k < 3; k++) {
                    c[k] = mixf(c[k], 1.0f, lit);
                }
                over(&o, c[0], c[1], c[2], cov);
            }
            const uint32_t a = (uint32_t)lroundf(clampf(o.a, 0, 1) * 255);
            const uint32_t r = (uint32_t)lroundf(clampf(o.r, 0, 1) * 255);
            const uint32_t g = (uint32_t)lroundf(clampf(o.g, 0, 1) * 255);
            const uint32_t b = (uint32_t)lroundf(clampf(o.b, 0, 1) * 255);
            argb[py * n + px] = a ? a << 24 | r << 16 | g << 8 | b : 0;
        }
    }
    *size = n;
    *hot = m;
}

void ptr_install(SDL_Window *win)
{
    static SDL_Cursor *cur;
    if (!cur) {
        float scale = win ? SDL_GetWindowDisplayScale(win) : 1.0f;
        if (!(scale > 0)) {
            scale = 1;
        }
        const int n = ptr_art_size(scale);
        uint32_t *px = malloc((size_t)n * n * sizeof(uint32_t));
        if (!px) {
            return;
        }
        int size = 0, hot = 0;
        ptr_art(scale, px, &size, &hot);
        SDL_Surface *sf = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_ARGB8888);
        if (sf) {
            for (int y = 0; y < size; y++) {
                memcpy((uint8_t *)sf->pixels + (size_t)y * sf->pitch, px + (size_t)y * size,
                       (size_t)size * sizeof(uint32_t));
            }
            cur = SDL_CreateColorCursor(sf, hot, hot);
            SDL_DestroySurface(sf);
        }
        free(px);
    }
    if (cur) {
        SDL_SetCursor(cur);
    }
}
