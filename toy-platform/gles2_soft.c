/*
 * A CPU implementation of the GL ES 2 subset the toys use, for systems whose
 * GL has no GLSL (SerenityOS). It does not compile shaders: every program is
 * the toys' one textured-quad program, run as C.
 *
 *     attribute vec2 pos, uv_in             location 0, 1
 *     uniform sampler2D tex                 0
 *     uniform float alpha, recolor          1, 2
 *     uniform vec3 light_color, dark_color  3, 4
 *
 *     mapped = tex.r * light_color + tex.g * dark_color
 *     out    = vec4(mix(tex.rgb, mapped, recolor), tex.a) * alpha
 *
 * Draws are axis-aligned quads (a 4-vertex GL_TRIANGLE_STRIP), which is all
 * the toys send. Anything else is reported once and skipped.
 */
#include "gles2_soft.h"

#include <GLES2/gl2.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SSE2__) && !defined(SOFT_NO_SIMD)
#include <emmintrin.h>
#define SOFT_SSE2 1
#endif

#define SOFT_ATTRIB_COUNT 2
#define SOFT_UNIFORM_TEX 0
#define SOFT_UNIFORM_ALPHA 1
#define SOFT_UNIFORM_RECOLOR 2
#define SOFT_UNIFORM_LIGHT 3
#define SOFT_UNIFORM_DARK 4
#define SOFT_QUAD_VERTICES 4
/* Vertices within this many pixels of a quad edge count as on it. */
#define SOFT_EDGE_EPSILON 0.01f
/* 8.8 fixed point: 256 is 1.0. */
#define SOFT_ONE 256
#define SOFT_TEXTURES_MIN_CAP 16

typedef struct {
    bool used;
    int w;
    int h;
    uint8_t *rgba;
    GLint min_filter;
    GLint mag_filter;
} SoftTexture;

typedef struct {
    const GLfloat *pointer;
    GLsizei stride;
} SoftAttrib;

typedef struct {
    float x;
    float y;
    float u;
    float v;
} SoftVertex;

/* The part of a draw that is fixed across its pixels. */
typedef struct {
    int recolor;
    int alpha;
    int light[3];
    int dark[3];
} SoftShade;

static struct {
    uint32_t *pixels;
    int width;
    int height;

    int viewport[4];
    int scissor[4];
    bool scissor_on;
    bool blend_on;
    GLenum blend[4];    /* src rgb, dst rgb, src alpha, dst alpha */
    uint32_t clear;

    SoftTexture *textures;
    GLuint texture_cap;
    GLuint bound;

    SoftAttrib attribs[SOFT_ATTRIB_COUNT];
    float alpha;
    float recolor;
    float light[3];
    float dark[3];

    GLuint next_object;
    bool warned_shape;
} g_soft = {
    .blend = { GL_ONE, GL_ZERO, GL_ONE, GL_ZERO },
    .alpha = 1.0f,
    .next_object = 1,
};

void soft_gl_target(uint32_t *pixels, int width, int height)
{
    g_soft.pixels = pixels;
    g_soft.width = width;
    g_soft.height = height;
}

static int soft_clamp(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

static uint8_t soft_unit(float value)
{
    return (uint8_t)soft_clamp((int)lroundf(value * 255.0f), 0, 255);
}

/* x * y / 255 for bytes, exact to rounding. */
static int soft_mul255(int x, int y)
{
    int t = x * y + 128;
    return (t + (t >> 8)) >> 8;
}

static SoftTexture *soft_texture(GLuint name)
{
    if(name == 0 || name >= g_soft.texture_cap || !g_soft.textures[name].used) {
        return NULL;
    }
    return &g_soft.textures[name];
}

/* ---- Programs: accepted, never compiled. ---- */

GLuint glCreateShader(GLenum type)
{
    (void)type;
    return g_soft.next_object++;
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)
{
    (void)shader;
    (void)count;
    (void)string;
    (void)length;
}

void glCompileShader(GLuint shader)
{
    (void)shader;
}

void glDeleteShader(GLuint shader)
{
    (void)shader;
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
    (void)shader;
    *params = pname == GL_COMPILE_STATUS ? GL_TRUE : 0;
}

void glGetShaderInfoLog(GLuint shader, GLsizei max_length, GLsizei *length, GLchar *info_log)
{
    (void)shader;
    if(length) {
        *length = 0;
    }
    if(info_log && max_length > 0) {
        info_log[0] = '\0';
    }
}

GLuint glCreateProgram(void)
{
    return g_soft.next_object++;
}

void glAttachShader(GLuint program, GLuint shader)
{
    (void)program;
    (void)shader;
}

void glLinkProgram(GLuint program)
{
    (void)program;
}

void glUseProgram(GLuint program)
{
    (void)program;
}

void glDeleteProgram(GLuint program)
{
    (void)program;
}

void glGetProgramiv(GLuint program, GLenum pname, GLint *params)
{
    (void)program;
    *params = pname == GL_LINK_STATUS ? GL_TRUE : 0;
}

void glGetProgramInfoLog(GLuint program, GLsizei max_length, GLsizei *length, GLchar *info_log)
{
    glGetShaderInfoLog(program, max_length, length, info_log);
}

GLint glGetAttribLocation(GLuint program, const GLchar *name)
{
    (void)program;
    if(strcmp(name, "pos") == 0) {
        return 0;
    }
    if(strcmp(name, "uv_in") == 0) {
        return 1;
    }
    return -1;
}

GLint glGetUniformLocation(GLuint program, const GLchar *name)
{
    (void)program;
    static const char *const names[] = { "tex", "alpha", "recolor", "light_color", "dark_color" };
    for(int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        if(strcmp(name, names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

void glUniform1i(GLint location, GLint v0)
{
    /* Only the sampler takes an int, and it is always unit 0. */
    (void)location;
    (void)v0;
}

void glUniform1f(GLint location, GLfloat v0)
{
    if(location == SOFT_UNIFORM_ALPHA) {
        g_soft.alpha = v0;
    } else if(location == SOFT_UNIFORM_RECOLOR) {
        g_soft.recolor = v0;
    }
}

void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2)
{
    float *target = location == SOFT_UNIFORM_LIGHT ? g_soft.light
                    : location == SOFT_UNIFORM_DARK ? g_soft.dark : NULL;
    if(!target) {
        return;
    }
    target[0] = v0;
    target[1] = v1;
    target[2] = v2;
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                           GLsizei stride, const void *pointer)
{
    (void)normalized;
    if(index >= SOFT_ATTRIB_COUNT || size != 2 || type != GL_FLOAT) {
        return;
    }
    g_soft.attribs[index].pointer = pointer;
    g_soft.attribs[index].stride = stride > 0 ? stride : (GLsizei)(2 * sizeof(GLfloat));
}

void glEnableVertexAttribArray(GLuint index)
{
    (void)index;
}

/* ---- State. ---- */

void glActiveTexture(GLenum texture)
{
    (void)texture;
}

static bool *soft_cap(GLenum cap)
{
    if(cap == GL_BLEND) {
        return &g_soft.blend_on;
    }
    if(cap == GL_SCISSOR_TEST) {
        return &g_soft.scissor_on;
    }
    return NULL;
}

void glEnable(GLenum cap)
{
    bool *flag = soft_cap(cap);
    if(flag) {
        *flag = true;
    }
}

void glDisable(GLenum cap)
{
    bool *flag = soft_cap(cap);
    if(flag) {
        *flag = false;
    }
}

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    glBlendFuncSeparate(sfactor, dfactor, sfactor, dfactor);
}

void glBlendFuncSeparate(GLenum src_rgb, GLenum dst_rgb, GLenum src_alpha, GLenum dst_alpha)
{
    g_soft.blend[0] = src_rgb;
    g_soft.blend[1] = dst_rgb;
    g_soft.blend[2] = src_alpha;
    g_soft.blend[3] = dst_alpha;
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    g_soft.viewport[0] = x;
    g_soft.viewport[1] = y;
    g_soft.viewport[2] = width;
    g_soft.viewport[3] = height;
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
    g_soft.scissor[0] = x;
    g_soft.scissor[1] = y;
    g_soft.scissor[2] = width;
    g_soft.scissor[3] = height;
}

void glPixelStorei(GLenum pname, GLint param)
{
    /* RGBA rows are whole multiples of every alignment. */
    (void)pname;
    (void)param;
}

void glFinish(void)
{
}

/* ---- Textures. ---- */

void glGenTextures(GLsizei n, GLuint *textures)
{
    GLuint name = 1;
    for(GLsizei i = 0; i < n; i++) {
        while(name < g_soft.texture_cap && g_soft.textures[name].used) {
            name++;
        }
        if(name >= g_soft.texture_cap) {
            GLuint cap = g_soft.texture_cap ? g_soft.texture_cap * 2 : SOFT_TEXTURES_MIN_CAP;
            SoftTexture *grown = realloc(g_soft.textures, cap * sizeof(*grown));
            if(!grown) {
                textures[i] = 0;
                continue;
            }
            memset(grown + g_soft.texture_cap, 0, (cap - g_soft.texture_cap) * sizeof(*grown));
            g_soft.textures = grown;
            g_soft.texture_cap = cap;
        }
        g_soft.textures[name] = (SoftTexture) {
            .used = true,
            .min_filter = GL_LINEAR,
            .mag_filter = GL_LINEAR,
        };
        textures[i] = name;
    }
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    for(GLsizei i = 0; i < n; i++) {
        SoftTexture *texture = soft_texture(textures[i]);
        if(!texture) {
            continue;
        }
        free(texture->rgba);
        *texture = (SoftTexture) {
            0
        };
        if(g_soft.bound == textures[i]) {
            g_soft.bound = 0;
        }
    }
}

void glBindTexture(GLenum target, GLuint texture)
{
    (void)target;
    g_soft.bound = texture;
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    (void)target;
    SoftTexture *texture = soft_texture(g_soft.bound);
    if(!texture) {
        return;
    }
    if(pname == GL_TEXTURE_MIN_FILTER) {
        texture->min_filter = param;
    } else if(pname == GL_TEXTURE_MAG_FILTER) {
        texture->mag_filter = param;
    }
}

void glTexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height,
                  GLint border, GLenum format, GLenum type, const void *pixels)
{
    (void)target;
    (void)level;
    (void)internal_format;
    (void)border;
    SoftTexture *texture = soft_texture(g_soft.bound);
    if(!texture || width <= 0 || height <= 0 || format != GL_RGBA || type != GL_UNSIGNED_BYTE) {
        return;
    }

    size_t bytes = (size_t)width * (size_t)height * 4;
    uint8_t *rgba = texture->w == width && texture->h == height ? texture->rgba : NULL;
    if(!rgba) {
        rgba = malloc(bytes);
        if(!rgba) {
            return;
        }
        free(texture->rgba);
        texture->rgba = rgba;
        texture->w = width;
        texture->h = height;
    }

    if(pixels) {
        memcpy(rgba, pixels, bytes);
    } else {
        memset(rgba, 0, bytes);
    }
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                     GLsizei height, GLenum format, GLenum type, const void *pixels)
{
    (void)target;
    (void)level;
    SoftTexture *texture = soft_texture(g_soft.bound);
    if(!texture || !texture->rgba || !pixels || format != GL_RGBA || type != GL_UNSIGNED_BYTE) {
        return;
    }
    if(xoffset < 0 || yoffset < 0 || width <= 0 || height <= 0 ||
            xoffset + width > texture->w || yoffset + height > texture->h) {
        return;
    }

    const uint8_t *src = pixels;
    size_t row_bytes = (size_t)width * 4;
    for(GLsizei y = 0; y < height; y++) {
        uint8_t *dst = texture->rgba + ((size_t)(yoffset + y) * (size_t)texture->w + (size_t)xoffset) * 4;
        memcpy(dst, src + (size_t)y * row_bytes, row_bytes);
    }
}

/* ---- Drawing. ---- */

/* The draw's clip in top-down pixels: the target, cut by any scissor. */
static void soft_clip(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = 0;
    *y0 = 0;
    *x1 = g_soft.width;
    *y1 = g_soft.height;
    if(!g_soft.scissor_on) {
        return;
    }

    int top = g_soft.height - (g_soft.scissor[1] + g_soft.scissor[3]);
    *x0 = soft_clamp(g_soft.scissor[0], 0, g_soft.width);
    *y0 = soft_clamp(top, 0, g_soft.height);
    *x1 = soft_clamp(g_soft.scissor[0] + g_soft.scissor[2], *x0, g_soft.width);
    *y1 = soft_clamp(top + g_soft.scissor[3], *y0, g_soft.height);
}

void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    g_soft.clear = (uint32_t)soft_unit(alpha) << 24 | (uint32_t)soft_unit(red) << 16 |
                   (uint32_t)soft_unit(green) << 8 | soft_unit(blue);
}

void glClear(GLbitfield mask)
{
    if(!(mask & GL_COLOR_BUFFER_BIT) || !g_soft.pixels) {
        return;
    }

    int x0, y0, x1, y1;
    soft_clip(&x0, &y0, &x1, &y1);
    for(int y = y0; y < y1; y++) {
        uint32_t *row = g_soft.pixels + (size_t)y * (size_t)g_soft.width;
        for(int x = x0; x < x1; x++) {
            row[x] = g_soft.clear;
        }
    }
}

/* Vertex i of the bound arrays, in top-down window pixels. */
static SoftVertex soft_vertex(GLint i)
{
    const SoftAttrib *pos = &g_soft.attribs[0];
    const SoftAttrib *uv = &g_soft.attribs[1];
    const GLfloat *p = (const GLfloat *)((const char *)pos->pointer + (size_t)i * (size_t)pos->stride);
    const GLfloat *t = (const GLfloat *)((const char *)uv->pointer + (size_t)i * (size_t)uv->stride);

    float window_y = (p[1] + 1.0f) * 0.5f * (float)g_soft.viewport[3] + (float)g_soft.viewport[1];
    return (SoftVertex) {
        .x = (p[0] + 1.0f) * 0.5f * (float)g_soft.viewport[2] + (float)g_soft.viewport[0],
        .y = (float)g_soft.height - window_y,
        .u = t[0],
        .v = t[1],
    };
}

static bool soft_near(float a, float b)
{
    return fabsf(a - b) <= SOFT_EDGE_EPSILON;
}

/* The quad's corners: lo is top-left, hi bottom-right, each with its uv.
   False unless the four vertices form an axis-aligned rectangle. */
static bool soft_quad(GLint first, SoftVertex *lo, SoftVertex *hi)
{
    SoftVertex v[SOFT_QUAD_VERTICES];
    for(int i = 0; i < SOFT_QUAD_VERTICES; i++) {
        v[i] = soft_vertex(first + i);
    }

    *lo = v[0];
    *hi = v[0];
    for(int i = 1; i < SOFT_QUAD_VERTICES; i++) {
        if(v[i].x < lo->x) {
            lo->x = v[i].x;
            lo->u = v[i].u;
        }
        if(v[i].x > hi->x) {
            hi->x = v[i].x;
            hi->u = v[i].u;
        }
        if(v[i].y < lo->y) {
            lo->y = v[i].y;
            lo->v = v[i].v;
        }
        if(v[i].y > hi->y) {
            hi->y = v[i].y;
            hi->v = v[i].v;
        }
    }

    for(int i = 0; i < SOFT_QUAD_VERTICES; i++) {
        bool on_x = soft_near(v[i].x, lo->x) || soft_near(v[i].x, hi->x);
        bool on_y = soft_near(v[i].y, lo->y) || soft_near(v[i].y, hi->y);
        if(!on_x || !on_y) {
            return false;
        }
    }
    return hi->x > lo->x && hi->y > lo->y;
}

typedef enum {
    SOFT_BLEND_REPLACE,     /* blending off */
    SOFT_BLEND_PREMUL,      /* ONE, ONE_MINUS_SRC_ALPHA */
    SOFT_BLEND_STRAIGHT,    /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA; ONE, ONE_MINUS_SRC_ALPHA */
    SOFT_BLEND_GENERIC,
} SoftBlend;

typedef enum {
    SOFT_RECOLOR_OFF,
    SOFT_RECOLOR_FULL,
    SOFT_RECOLOR_MIX,
} SoftRecolor;

/* Where each target column samples: two texel offsets into a row and the
   8-bit weight of the second. Built once per draw, shared by every row. */
typedef struct {
    int32_t *x0;
    int32_t *x1;
    int32_t *fx;
    int cap;
} SoftColumns;

static SoftColumns g_columns;

static bool soft_columns_reserve(int count)
{
    if(count <= g_columns.cap) {
        return true;
    }
    int32_t *x0 = realloc(g_columns.x0, (size_t)count * sizeof(int32_t));
    if(x0) {
        g_columns.x0 = x0;
    }
    int32_t *x1 = realloc(g_columns.x1, (size_t)count * sizeof(int32_t));
    if(x1) {
        g_columns.x1 = x1;
    }
    int32_t *fx = realloc(g_columns.fx, (size_t)count * sizeof(int32_t));
    if(fx) {
        g_columns.fx = fx;
    }
    if(!x0 || !x1 || !fx) {
        return false;
    }
    g_columns.cap = count;
    return true;
}

static SoftBlend soft_blend_mode(void)
{
    const GLenum *b = g_soft.blend;
    if(!g_soft.blend_on) {
        return SOFT_BLEND_REPLACE;
    }
    if(b[0] == GL_ONE && b[1] == GL_ONE_MINUS_SRC_ALPHA && b[2] == GL_ONE && b[3] == GL_ONE_MINUS_SRC_ALPHA) {
        return SOFT_BLEND_PREMUL;
    }
    if(b[0] == GL_SRC_ALPHA && b[1] == GL_ONE_MINUS_SRC_ALPHA && b[2] == GL_ONE && b[3] == GL_ONE_MINUS_SRC_ALPHA) {
        return SOFT_BLEND_STRAIGHT;
    }
    return SOFT_BLEND_GENERIC;
}

static int soft_factor(GLenum factor, int src_alpha)
{
    switch(factor) {
    case GL_ONE:
        return 255;
    case GL_SRC_ALPHA:
        return src_alpha;
    case GL_ONE_MINUS_SRC_ALPHA:
        return 255 - src_alpha;
    default:
        return 0;
    }
}

static inline uint32_t soft_swap_rb(uint32_t w)
{
    return (w & 0xFF00FF00u) | ((w >> 16) & 0xFFu) | ((w & 0xFFu) << 16);
}

static inline uint32_t soft_pack(int r, int g, int b, int a)
{
    return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

/* One fragment: shade the filtered texel c (bytes RGBA) and blend it into
   *dst. Always inlined with constant modes, so each combination compiles to
   its own branch-free path. */
static inline __attribute__((always_inline)) void soft_fragment(const int c[4], const SoftShade *shade,
        SoftRecolor recolor, bool full_alpha,
        SoftBlend blend, uint32_t *dst)
{
    int rgb[3] = { c[0], c[1], c[2] };
    if(recolor != SOFT_RECOLOR_OFF) {
        for(int k = 0; k < 3; k++) {
            int mapped = (c[0] * shade->light[k] + c[1] * shade->dark[k]) >> 8;
            rgb[k] = recolor == SOFT_RECOLOR_FULL ? mapped : c[k] + (((mapped - c[k]) * shade->recolor) >> 8);
        }
    }

    int a = c[3];
    if(!full_alpha) {
        for(int k = 0; k < 3; k++) {
            rgb[k] = (rgb[k] * shade->alpha) >> 8;
        }
        a = (a * shade->alpha) >> 8;
    }
    for(int k = 0; k < 3; k++) {
        rgb[k] = rgb[k] < 0 ? 0 : (rgb[k] > 255 ? 255 : rgb[k]);
    }
    a = a > 255 ? 255 : a;

    uint32_t d = *dst;
    int d_rgb[3] = { (d >> 16) & 0xFF, (d >> 8) & 0xFF, d & 0xFF };
    int d_a = d >> 24;
    int inv = 255 - a;

    switch(blend) {
    case SOFT_BLEND_REPLACE:
        *dst = soft_pack(rgb[0], rgb[1], rgb[2], a);
        return;
    case SOFT_BLEND_PREMUL:
        if(inv == 0) {
            *dst = soft_pack(rgb[0], rgb[1], rgb[2], 255);
            return;
        }
        for(int k = 0; k < 3; k++) {
            int v = rgb[k] + soft_mul255(d_rgb[k], inv);
            rgb[k] = v > 255 ? 255 : v;
        }
        *dst = soft_pack(rgb[0], rgb[1], rgb[2], a + soft_mul255(d_a, inv));
        return;
    case SOFT_BLEND_STRAIGHT:
        for(int k = 0; k < 3; k++) {
            rgb[k] = soft_mul255(rgb[k], a) + soft_mul255(d_rgb[k], inv);
        }
        *dst = soft_pack(rgb[0], rgb[1], rgb[2], a + soft_mul255(d_a, inv));
        return;
    case SOFT_BLEND_GENERIC:
        break;
    }

    const GLenum *f = g_soft.blend;
    int out[4];
    for(int k = 0; k < 3; k++) {
        int v = soft_mul255(rgb[k], soft_factor(f[0], a)) + soft_mul255(d_rgb[k], soft_factor(f[1], a));
        out[k] = v > 255 ? 255 : v;
    }
    int v = soft_mul255(a, soft_factor(f[2], a)) + soft_mul255(d_a, soft_factor(f[3], a));
    *dst = soft_pack(out[0], out[1], out[2], v > 255 ? 255 : v);
}

#ifdef SOFT_SSE2
/* soft_fragment for the three common blends, four channels at a time in
   16-bit lanes. Rounding differs from the scalar path by at most one step.
   Lanes run B, G, R, A: the order of a 0xAARRGGBB pixel in memory. */
static inline __attribute__((always_inline)) void soft_fragment_sse2(uint32_t wa, uint32_t wb, uint32_t wc,
        uint32_t wd, int fx, int fy,
        const SoftShade *shade, SoftRecolor recolor,
        bool full_alpha, SoftBlend blend,
        uint32_t *dst)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i one = _mm_set1_epi16(SOFT_ONE);
    const __m128i max = _mm_set1_epi16(255);
    const __m128i half = _mm_set1_epi16(128);

    __m128i t = _mm_set_epi32((int)wd, (int)wc, (int)wb, (int)wa);
    __m128i a = _mm_unpacklo_epi8(t, zero);
    __m128i b = _mm_unpacklo_epi8(_mm_srli_si128(t, 4), zero);
    __m128i c = _mm_unpacklo_epi8(_mm_srli_si128(t, 8), zero);
    __m128i d = _mm_unpacklo_epi8(_mm_srli_si128(t, 12), zero);

    __m128i wx = _mm_set1_epi16((short)fx);
    __m128i wx_inv = _mm_sub_epi16(one, wx);
    __m128i wy = _mm_set1_epi16((short)fy);
    __m128i wy_inv = _mm_sub_epi16(one, wy);
    __m128i upper = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(a, wx_inv), _mm_mullo_epi16(b, wx)), 8);
    __m128i lower = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(c, wx_inv), _mm_mullo_epi16(d, wx)), 8);
    __m128i v = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(upper, wy_inv),
                               _mm_mullo_epi16(lower, wy)), half), 8);

    if(recolor != SOFT_RECOLOR_OFF) {
        /* mapped = r * light + g * dark, per channel; lanes 2 and 1 hold r, g. */
        __m128i r = _mm_set1_epi16((short)_mm_extract_epi16(v, 2));
        __m128i g = _mm_set1_epi16((short)_mm_extract_epi16(v, 1));
        __m128i light = _mm_set_epi16(0, 0, 0, 0, 0, (short)shade->light[0], (short)shade->light[1], (short)shade->light[2]);
        __m128i dark = _mm_set_epi16(0, 0, 0, 0, 0, (short)shade->dark[0], (short)shade->dark[1], (short)shade->dark[2]);
        __m128i mapped = _mm_add_epi16(_mm_srli_epi16(_mm_mullo_epi16(r, light), 8),
                                       _mm_srli_epi16(_mm_mullo_epi16(g, dark), 8));
        mapped = _mm_min_epi16(mapped, _mm_set1_epi16(4 * 255));
        __m128i alpha_lane = _mm_and_si128(v, _mm_set_epi16(0, 0, 0, 0, -1, 0, 0, 0));
        if(recolor == SOFT_RECOLOR_FULL) {
            v = _mm_or_si128(mapped, alpha_lane);
        } else {
            __m128i mix = _mm_set1_epi16((short)shade->recolor);
            __m128i mix_inv = _mm_sub_epi16(one, mix);
            __m128i color = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(v, mix_inv),
                                           _mm_mullo_epi16(_mm_min_epi16(mapped, max), mix)), 8);
            v = _mm_or_si128(_mm_andnot_si128(_mm_set_epi16(0, 0, 0, 0, -1, 0, 0, 0), color), alpha_lane);
        }
    }

    if(!full_alpha) {
        v = _mm_srli_epi16(_mm_mullo_epi16(_mm_min_epi16(v, max), _mm_set1_epi16((short)shade->alpha)), 8);
    }
    v = _mm_min_epi16(v, max);

    int alpha = _mm_extract_epi16(v, 3);
    if(blend == SOFT_BLEND_REPLACE || (blend == SOFT_BLEND_PREMUL && alpha == 255)) {
        *dst = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(v, zero));
        return;
    }

    /* x * y / 255, rounded: (t + (t >> 8)) >> 8 with t = x * y + 128. */
    __m128i inv = _mm_set1_epi16((short)(255 - alpha));
    __m128i dv = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int) * dst), zero);
    __m128i dt = _mm_add_epi16(_mm_mullo_epi16(dv, inv), half);
    __m128i dst_part = _mm_srli_epi16(_mm_add_epi16(dt, _mm_srli_epi16(dt, 8)), 8);

    if(blend == SOFT_BLEND_STRAIGHT) {
        /* Colour takes src * alpha; alpha takes src * 1. */
        __m128i factor = _mm_set_epi16(0, 0, 0, 0, 255, (short)alpha, (short)alpha, (short)alpha);
        __m128i st = _mm_add_epi16(_mm_mullo_epi16(v, factor), half);
        v = _mm_srli_epi16(_mm_add_epi16(st, _mm_srli_epi16(st, 8)), 8);
    }
    v = _mm_add_epi16(v, dst_part);
    *dst = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(v, zero));
}
#endif

/* The rows of one draw, from y0 to y1. Same always-inline trick as above. */
static inline __attribute__((always_inline)) void soft_rows(const SoftTexture *tex, int x0, int x1, int y0, int y1,
        int32_t ty, int32_t step_y, bool linear,
        const SoftShade *shade, SoftRecolor recolor,
        bool full_alpha, SoftBlend blend)
{
    int32_t *cx0 = g_columns.x0;
    int32_t *cx1 = g_columns.x1;
    int32_t *cfx = g_columns.fx;
    size_t stride = (size_t)tex->w * 4;

    for(int y = y0; y < y1; y++, ty += step_y) {
        int iy = linear ? ty >> 16 : (ty + (1 << 15)) >> 16;
        int fy = linear ? (ty >> 8) & 0xFF : 0;
        int row0 = soft_clamp(iy, 0, tex->h - 1);
        int row1 = soft_clamp(iy + 1, 0, tex->h - 1);
        const uint8_t *top = tex->rgba + (size_t)row0 * stride;
        const uint8_t *bottom = tex->rgba + (size_t)row1 * stride;
        uint32_t *dst = g_soft.pixels + (size_t)y * (size_t)g_soft.width;

        for(int x = x0; x < x1; x++) {
            const uint8_t *a = top + cx0[x - x0];
            const uint8_t *b = top + cx1[x - x0];
            const uint8_t *c = bottom + cx0[x - x0];
            const uint8_t *d = bottom + cx1[x - x0];

            /* Four blank texels filter to blank, and blank changes nothing
               under the blends the toys use. */
            uint32_t wa, wb, wc, wd;
            memcpy(&wa, a, 4);
            memcpy(&wb, b, 4);
            memcpy(&wc, c, 4);
            memcpy(&wd, d, 4);
            if((wa | wb | wc | wd) == 0 && blend != SOFT_BLEND_REPLACE) {
                continue;
            }

            int fx = cfx[x - x0];
#ifdef SOFT_SSE2
            if(blend != SOFT_BLEND_GENERIC) {
                /* Texels are RGBA bytes; swap R and B so the lanes match the target. */
                soft_fragment_sse2(soft_swap_rb(wa), soft_swap_rb(wb), soft_swap_rb(wc), soft_swap_rb(wd),
                                   fx, fy, shade, recolor, full_alpha, blend, &dst[x]);
                continue;
            }
#endif
            int texel[4];
            for(int k = 0; k < 4; k++) {
                int upper = a[k] * (SOFT_ONE - fx) + b[k] * fx;
                int lower = c[k] * (SOFT_ONE - fx) + d[k] * fx;
                texel[k] = (upper * (SOFT_ONE - fy) + lower * fy + (1 << 15)) >> 16;
            }
            soft_fragment(texel, shade, recolor, full_alpha, blend, &dst[x]);
        }
    }
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    const SoftTexture *tex = soft_texture(g_soft.bound);
    if(mode != GL_TRIANGLE_STRIP || count != SOFT_QUAD_VERTICES || !tex || !tex->rgba || !g_soft.pixels ||
            !g_soft.attribs[0].pointer || !g_soft.attribs[1].pointer) {
        return;
    }

    SoftVertex lo, hi;
    if(!soft_quad(first, &lo, &hi)) {
        if(!g_soft.warned_shape) {
            fprintf(stderr, "gles2_soft: only axis-aligned quads are drawn\n");
            g_soft.warned_shape = true;
        }
        return;
    }

    int clip_x0, clip_y0, clip_x1, clip_y1;
    soft_clip(&clip_x0, &clip_y0, &clip_x1, &clip_y1);
    /* Pixels whose centres fall inside the quad. */
    int x0 = soft_clamp((int)ceilf(lo.x - 0.5f), clip_x0, clip_x1);
    int x1 = soft_clamp((int)ceilf(hi.x - 0.5f), clip_x0, clip_x1);
    int y0 = soft_clamp((int)ceilf(lo.y - 0.5f), clip_y0, clip_y1);
    int y1 = soft_clamp((int)ceilf(hi.y - 0.5f), clip_y0, clip_y1);
    if(x0 >= x1 || y0 >= y1 || !soft_columns_reserve(x1 - x0)) {
        return;
    }

    /* Texel coordinates advance linearly across the quad: 16.16 per pixel,
       offset half a texel so the integer part is the left/top neighbour. */
    double du = (double)(hi.u - lo.u) * tex->w / (double)(hi.x - lo.x);
    double dv = (double)(hi.v - lo.v) * tex->h / (double)(hi.y - lo.y);
    double u0 = (double)lo.u * tex->w + ((double)x0 + 0.5 - lo.x) * du - 0.5;
    double v0 = (double)lo.v * tex->h + ((double)y0 + 0.5 - lo.y) * dv - 0.5;
    int32_t step_x = (int32_t)lround(du * 65536.0);
    int32_t step_y = (int32_t)lround(dv * 65536.0);
    int32_t start_x = (int32_t)lround(u0 * 65536.0);
    int32_t start_y = (int32_t)lround(v0 * 65536.0);

    bool magnified = fabs(du) <= 1.0 && fabs(dv) <= 1.0;
    GLint filter = magnified ? tex->mag_filter : tex->min_filter;
    bool linear = filter == GL_LINEAR;

    int32_t tx = start_x;
    for(int i = 0; i < x1 - x0; i++, tx += step_x) {
        int ix = linear ? tx >> 16 : (tx + (1 << 15)) >> 16;
        g_columns.x0[i] = soft_clamp(ix, 0, tex->w - 1) * 4;
        g_columns.x1[i] = linear ? soft_clamp(ix + 1, 0, tex->w - 1) * 4 : g_columns.x0[i];
        g_columns.fx[i] = linear ? (tx >> 8) & 0xFF : 0;
    }

    SoftShade shade = {
        .recolor = soft_clamp((int)lroundf(g_soft.recolor * SOFT_ONE), 0, SOFT_ONE),
        .alpha = soft_clamp((int)lroundf(g_soft.alpha * SOFT_ONE), 0, SOFT_ONE),
    };
    for(int k = 0; k < 3; k++) {
        shade.light[k] = (int)lroundf(g_soft.light[k] * SOFT_ONE);
        shade.dark[k] = (int)lroundf(g_soft.dark[k] * SOFT_ONE);
    }

    SoftRecolor recolor = shade.recolor == 0 ? SOFT_RECOLOR_OFF
                          : shade.recolor == SOFT_ONE ? SOFT_RECOLOR_FULL : SOFT_RECOLOR_MIX;
    bool full_alpha = shade.alpha == SOFT_ONE;
    SoftBlend blend = soft_blend_mode();

    /* The combinations the toys draw get their own compiled loop; the rest
       share the generic one. */
    if(blend == SOFT_BLEND_PREMUL && recolor == SOFT_RECOLOR_FULL && full_alpha) {
        soft_rows(tex, x0, x1, y0, y1, start_y, step_y, linear, &shade, SOFT_RECOLOR_FULL, true, SOFT_BLEND_PREMUL);
    } else if(blend == SOFT_BLEND_PREMUL && recolor == SOFT_RECOLOR_FULL) {
        soft_rows(tex, x0, x1, y0, y1, start_y, step_y, linear, &shade, SOFT_RECOLOR_FULL, false, SOFT_BLEND_PREMUL);
    } else if(blend == SOFT_BLEND_STRAIGHT && recolor == SOFT_RECOLOR_OFF && full_alpha) {
        soft_rows(tex, x0, x1, y0, y1, start_y, step_y, linear, &shade, SOFT_RECOLOR_OFF, true, SOFT_BLEND_STRAIGHT);
    } else {
        soft_rows(tex, x0, x1, y0, y1, start_y, step_y, linear, &shade, recolor, full_alpha, blend);
    }
}
