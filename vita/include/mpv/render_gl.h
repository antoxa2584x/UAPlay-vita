#ifndef UAPLAY_VITA_MPV_RENDER_GL_H
#define UAPLAY_VITA_MPV_RENDER_GL_H

#include "client.h"

typedef struct mpv_render_context mpv_render_context;

typedef enum mpv_render_param_type {
    MPV_RENDER_PARAM_INVALID = 0,
    MPV_RENDER_PARAM_API_TYPE = 1,
    MPV_RENDER_PARAM_OPENGL_INIT_PARAMS = 2,
    MPV_RENDER_PARAM_OPENGL_FBO = 3,
    MPV_RENDER_PARAM_FLIP_Y = 4
} mpv_render_param_type;

#define MPV_RENDER_API_TYPE_OPENGL "opengl"

typedef struct mpv_render_param {
    mpv_render_param_type type;
    void *data;
} mpv_render_param;

typedef struct mpv_opengl_init_params {
    void *(*get_proc_address)(void *ctx, const char *name);
    void *get_proc_address_ctx;
} mpv_opengl_init_params;

typedef struct mpv_opengl_fbo {
    int fbo;
    int w, h;
    int internal_format;
} mpv_opengl_fbo;

int mpv_render_context_create(mpv_render_context **res, mpv_handle *mpv,
                              mpv_render_param *params);
/* Draws the newest decoded frame, letterboxed, into the current viewport.
   Must run on the thread that owns the vitaGL context. */
int mpv_render_context_render(mpv_render_context *ctx, mpv_render_param *params);
void mpv_render_context_free(mpv_render_context *ctx);

#endif
