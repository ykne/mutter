/*
 * Cogl
 *
 * A Low Level GPU Graphics and Utilities API
 *
 * Copyright (C) 2011,2013 Intel Corporation.
 *               2026 GNOME-X11 restoration for mutter 50.4's winsys class
 *               architecture.
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
 * Authors:
 *   Robert Bragg <robert@linux.intel.com>
 *   Neil Roberts <neil@linux.intel.com>
 */

#include "config.h"

#include <X11/Xlib.h>

#include "cogl/cogl-xlib-renderer-private.h"
#include "cogl/cogl-xlib-renderer.h"
#include "cogl/cogl-framebuffer-private.h"
#include "cogl/cogl-onscreen-private.h"
#include "cogl/cogl-display-private.h"
#include "cogl/cogl-renderer-private.h"
#include "cogl/cogl-context-private.h"
#include "cogl/winsys/cogl-texture-pixmap-x11-private.h"
#include "cogl/cogl-texture-2d-private.h"
#include "cogl/driver/gl/cogl-texture-2d-gl-private.h"
#include "cogl/cogl-texture-2d.h"
#include "cogl/winsys/cogl-onscreen-egl.h"
#include "cogl/winsys/cogl-onscreen-xlib.h"
#include "cogl/winsys/cogl-winsys-egl-x11-private.h"
#include "cogl/winsys/cogl-winsys-egl.h"

/* EGL_KHR_image_pixmap function pointers. Cogl's generic EGL image
 * helpers were dropped upstream along with the rest of X11 support,
 * since nothing outside this X11-only "texture from pixmap" path used
 * them, so we resolve the two entry points we need ourselves. */
typedef EGLImageKHR (EGLAPIENTRYP CoglEglCreateImageKHRProc) (EGLDisplay dpy,
                                                              EGLContext ctx,
                                                              EGLenum target,
                                                              EGLClientBuffer buffer,
                                                              const EGLint *attrib_list);
typedef EGLBoolean (EGLAPIENTRYP CoglEglDestroyImageKHRProc) (EGLDisplay dpy,
                                                              EGLImageKHR image);

static CoglEglCreateImageKHRProc create_image_khr;
static CoglEglDestroyImageKHRProc destroy_image_khr;
static gboolean image_khr_resolved;

static void
ensure_egl_image_khr_resolved (void)
{
  if (image_khr_resolved)
    return;

  create_image_khr =
    (CoglEglCreateImageKHRProc) eglGetProcAddress ("eglCreateImageKHR");
  destroy_image_khr =
    (CoglEglDestroyImageKHRProc) eglGetProcAddress ("eglDestroyImageKHR");
  image_khr_resolved = TRUE;
}

struct _CoglWinsysEglX11
{
  CoglWinsysEGL parent;

  /* Not owned: CoglRenderer owns us (via cogl_renderer_set_custom_winsys),
   * not the other way around. Kept only so dispose() can tear down the
   * Xlib connection it opened in renderer_connect(). */
  CoglRenderer *renderer;
};

G_DEFINE_FINAL_TYPE (CoglWinsysEglX11, cogl_winsys_egl_x11,
                    COGL_TYPE_WINSYS_EGL)

typedef struct _CoglDisplayXlib
{
  Window dummy_xwin;
} CoglDisplayXlib;

typedef struct _CoglTexturePixmapEGL
{
  EGLImageKHR image;
  CoglTexture *texture;
  gboolean bind_tex_image_queued;
} CoglTexturePixmapEGL;

static CoglOnscreen *
find_onscreen_for_xid (CoglContext *context, uint32_t xid)
{
  GList *l;

  for (l = context->framebuffers; l; l = l->next)
    {
      CoglFramebuffer *framebuffer = l->data;
      CoglOnscreen *onscreen;

      if (!COGL_IS_ONSCREEN (framebuffer))
        continue;

      onscreen = COGL_ONSCREEN (framebuffer);
      if (cogl_onscreen_xlib_is_for_window (onscreen, (Window) xid))
        return onscreen;
    }

  return NULL;
}

static void
notify_resize (CoglContext *context,
               Window drawable,
               int width,
               int height)
{
  CoglOnscreen *onscreen;

  onscreen = find_onscreen_for_xid (context, drawable);
  if (!onscreen)
    return;

  cogl_onscreen_xlib_resize (onscreen, width, height);
}

static CoglFilterReturn
event_filter_cb (XEvent *xevent, void *data)
{
  CoglContext *context = data;

  if (xevent->type == ConfigureNotify)
    {
      notify_resize (context,
                     xevent->xconfigure.window,
                     xevent->xconfigure.width,
                     xevent->xconfigure.height);
    }
  else if (xevent->type == Expose)
    {
      CoglOnscreen *onscreen =
        find_onscreen_for_xid (context, xevent->xexpose.window);

      if (onscreen)
        {
          MtkRectangle info;

          info.x = xevent->xexpose.x;
          info.y = xevent->xexpose.y;
          info.width = xevent->xexpose.width;
          info.height = xevent->xexpose.height;

          _cogl_onscreen_queue_dirty (onscreen, &info);
        }
    }

  return COGL_FILTER_CONTINUE;
}

XVisualInfo *
cogl_display_xlib_get_visual_info (CoglDisplay *display,
                                   EGLConfig    egl_config)
{
  CoglXlibRenderer *xlib_renderer =
    _cogl_xlib_renderer_get_data (display->renderer);
  CoglRendererEGL *egl_renderer =
    cogl_renderer_get_winsys_data (display->renderer);
  XVisualInfo visinfo_template;
  int template_mask = 0;
  XVisualInfo *visinfo = NULL;
  int visinfos_count;
  EGLint visualid, red_size, green_size, blue_size, alpha_size;

  eglGetConfigAttrib (egl_renderer->edpy, egl_config,
                      EGL_NATIVE_VISUAL_ID, &visualid);

  if (visualid != 0)
    {
      visinfo_template.visualid = visualid;
      template_mask |= VisualIDMask;
    }
  else
    {
      /* some EGL drivers don't implement the EGL_NATIVE_VISUAL_ID
       * attribute, so attempt to find the closest match. */

      eglGetConfigAttrib (egl_renderer->edpy, egl_config,
                          EGL_RED_SIZE, &red_size);
      eglGetConfigAttrib (egl_renderer->edpy, egl_config,
                          EGL_GREEN_SIZE, &green_size);
      eglGetConfigAttrib (egl_renderer->edpy, egl_config,
                          EGL_BLUE_SIZE, &blue_size);
      eglGetConfigAttrib (egl_renderer->edpy, egl_config,
                          EGL_ALPHA_SIZE, &alpha_size);

      visinfo_template.depth = red_size + green_size + blue_size + alpha_size;
      template_mask |= VisualDepthMask;

      visinfo_template.screen = DefaultScreen (xlib_renderer->xdpy);
      template_mask |= VisualScreenMask;
    }

  visinfo = XGetVisualInfo (xlib_renderer->xdpy,
                            template_mask,
                            &visinfo_template,
                            &visinfos_count);

  return visinfo;
}

static gboolean
cogl_winsys_egl_x11_renderer_connect (CoglWinsys   *winsys,
                                      CoglRenderer *renderer,
                                      GError      **error)
{
  CoglWinsysClass *parent_class;
  CoglRendererEGL *egl_renderer;
  CoglXlibRenderer *xlib_renderer;

  COGL_WINSYS_EGL_X11 (winsys)->renderer = renderer;

  cogl_renderer_set_winsys_data (renderer, g_new0 (CoglRendererEGL, 1), g_free);
  egl_renderer = cogl_renderer_get_winsys_data (renderer);
  xlib_renderer = _cogl_xlib_renderer_get_data (renderer);

  egl_renderer->sync = EGL_NO_SYNC_KHR;
  egl_renderer->needs_config = TRUE;

  if (!_cogl_xlib_renderer_connect (renderer, error))
    return FALSE;

  egl_renderer->edpy = eglGetDisplay ((EGLNativeDisplayType) xlib_renderer->xdpy);

  parent_class = COGL_WINSYS_CLASS (cogl_winsys_egl_x11_parent_class);
  if (!parent_class->renderer_connect (winsys, renderer, error))
    {
      _cogl_xlib_renderer_disconnect (renderer);
      return FALSE;
    }

  return TRUE;
}

static int
cogl_winsys_egl_x11_add_config_attributes (CoglWinsysEGL *winsys,
                                           CoglDisplay   *display,
                                           EGLint        *attributes)
{
  int i = 0;

  attributes[i++] = EGL_SURFACE_TYPE;
  attributes[i++] = EGL_WINDOW_BIT;

  return i;
}

static gboolean
cogl_winsys_egl_x11_choose_config (CoglWinsysEGL  *winsys,
                                   CoglDisplay    *display,
                                   EGLint         *attributes,
                                   EGLConfig      *out_config,
                                   GError        **error)
{
  CoglRenderer *renderer = display->renderer;
  CoglRendererEGL *egl_renderer = cogl_renderer_get_winsys_data (renderer);
  EGLint config_count = 0;
  EGLBoolean status;

  status = eglChooseConfig (egl_renderer->edpy,
                            attributes,
                            out_config, 1,
                            &config_count);
  if (status != EGL_TRUE || config_count == 0)
    {
      g_set_error (error, COGL_WINSYS_ERROR,
                   COGL_WINSYS_ERROR_CREATE_CONTEXT,
                   "No compatible EGL configs found");
      return FALSE;
    }

  return TRUE;
}

static gboolean
cogl_winsys_egl_x11_display_setup (CoglWinsys   *winsys,
                                   CoglDisplay  *display,
                                   GError      **error)
{
  CoglDisplayEGL *egl_display;
  CoglDisplayXlib *xlib_display;
  CoglWinsysClass *parent_class = COGL_WINSYS_CLASS (cogl_winsys_egl_x11_parent_class);

  if (!parent_class->display_setup (winsys, display, error))
    return FALSE;

  egl_display = display->winsys;
  xlib_display = g_new0 (CoglDisplayXlib, 1);
  egl_display->platform = xlib_display;

  return TRUE;
}

static void
cogl_winsys_egl_x11_display_destroy (CoglWinsys  *winsys,
                                     CoglDisplay *display)
{
  CoglDisplayEGL *egl_display = display->winsys;

  g_clear_pointer (&egl_display->platform, g_free);

  COGL_WINSYS_CLASS (cogl_winsys_egl_x11_parent_class)->display_destroy (winsys,
                                                                         display);
}

static gboolean
cogl_winsys_egl_x11_context_init (CoglWinsys   *winsys,
                                  CoglContext  *context,
                                  GError      **error)
{
  CoglWinsysClass *parent_class = COGL_WINSYS_CLASS (cogl_winsys_egl_x11_parent_class);

  if (!parent_class->context_init (winsys, context, error))
    return FALSE;

  /* We manually queue dirty events in response to Expose events from X
   * (see event_filter_cb() below) - upstream's COGL_PRIVATE_FEATURE_DIRTY_EVENTS
   * flag that used to advertise this no longer exists (it was X11-only
   * and dropped along with the rest of X11 support), but nothing else in
   * the current codebase gates behavior on it, so there's nothing to
   * replace it with. */
  _cogl_renderer_add_native_filter (context->display->renderer,
                                    (CoglNativeFilterFunc) event_filter_cb,
                                    context);

  return TRUE;
}

static gboolean
cogl_winsys_egl_x11_context_created (CoglWinsysEGL  *winsys,
                                     CoglDisplay    *display,
                                     GError        **error)
{
  CoglRenderer *renderer = display->renderer;
  CoglDisplayEGL *egl_display = display->winsys;
  CoglRendererEGL *egl_renderer = cogl_renderer_get_winsys_data (renderer);
  CoglXlibRenderer *xlib_renderer =
    _cogl_xlib_renderer_get_data (renderer);
  CoglDisplayXlib *xlib_display = egl_display->platform;
  XVisualInfo *xvisinfo;
  XSetWindowAttributes attrs;
  const char *error_message;

  xvisinfo = cogl_display_xlib_get_visual_info (display,
                                                egl_display->egl_config);
  if (xvisinfo == NULL)
    {
      error_message = "Unable to find suitable X visual";
      goto fail;
    }

  attrs.override_redirect = True;
  attrs.colormap = XCreateColormap (xlib_renderer->xdpy,
                                    DefaultRootWindow (xlib_renderer->xdpy),
                                    xvisinfo->visual,
                                    AllocNone);
  attrs.border_pixel = 0;

  if ((egl_renderer->private_features &
       COGL_EGL_WINSYS_FEATURE_SURFACELESS_CONTEXT) == 0)
    {
      xlib_display->dummy_xwin =
        XCreateWindow (xlib_renderer->xdpy,
                       DefaultRootWindow (xlib_renderer->xdpy),
                       -100, -100, 1, 1,
                       0,
                       xvisinfo->depth,
                       CopyFromParent,
                       xvisinfo->visual,
                       CWOverrideRedirect |
                       CWColormap |
                       CWBorderPixel,
                       &attrs);

      egl_display->dummy_surface =
        eglCreateWindowSurface (egl_renderer->edpy,
                                egl_display->egl_config,
                                (EGLNativeWindowType) xlib_display->dummy_xwin,
                                NULL);

      if (egl_display->dummy_surface == EGL_NO_SURFACE)
        {
          error_message = "Unable to create an EGL surface";
          XFree (xvisinfo);
          goto fail;
        }
    }

  g_clear_pointer (&xvisinfo, XFree);

  if (!_cogl_winsys_egl_make_current (display,
                                      egl_display->dummy_surface,
                                      egl_display->dummy_surface,
                                      egl_display->egl_context))
    {
      if (egl_display->dummy_surface == EGL_NO_SURFACE)
        error_message = "Unable to eglMakeCurrent with no surface";
      else
        error_message = "Unable to eglMakeCurrent with dummy surface";
      goto fail;
    }

  return TRUE;

fail:
  g_set_error (error, COGL_WINSYS_ERROR,
               COGL_WINSYS_ERROR_CREATE_CONTEXT,
               "%s", error_message);
  return FALSE;
}

static void
cogl_winsys_egl_x11_cleanup_context (CoglWinsysEGL *winsys,
                                     CoglDisplay   *display)
{
  CoglDisplayEGL *egl_display = display->winsys;
  CoglDisplayXlib *xlib_display = egl_display->platform;
  CoglRenderer *renderer = display->renderer;
  CoglXlibRenderer *xlib_renderer =
    _cogl_xlib_renderer_get_data (renderer);
  CoglRendererEGL *egl_renderer = cogl_renderer_get_winsys_data (renderer);

  if (egl_display->dummy_surface != EGL_NO_SURFACE)
    {
      eglDestroySurface (egl_renderer->edpy, egl_display->dummy_surface);
      egl_display->dummy_surface = EGL_NO_SURFACE;
    }

  if (xlib_display->dummy_xwin)
    {
      XDestroyWindow (xlib_renderer->xdpy, xlib_display->dummy_xwin);
      xlib_display->dummy_xwin = None;
    }
}

static void
cogl_winsys_egl_x11_dispose (GObject *object)
{
  CoglWinsysEglX11 *self = COGL_WINSYS_EGL_X11 (object);

  if (self->renderer)
    {
      CoglRendererEGL *egl_renderer =
        cogl_renderer_get_winsys_data (self->renderer);

      if (egl_renderer && egl_renderer->edpy)
        eglTerminate (egl_renderer->edpy);

      _cogl_xlib_renderer_disconnect (self->renderer);
      self->renderer = NULL;
    }

  G_OBJECT_CLASS (cogl_winsys_egl_x11_parent_class)->dispose (object);
}

static void
cogl_winsys_egl_x11_init (CoglWinsysEglX11 *winsys)
{
}

static void
cogl_winsys_egl_x11_class_init (CoglWinsysEglX11Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  CoglWinsysClass *winsys_class = COGL_WINSYS_CLASS (klass);
  CoglWinsysEGLClass *winsys_egl_class = COGL_WINSYS_EGL_CLASS (klass);

  object_class->dispose = cogl_winsys_egl_x11_dispose;

  winsys_class->renderer_connect = cogl_winsys_egl_x11_renderer_connect;
  winsys_class->display_setup = cogl_winsys_egl_x11_display_setup;
  winsys_class->display_destroy = cogl_winsys_egl_x11_display_destroy;
  winsys_class->context_init = cogl_winsys_egl_x11_context_init;

  winsys_egl_class->add_config_attributes = cogl_winsys_egl_x11_add_config_attributes;
  winsys_egl_class->choose_config = cogl_winsys_egl_x11_choose_config;
  winsys_egl_class->context_created = cogl_winsys_egl_x11_context_created;
  winsys_egl_class->cleanup_context = cogl_winsys_egl_x11_cleanup_context;
}

/* Texture-from-pixmap support. This is X11/EGL specific (GLX support
 * is disabled in this build), so unlike upstream's pre-refactor code
 * there's no need for a winsys vtable indirection here any more -
 * cogl-texture-pixmap-x11.c calls these directly. */

gboolean
cogl_winsys_egl_x11_texture_pixmap_create (CoglTexturePixmapX11 *tex_pixmap)
{
  CoglTexture *tex = COGL_TEXTURE (tex_pixmap);
  CoglContext *ctx = cogl_texture_get_context (tex);
  CoglTexturePixmapEGL *egl_tex_pixmap;
  EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
  CoglPixelFormat texture_format;
  CoglRendererEGL *egl_renderer;

  egl_renderer = cogl_renderer_get_winsys_data (ctx->display->renderer);

  ensure_egl_image_khr_resolved ();
  if (!create_image_khr || !destroy_image_khr)
    {
      tex_pixmap->winsys = NULL;
      return FALSE;
    }

  egl_tex_pixmap = g_new0 (CoglTexturePixmapEGL, 1);

  egl_tex_pixmap->image =
    create_image_khr (egl_renderer->edpy,
                      EGL_NO_CONTEXT,
                      EGL_NATIVE_PIXMAP_KHR,
                      (EGLClientBuffer) tex_pixmap->pixmap,
                      attribs);
  if (egl_tex_pixmap->image == EGL_NO_IMAGE_KHR)
    {
      g_free (egl_tex_pixmap);
      return FALSE;
    }

  texture_format = (tex_pixmap->depth >= 32 ?
                    COGL_PIXEL_FORMAT_RGBA_8888_PRE :
                    COGL_PIXEL_FORMAT_RGB_888);

  egl_tex_pixmap->texture =
    cogl_texture_2d_new_from_egl_image (ctx,
                                       cogl_texture_get_width (tex),
                                       cogl_texture_get_height (tex),
                                       texture_format,
                                       egl_tex_pixmap->image,
                                       COGL_EGL_IMAGE_FLAG_NONE,
                                       NULL);

  /* The image is initially bound as part of the creation */
  egl_tex_pixmap->bind_tex_image_queued = FALSE;

  tex_pixmap->winsys = egl_tex_pixmap;

  return TRUE;
}

void
cogl_winsys_egl_x11_texture_pixmap_free (CoglTexturePixmapX11 *tex_pixmap)
{
  CoglTexturePixmapEGL *egl_tex_pixmap;
  CoglContext *ctx;
  CoglRendererEGL *egl_renderer;

  ctx = cogl_texture_get_context (COGL_TEXTURE (tex_pixmap));

  if (!tex_pixmap->winsys)
    return;

  egl_tex_pixmap = tex_pixmap->winsys;
  egl_renderer = cogl_renderer_get_winsys_data (ctx->display->renderer);

  if (egl_tex_pixmap->texture)
    g_object_unref (egl_tex_pixmap->texture);

  if (egl_tex_pixmap->image != EGL_NO_IMAGE_KHR)
    destroy_image_khr (egl_renderer->edpy, egl_tex_pixmap->image);

  tex_pixmap->winsys = NULL;
  g_free (egl_tex_pixmap);
}

gboolean
cogl_winsys_egl_x11_texture_pixmap_update (CoglTexturePixmapX11 *tex_pixmap,
                                           CoglTexturePixmapStereoMode stereo_mode,
                                           gboolean needs_mipmap)
{
  CoglTexturePixmapEGL *egl_tex_pixmap = tex_pixmap->winsys;
  CoglTexture2D *tex_2d;
  GError *error = NULL;

  if (needs_mipmap)
    return FALSE;

  if (egl_tex_pixmap->bind_tex_image_queued)
    {
      COGL_NOTE (TEXTURE_PIXMAP, "Rebinding GLXPixmap for %p", tex_pixmap);

      tex_2d = COGL_TEXTURE_2D (egl_tex_pixmap->texture);

      if (cogl_texture_2d_gl_bind_egl_image (tex_2d,
                                             egl_tex_pixmap->image,
                                             &error))
        {
          egl_tex_pixmap->bind_tex_image_queued = FALSE;
        }
      else
        {
          g_warning ("Failed to rebind EGLImage to CoglTexture2D: %s",
                     error->message);
          g_error_free (error);
        }
    }

  return TRUE;
}

void
cogl_winsys_egl_x11_texture_pixmap_damage_notify (CoglTexturePixmapX11 *tex_pixmap)
{
  CoglTexturePixmapEGL *egl_tex_pixmap = tex_pixmap->winsys;

  egl_tex_pixmap->bind_tex_image_queued = TRUE;
}

CoglTexture *
cogl_winsys_egl_x11_texture_pixmap_get_texture (CoglTexturePixmapX11 *tex_pixmap,
                                                CoglTexturePixmapStereoMode stereo_mode)
{
  CoglTexturePixmapEGL *egl_tex_pixmap = tex_pixmap->winsys;

  return egl_tex_pixmap->texture;
}
