#ifndef TOY_GLES2_SOFT_H
#define TOY_GLES2_SOFT_H

/*
 * The backend's side of gles2_soft.c: where GL draws. Pixels are
 * premultiplied 0xAARRGGBB, top row first, owned by the caller.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void soft_gl_target(uint32_t *pixels, int width, int height);

#ifdef __cplusplus
}
#endif

#endif
