/*
 * Copyright 2013, 2018 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include "backends/x11/cm/meta-cursor-sprite-xfixes.h"

#include <X11/extensions/Xfixes.h>

#include "compositor/compositor-private.h"
#include "core/display-private.h"
#include "meta/meta-x11-display.h"

enum
{
  PROP_0,

  PROP_DISPLAY,

  N_PROPS
};

static GParamSpec *obj_props[N_PROPS];

struct _MetaCursorSpriteXfixes
{
  ClutterCursor parent;

  MetaDisplay *display;

  /* Upstream's MetaCursorSprite (this class's old parent) took a texture
   * via meta_cursor_sprite_set_texture() and exposed it generically; its
   * replacement, ClutterCursor, instead has subclasses store their own
   * texture/hotspot and implement get_texture() to hand it back on
   * request (see the stock MetaCursorXcursor class for the pattern this
   * follows). */
  CoglTexture *texture;
  int hot_x;
  int hot_y;
};

static void
meta_screen_cast_xfixes_init_initable_iface (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (MetaCursorSpriteXfixes,
                         meta_cursor_sprite_xfixes,
                         CLUTTER_TYPE_CURSOR,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                meta_screen_cast_xfixes_init_initable_iface))

static gboolean
meta_cursor_sprite_xfixes_realize_texture (ClutterCursor *cursor)
{
  return TRUE;
}

static gboolean
meta_cursor_sprite_xfixes_is_animated (ClutterCursor *cursor)
{
  return FALSE;
}

static CoglTexture *
meta_cursor_sprite_xfixes_get_texture (ClutterCursor *cursor,
                                       int           *hot_x,
                                       int           *hot_y)
{
  MetaCursorSpriteXfixes *sprite_xfixes = META_CURSOR_SPRITE_XFIXES (cursor);

  if (hot_x)
    *hot_x = sprite_xfixes->hot_x;
  if (hot_y)
    *hot_y = sprite_xfixes->hot_y;

  return sprite_xfixes->texture;
}

static void
meta_cursor_sprite_xfixes_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  MetaCursorSpriteXfixes *sprite_xfixes = META_CURSOR_SPRITE_XFIXES (object);

  switch (prop_id)
    {
    case PROP_DISPLAY:
      g_value_set_object (value, sprite_xfixes->display);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
meta_cursor_sprite_xfixes_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  MetaCursorSpriteXfixes *sprite_xfixes = META_CURSOR_SPRITE_XFIXES (object);

  switch (prop_id)
    {
    case PROP_DISPLAY:
      sprite_xfixes->display = g_value_get_object (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

MetaCursorSpriteXfixes *
meta_cursor_sprite_xfixes_new (MetaDisplay        *display,
                               MetaCursorTracker  *cursor_tracker,
                               GError            **error)
{
  return g_initable_new (META_TYPE_CURSOR_SPRITE_XFIXES,
                         NULL, error,
                         "display", display,
                         NULL);
}

static gboolean
meta_cursor_sprite_xfixes_initable_init (GInitable     *initable,
                                         GCancellable  *cancellable,
                                         GError       **error)
{
  MetaCursorSpriteXfixes *sprite_xfixes =
    META_CURSOR_SPRITE_XFIXES (initable);
  MetaX11Display *x11_display;
  Display *xdisplay;
  XFixesCursorImage *cursor_image;
  CoglTexture *texture;
  uint8_t *cursor_data;
  gboolean free_cursor_data;
  MetaCompositor *compositor;
  MetaBackend *backend;
  ClutterBackend *clutter_backend;
  CoglContext *cogl_context;

  x11_display = meta_display_get_x11_display (sprite_xfixes->display);
  xdisplay = meta_x11_display_get_xdisplay (x11_display);
  cursor_image = XFixesGetCursorImage (xdisplay);
  if (!cursor_image)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to get cursor image");
      return FALSE;
    }

  /*
   * Like all X APIs, XFixesGetCursorImage() returns arrays of 32-bit
   * quantities as arrays of long; we need to convert on 64 bit
   */
  if (sizeof (long) == 4)
    {
      cursor_data = (uint8_t *) cursor_image->pixels;
      free_cursor_data = FALSE;
    }
  else
    {
      int i, j;
      uint32_t *cursor_words;
      unsigned long *p;
      uint32_t *q;

      cursor_words = g_new (uint32_t,
                            cursor_image->width * cursor_image->height);
      cursor_data = (uint8_t *) cursor_words;

      p = cursor_image->pixels;
      q = cursor_words;
      for (j = 0; j < cursor_image->height; j++)
        {
          for (i = 0; i < cursor_image->width; i++)
            *(q++) = *(p++);
        }

      free_cursor_data = TRUE;
    }

  compositor = meta_display_get_compositor (sprite_xfixes->display);
  backend = meta_compositor_get_backend (compositor);
  clutter_backend = meta_backend_get_clutter_backend (backend);
  cogl_context = clutter_backend_get_cogl_context (clutter_backend);
  texture = cogl_texture_2d_new_from_data (cogl_context,
                                           cursor_image->width,
                                           cursor_image->height,
                                           COGL_PIXEL_FORMAT_CAIRO_ARGB32_COMPAT,
                                           cursor_image->width * 4, /* stride */
                                           cursor_data,
                                           error);

  if (free_cursor_data)
    g_free (cursor_data);

  if (!texture)
    {
      XFree (cursor_image);
      return FALSE;
    }

  sprite_xfixes->texture = texture;
  sprite_xfixes->hot_x = cursor_image->xhot;
  sprite_xfixes->hot_y = cursor_image->yhot;
  XFree (cursor_image);

  return TRUE;
}

static void
meta_screen_cast_xfixes_init_initable_iface (GInitableIface *iface)
{
  iface->init = meta_cursor_sprite_xfixes_initable_init;
}

static void
meta_cursor_sprite_xfixes_init (MetaCursorSpriteXfixes *sprite_xfixes)
{
}

static void
meta_cursor_sprite_xfixes_finalize (GObject *object)
{
  MetaCursorSpriteXfixes *sprite_xfixes = META_CURSOR_SPRITE_XFIXES (object);

  g_clear_object (&sprite_xfixes->texture);

  G_OBJECT_CLASS (meta_cursor_sprite_xfixes_parent_class)->finalize (object);
}

static void
meta_cursor_sprite_xfixes_class_init (MetaCursorSpriteXfixesClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterCursorClass *cursor_class = CLUTTER_CURSOR_CLASS (klass);

  object_class->get_property = meta_cursor_sprite_xfixes_get_property;
  object_class->set_property = meta_cursor_sprite_xfixes_set_property;
  object_class->finalize = meta_cursor_sprite_xfixes_finalize;

  cursor_class->realize_texture = meta_cursor_sprite_xfixes_realize_texture;
  cursor_class->is_animated = meta_cursor_sprite_xfixes_is_animated;
  cursor_class->get_texture = meta_cursor_sprite_xfixes_get_texture;

  obj_props[PROP_DISPLAY] =
    g_param_spec_object ("display", NULL, NULL,
                         META_TYPE_DISPLAY,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, obj_props);
}
