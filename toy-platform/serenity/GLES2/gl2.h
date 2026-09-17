#ifndef TOY_SOFT_GLES2_H
#define TOY_SOFT_GLES2_H

/*
 * The GL ES 2 subset the toys call, for systems without a GL ES 2 driver.
 * gles2_soft.c implements it on the CPU. Enum values match Khronos gl2.h.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void GLvoid;
typedef char GLchar;
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef ptrdiff_t GLsizeiptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0

#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303

#define GL_TRIANGLE_STRIP 0x0005
#define GL_COLOR_BUFFER_BIT 0x00004000

#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11

#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_ALIGNMENT 0x0CF5

#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908

#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE0 0x84C0

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82

void glActiveTexture(GLenum texture);
void glAttachShader(GLuint program, GLuint shader);
void glBindTexture(GLenum target, GLuint texture);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glBlendFuncSeparate(GLenum src_rgb, GLenum dst_rgb, GLenum src_alpha, GLenum dst_alpha);
void glClear(GLbitfield mask);
void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glCompileShader(GLuint shader);
GLuint glCreateProgram(void);
GLuint glCreateShader(GLenum type);
void glDeleteProgram(GLuint program);
void glDeleteShader(GLuint shader);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glDisable(GLenum cap);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glEnable(GLenum cap);
void glEnableVertexAttribArray(GLuint index);
void glFinish(void);
void glGenTextures(GLsizei n, GLuint *textures);
GLint glGetAttribLocation(GLuint program, const GLchar *name);
void glGetProgramInfoLog(GLuint program, GLsizei max_length, GLsizei *length, GLchar *info_log);
void glGetProgramiv(GLuint program, GLenum pname, GLint *params);
void glGetShaderInfoLog(GLuint shader, GLsizei max_length, GLsizei *length, GLchar *info_log);
void glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
GLint glGetUniformLocation(GLuint program, const GLchar *name);
void glLinkProgram(GLuint program);
void glPixelStorei(GLenum pname, GLint param);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length);
void glTexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height,
                  GLint border, GLenum format, GLenum type, const void *pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                     GLsizei height, GLenum format, GLenum type, const void *pixels);
void glUniform1f(GLint location, GLfloat v0);
void glUniform1i(GLint location, GLint v0);
void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
void glUseProgram(GLuint program);
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                           GLsizei stride, const void *pointer);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);

#ifdef __cplusplus
}
#endif

#endif
