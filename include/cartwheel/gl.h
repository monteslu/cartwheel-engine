/* The GL API cartwheel renders with: OpenGL ES 3.0 with WebGL2 semantics, the
 * single feature level on every target. Native links libGLESv2 (Mesa, or
 * ANGLE on Windows/macOS/iOS); carts import the same entry points from the
 * wasmcart "gl" module. */
#ifndef CW_GL_H
#define CW_GL_H

#if defined(__wasm__)
#ifndef WC_USE_GL
#define WC_USE_GL
#endif
#include "wasmcart.h"
#define CW_GL_IMPORT(name) __attribute__((import_module("gl"), import_name(#name)))
/* GLES 3.0 entry points the wasmcart hosts implement but wasmcart.h does not
 * declare (wasmcart's docs/gl-surface.md lists the full surface). */
CW_GL_IMPORT(glRenderbufferStorageMultisample)
extern void glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                             GLsizei height);
CW_GL_IMPORT(glGetFloatv) extern void glGetFloatv(GLenum pname, GLfloat *data);
/* query objects (GPU timing in the render pipeline) */
CW_GL_IMPORT(glGenQueries) extern void glGenQueries(GLsizei n, GLuint *ids);
CW_GL_IMPORT(glDeleteQueries) extern void glDeleteQueries(GLsizei n, const GLuint *ids);
CW_GL_IMPORT(glBeginQuery) extern void glBeginQuery(GLenum target, GLuint id);
CW_GL_IMPORT(glEndQuery) extern void glEndQuery(GLenum target);
CW_GL_IMPORT(glGetQueryObjectuiv) extern void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint *params);
/* transform feedback (GPU particles); the default transform feedback object
 * only (WebGL2 has one per context too) */
CW_GL_IMPORT(glBeginTransformFeedback) extern void glBeginTransformFeedback(GLenum primitiveMode);
CW_GL_IMPORT(glEndTransformFeedback) extern void glEndTransformFeedback(void);
CW_GL_IMPORT(glTransformFeedbackVaryings)
extern void glTransformFeedbackVaryings(GLuint program, GLsizei count, const char *const *varyings, GLenum bufferMode);
CW_GL_IMPORT(glUniform1ui) extern void glUniform1ui(GLint location, GLuint v0);
CW_GL_IMPORT(glVertexAttrib4fv) extern void glVertexAttrib4fv(GLuint index, const GLfloat *v);
CW_GL_IMPORT(glClearBufferfv) extern void glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat *value);
#ifndef GL_COLOR
#define GL_COLOR 0x1800
#endif
#ifndef GL_DYNAMIC_COPY
#define GL_DYNAMIC_COPY 0x88EA
#endif
#ifndef GL_TRANSFORM_FEEDBACK_BUFFER
#define GL_TRANSFORM_FEEDBACK_BUFFER 0x8C8E
#endif
#ifndef GL_RASTERIZER_DISCARD
#define GL_RASTERIZER_DISCARD 0x8C89
#endif
#ifndef GL_SEPARATE_ATTRIBS
#define GL_SEPARATE_ATTRIBS 0x8C8D
#endif
#ifndef GL_INTERLEAVED_ATTRIBS
#define GL_INTERLEAVED_ATTRIBS 0x8C8C
#endif
/* uniform buffers (volumetric fog) */
#ifndef GL_UNIFORM_BUFFER
#define GL_UNIFORM_BUFFER 0x8A11
#endif
#ifndef GL_INVALID_INDEX
#define GL_INVALID_INDEX 0xFFFFFFFFu
#endif
/* occlusion queries (culling) */
#ifndef GL_ANY_SAMPLES_PASSED
#define GL_ANY_SAMPLES_PASSED 0x8C2F
#endif
#ifndef GL_ANY_SAMPLES_PASSED_CONSERVATIVE
#define GL_ANY_SAMPLES_PASSED_CONSERVATIVE 0x8D6A
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif
#ifndef GL_RGB16F
#define GL_RGB16F 0x881B
#endif
#ifndef GL_UNPACK_ALIGNMENT
#define GL_UNPACK_ALIGNMENT 0x0CF5
#endif
#ifndef GL_SHADING_LANGUAGE_VERSION
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#endif
#ifndef GL_PACK_ALIGNMENT
#define GL_PACK_ALIGNMENT 0x0D05
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_SAMPLES
#define GL_SAMPLES 0x80A9
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
#ifndef GL_EXTENSIONS
#define GL_EXTENSIONS 0x1F03
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
/* integer and depth-compare textures */
#ifndef GL_RG32UI
#define GL_RG32UI 0x823C
#endif
#ifndef GL_R32UI
#define GL_R32UI 0x8236
#endif
#ifndef GL_RG_INTEGER
#define GL_RG_INTEGER 0x8228
#endif
#ifndef GL_RED_INTEGER
#define GL_RED_INTEGER 0x8D94
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE 0x884C
#endif
#ifndef GL_TEXTURE_COMPARE_FUNC
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#endif
#ifndef GL_COMPARE_REF_TO_TEXTURE
#define GL_COMPARE_REF_TO_TEXTURE 0x884E
#endif
CW_GL_IMPORT(glCopyTexSubImage2D)
extern void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width,
                                GLsizei height);
/* texture units past 0 (render2d binds normals and light buffers there) */
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_TEXTURE2
#define GL_TEXTURE2 0x84C2
#endif
#else
#include <GLES3/gl3.h>
#endif

/* EXT_disjoint_timer_query (GLES) / EXT_disjoint_timer_query_webgl2 */
#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif

/* Framebuffer 0 is "the window" to engine and game GL code. With
 * cw_app_desc.pixel_scale or crisp UI the frame draws into an offscreen
 * target that stands in for the window (src/platform/pixel.c), so binding 0
 * binds that target while it is current. */
extern unsigned cw__gl_window_fbo;
static inline void cw__gl_bind_framebuffer(GLenum target, GLuint fb) { glBindFramebuffer(target, fb ? fb : cw__gl_window_fbo); }
#define glBindFramebuffer(target, fb) cw__gl_bind_framebuffer((target), (fb))

#endif
