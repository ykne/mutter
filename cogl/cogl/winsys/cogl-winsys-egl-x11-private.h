/*
 * Cogl
 *
 * A Low Level GPU Graphics and Utilities API
 *
 * Copyright (C) 2011 Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *
 */

#pragma once

#include "cogl/winsys/cogl-winsys-egl.h"
#include "cogl/winsys/cogl-texture-pixmap-x11-private.h"

#define COGL_TYPE_WINSYS_EGL_X11 (cogl_winsys_egl_x11_get_type ())
COGL_EXPORT
G_DECLARE_FINAL_TYPE (CoglWinsysEglX11, cogl_winsys_egl_x11,
                     COGL, WINSYS_EGL_X11, CoglWinsysEGL)

XVisualInfo *
cogl_display_xlib_get_visual_info (CoglDisplay *display,
                                   EGLConfig    egl_config);

gboolean
cogl_winsys_egl_x11_texture_pixmap_create (CoglTexturePixmapX11 *tex_pixmap);

void
cogl_winsys_egl_x11_texture_pixmap_free (CoglTexturePixmapX11 *tex_pixmap);

gboolean
cogl_winsys_egl_x11_texture_pixmap_update (CoglTexturePixmapX11 *tex_pixmap,
                                           CoglTexturePixmapStereoMode stereo_mode,
                                           gboolean needs_mipmap);

void
cogl_winsys_egl_x11_texture_pixmap_damage_notify (CoglTexturePixmapX11 *tex_pixmap);

CoglTexture *
cogl_winsys_egl_x11_texture_pixmap_get_texture (CoglTexturePixmapX11 *tex_pixmap,
                                                CoglTexturePixmapStereoMode stereo_mode);
