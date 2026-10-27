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
/* GL on a WebGPU run. A build that can render with WebGPU (CW_WGPU: the dual
 * cart tree, native-dawn) has no GL context while WebGPU is selected; a dual
 * cart that calls GL then traps in the host ("the cart called glX, but this
 * host selected WebGPU"), which names the GL function but not who called it.
 * So every GL entry point is checked here first: on a WebGPU run the engine
 * stops with an error naming the GL function, the file and line of the call
 * and the feature that was running (the render pass, or the label a GL-only
 * path sets in cw__gl_feature). GL runs pay one predictable branch. */
#if CW_WGPU
#include <stdbool.h>
extern bool cw__gl_banned;         /* WebGPU is the selected backend (src/platform/gpu.c) */
extern const char *cw__gl_feature; /* what is running: a pass name or a GL-only path's label */
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
void cw__gl_trap(const char *fn, const char *file, int line);
static inline void cw__gl_guard(const char *fn, const char *file, int line) {
  if (__builtin_expect(cw__gl_banned, 0)) cw__gl_trap(fn, file, line);
}
#define CW__GL_GUARDED(fn, ...) (cw__gl_guard(#fn, __FILE__, __LINE__), (fn)(__VA_ARGS__))
static inline void cw__gl_bind_framebuffer_at(GLenum target, GLuint fb, const char *file, int line) {
  cw__gl_guard("glBindFramebuffer", file, line);
  glBindFramebuffer(target, fb ? fb : cw__gl_window_fbo);
}
#define glBindFramebuffer(target, fb) cw__gl_bind_framebuffer_at((target), (fb), __FILE__, __LINE__)
/* clang-format off */
#define glActiveTexture(...) CW__GL_GUARDED(glActiveTexture, __VA_ARGS__)
#define glAttachShader(...) CW__GL_GUARDED(glAttachShader, __VA_ARGS__)
#define glBeginQuery(...) CW__GL_GUARDED(glBeginQuery, __VA_ARGS__)
#define glBeginTransformFeedback(...) CW__GL_GUARDED(glBeginTransformFeedback, __VA_ARGS__)
#define glBindAttribLocation(...) CW__GL_GUARDED(glBindAttribLocation, __VA_ARGS__)
#define glBindBuffer(...) CW__GL_GUARDED(glBindBuffer, __VA_ARGS__)
#define glBindBufferBase(...) CW__GL_GUARDED(glBindBufferBase, __VA_ARGS__)
#define glBindBufferRange(...) CW__GL_GUARDED(glBindBufferRange, __VA_ARGS__)
#define glBindRenderbuffer(...) CW__GL_GUARDED(glBindRenderbuffer, __VA_ARGS__)
#define glBindSampler(...) CW__GL_GUARDED(glBindSampler, __VA_ARGS__)
#define glBindTexture(...) CW__GL_GUARDED(glBindTexture, __VA_ARGS__)
#define glBindTransformFeedback(...) CW__GL_GUARDED(glBindTransformFeedback, __VA_ARGS__)
#define glBindVertexArray(...) CW__GL_GUARDED(glBindVertexArray, __VA_ARGS__)
#define glBlendColor(...) CW__GL_GUARDED(glBlendColor, __VA_ARGS__)
#define glBlendEquation(...) CW__GL_GUARDED(glBlendEquation, __VA_ARGS__)
#define glBlendEquationSeparate(...) CW__GL_GUARDED(glBlendEquationSeparate, __VA_ARGS__)
#define glBlendFunc(...) CW__GL_GUARDED(glBlendFunc, __VA_ARGS__)
#define glBlendFuncSeparate(...) CW__GL_GUARDED(glBlendFuncSeparate, __VA_ARGS__)
#define glBlitFramebuffer(...) CW__GL_GUARDED(glBlitFramebuffer, __VA_ARGS__)
#define glBufferData(...) CW__GL_GUARDED(glBufferData, __VA_ARGS__)
#define glBufferSubData(...) CW__GL_GUARDED(glBufferSubData, __VA_ARGS__)
#define glCheckFramebufferStatus(...) CW__GL_GUARDED(glCheckFramebufferStatus, __VA_ARGS__)
#define glClear(...) CW__GL_GUARDED(glClear, __VA_ARGS__)
#define glClearBufferfi(...) CW__GL_GUARDED(glClearBufferfi, __VA_ARGS__)
#define glClearBufferfv(...) CW__GL_GUARDED(glClearBufferfv, __VA_ARGS__)
#define glClearBufferiv(...) CW__GL_GUARDED(glClearBufferiv, __VA_ARGS__)
#define glClearBufferuiv(...) CW__GL_GUARDED(glClearBufferuiv, __VA_ARGS__)
#define glClearColor(...) CW__GL_GUARDED(glClearColor, __VA_ARGS__)
#define glClearDepthf(...) CW__GL_GUARDED(glClearDepthf, __VA_ARGS__)
#define glClearStencil(...) CW__GL_GUARDED(glClearStencil, __VA_ARGS__)
#define glClientWaitSync(...) CW__GL_GUARDED(glClientWaitSync, __VA_ARGS__)
#define glColorMask(...) CW__GL_GUARDED(glColorMask, __VA_ARGS__)
#define glCompileShader(...) CW__GL_GUARDED(glCompileShader, __VA_ARGS__)
#define glCompressedTexImage2D(...) CW__GL_GUARDED(glCompressedTexImage2D, __VA_ARGS__)
#define glCompressedTexImage3D(...) CW__GL_GUARDED(glCompressedTexImage3D, __VA_ARGS__)
#define glCompressedTexSubImage2D(...) CW__GL_GUARDED(glCompressedTexSubImage2D, __VA_ARGS__)
#define glCompressedTexSubImage3D(...) CW__GL_GUARDED(glCompressedTexSubImage3D, __VA_ARGS__)
#define glCopyBufferSubData(...) CW__GL_GUARDED(glCopyBufferSubData, __VA_ARGS__)
#define glCopyTexImage2D(...) CW__GL_GUARDED(glCopyTexImage2D, __VA_ARGS__)
#define glCopyTexSubImage2D(...) CW__GL_GUARDED(glCopyTexSubImage2D, __VA_ARGS__)
#define glCopyTexSubImage3D(...) CW__GL_GUARDED(glCopyTexSubImage3D, __VA_ARGS__)
#define glCreateProgram(...) CW__GL_GUARDED(glCreateProgram, __VA_ARGS__)
#define glCreateShader(...) CW__GL_GUARDED(glCreateShader, __VA_ARGS__)
#define glCullFace(...) CW__GL_GUARDED(glCullFace, __VA_ARGS__)
#define glDeleteBuffers(...) CW__GL_GUARDED(glDeleteBuffers, __VA_ARGS__)
#define glDeleteFramebuffers(...) CW__GL_GUARDED(glDeleteFramebuffers, __VA_ARGS__)
#define glDeleteProgram(...) CW__GL_GUARDED(glDeleteProgram, __VA_ARGS__)
#define glDeleteQueries(...) CW__GL_GUARDED(glDeleteQueries, __VA_ARGS__)
#define glDeleteRenderbuffers(...) CW__GL_GUARDED(glDeleteRenderbuffers, __VA_ARGS__)
#define glDeleteSamplers(...) CW__GL_GUARDED(glDeleteSamplers, __VA_ARGS__)
#define glDeleteShader(...) CW__GL_GUARDED(glDeleteShader, __VA_ARGS__)
#define glDeleteSync(...) CW__GL_GUARDED(glDeleteSync, __VA_ARGS__)
#define glDeleteTextures(...) CW__GL_GUARDED(glDeleteTextures, __VA_ARGS__)
#define glDeleteTransformFeedbacks(...) CW__GL_GUARDED(glDeleteTransformFeedbacks, __VA_ARGS__)
#define glDeleteVertexArrays(...) CW__GL_GUARDED(glDeleteVertexArrays, __VA_ARGS__)
#define glDepthFunc(...) CW__GL_GUARDED(glDepthFunc, __VA_ARGS__)
#define glDepthMask(...) CW__GL_GUARDED(glDepthMask, __VA_ARGS__)
#define glDepthRangef(...) CW__GL_GUARDED(glDepthRangef, __VA_ARGS__)
#define glDetachShader(...) CW__GL_GUARDED(glDetachShader, __VA_ARGS__)
#define glDisable(...) CW__GL_GUARDED(glDisable, __VA_ARGS__)
#define glDisableVertexAttribArray(...) CW__GL_GUARDED(glDisableVertexAttribArray, __VA_ARGS__)
#define glDrawArrays(...) CW__GL_GUARDED(glDrawArrays, __VA_ARGS__)
#define glDrawArraysInstanced(...) CW__GL_GUARDED(glDrawArraysInstanced, __VA_ARGS__)
#define glDrawBuffers(...) CW__GL_GUARDED(glDrawBuffers, __VA_ARGS__)
#define glDrawElements(...) CW__GL_GUARDED(glDrawElements, __VA_ARGS__)
#define glDrawElementsInstanced(...) CW__GL_GUARDED(glDrawElementsInstanced, __VA_ARGS__)
#define glDrawRangeElements(...) CW__GL_GUARDED(glDrawRangeElements, __VA_ARGS__)
#define glEnable(...) CW__GL_GUARDED(glEnable, __VA_ARGS__)
#define glEnableVertexAttribArray(...) CW__GL_GUARDED(glEnableVertexAttribArray, __VA_ARGS__)
#define glEndQuery(...) CW__GL_GUARDED(glEndQuery, __VA_ARGS__)
#define glEndTransformFeedback(...) CW__GL_GUARDED(glEndTransformFeedback, __VA_ARGS__)
#define glFenceSync(...) CW__GL_GUARDED(glFenceSync, __VA_ARGS__)
#define glFinish(...) CW__GL_GUARDED(glFinish, __VA_ARGS__)
#define glFlush(...) CW__GL_GUARDED(glFlush, __VA_ARGS__)
#define glFlushMappedBufferRange(...) CW__GL_GUARDED(glFlushMappedBufferRange, __VA_ARGS__)
#define glFramebufferRenderbuffer(...) CW__GL_GUARDED(glFramebufferRenderbuffer, __VA_ARGS__)
#define glFramebufferTexture2D(...) CW__GL_GUARDED(glFramebufferTexture2D, __VA_ARGS__)
#define glFramebufferTextureLayer(...) CW__GL_GUARDED(glFramebufferTextureLayer, __VA_ARGS__)
#define glFrontFace(...) CW__GL_GUARDED(glFrontFace, __VA_ARGS__)
#define glGenBuffers(...) CW__GL_GUARDED(glGenBuffers, __VA_ARGS__)
#define glGenFramebuffers(...) CW__GL_GUARDED(glGenFramebuffers, __VA_ARGS__)
#define glGenQueries(...) CW__GL_GUARDED(glGenQueries, __VA_ARGS__)
#define glGenRenderbuffers(...) CW__GL_GUARDED(glGenRenderbuffers, __VA_ARGS__)
#define glGenSamplers(...) CW__GL_GUARDED(glGenSamplers, __VA_ARGS__)
#define glGenTextures(...) CW__GL_GUARDED(glGenTextures, __VA_ARGS__)
#define glGenTransformFeedbacks(...) CW__GL_GUARDED(glGenTransformFeedbacks, __VA_ARGS__)
#define glGenVertexArrays(...) CW__GL_GUARDED(glGenVertexArrays, __VA_ARGS__)
#define glGenerateMipmap(...) CW__GL_GUARDED(glGenerateMipmap, __VA_ARGS__)
#define glGetActiveAttrib(...) CW__GL_GUARDED(glGetActiveAttrib, __VA_ARGS__)
#define glGetActiveUniform(...) CW__GL_GUARDED(glGetActiveUniform, __VA_ARGS__)
#define glGetActiveUniformBlockName(...) CW__GL_GUARDED(glGetActiveUniformBlockName, __VA_ARGS__)
#define glGetActiveUniformBlockiv(...) CW__GL_GUARDED(glGetActiveUniformBlockiv, __VA_ARGS__)
#define glGetActiveUniformsiv(...) CW__GL_GUARDED(glGetActiveUniformsiv, __VA_ARGS__)
#define glGetAttachedShaders(...) CW__GL_GUARDED(glGetAttachedShaders, __VA_ARGS__)
#define glGetAttribLocation(...) CW__GL_GUARDED(glGetAttribLocation, __VA_ARGS__)
#define glGetBooleanv(...) CW__GL_GUARDED(glGetBooleanv, __VA_ARGS__)
#define glGetBufferParameteri64v(...) CW__GL_GUARDED(glGetBufferParameteri64v, __VA_ARGS__)
#define glGetBufferParameteriv(...) CW__GL_GUARDED(glGetBufferParameteriv, __VA_ARGS__)
#define glGetBufferPointerv(...) CW__GL_GUARDED(glGetBufferPointerv, __VA_ARGS__)
#define glGetError(...) CW__GL_GUARDED(glGetError, __VA_ARGS__)
#define glGetFloatv(...) CW__GL_GUARDED(glGetFloatv, __VA_ARGS__)
#define glGetFragDataLocation(...) CW__GL_GUARDED(glGetFragDataLocation, __VA_ARGS__)
#define glGetFramebufferAttachmentParameteriv(...) CW__GL_GUARDED(glGetFramebufferAttachmentParameteriv, __VA_ARGS__)
#define glGetInteger64i(...) CW__GL_GUARDED(glGetInteger64i, __VA_ARGS__)
#define glGetInteger64v(...) CW__GL_GUARDED(glGetInteger64v, __VA_ARGS__)
#define glGetIntegeri(...) CW__GL_GUARDED(glGetIntegeri, __VA_ARGS__)
#define glGetIntegerv(...) CW__GL_GUARDED(glGetIntegerv, __VA_ARGS__)
#define glGetInternalformativ(...) CW__GL_GUARDED(glGetInternalformativ, __VA_ARGS__)
#define glGetProgramBinary(...) CW__GL_GUARDED(glGetProgramBinary, __VA_ARGS__)
#define glGetProgramInfoLog(...) CW__GL_GUARDED(glGetProgramInfoLog, __VA_ARGS__)
#define glGetProgramiv(...) CW__GL_GUARDED(glGetProgramiv, __VA_ARGS__)
#define glGetQueryObjectuiv(...) CW__GL_GUARDED(glGetQueryObjectuiv, __VA_ARGS__)
#define glGetQueryiv(...) CW__GL_GUARDED(glGetQueryiv, __VA_ARGS__)
#define glGetRenderbufferParameteriv(...) CW__GL_GUARDED(glGetRenderbufferParameteriv, __VA_ARGS__)
#define glGetSamplerParameterfv(...) CW__GL_GUARDED(glGetSamplerParameterfv, __VA_ARGS__)
#define glGetSamplerParameteriv(...) CW__GL_GUARDED(glGetSamplerParameteriv, __VA_ARGS__)
#define glGetShaderInfoLog(...) CW__GL_GUARDED(glGetShaderInfoLog, __VA_ARGS__)
#define glGetShaderPrecisionFormat(...) CW__GL_GUARDED(glGetShaderPrecisionFormat, __VA_ARGS__)
#define glGetShaderSource(...) CW__GL_GUARDED(glGetShaderSource, __VA_ARGS__)
#define glGetShaderiv(...) CW__GL_GUARDED(glGetShaderiv, __VA_ARGS__)
#define glGetString(...) CW__GL_GUARDED(glGetString, __VA_ARGS__)
#define glGetStringi(...) CW__GL_GUARDED(glGetStringi, __VA_ARGS__)
#define glGetSynciv(...) CW__GL_GUARDED(glGetSynciv, __VA_ARGS__)
#define glGetTexParameterfv(...) CW__GL_GUARDED(glGetTexParameterfv, __VA_ARGS__)
#define glGetTexParameteriv(...) CW__GL_GUARDED(glGetTexParameteriv, __VA_ARGS__)
#define glGetTransformFeedbackVarying(...) CW__GL_GUARDED(glGetTransformFeedbackVarying, __VA_ARGS__)
#define glGetUniformBlockIndex(...) CW__GL_GUARDED(glGetUniformBlockIndex, __VA_ARGS__)
#define glGetUniformIndices(...) CW__GL_GUARDED(glGetUniformIndices, __VA_ARGS__)
#define glGetUniformLocation(...) CW__GL_GUARDED(glGetUniformLocation, __VA_ARGS__)
#define glGetUniformfv(...) CW__GL_GUARDED(glGetUniformfv, __VA_ARGS__)
#define glGetUniformiv(...) CW__GL_GUARDED(glGetUniformiv, __VA_ARGS__)
#define glGetUniformuiv(...) CW__GL_GUARDED(glGetUniformuiv, __VA_ARGS__)
#define glGetVertexAttribIiv(...) CW__GL_GUARDED(glGetVertexAttribIiv, __VA_ARGS__)
#define glGetVertexAttribIuiv(...) CW__GL_GUARDED(glGetVertexAttribIuiv, __VA_ARGS__)
#define glGetVertexAttribPointerv(...) CW__GL_GUARDED(glGetVertexAttribPointerv, __VA_ARGS__)
#define glGetVertexAttribfv(...) CW__GL_GUARDED(glGetVertexAttribfv, __VA_ARGS__)
#define glGetVertexAttribiv(...) CW__GL_GUARDED(glGetVertexAttribiv, __VA_ARGS__)
#define glHint(...) CW__GL_GUARDED(glHint, __VA_ARGS__)
#define glInvalidateFramebuffer(...) CW__GL_GUARDED(glInvalidateFramebuffer, __VA_ARGS__)
#define glInvalidateSubFramebuffer(...) CW__GL_GUARDED(glInvalidateSubFramebuffer, __VA_ARGS__)
#define glIsBuffer(...) CW__GL_GUARDED(glIsBuffer, __VA_ARGS__)
#define glIsEnabled(...) CW__GL_GUARDED(glIsEnabled, __VA_ARGS__)
#define glIsFramebuffer(...) CW__GL_GUARDED(glIsFramebuffer, __VA_ARGS__)
#define glIsProgram(...) CW__GL_GUARDED(glIsProgram, __VA_ARGS__)
#define glIsQuery(...) CW__GL_GUARDED(glIsQuery, __VA_ARGS__)
#define glIsRenderbuffer(...) CW__GL_GUARDED(glIsRenderbuffer, __VA_ARGS__)
#define glIsSampler(...) CW__GL_GUARDED(glIsSampler, __VA_ARGS__)
#define glIsShader(...) CW__GL_GUARDED(glIsShader, __VA_ARGS__)
#define glIsSync(...) CW__GL_GUARDED(glIsSync, __VA_ARGS__)
#define glIsTexture(...) CW__GL_GUARDED(glIsTexture, __VA_ARGS__)
#define glIsTransformFeedback(...) CW__GL_GUARDED(glIsTransformFeedback, __VA_ARGS__)
#define glIsVertexArray(...) CW__GL_GUARDED(glIsVertexArray, __VA_ARGS__)
#define glLineWidth(...) CW__GL_GUARDED(glLineWidth, __VA_ARGS__)
#define glLinkProgram(...) CW__GL_GUARDED(glLinkProgram, __VA_ARGS__)
#define glMapBufferRange(...) CW__GL_GUARDED(glMapBufferRange, __VA_ARGS__)
#define glPauseTransformFeedback(...) CW__GL_GUARDED(glPauseTransformFeedback, __VA_ARGS__)
#define glPixelStorei(...) CW__GL_GUARDED(glPixelStorei, __VA_ARGS__)
#define glPolygonOffset(...) CW__GL_GUARDED(glPolygonOffset, __VA_ARGS__)
#define glProgramBinary(...) CW__GL_GUARDED(glProgramBinary, __VA_ARGS__)
#define glProgramParameteri(...) CW__GL_GUARDED(glProgramParameteri, __VA_ARGS__)
#define glReadBuffer(...) CW__GL_GUARDED(glReadBuffer, __VA_ARGS__)
#define glReadPixels(...) CW__GL_GUARDED(glReadPixels, __VA_ARGS__)
#define glReleaseShaderCompiler(...) CW__GL_GUARDED(glReleaseShaderCompiler, __VA_ARGS__)
#define glRenderbufferStorage(...) CW__GL_GUARDED(glRenderbufferStorage, __VA_ARGS__)
#define glRenderbufferStorageMultisample(...) CW__GL_GUARDED(glRenderbufferStorageMultisample, __VA_ARGS__)
#define glResumeTransformFeedback(...) CW__GL_GUARDED(glResumeTransformFeedback, __VA_ARGS__)
#define glSampleCoverage(...) CW__GL_GUARDED(glSampleCoverage, __VA_ARGS__)
#define glSamplerParameterf(...) CW__GL_GUARDED(glSamplerParameterf, __VA_ARGS__)
#define glSamplerParameterfv(...) CW__GL_GUARDED(glSamplerParameterfv, __VA_ARGS__)
#define glSamplerParameteri(...) CW__GL_GUARDED(glSamplerParameteri, __VA_ARGS__)
#define glSamplerParameteriv(...) CW__GL_GUARDED(glSamplerParameteriv, __VA_ARGS__)
#define glScissor(...) CW__GL_GUARDED(glScissor, __VA_ARGS__)
#define glShaderBinary(...) CW__GL_GUARDED(glShaderBinary, __VA_ARGS__)
#define glShaderSource(...) CW__GL_GUARDED(glShaderSource, __VA_ARGS__)
#define glStencilFunc(...) CW__GL_GUARDED(glStencilFunc, __VA_ARGS__)
#define glStencilFuncSeparate(...) CW__GL_GUARDED(glStencilFuncSeparate, __VA_ARGS__)
#define glStencilMask(...) CW__GL_GUARDED(glStencilMask, __VA_ARGS__)
#define glStencilMaskSeparate(...) CW__GL_GUARDED(glStencilMaskSeparate, __VA_ARGS__)
#define glStencilOp(...) CW__GL_GUARDED(glStencilOp, __VA_ARGS__)
#define glStencilOpSeparate(...) CW__GL_GUARDED(glStencilOpSeparate, __VA_ARGS__)
#define glTexImage2D(...) CW__GL_GUARDED(glTexImage2D, __VA_ARGS__)
#define glTexImage3D(...) CW__GL_GUARDED(glTexImage3D, __VA_ARGS__)
#define glTexParameterf(...) CW__GL_GUARDED(glTexParameterf, __VA_ARGS__)
#define glTexParameterfv(...) CW__GL_GUARDED(glTexParameterfv, __VA_ARGS__)
#define glTexParameteri(...) CW__GL_GUARDED(glTexParameteri, __VA_ARGS__)
#define glTexParameteriv(...) CW__GL_GUARDED(glTexParameteriv, __VA_ARGS__)
#define glTexStorage2D(...) CW__GL_GUARDED(glTexStorage2D, __VA_ARGS__)
#define glTexStorage3D(...) CW__GL_GUARDED(glTexStorage3D, __VA_ARGS__)
#define glTexSubImage2D(...) CW__GL_GUARDED(glTexSubImage2D, __VA_ARGS__)
#define glTexSubImage3D(...) CW__GL_GUARDED(glTexSubImage3D, __VA_ARGS__)
#define glTransformFeedbackVaryings(...) CW__GL_GUARDED(glTransformFeedbackVaryings, __VA_ARGS__)
#define glUniform1f(...) CW__GL_GUARDED(glUniform1f, __VA_ARGS__)
#define glUniform1fv(...) CW__GL_GUARDED(glUniform1fv, __VA_ARGS__)
#define glUniform1i(...) CW__GL_GUARDED(glUniform1i, __VA_ARGS__)
#define glUniform1iv(...) CW__GL_GUARDED(glUniform1iv, __VA_ARGS__)
#define glUniform1ui(...) CW__GL_GUARDED(glUniform1ui, __VA_ARGS__)
#define glUniform1uiv(...) CW__GL_GUARDED(glUniform1uiv, __VA_ARGS__)
#define glUniform2f(...) CW__GL_GUARDED(glUniform2f, __VA_ARGS__)
#define glUniform2fv(...) CW__GL_GUARDED(glUniform2fv, __VA_ARGS__)
#define glUniform2i(...) CW__GL_GUARDED(glUniform2i, __VA_ARGS__)
#define glUniform2iv(...) CW__GL_GUARDED(glUniform2iv, __VA_ARGS__)
#define glUniform2ui(...) CW__GL_GUARDED(glUniform2ui, __VA_ARGS__)
#define glUniform2uiv(...) CW__GL_GUARDED(glUniform2uiv, __VA_ARGS__)
#define glUniform3f(...) CW__GL_GUARDED(glUniform3f, __VA_ARGS__)
#define glUniform3fv(...) CW__GL_GUARDED(glUniform3fv, __VA_ARGS__)
#define glUniform3i(...) CW__GL_GUARDED(glUniform3i, __VA_ARGS__)
#define glUniform3iv(...) CW__GL_GUARDED(glUniform3iv, __VA_ARGS__)
#define glUniform3ui(...) CW__GL_GUARDED(glUniform3ui, __VA_ARGS__)
#define glUniform3uiv(...) CW__GL_GUARDED(glUniform3uiv, __VA_ARGS__)
#define glUniform4f(...) CW__GL_GUARDED(glUniform4f, __VA_ARGS__)
#define glUniform4fv(...) CW__GL_GUARDED(glUniform4fv, __VA_ARGS__)
#define glUniform4i(...) CW__GL_GUARDED(glUniform4i, __VA_ARGS__)
#define glUniform4iv(...) CW__GL_GUARDED(glUniform4iv, __VA_ARGS__)
#define glUniform4ui(...) CW__GL_GUARDED(glUniform4ui, __VA_ARGS__)
#define glUniform4uiv(...) CW__GL_GUARDED(glUniform4uiv, __VA_ARGS__)
#define glUniformBlockBinding(...) CW__GL_GUARDED(glUniformBlockBinding, __VA_ARGS__)
#define glUniformMatrix2fv(...) CW__GL_GUARDED(glUniformMatrix2fv, __VA_ARGS__)
#define glUniformMatrix2x3fv(...) CW__GL_GUARDED(glUniformMatrix2x3fv, __VA_ARGS__)
#define glUniformMatrix2x4fv(...) CW__GL_GUARDED(glUniformMatrix2x4fv, __VA_ARGS__)
#define glUniformMatrix3fv(...) CW__GL_GUARDED(glUniformMatrix3fv, __VA_ARGS__)
#define glUniformMatrix3x2fv(...) CW__GL_GUARDED(glUniformMatrix3x2fv, __VA_ARGS__)
#define glUniformMatrix3x4fv(...) CW__GL_GUARDED(glUniformMatrix3x4fv, __VA_ARGS__)
#define glUniformMatrix4fv(...) CW__GL_GUARDED(glUniformMatrix4fv, __VA_ARGS__)
#define glUniformMatrix4x2fv(...) CW__GL_GUARDED(glUniformMatrix4x2fv, __VA_ARGS__)
#define glUniformMatrix4x3fv(...) CW__GL_GUARDED(glUniformMatrix4x3fv, __VA_ARGS__)
#define glUnmapBuffer(...) CW__GL_GUARDED(glUnmapBuffer, __VA_ARGS__)
#define glUseProgram(...) CW__GL_GUARDED(glUseProgram, __VA_ARGS__)
#define glValidateProgram(...) CW__GL_GUARDED(glValidateProgram, __VA_ARGS__)
#define glVertexAttrib1f(...) CW__GL_GUARDED(glVertexAttrib1f, __VA_ARGS__)
#define glVertexAttrib1fv(...) CW__GL_GUARDED(glVertexAttrib1fv, __VA_ARGS__)
#define glVertexAttrib2f(...) CW__GL_GUARDED(glVertexAttrib2f, __VA_ARGS__)
#define glVertexAttrib2fv(...) CW__GL_GUARDED(glVertexAttrib2fv, __VA_ARGS__)
#define glVertexAttrib3f(...) CW__GL_GUARDED(glVertexAttrib3f, __VA_ARGS__)
#define glVertexAttrib3fv(...) CW__GL_GUARDED(glVertexAttrib3fv, __VA_ARGS__)
#define glVertexAttrib4f(...) CW__GL_GUARDED(glVertexAttrib4f, __VA_ARGS__)
#define glVertexAttrib4fv(...) CW__GL_GUARDED(glVertexAttrib4fv, __VA_ARGS__)
#define glVertexAttribDivisor(...) CW__GL_GUARDED(glVertexAttribDivisor, __VA_ARGS__)
#define glVertexAttribI4i(...) CW__GL_GUARDED(glVertexAttribI4i, __VA_ARGS__)
#define glVertexAttribI4iv(...) CW__GL_GUARDED(glVertexAttribI4iv, __VA_ARGS__)
#define glVertexAttribI4ui(...) CW__GL_GUARDED(glVertexAttribI4ui, __VA_ARGS__)
#define glVertexAttribI4uiv(...) CW__GL_GUARDED(glVertexAttribI4uiv, __VA_ARGS__)
#define glVertexAttribIPointer(...) CW__GL_GUARDED(glVertexAttribIPointer, __VA_ARGS__)
#define glVertexAttribPointer(...) CW__GL_GUARDED(glVertexAttribPointer, __VA_ARGS__)
#define glViewport(...) CW__GL_GUARDED(glViewport, __VA_ARGS__)
#define glWaitSync(...) CW__GL_GUARDED(glWaitSync, __VA_ARGS__)
/* clang-format on */
#else
static inline void cw__gl_bind_framebuffer(GLenum target, GLuint fb) { glBindFramebuffer(target, fb ? fb : cw__gl_window_fbo); }
#define glBindFramebuffer(target, fb) cw__gl_bind_framebuffer((target), (fb))
#endif

#endif
