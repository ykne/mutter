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
#include "cogl/cogl-renderer-private.h"
#include "cogl/cogl-context-private.h"
#include "cogl/winsys/cogl-texture-pixmap-x11-private.h"
#include "cogl/cogl-texture-2d-private.h"
#include "cogl/driver/gl/cogl-texture-2d-gl-private.h"
#include "cogl/cogl-texture-2d.h"
#include "cogl/winsys/cogl-onscreen-egl.h"
#include "cogl/winsys/cogl-onscreen-xlib.h"
#include "cogl/winsys/cogl-winsys-egl-x11-private.h"
#include "cogl/cogl-context-egl.h"
#include "cogl/cogl-display-egl.h"
#include "cogl/cogl-renderer-egl.h"
#include "cogl/winsys/cogl-winsys.h"

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

struct _CoglRendererEglX11
{
  CoglRendererEGL parent;
};

G_DEFINE_FINAL_TYPE (CoglRendererEglX11, cogl_renderer_egl_x11,
                     COGL_TYPE_RENDERER_EGL)

struct _CoglDisplayEglX11
{
  CoglDisplayEGL parent;

  Window dummy_xwin;
};

G_DEFINE_FINAL_TYPE (CoglDisplayEglX11, cogl_display_egl_x11,
                     COGL_TYPE_DISPLAY_EGL)

typedef struct _CoglTexturePixmapEGL
{
  EGLImageKHR image;
  CoglTexture *texture;
  gboolean bind_tex_image_queued;
} CoglTexturePixmapEGL;

static EGLDisplay
context_get_edisplay (CoglContext *context)
{
  CoglRenderer *renderer = cogl_context_get_renderer (context);

  return cogl_renderer_egl_get_edisplay (COGL_RENDERER_EGL (renderer));
}

static CoglOnscreen *
find_onscreen_for_xid (CoglContext *context, uint32_t xid)
{
  GList *l;

  for (l = cogl_context_get_framebuffers (context); l; l = l->next)
    {
      CoglFramebuffer *framebuffer = l->data;
      CoglOnscreen *onscreen;

      if (!COGL_IS_ONSCREEN (framebuffer))
        continue;

      onscreen = COGL_ONSCREEN (framebuffer);
      if (COGL_IS_ONSCREEN_XLIB (onscreen) &&
          cogl_onscreen_xlib_is_for_window (onscreen, (Window) xid))
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

/* The filter outlives the context it was registered for (it lives on
 * the renderer), so it only holds a weak reference to it. */
typedef struct _CoglEventFilterData
{
  GWeakRef context;
} CoglEventFilterData;

static CoglFilterReturn
event_filter_cb (XEvent *xevent, void *data)
{
  CoglEventFilterData *filter_data = data;
  g_autoptr (CoglContext) context = g_weak_ref_get (&filter_data->context);

  if (!context)
    return COGL_FILTER_CONTINUE;

  /* Expose events used to be turned into onscreen "dirty" events here.
   * Upstream dropped the whole dirty-event queue (nothing but the X11
   * backend ever produced them, and nothing consumes them), so there is
   * nothing left to notify. */
  if (xevent->type == ConfigureNotify)
    {
      notify_resize (context,
                     xevent->xconfigure.window,
                     xevent->xconfigure.width,
                     xevent->xconfigure.height);
    }

  return COGL_FILTER_CONTINUE;
}

XVisualInfo *
cogl_display_xlib_get_visual_info (CoglDisplay *display,
                                   EGLConfig    egl_config)
{
  CoglRenderer *renderer = cogl_display_get_renderer (display);
  CoglXlibRenderer *xlib_renderer = _cogl_xlib_renderer_get_data (renderer);
  EGLDisplay edpy =
    cogl_renderer_egl_get_edisplay (COGL_RENDERER_EGL (renderer));
  XVisualInfo visinfo_template;
  int template_mask = 0;
  XVisualInfo *visinfo = NULL;
  int visinfos_count;
  EGLint visualid, red_size, green_size, blue_size, alpha_size;

  eglGetConfigAttrib (edpy, egl_config, EGL_NATIVE_VISUAL_ID, &visualid);

  if (visualid != 0)
    {
      visinfo_template.visualid = visualid;
      template_mask |= VisualIDMask;
    }
  else
    {
      /* some EGL drivers don't implement the EGL_NATIVE_VISUAL_ID
       * attribute, so attempt to find the closest match. */

      eglGetConfigAttrib (edpy, egl_config, EGL_RED_SIZE, &red_size);
      eglGetConfigAttrib (edpy, egl_config, EGL_GREEN_SIZE, &green_size);
      eglGetConfigAttrib (edpy, egl_config, EGL_BLUE_SIZE, &blue_size);
      eglGetConfigAttrib (edpy, egl_config, EGL_ALPHA_SIZE, &alpha_size);

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
cogl_renderer_egl_x11_connect (CoglRenderer  *renderer,
                               GError       **error)
{
  CoglRendererClass *parent_class =
    COGL_RENDERER_CLASS (cogl_renderer_egl_x11_parent_class);
  CoglXlibRenderer *xlib_renderer;

  if (!_cogl_xlib_renderer_connect (renderer, error))
    return FALSE;

  xlib_renderer = _cogl_xlib_renderer_get_data (renderer);

  cogl_renderer_egl_set_edisplay (COGL_RENDERER_EGL (renderer),
                                  eglGetDisplay ((EGLNativeDisplayType) xlib_renderer->xdpy));

  if (!parent_class->connect (renderer, error))
    {
      _cogl_xlib_renderer_disconnect (renderer);
      return FALSE;
    }

  return TRUE;
}

static void
cogl_renderer_egl_x11_dispose (GObject *object)
{
  CoglRenderer *renderer = COGL_RENDERER (object);

  /* The parent's dispose() eglTerminate()s the EGLDisplay, which was
   * created from our Xlib connection, so the connection must stay open
   * until after it has run. */
  G_OBJECT_CLASS (cogl_renderer_egl_x11_parent_class)->dispose (object);

  _cogl_xlib_renderer_disconnect (renderer);
}

static void
cogl_renderer_egl_x11_init (CoglRendererEglX11 *renderer_egl_x11)
{
}

static void
cogl_renderer_egl_x11_class_init (CoglRendererEglX11Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  CoglRendererClass *renderer_class = COGL_RENDERER_CLASS (klass);

  object_class->dispose = cogl_renderer_egl_x11_dispose;

  renderer_class->connect = cogl_renderer_egl_x11_connect;
}

CoglRenderer *
cogl_renderer_egl_x11_new (void)
{
  return g_object_new (COGL_TYPE_RENDERER_EGL_X11, NULL);
}

static int
cogl_display_egl_x11_add_config_attributes (CoglDisplayEGL *display_egl,
                                            EGLint         *attributes)
{
  int i = 0;

  attributes[i++] = EGL_SURFACE_TYPE;
  attributes[i++] = EGL_WINDOW_BIT;

  return i;
}

static gboolean
cogl_display_egl_x11_choose_config (CoglDisplayEGL  *display_egl,
                                    EGLint          *attributes,
                                    EGLConfig       *out_config,
                                    GError         **error)
{
  CoglRenderer *renderer = cogl_display_get_renderer (COGL_DISPLAY (display_egl));
  EGLDisplay edpy =
    cogl_renderer_egl_get_edisplay (COGL_RENDERER_EGL (renderer));
  EGLint config_count = 0;
  EGLBoolean status;

  status = eglChooseConfig (edpy,
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
cogl_display_egl_x11_context_created (CoglDisplayEGL  *display_egl,
                                      GError         **error)
{
  CoglDisplay *display = COGL_DISPLAY (display_egl);
  CoglDisplayEglX11 *display_egl_x11 = COGL_DISPLAY_EGL_X11 (display_egl);
  CoglRenderer *renderer = cogl_display_get_renderer (display);
  CoglRendererEGL *renderer_egl = COGL_RENDERER_EGL (renderer);
  EGLDisplay edpy = cogl_renderer_egl_get_edisplay (renderer_egl);
  CoglXlibRenderer *xlib_renderer = _cogl_xlib_renderer_get_data (renderer);
  EGLConfig egl_config = cogl_display_egl_get_egl_config (display_egl);
  EGLSurface dummy_surface = cogl_display_egl_get_dummy_surface (display_egl);
  XVisualInfo *xvisinfo;
  XSetWindowAttributes attrs;
  const char *error_message;

  xvisinfo = cogl_display_xlib_get_visual_info (display, egl_config);
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

  if (!cogl_renderer_egl_has_feature (renderer_egl,
                                      COGL_EGL_WINSYS_FEATURE_SURFACELESS_CONTEXT))
    {
      display_egl_x11->dummy_xwin =
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

      dummy_surface =
        eglCreateWindowSurface (edpy,
                                egl_config,
                                (EGLNativeWindowType) display_egl_x11->dummy_xwin,
                                NULL);
      cogl_display_egl_set_dummy_surface (display_egl, dummy_surface);

      if (dummy_surface == EGL_NO_SURFACE)
        {
          error_message = "Unable to create an EGL surface";
          XFree (xvisinfo);
          goto fail;
        }
    }

  g_clear_pointer (&xvisinfo, XFree);

  if (!cogl_display_egl_make_current (display_egl,
                                      dummy_surface,
                                      dummy_surface,
                                      cogl_display_egl_get_egl_context (display_egl)))
    {
      if (dummy_surface == EGL_NO_SURFACE)
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
cogl_display_egl_x11_cleanup_context (CoglDisplayEGL *display_egl)
{
  CoglDisplayEglX11 *display_egl_x11 = COGL_DISPLAY_EGL_X11 (display_egl);
  CoglRenderer *renderer =
    cogl_display_get_renderer (COGL_DISPLAY (display_egl));
  EGLDisplay edpy =
    cogl_renderer_egl_get_edisplay (COGL_RENDERER_EGL (renderer));
  CoglXlibRenderer *xlib_renderer = _cogl_xlib_renderer_get_data (renderer);
  EGLSurface dummy_surface = cogl_display_egl_get_dummy_surface (display_egl);

  if (dummy_surface != EGL_NO_SURFACE)
    {
      eglDestroySurface (edpy, dummy_surface);
      cogl_display_egl_set_dummy_surface (display_egl, EGL_NO_SURFACE);
    }

  if (display_egl_x11->dummy_xwin)
    {
      XDestroyWindow (xlib_renderer->xdpy, display_egl_x11->dummy_xwin);
      display_egl_x11->dummy_xwin = None;
    }
}

static void
cogl_display_egl_x11_init (CoglDisplayEglX11 *display_egl_x11)
{
}

static void
cogl_display_egl_x11_class_init (CoglDisplayEglX11Class *klass)
{
  CoglDisplayEGLClass *display_egl_class = COGL_DISPLAY_EGL_CLASS (klass);

  display_egl_class->add_config_attributes =
    cogl_display_egl_x11_add_config_attributes;
  display_egl_class->choose_config = cogl_display_egl_x11_choose_config;
  display_egl_class->context_created = cogl_display_egl_x11_context_created;
  display_egl_class->cleanup_context = cogl_display_egl_x11_cleanup_context;
}

CoglDisplay *
cogl_display_egl_x11_new (CoglRenderer *renderer)
{
  return g_object_new (COGL_TYPE_DISPLAY_EGL_X11,
                       "renderer", renderer,
                       NULL);
}

CoglContext *
cogl_context_egl_x11_new (CoglDisplay  *display,
                          GError      **error)
{
  CoglContext *context;
  CoglEventFilterData *filter_data;

  context = cogl_context_egl_new (display, error);
  if (!context)
    return NULL;

  filter_data = g_new0 (CoglEventFilterData, 1);
  g_weak_ref_init (&filter_data->context, context);
  _cogl_renderer_add_native_filter (cogl_display_get_renderer (display),
                                    (CoglNativeFilterFunc) event_filter_cb,
                                    filter_data);

  return context;
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

  ensure_egl_image_khr_resolved ();
  if (!create_image_khr || !destroy_image_khr)
    {
      tex_pixmap->winsys = NULL;
      return FALSE;
    }

  egl_tex_pixmap = g_new0 (CoglTexturePixmapEGL, 1);

  egl_tex_pixmap->image =
    create_image_khr (context_get_edisplay (ctx),
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

  ctx = cogl_texture_get_context (COGL_TEXTURE (tex_pixmap));

  if (!tex_pixmap->winsys)
    return;

  egl_tex_pixmap = tex_pixmap->winsys;

  if (egl_tex_pixmap->texture)
    g_object_unref (egl_tex_pixmap->texture);

  if (egl_tex_pixmap->image != EGL_NO_IMAGE_KHR)
    destroy_image_khr (context_get_edisplay (ctx), egl_tex_pixmap->image);

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
