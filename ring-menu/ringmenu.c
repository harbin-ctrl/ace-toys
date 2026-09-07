// ringmenu - implementation. See ringmenu.h for the contract.
//
// The menu is an annulus split into equal wedges, one per item, first item
// centered straight up. It is rendered once into an internal RGBA buffer
// whenever its appearance changes (open, highlight move); ringmenu_draw just
// composites that buffer over the caller's canvas. All sizing happens in
// ringmenu_create: the content radius grows until the widest label or image
// fits inside its wedge, so the menu is exactly as big as it needs to be.

#include "ringmenu.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef RM_PI
#define RM_PI 3.14159265358979323846f
#endif

// ---------------------------------------------------------------------------
// Built-in font: 5x7 small caps, drawn at 2x (10x14 px per glyph).
// Glyph order: A-Z, 0-9, dash, plus, space. Each row byte holds 5 pixels, bit 4
// is the leftmost column.
// ---------------------------------------------------------------------------

#define RM_GLYPH_W 5
#define RM_GLYPH_H 7
#define RM_SCALE 2
#define RM_ADVANCE (RM_GLYPH_W + 1)   // one blank column between glyphs
#define RM_GLYPH_SPACE 38             // index of the blank glyph

static const uint8_t RM_FONT[39][RM_GLYPH_H] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, // D
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // G
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // H
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // I
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, // J
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // L
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // M
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // N
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // Q
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // R
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // T
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // U
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // V
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, // W
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // X
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // Y
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // Z
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // -
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, // +
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space
};

static int rm_glyph_index(char c) {
    if (c >= 'a' && c <= 'z') {
        return c - 'a';
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= '0' && c <= '9') {
        return 26 + (c - '0');
    }
    if (c == '-') {
        return 36;
    }
    if (c == '+') {
        return 37;
    }
    return RM_GLYPH_SPACE;
}

// ---------------------------------------------------------------------------
// Look and geometry constants
// ---------------------------------------------------------------------------

#define RM_INNER_RADIUS 14.0f   // dead-zone hole around the pointer
#define RM_PAD 5.0f             // clearance between content and wedge edges
#define RM_MIN_OUTER 48.0f      // an easily hittable minimum, whatever the content

static const uint8_t RM_BG[4]     = {34, 36, 42, 235};    // wedge
static const uint8_t RM_HI[4]     = {66, 120, 230, 245};  // highlighted wedge
static const uint8_t RM_BORDER[4] = {150, 155, 165, 255}; // outlines/separators
static const uint8_t RM_TEXT[4]   = {235, 235, 240, 255};
static const uint8_t RM_GRAY_BG[4] = {25, 26, 30, 180};
static const uint8_t RM_GRAY_TEXT[4] = {120, 120, 125, 160};
static const uint8_t RM_GROUP[4] = {50, 92, 150, 245};
static const uint8_t RM_RADIO[4] = {214, 230, 255, 255};

#define RM_GROUP_RIM 7.0f
#define RM_RADIO_RADIUS 3.5f
#define RM_RADIO_GAP 6.0f

// Toggle light ("LED") look and placement. The light sits on the slot's
// bisector at RM_LED_FRAC of the way out to the label; its band outer radius
// is RM_LED_RADIUS, the silver band is RM_LED_RING thick, and when lit the
// glow reaches RM_LED_GLOW past the band. RM_LED_GAP is the clearance kept
// from the center hole and the label when reserving room at create time.
#define RM_LED_RADIUS 7.0f
#define RM_LED_RING   2.2f
#define RM_LED_GLOW   7.0f
#define RM_LED_FRAC   0.58f
#define RM_LED_GAP    3.0f

// ---------------------------------------------------------------------------

struct RingMenu {
    int count;
    struct {
        char label[RINGMENU_LABEL_MAX + 1];
        uint32_t *image;    // NULL for text items
        int cw, ch;         // content (label or image) size in px
        int led;            // RINGMENU_LED_* toggle-light state
        RingMenuItemState state;
    } items[RINGMENU_MAX_ITEMS];
    struct {
        int first, count, selected;
    } groups[RINGMENU_MAX_GROUPS];
    int group_count;
    int group_of[RINGMENU_MAX_ITEMS];

    float r0, r1;   // inner/outer ring radius
    float rc;       // radius the content centers sit on
    int size;       // pixel buffer is size x size, menu centered in it
    uint32_t *pix;  // rendered menu, straight-alpha RGBA
    // The ring itself costs a sqrt + atan2 per pixel but only changes when
    // the highlight moves, so it is kept apart from the content drawn over
    // it: callers that repaint on every pointer motion (a colour field being
    // scrubbed, say) then pay for the content alone.
    uint32_t *bg;
    int bg_highlight;   // highlight bg was built for, -2 when unbuilt

    bool open;
    bool dirty;
    bool needs_render;
    int cx, cy;     // menu center in screen coordinates
    int highlight;  // slot under the pointer, or -1
    int last_x, last_y;
};

static uint32_t rm_pack(const uint8_t c[4], uint8_t a) {
    return (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) |
           ((uint32_t)a << 24);
}

// Straight-alpha "src over dst".
static uint32_t rm_over(uint32_t src, uint32_t dst) {
    uint32_t sa = src >> 24;
    if (sa == 0) return dst;
    if (sa == 255) return src;
    uint32_t da = dst >> 24;
    uint32_t inv = 255 - sa;
    uint32_t oa = sa + (da * inv + 127) / 255;
    if (oa == 0) return 0;
    uint32_t out = oa << 24;
    for (int shift = 0; shift <= 16; shift += 8) {
        uint32_t s = (src >> shift) & 0xFF;
        uint32_t d = (dst >> shift) & 0xFF;
        // Channels are straight, so weight them by their own alphas.
        uint32_t v = (s * sa * 255 + d * da * inv + oa * 127) / (oa * 255);
        if (v > 255) v = 255;
        out |= v << shift;
    }
    return out;
}

static float rm_clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Clearance of an item's upright content rect (half-size w2 x h2, centered
// at radius rc along direction angle A) inside its wedge: the smallest of
// its distance to the center hole and, at each corner, the perpendicular
// distance to the two separator lines (linear over the rect, so corners
// suffice). Grows monotonically with rc, which makes it binary-searchable.
static float rm_fit_clearance(float rc, float A, float w2, float h2,
                              float half, float r0, int count) {
    float cx = rc * cosf(A);
    float cy = rc * sinf(A);

    float dx = fabsf(cx) - w2;
    if (dx < 0.0f) dx = 0.0f;
    float dy = fabsf(cy) - h2;
    if (dy < 0.0f) dy = 0.0f;
    float min_c = sqrtf(dx * dx + dy * dy) - r0;

    if (count >= 2) {
        float b1 = A + half, b2 = A - half;
        float c1x = cosf(b1), c1y = sinf(b1);
        float c2x = cosf(b2), c2y = sinf(b2);
        for (int j = 0; j < 4; j++) {
            float px = cx + ((j & 1) ? w2 : -w2);
            float py = cy + ((j & 2) ? h2 : -h2);
            // Signed distance to each boundary ray, positive inside the wedge.
            float d1 = -(c1x * py - c1y * px);
            float d2 = c2x * py - c2y * px;
            if (d1 < min_c) min_c = d1;
            if (d2 < min_c) min_c = d2;
        }
    }
    return min_c;
}

// Which slot the direction (fx, fy) from the menu center points at.
// Slot 0 is centered straight up; slots advance clockwise.
static int rm_slot_of(const RingMenu *m, float fx, float fy) {
    float half = RM_PI / (float)m->count;
    float a = atan2f(fy, fx) + RM_PI / 2.0f + half;
    a = fmodf(a, 2.0f * RM_PI);
    if (a < 0.0f) a += 2.0f * RM_PI;
    int slot = (int)(a / (2.0f * half));
    return slot >= m->count ? m->count - 1 : slot;
}

// Slot under screen point (x, y), or -1 for the hole / outside the ring.
static int rm_hit(const RingMenu *m, int x, int y) {
    float fx = (float)x - (float)m->cx;
    float fy = (float)y - (float)m->cy;
    float r = sqrtf(fx * fx + fy * fy);
    if (r < m->r0 || r > m->r1) return -1;
    return rm_slot_of(m, fx, fy);
}

static bool rm_active(const RingMenu *m, int slot) {
    return m->items[slot].state == RINGMENU_ITEM_ACTIVE;
}

static int rm_group_of(const RingMenu *m, int slot) {
    return m->group_of[slot];
}

// ---------------------------------------------------------------------------
// Rendering (into the internal buffer)
// ---------------------------------------------------------------------------

// Content always fits by construction (see the sizing in ringmenu_create),
// but both writers still clip to the buffer for safety.

static void rm_draw_label(RingMenu *m, const char *label, int x0, int y0,
                          RingMenuItemState state) {
    const uint8_t *color = state == RINGMENU_ITEM_GRAYED ? RM_GRAY_TEXT : RM_TEXT;
    uint32_t ink = rm_pack(color, color[3]);
    for (int i = 0; label[i]; i++) {
        const uint8_t *rows = RM_FONT[rm_glyph_index(label[i])];
        int gx = x0 + i * RM_ADVANCE * RM_SCALE;
        for (int ry = 0; ry < RM_GLYPH_H * RM_SCALE; ry++) {
            int py = y0 + ry;
            if (py < 0 || py >= m->size) continue;
            uint32_t *row = m->pix + (size_t)py * m->size;
            for (int rx = 0; rx < RM_GLYPH_W * RM_SCALE; rx++) {
                int px = gx + rx;
                if (px < 0 || px >= m->size) continue;
                if (rows[ry / RM_SCALE] & (0x10 >> (rx / RM_SCALE))) {
                    row[px] = ink;
                }
            }
        }
    }
}

static void rm_draw_image(RingMenu *m, const uint32_t *img, int iw, int ih,
                          int x0, int y0, RingMenuItemState state) {
    for (int y = 0; y < ih; y++) {
        int py = y0 + y;
        if (py < 0 || py >= m->size) continue;
        uint32_t *row = m->pix + (size_t)py * m->size;
        const uint32_t *src = img + (size_t)y * iw;
        for (int x = 0; x < iw; x++) {
            int px = x0 + x;
            if (px < 0 || px >= m->size) continue;
            uint32_t pixel = src[x];
            if (state == RINGMENU_ITEM_GRAYED) {
                uint32_t r = pixel & 0xFF;
                uint32_t g = (pixel >> 8) & 0xFF;
                uint32_t b = (pixel >> 16) & 0xFF;
                uint32_t gray = (r * 30 + g * 59 + b * 11) / 100;
                pixel = gray | (gray << 8) | (gray << 16) | ((pixel >> 1) & 0xFF000000);
            }
            row[px] = rm_over(pixel, row[px]);
        }
    }
}

// Straight-alpha blend one solid color over the internal buffer.
static void rm_led_blend(RingMenu *m, int px, int py,
                         float r, float g, float b, float a) {
    if (px < 0 || px >= m->size || py < 0 || py >= m->size || a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    uint8_t col[4] = {(uint8_t)(r + 0.5f), (uint8_t)(g + 0.5f),
                      (uint8_t)(b + 0.5f), 0};
    uint32_t src = rm_pack(col, (uint8_t)(a * 255.0f + 0.5f));
    uint32_t *row = m->pix + (size_t)py * m->size;
    row[px] = rm_over(src, row[px]);
}

static void rm_draw_radio(RingMenu *m, float cx, float cy) {
    int x0 = (int)floorf(cx - RM_RADIO_RADIUS);
    int x1 = (int)ceilf(cx + RM_RADIO_RADIUS);
    int y0 = (int)floorf(cy - RM_RADIO_RADIUS);
    int y1 = (int)ceilf(cy + RM_RADIO_RADIUS);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float dy = (float)y + 0.5f - cy;
            float cov = rm_clamp01(RM_RADIO_RADIUS + 0.5f - sqrtf(dx * dx + dy * dy));
            if (cov > 0.0f) {
                rm_led_blend(m, x, y, RM_RADIO[0], RM_RADIO[1], RM_RADIO[2], cov);
            }
        }
    }
}

// A toggle light centered at (lx, ly): a dark-silver band around either a
// dull dark glassy orb (off) or a bright yellow-gold glow (on) that spills a
// little past the band into the slot. Composited over the already-drawn wedge.
static void rm_draw_led(RingMenu *m, float lx, float ly, bool on) {
    const float R = RM_LED_RADIUS;          // band outer radius
    const float orbR = R - RM_LED_RING;     // orb / band inner radius
    const float glowR = R + RM_LED_GLOW;    // how far the lit glow reaches
    float reach = on ? glowR : R + 1.0f;

    int x0 = (int)floorf(lx - reach) - 1, x1 = (int)ceilf(lx + reach) + 1;
    int y0 = (int)floorf(ly - reach) - 1, y1 = (int)ceilf(ly + reach) + 1;

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - lx;
            float dy = (float)y + 0.5f - ly;
            float d = sqrtf(dx * dx + dy * dy);

            // 1. Lit glow: under the band and spilling out into the slot.
            if (on && d > orbR - 1.0f && d < glowR) {
                float t = (glowR - d) / (glowR - orbR);
                if (t > 1.0f) t = 1.0f;
                rm_led_blend(m, x, y, 255.0f, 200.0f, 96.0f, t * t * 0.85f);
            }

            // 2. Orb fill.
            float orbCov = rm_clamp01(orbR + 0.5f - d);
            if (orbCov > 0.0f) {
                float rr, gg, bb;
                if (on) {
                    float u = rm_clamp01(d / orbR);   // hot center -> gold edge
                    rr = 255.0f;
                    gg = 246.0f + (190.0f - 246.0f) * u;
                    bb = 196.0f + (54.0f - 196.0f) * u;
                } else {
                    // Dark glass, a touch brighter toward the top (dy < 0).
                    float top = rm_clamp01(0.5f - dy / (orbR * 1.6f));
                    rr = 20.0f + (74.0f - 20.0f) * top;
                    gg = 22.0f + (80.0f - 22.0f) * top;
                    bb = 30.0f + (98.0f - 30.0f) * top;
                }
                rm_led_blend(m, x, y, rr, gg, bb, orbCov);
            }

            // 3. Dark-silver band (annulus), lightly beveled light-to-dark.
            float bandCov = fminf(rm_clamp01(R + 0.5f - d),
                                  rm_clamp01(d - orbR + 0.5f));
            if (bandCov > 0.0f) {
                float u = rm_clamp01((d - orbR) / RM_LED_RING);
                float br = 182.0f + (92.0f - 182.0f) * u;
                float bg = 186.0f + (96.0f - 186.0f) * u;
                float bb = 196.0f + (108.0f - 196.0f) * u;
                rm_led_blend(m, x, y, br, bg, bb, bandCov);
            }

            // 4. A small glassy specular highlight up and to the left.
            if (d < orbR + 0.5f) {
                float sdx = dx + orbR * 0.33f, sdy = dy + orbR * 0.33f;
                float sd = sqrtf(sdx * sdx + sdy * sdy);
                float spec = rm_clamp01(1.0f - sd / (orbR * 0.55f));
                if (spec > 0.0f) {
                    rm_led_blend(m, x, y, 255.0f, 255.0f, 255.0f,
                                 spec * spec * (on ? 0.30f : 0.50f));
                }
            }
        }
    }
}

static void rm_render_bg(RingMenu *m, uint32_t *dst) {
    float c = m->size / 2.0f;
    float half = RM_PI / (float)m->count;

    for (int y = 0; y < m->size; y++) {
        uint32_t *row = dst + (size_t)y * m->size;
        float fy = (float)y + 0.5f - c;
        for (int x = 0; x < m->size; x++) {
            float fx = (float)x + 0.5f - c;
            float r = sqrtf(fx * fx + fy * fy);

            // Ring coverage with a 1px anti-aliasing ramp at both edges.
            float cov = rm_clamp01(m->r1 + 0.5f - r) *
                        rm_clamp01(r - (m->r0 - 0.5f));
            if (cov <= 0.0f) {
                row[x] = 0;
                continue;
            }

            int slot = rm_slot_of(m, fx, fy);
            const uint8_t *base = (slot == m->highlight) ? RM_HI : RM_BG;
            if (slot != m->highlight && !rm_active(m, slot)) {
                base = RM_GRAY_BG;
            }

            // Border: the two ring outlines plus the wedge separators,
            // each about 1px wide with a soft falloff.
            float edge = m->r1 - r;
            float e2 = r - m->r0;
            if (e2 < edge) edge = e2;
            float border = rm_clamp01(1.5f - edge) * 0.9f;
            if (m->count > 1) {
                float a = atan2f(fy, fx) + RM_PI / 2.0f + half;
                a = fmodf(a, 2.0f * half);
                if (a < 0.0f) a += 2.0f * half;
                float sep = (a < half ? a : 2.0f * half - a) * r;
                float sb = rm_clamp01(1.5f - sep) * 0.9f;
                if (sb > border) border = sb;
            }

            float cr = base[0] + (RM_BORDER[0] - base[0]) * border;
            float cg = base[1] + (RM_BORDER[1] - base[1]) * border;
            float cb = base[2] + (RM_BORDER[2] - base[2]) * border;
            float ca = base[3] + (RM_BORDER[3] - base[3]) * border;
            if (rm_group_of(m, slot) >= 0 && r > m->r1 - RM_GROUP_RIM) {
                float tint = rm_clamp01((r - (m->r1 - RM_GROUP_RIM)) / RM_GROUP_RIM) * 0.45f;
                cr += (RM_GROUP[0] - cr) * tint;
                cg += (RM_GROUP[1] - cg) * tint;
                cb += (RM_GROUP[2] - cb) * tint;
            }
            uint8_t out[4] = {(uint8_t)cr, (uint8_t)cg, (uint8_t)cb, 0};
            row[x] = rm_pack(out, (uint8_t)(ca * cov));
        }
    }
}

static void rm_render(RingMenu *m) {
    float c = m->size / 2.0f;
    float half = RM_PI / (float)m->count;
    size_t px_bytes = (size_t)m->size * m->size * 4;

    if (m->bg) {
        if (m->bg_highlight != m->highlight) {
            rm_render_bg(m, m->bg);
            m->bg_highlight = m->highlight;
        }
        memcpy(m->pix, m->bg, px_bytes);
    } else {
        rm_render_bg(m, m->pix);
    }

    // Content, centered on each wedge's bisector at radius rc.
    for (int i = 0; i < m->count; i++) {
        float ang = -RM_PI / 2.0f + (float)i * 2.0f * half;
        int px = (int)lroundf(c + m->rc * cosf(ang) - m->items[i].cw / 2.0f);
        int py = (int)lroundf(c + m->rc * sinf(ang) - m->items[i].ch / 2.0f);
        if (m->items[i].image) {
            rm_draw_image(m, m->items[i].image, m->items[i].cw,
                          m->items[i].ch, px, py, m->items[i].state);
        } else if (m->items[i].label[0]) {
            rm_draw_label(m, m->items[i].label, px, py, m->items[i].state);
        }
    }

    // Toggle lights, drawn over the wedge between each slot's center and label.
    for (int i = 0; i < m->count; i++) {
        if (m->items[i].led == RINGMENU_LED_NONE || !rm_active(m, i)) {
            continue;
        }
        float ang = -RM_PI / 2.0f + (float)i * 2.0f * half;
        float r_led = RM_LED_FRAC * m->rc;
        rm_draw_led(m, c + r_led * cosf(ang), c + r_led * sinf(ang),
                    m->items[i].led == RINGMENU_LED_ON);
    }

    // A radio dot identifies the one active item in each related arc.
    for (int i = 0; i < m->group_count; i++) {
        int slot = m->groups[i].selected;
        float ang = -RM_PI / 2.0f + (float)slot * 2.0f * half;
        float r = m->r0 + RM_RADIO_GAP + RM_RADIO_RADIUS;
        rm_draw_radio(m, c + r * cosf(ang), c + r * sinf(ang));
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static bool rm_groups_valid(const RingMenuItem *items, int item_count,
                            const RingMenuGroup *groups, int group_count) {
    if (group_count < 0 || group_count > RINGMENU_MAX_GROUPS) {
        return false;
    }
    if (group_count > 0 && !groups) {
        return false;
    }

    int owner[RINGMENU_MAX_ITEMS];
    for (int i = 0; i < item_count; i++) {
        owner[i] = -1;
    }
    for (int i = 0; i < group_count; i++) {
        int first = groups[i].first;
        int count = groups[i].count;
        int selected = groups[i].selected;
        if (count < 2 || first < 0 || first + count > item_count ||
            selected < first || selected >= first + count ||
            items[selected].state == RINGMENU_ITEM_GRAYED) {
            return false;
        }
        for (int slot = first; slot < first + count; slot++) {
            if (owner[slot] >= 0) {
                return false;
            }
            owner[slot] = i;
        }
    }
    return true;
}

static RingMenu *rm_create(const RingMenuItem *items, int count,
                           const RingMenuGroup *groups, int group_count) {
    if (!items || count < 1 || count > RINGMENU_MAX_ITEMS) return NULL;
    if (!rm_groups_valid(items, count, groups, group_count)) {
        return NULL;
    }

    RingMenu *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->count = count;
    m->highlight = -1;
    for (int i = 0; i < count; i++) {
        m->group_of[i] = -1;
    }
    m->group_count = group_count;
    for (int i = 0; i < group_count; i++) {
        m->groups[i].first = groups[i].first;
        m->groups[i].count = groups[i].count;
        m->groups[i].selected = groups[i].selected;
        for (int slot = groups[i].first; slot < groups[i].first + groups[i].count; slot++) {
            m->group_of[slot] = i;
        }
    }

    for (int i = 0; i < count; i++) {
        if (items[i].image) {
            int iw = items[i].image_w, ih = items[i].image_h;
            if (iw < 1 || ih < 1 || iw > RINGMENU_IMAGE_MAX ||
                ih > RINGMENU_IMAGE_MAX) {
                ringmenu_destroy(m);
                return NULL;
            }
            m->items[i].image = malloc((size_t)iw * ih * 4);
            if (!m->items[i].image) {
                ringmenu_destroy(m);
                return NULL;
            }
            memcpy(m->items[i].image, items[i].image, (size_t)iw * ih * 4);
            m->items[i].cw = iw;
            m->items[i].ch = ih;
        } else {
            const char *src = items[i].label ? items[i].label : "";
            int len = 0;
            while (len < RINGMENU_LABEL_MAX && src[len]) {
                m->items[i].label[len] = src[len];
                len++;
            }
            m->items[i].label[len] = '\0';
            m->items[i].cw = len > 0 ? (len * RM_ADVANCE - 1) * RM_SCALE : 0;
            m->items[i].ch = RM_GLYPH_H * RM_SCALE;
        }
        int led = items[i].led;
        m->items[i].led = (led < RINGMENU_LED_NONE || led > RINGMENU_LED_ON)
                              ? RINGMENU_LED_NONE : led;
        m->items[i].state = items[i].state == RINGMENU_ITEM_GRAYED
                            ? RINGMENU_ITEM_GRAYED : RINGMENU_ITEM_ACTIVE;
    }

    // Size the ring so every item fits inside its wedge. Content is drawn
    // upright at a known direction, so fit each item's rect exactly against
    // its own wedge: binary-search the smallest content radius that gives
    // the rect RM_PAD of clearance from the hole and the separators (the
    // clearance is monotone in the radius). The circumscribed-circle bound,
    // which always fits, serves as the search's upper limit.
    float half = RM_PI / (float)count;
    float rc = RM_INNER_RADIUS + RM_PAD;
    for (int i = 0; i < count; i++) {
        float w2 = m->items[i].cw / 2.0f;
        float h2 = m->items[i].ch / 2.0f;
        float A = -RM_PI / 2.0f + (float)i * 2.0f * half;
        float rho = sqrtf(w2 * w2 + h2 * h2);
        float lo = RM_INNER_RADIUS;
        float hi = RM_INNER_RADIUS + RM_PAD + rho;
        if (count >= 2) {
            float lateral = (rho + RM_PAD) / sinf(half);
            if (lateral > hi) hi = lateral;
        }
        for (int step = 0; step < 40; step++) {
            float mid = 0.5f * (lo + hi);
            if (rm_fit_clearance(mid, A, w2, h2, half,
                                 RM_INNER_RADIUS, count) >= RM_PAD) {
                hi = mid;
            } else {
                lo = mid;
            }
        }
        if (hi > rc) rc = hi;
    }
    // Reserve radial room for any slot's toggle light. The light sits at
    // RM_LED_FRAC*rc and must clear the center hole, its own label, and (for
    // narrow wedges) the wedge sides. Growing rc pushes every label out
    // uniformly, so the lights line up and never crowd the text.
    for (int i = 0; i < count; i++) {
        if (m->items[i].led == RINGMENU_LED_NONE) continue;
        float ch2 = m->items[i].ch / 2.0f;
        float f = RM_LED_FRAC;
        float need_hole = (RM_INNER_RADIUS + RM_LED_GAP + RM_LED_RADIUS) / f;
        float need_label = (RM_LED_GAP + RM_LED_RADIUS + ch2) / (1.0f - f);
        float need_side = (count >= 2)
            ? (RM_LED_RADIUS + RM_LED_GAP) / (f * sinf(half)) : 0.0f;
        float need = need_hole;
        if (need_label > need) need = need_label;
        if (need_side > need) need = need_side;
        if (need > rc) rc = need;
    }
    // Rim: enclose every content corner at the chosen radius, plus padding.
    float r1 = RM_MIN_OUTER;
    for (int i = 0; i < count; i++) {
        float w2 = m->items[i].cw / 2.0f;
        float h2 = m->items[i].ch / 2.0f;
        float A = -RM_PI / 2.0f + (float)i * 2.0f * half;
        float cxp = rc * cosf(A);
        float cyp = rc * sinf(A);
        for (int j = 0; j < 4; j++) {
            float px = cxp + ((j & 1) ? w2 : -w2);
            float py = cyp + ((j & 2) ? h2 : -h2);
            float need = sqrtf(px * px + py * py) + RM_PAD;
            if (need > r1) r1 = need;
        }
    }

    m->r0 = RM_INNER_RADIUS;
    m->rc = rc;
    m->r1 = ceilf(r1);
    m->size = 2 * ((int)m->r1 + 2);
    m->pix = malloc((size_t)m->size * m->size * 4);
    if (!m->pix) {
        ringmenu_destroy(m);
        return NULL;
    }
    // Optional: rm_render falls back to drawing the ring straight into pix.
    m->bg = malloc((size_t)m->size * m->size * 4);
    m->bg_highlight = -2;
    return m;
}

RingMenu *ringmenu_create(const RingMenuItem *items, int count) {
    return rm_create(items, count, NULL, 0);
}

RingMenu *ringmenu_create_grouped(const RingMenuItem *items, int item_count,
                                  const RingMenuGroup *groups, int group_count) {
    return rm_create(items, item_count, groups, group_count);
}

void ringmenu_destroy(RingMenu *m) {
    if (!m) return;
    for (int i = 0; i < m->count; i++) free(m->items[i].image);
    free(m->pix);
    free(m->bg);
    free(m);
}

void ringmenu_open(RingMenu *m, int x, int y, int bounds_w, int bounds_h) {
    if (!m) return;
    int r = m->size / 2;
    // Nudge the center so the whole menu stays on screen.
    if (bounds_w >= m->size) {
        if (x < r) x = r;
        if (x > bounds_w - r) x = bounds_w - r;
    } else {
        x = bounds_w / 2;
    }
    if (bounds_h >= m->size) {
        if (y < r) y = r;
        if (y > bounds_h - r) y = bounds_h - r;
    } else {
        y = bounds_h / 2;
    }
    m->cx = x;
    m->cy = y;
    m->last_x = x;
    m->last_y = y;
    m->highlight = -1;
    m->open = true;
    m->dirty = true;
    m->needs_render = true;
    rm_render(m);
}

bool ringmenu_is_open(const RingMenu *m) {
    return m && m->open;
}

int ringmenu_motion(RingMenu *m, int x, int y) {
    if (!m || !m->open) return RINGMENU_NONE;
    m->last_x = x;
    m->last_y = y;
    int slot = rm_hit(m, x, y);
    if (slot >= 0 && !rm_active(m, slot)) {
        slot = -1;
    }
    if (slot != m->highlight) {
        m->highlight = slot;
        m->dirty = true;
        m->needs_render = true;
    }
    return RINGMENU_NONE;
}

static int rm_close(RingMenu *m, int result) {
    m->open = false;
    m->highlight = -1;
    m->dirty = true;
    return result;
}

int ringmenu_button(RingMenu *m, int button, bool pressed) {
    if (!m || !m->open) return RINGMENU_NONE;

    if (button == RINGMENU_BTN_MIDDLE && pressed) {
        return rm_close(m, RINGMENU_CANCELLED);
    }
    // Releasing the right button and pressing the left one both commit:
    // select the slot under the pointer, cancel if there isn't one.
    if ((button == RINGMENU_BTN_RIGHT && !pressed) ||
        (button == RINGMENU_BTN_LEFT && pressed)) {
        int slot = rm_hit(m, m->last_x, m->last_y);
        if (slot >= 0 && !rm_active(m, slot)) {
            return RINGMENU_NONE;
        }
        int group = slot >= 0 ? rm_group_of(m, slot) : -1;
        if (group >= 0) {
            m->groups[group].selected = slot;
        }
        return rm_close(m, slot < 0 ? RINGMENU_CANCELLED : slot + 1);
    }
    return RINGMENU_NONE;
}

void ringmenu_rect(const RingMenu *m, int *x, int *y, int *w, int *h) {
    if (!m) return;
    if (x) *x = m->cx - m->size / 2;
    if (y) *y = m->cy - m->size / 2;
    if (w) *w = m->size;
    if (h) *h = m->size;
}

int ringmenu_size(const RingMenu *m) {
    return m ? m->size : 0;
}

void ringmenu_geometry(const RingMenu *m, int *cx, int *cy, float *r0, float *r1) {
    if (!m) return;
    if (cx) *cx = m->cx;
    if (cy) *cy = m->cy;
    if (r0) *r0 = m->r0;
    if (r1) *r1 = m->r1;
}

void ringmenu_update_image(RingMenu *m, int index, const uint32_t *image) {
    if (!m || index < 0 || index >= m->count || !m->items[index].image || !image) return;
    int iw = m->items[index].cw, ih = m->items[index].ch;
    memcpy(m->items[index].image, image, (size_t)iw * ih * 4);
    m->dirty = true;
    m->needs_render = true;
}

void ringmenu_set_led(RingMenu *m, int index, int led) {
    if (!m || index < 0 || index >= m->count) return;
    if (led < RINGMENU_LED_NONE || led > RINGMENU_LED_ON) return;
    if (m->items[index].led == led) return;
    m->items[index].led = led;
    m->dirty = true;
    m->needs_render = true;
}

bool ringmenu_set_item_state(RingMenu *m, int index, RingMenuItemState state) {
    if (!m || index < 0 || index >= m->count ||
        (state != RINGMENU_ITEM_ACTIVE && state != RINGMENU_ITEM_GRAYED)) {
        return false;
    }
    int group = rm_group_of(m, index);
    if (state == RINGMENU_ITEM_GRAYED && group >= 0 &&
        m->groups[group].selected == index) {
        return false;
    }
    if (m->items[index].state == state) {
        return true;
    }

    m->items[index].state = state;
    if (m->highlight == index && state == RINGMENU_ITEM_GRAYED) {
        m->highlight = -1;
    }
    m->dirty = true;
    m->needs_render = true;
    return true;
}

int ringmenu_group_selected(const RingMenu *m, int group) {
    if (!m || group < 0 || group >= m->group_count) {
        return RINGMENU_NONE;
    }
    return m->groups[group].selected;
}

bool ringmenu_set_group_selected(RingMenu *m, int group, int item) {
    if (!m || group < 0 || group >= m->group_count ||
        item < m->groups[group].first ||
        item >= m->groups[group].first + m->groups[group].count ||
        !rm_active(m, item)) {
        return false;
    }
    if (m->groups[group].selected == item) {
        return true;
    }

    m->groups[group].selected = item;
    m->dirty = true;
    m->needs_render = true;
    return true;
}

bool ringmenu_take_dirty(RingMenu *m) {
    if (!m || !m->dirty) return false;
    m->dirty = false;
    return true;
}

void ringmenu_draw(RingMenu *m, uint32_t *dst, int dst_w, int dst_h,
                   int dst_x, int dst_y) {
    if (!m || !m->open || !dst) return;
    if (m->needs_render) {
        rm_render(m);
        m->needs_render = false;
    }
    int ox = m->cx - m->size / 2;
    int oy = m->cy - m->size / 2;
    for (int y = 0; y < m->size; y++) {
        int dy = oy + y - dst_y;
        if (dy < 0 || dy >= dst_h) continue;
        const uint32_t *src = m->pix + (size_t)y * m->size;
        uint32_t *drow = dst + (size_t)dy * dst_w;
        for (int x = 0; x < m->size; x++) {
            int dx = ox + x - dst_x;
            if (dx < 0 || dx >= dst_w) continue;
            if (src[x] >> 24) drow[dx] = rm_over(src[x], drow[dx]);
        }
    }
}

// ---------------------------------------------------------------------------
// Color-field extension
// ---------------------------------------------------------------------------

static float rm_hue_to_rgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

static void rm_hsl_to_rgb(float h, float s, float l,
                          float *r, float *g, float *b) {
    if (s == 0.0f) {
        *r = *g = *b = l;
    } else {
        float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
        float p = 2.0f * l - q;
        *r = rm_hue_to_rgb(p, q, h + 1.0f / 3.0f);
        *g = rm_hue_to_rgb(p, q, h);
        *b = rm_hue_to_rgb(p, q, h - 1.0f / 3.0f);
    }
}

// Signed distance to a teardrop: circles of radius r1 (at the origin) and
// r2 (at (0, h)) joined by their common tangents.
static float rm_sd_teardrop(float px, float py, float r1, float r2, float h) {
    float b = (r1 - r2) / h;
    float a = sqrtf(1.0f - b * b);
    px = fabsf(px);
    float k = py * a - px * b;
    if (k < 0.0f) return sqrtf(px * px + py * py) - r1;
    if (k > h * a) return sqrtf(px * px + (py - h) * (py - h)) - r2;
    return px * a + py * b - r1;
}

uint32_t *ringmenu_color_drop(uint8_t r, uint8_t g, uint8_t b,
                              int slot, int count, int size) {
    if (count < 1 || size < 1) return NULL;
    // No calloc: ringmenu_color_drop_into clears the buffer itself.
    uint32_t *px_buf = malloc((size_t)size * size * 4);
    if (!px_buf) return NULL;
    if (!ringmenu_color_drop_into(px_buf, (size_t)size * size,
                                  r, g, b, slot, count, size)) {
        free(px_buf);
        return NULL;
    }
    return px_buf;
}

bool ringmenu_color_drop_into(uint32_t *px_buf, size_t capacity,
                              uint8_t r, uint8_t g, uint8_t b,
                              int slot, int count, int size) {
    if (!px_buf || count < 1 || size < 1 || slot < 0 || slot >= count ||
        capacity < (size_t)size * size) return false;
    memset(px_buf, 0, (size_t)size * size * sizeof(*px_buf));
    float ctr = size / 2.0f;

    // The drop's tail points back at the menu center.
    float half = RM_PI / (float)count;
    float target_a = -RM_PI / 2.0f + (float)slot * 2.0f * half;
    float in_x = -cosf(target_a);
    float in_y = -sinf(target_a);

    float r1 = size * 0.32f;
    float r2 = size * 0.08f;
    float h = size * 0.45f;
    float shift_y = 0.10f * size;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float dx = (float)x + 0.5f - ctr;
            float dy = (float)y + 0.5f - ctr;

            float px_rot = dx * (-in_y) + dy * (in_x);
            float py_rot = dx * (in_x) + dy * (in_y);

            float d = rm_sd_teardrop(px_rot, py_rot + shift_y, r1, r2, h);
            float alpha = 0.5f - d;

            if (alpha <= 0.0f) continue;
            if (alpha > 1.0f) alpha = 1.0f;

            px_buf[(size_t)y * size + x] =
                (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) |
                ((uint32_t)(alpha * 255.0f + 0.5f) << 24);
        }
    }
    return true;
}

float ringmenu_field_radius(const RingMenu *m) {
    if (!m) return 0.0f;
    return m->r1 + RINGMENU_FIELD_PAD + RINGMENU_FIELD_WIDTH + 2.0f;
}

int ringmenu_field_slot(const RingMenu *m, int x, int y) {
    if (!m || !m->open) return -1;
    float fx = (float)x - (float)m->cx;
    float fy = (float)y - (float)m->cy;
    if (sqrtf(fx * fx + fy * fy) <= m->r1) return -1;
    return rm_slot_of(m, fx, fy);
}

// Pointer position -> field coordinates for `slot`: u runs 0..1 along the
// arc (hue), v runs 0..1 across it, inner edge to outer (lightness).
static bool rm_field_uv(const RingMenu *m, int slot, int x, int y,
                        float *u, float *v) {
    float fx = (float)x - (float)m->cx;
    float fy = (float)y - (float)m->cy;
    float r = sqrtf(fx * fx + fy * fy);

    float r0 = m->r1 + RINGMENU_FIELD_PAD;
    float r1 = r0 + RINGMENU_FIELD_WIDTH;
    float half = RM_PI / (float)m->count;
    float target_a = -RM_PI / 2.0f + (float)slot * 2.0f * half;

    float a = atan2f(fy, fx);
    float diff = a - target_a;
    while (diff < -RM_PI) diff += 2.0f * RM_PI;
    while (diff > RM_PI) diff -= 2.0f * RM_PI;

    float raw_u = (diff + half) / (2.0f * half);
    float raw_v = (r - r0) / (r1 - r0);

    // Freeze updates if the pointer flies free of the color field, but allow
    // a 20% margin to comfortably snap to the absolute edges.
    if (raw_u < -0.2f || raw_u > 1.2f || raw_v < -0.2f || raw_v > 1.2f) {
        return false;
    }
    *u = rm_clamp01(raw_u);
    *v = rm_clamp01(raw_v);
    return true;
}

bool ringmenu_field_color(const RingMenu *m, int slot, int x, int y,
                          uint8_t *r, uint8_t *g, uint8_t *b) {
    if (!m || !m->open || slot < 0 || slot >= m->count) return false;
    float u, v;
    if (!rm_field_uv(m, slot, x, y, &u, &v)) return false;
    float rr, gg, bb;
    rm_hsl_to_rgb(u, 1.0f, v, &rr, &gg, &bb);
    if (r) *r = (uint8_t)(rr * 255.0f + 0.5f);
    if (g) *g = (uint8_t)(gg * 255.0f + 0.5f);
    if (b) *b = (uint8_t)(bb * 255.0f + 0.5f);
    return true;
}

void ringmenu_field_draw(const RingMenu *m, int slot, uint32_t *dst,
                         int dst_w, int dst_h, int dst_x, int dst_y) {
    if (!m || !m->open || !dst || slot < 0 || slot >= m->count) return;

    float r0 = m->r1 + RINGMENU_FIELD_PAD;
    float r1 = r0 + RINGMENU_FIELD_WIDTH;
    float half = RM_PI / (float)m->count;
    float target_a = -RM_PI / 2.0f + (float)slot * 2.0f * half;

    // Clip to the field's bounding square within dst.
    float rad = ringmenu_field_radius(m);
    int y_lo = (int)floorf(m->cy - rad) - dst_y;
    int y_hi = (int)ceilf(m->cy + rad) - dst_y;
    int x_lo = (int)floorf(m->cx - rad) - dst_x;
    int x_hi = (int)ceilf(m->cx + rad) - dst_x;
    if (y_lo < 0) y_lo = 0;
    if (x_lo < 0) x_lo = 0;
    if (y_hi > dst_h) y_hi = dst_h;
    if (x_hi > dst_w) x_hi = dst_w;

    for (int y = y_lo; y < y_hi; y++) {
        uint32_t *row = dst + (size_t)y * dst_w;
        float fy = (float)(dst_y + y) + 0.5f - m->cy;
        for (int x = x_lo; x < x_hi; x++) {
            float fx = (float)(dst_x + x) + 0.5f - m->cx;
            float r = sqrtf(fx * fx + fy * fy);

            float cov_r = (1.0f - (r > r1 ? r - r1 : 0.0f)) *
                          (1.0f - (r < r0 ? r0 - r : 0.0f));
            if (cov_r <= 0.0f) continue;
            if (cov_r > 1.0f) cov_r = 1.0f;

            float a = atan2f(fy, fx);
            float diff = a - target_a;
            while (diff < -RM_PI) diff += 2.0f * RM_PI;
            while (diff > RM_PI) diff -= 2.0f * RM_PI;

            float cov_a = 1.0f -
                (fabsf(diff) > half ? (fabsf(diff) - half) * r : 0.0f);
            if (cov_a <= 0.0f) continue;
            if (cov_a > 1.0f) cov_a = 1.0f;

            float cov = cov_r * cov_a;
            float u = rm_clamp01((diff + half) / (2.0f * half));
            float v = rm_clamp01((r - r0) / (r1 - r0));

            float rr, gg, bb;
            rm_hsl_to_rgb(u, 1.0f, v, &rr, &gg, &bb);

            float rc = rr * 255.0f;
            float gc = gg * 255.0f;
            float bc = bb * 255.0f;

            // Fold the menu's border color over the field's own rim.
            float d_edge = r - r0;
            if (r1 - r < d_edge) d_edge = r1 - r;
            float d_a = (half - fabsf(diff)) * r;
            if (d_a < d_edge) d_edge = d_a;

            if (d_edge < 1.5f) {
                float border_alpha = rm_clamp01(1.5f - d_edge);
                rc = rc + ((float)RM_BORDER[0] - rc) * border_alpha;
                gc = gc + ((float)RM_BORDER[1] - gc) * border_alpha;
                bc = bc + ((float)RM_BORDER[2] - bc) * border_alpha;
            }

            uint32_t src = (uint32_t)(rc + 0.5f) |
                           ((uint32_t)(gc + 0.5f) << 8) |
                           ((uint32_t)(bc + 0.5f) << 16) |
                           ((uint32_t)(cov * 255.0f) << 24);
            row[x] = rm_over(src, row[x]);
        }
    }
}
