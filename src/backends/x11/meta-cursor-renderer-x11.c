/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2014 Red Hat
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
 * Written by:
 *     Jasper St. Pierre <jstpierre@mecheye.net>
 */

#include "config.h"

#include "backends/x11/meta-cursor-renderer-x11.h"

#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xfixes.h>
#include <math.h>

#include "backends/meta-backend-private.h"
#include "backends/meta-cursor-xcursor.h"
#include "backends/meta-stage-private.h"
#include "backends/x11/meta-backend-x11.h"
#include "clutter/clutter-mutter.h"
#include "clutter/clutter.h"
#include "cogl/cogl.h"
#include "compositor/compositor-private.h"
#include "compositor/meta-window-drag.h"
#include "meta/display.h"
#include "meta/meta-context.h"
#include "mtk/mtk.h"

struct _MetaCursorRendererX11
{
  MetaCursorRenderer parent_instance;

  gboolean server_cursor_visible;
  gboolean force_sw_cursor;
  gboolean root_cursor_blanked;
  MetaOverlay *sw_cursor_overlay;
  guint force_sw_cursor_poll_id;
};

G_DEFINE_FINAL_TYPE (MetaCursorRendererX11, meta_cursor_renderer_x11, META_TYPE_CURSOR_RENDERER);


static Cursor
create_blank_cursor (Display *xdisplay)
{
  Pixmap pixmap;
  XColor color;
  Cursor cursor;
  XGCValues gc_values;
  GC gc;

  pixmap = XCreatePixmap (xdisplay, DefaultRootWindow (xdisplay), 1, 1, 1);

  gc_values.foreground = BlackPixel (xdisplay, DefaultScreen (xdisplay));
  gc = XCreateGC (xdisplay, pixmap, GCForeground, &gc_values);

  XFillRectangle (xdisplay, pixmap, gc, 0, 0, 1, 1);

  color.pixel = 0;
  color.red = color.blue = color.green = 0;

  cursor = XCreatePixmapCursor (xdisplay, pixmap, pixmap, &color, &color, 1, 1);

  XFreeGC (xdisplay, gc);
  XFreePixmap (xdisplay, pixmap);

  return cursor;
}

static Cursor
create_x_cursor (Display           *xdisplay,
                 ClutterCursorType  cursor)
{
  Cursor result;

  if (cursor == CLUTTER_CURSOR_NONE)
    return create_blank_cursor (xdisplay);

  result = XcursorLibraryLoadCursor (xdisplay, meta_cursor_get_name (cursor));
  if (!result)
    result = XcursorLibraryLoadCursor (xdisplay, meta_cursor_get_legacy_name (cursor));

  return result;
}

/* MetaCursorSpriteXfixes (backends/x11/cm/meta-cursor-sprite-xfixes.c -
 * this tracker's own XFixes-mirrored sprite, kept fresh by the
 * XFixesCursorNotify dispatch fix) derives directly from CLUTTER_TYPE_
 * CURSOR, NOT MetaCursorXcursor - so meta_cursor_renderer_x11_update_
 * cursor()'s own META_IS_CURSOR_XCURSOR() check (below) never matches
 * it, no matter how correctly it's routed through meta_cursor_
 * renderer_set_cursor(). That's why routing the polled sprite through
 * set_cursor() had no visible effect on the native cursor for a plain
 * hover: the type check gated it out before create_x_cursor()'s
 * theme-based lookup could ever run. Sidestep that entirely for the
 * no-active-grab case - build a native X Cursor directly from a fresh
 * XFixesGetCursorImage() query's own raw pixel/hotspot data (same
 * technique already proven correct for the SW overlay in
 * update_sw_cursor_overlay() below, just fed into XDefineCursor()
 * instead of a Cogl texture) rather than trying to route anything
 * through the xcursor-type-specific sprite machinery at all. */
static Cursor
create_x_cursor_from_xfixes_image (Display *xdisplay)
{
  XFixesCursorImage *image;
  XcursorImage *xc_image;
  Cursor xcursor = None;
  int n_pixels, i;

  image = XFixesGetCursorImage (xdisplay);
  if (!image)
    return None;

  if (image->width <= 0 || image->height <= 0)
    {
      XFree (image);
      return None;
    }

  xc_image = XcursorImageCreate (image->width, image->height);
  if (xc_image)
    {
      xc_image->xhot = image->xhot;
      xc_image->yhot = image->yhot;

      /* Same unsigned-long-to-uint32_t narrowing as
       * update_sw_cursor_overlay() below - XFixesCursorImage.pixels is
       * one premultiplied-ARGB32 pixel per `unsigned long` element (8
       * bytes on a 64-bit host), not a tightly-packed buffer.
       * XcursorPixel is already the tightly-packed uint32_t Xcursor
       * itself expects. */
      n_pixels = image->width * image->height;
      for (i = 0; i < n_pixels; i++)
        xc_image->pixels[i] = (XcursorPixel) image->pixels[i];

      xcursor = XcursorImageLoadCursor (xdisplay, xc_image);
      XcursorImageDestroy (xc_image);
    }

  XFree (image);
  return xcursor;
}

/* On a display device with no real hardware cursor plane at all (e.g.
 * QEMU's plain "std VGA", bound to the bochs-drm kernel driver, which
 * exposes exactly one DRM plane total), nothing else on this X server -
 * not just this compositor's own cursor sprite handling below, but ANY
 * X11 client's XDefineCursor() call, including the separate
 * mutter-x11-frames helper process that owns each window's CSD and sets
 * its own resize-edge cursors directly on its own X window - ever
 * results in a visible cursor. There's no way for the compositor to
 * intercept or override another client's own cursor-setting calls, but
 * XFixes's cursor-image query reflects whatever cursor the X server
 * considers "current" system-wide, regardless of which client set it -
 * so mirror that directly into our own always-on-top stage overlay
 * instead of relying on this renderer's own (necessarily narrower,
 * gnome-shell-only) idea of what the cursor should be. */
static void
update_sw_cursor_overlay (MetaCursorRendererX11 *x11,
                          MetaBackend           *backend,
                          Display               *xdisplay)
{
  ClutterActor *stage = meta_backend_get_stage (backend);
  XFixesCursorImage *image;
  g_autoptr (CoglTexture) texture = NULL;
  g_autofree uint32_t *pixel_data = NULL;
  CoglPixelFormat cogl_format;
  ClutterBackend *clutter_backend;
  CoglContext *cogl_context;
  g_autoptr (GError) error = NULL;
  graphene_matrix_t matrix;
  graphene_rect_t dst_rect;
  int width, height, rowstride;
  int n_pixels, i;

  if (!x11->sw_cursor_overlay)
    x11->sw_cursor_overlay = meta_stage_create_cursor_overlay (META_STAGE (stage));

  image = XFixesGetCursorImage (xdisplay);
  if (!image)
    {
      meta_overlay_set_visible (x11->sw_cursor_overlay, FALSE);
      return;
    }

  width = image->width;
  height = image->height;

  if (width <= 0 || height <= 0)
    {
      meta_overlay_set_visible (x11->sw_cursor_overlay, FALSE);
      XFree (image);
      return;
    }

  /* XFixesCursorImage.pixels is an array of `unsigned long`, one
   * premultiplied-ARGB32 pixel per element (the protocol's 32-bit wire
   * values, widened to `unsigned long` by Xlib - 8 bytes wide on a
   * 64-bit host), NOT a tightly-packed 4-bytes-per-pixel buffer -
   * narrow each element down to a real uint32_t before handing it to
   * Cogl, or the image comes out corrupted/shifted. */
  n_pixels = width * height;
  pixel_data = g_new (uint32_t, n_pixels);
  for (i = 0; i < n_pixels; i++)
    pixel_data[i] = (uint32_t) image->pixels[i];

  rowstride = width * 4;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  cogl_format = COGL_PIXEL_FORMAT_BGRA_8888_PRE;
#else
  cogl_format = COGL_PIXEL_FORMAT_ARGB_8888_PRE;
#endif

  clutter_backend = meta_backend_get_clutter_backend (backend);
  cogl_context = clutter_backend_get_cogl_context (clutter_backend);
  texture = cogl_texture_2d_new_from_data (cogl_context,
                                           width, height,
                                           cogl_format,
                                           rowstride,
                                           (uint8_t *) pixel_data,
                                           &error);
  if (!texture)
    {
      g_warning ("Failed to allocate software cursor texture: %s",
                error->message);
      meta_overlay_set_visible (x11->sw_cursor_overlay, FALSE);
      XFree (image);
      return;
    }

  /* Position and size come directly from this same XFixesCursorImage
   * query (image->x/y is the pointer's current root-relative position,
   * xhot/yhot its hotspot within the image) rather than from this
   * renderer's own separately-tracked pointer position - this is the
   * actual, authoritative current position/shape pair the X server is
   * using right now, guaranteed mutually consistent since both come
   * from the same round-trip. */
  dst_rect = (graphene_rect_t) {
    .origin = {
      .x = image->x - image->xhot,
      .y = image->y - image->yhot,
    },
    .size = {
      .width = width,
      .height = height,
    },
  };

  /* image->x/y are raw, unscaled X11 root-window pixel coordinates -
   * but the stage's own rendering expects positions relative to the
   * current ClutterStageView's own origin, snapped to that view's own
   * scale factor, the same way MetaCursorRenderer's own (unexported,
   * so duplicated here) align_cursor_position() does for the generic
   * overlay in meta-cursor-renderer.c. Skipping this produced a
   * position error that grows with distance from the view's own
   * origin - negligible near x=0, 10+ px by the right edge - confirmed
   * by direct on-screen comparison against the hardware pointer (whose
   * own position isn't computed by this code at all, so it wasn't
   * affected - see project_edge_resize_broken.md). */
  {
    ClutterStageView *view =
      clutter_stage_get_view_at (CLUTTER_STAGE (stage), image->x, image->y);

    if (view)
      {
        MtkRectangle view_layout;
        float view_scale;

        clutter_stage_view_get_layout (view, &view_layout);
        view_scale = clutter_stage_view_get_scale (view);

        graphene_rect_offset (&dst_rect, -view_layout.x, -view_layout.y);
        dst_rect.origin.x = floorf (dst_rect.origin.x * view_scale) / view_scale;
        dst_rect.origin.y = floorf (dst_rect.origin.y * view_scale) / view_scale;
        graphene_rect_offset (&dst_rect, view_layout.x, view_layout.y);
      }
  }

  graphene_matrix_init_identity (&matrix);

  meta_overlay_set_visible (x11->sw_cursor_overlay, TRUE);
  meta_stage_update_cursor_overlay (META_STAGE (stage),
                                    x11->sw_cursor_overlay,
                                    texture,
                                    &matrix,
                                    &dst_rect);

  XFree (image);
}

/* update_cursor() below (and therefore update_sw_cursor_overlay()) only
 * runs reactively, driven by Clutter stage motion events - but this
 * project's own _updateRegions() (see js/ui/layout.js and
 * project_edge_resize_broken.md) punches a hole in the stage's input
 * region matching every real client window's own claimed geometry, so
 * that clicks (and, just as much, plain pointer MOTION) landing there
 * get routed by the X server directly to the client instead of to
 * mutter's own compositor stage - meaning the stage never even sees a
 * motion event while hovering a real window's edge, and this overlay's
 * position/shape would only ever refresh while hovering bare desktop
 * background otherwise. A real hardware cursor plane doesn't have this
 * problem (it tracks raw device motion directly, independent of which
 * client owns input at that pixel), so poll XFixesGetCursorImage() on a
 * plain timer instead of relying solely on stage events, to guarantee
 * this stays correct everywhere input-region routing sends real motion
 * away from the compositor. */
static gboolean
force_sw_cursor_poll_cb (gpointer user_data)
{
  MetaCursorRendererX11 *x11 = user_data;
  MetaBackend *backend =
    meta_cursor_renderer_get_backend (META_CURSOR_RENDERER (x11));
  MetaBackendX11 *backend_x11 = META_BACKEND_X11 (backend);
  Display *xdisplay = meta_backend_x11_get_xdisplay (backend_x11);

  update_sw_cursor_overlay (x11, backend, xdisplay);

  return G_SOURCE_CONTINUE;
}

/* This project's own Clutter/X11-CM stage-grab machinery
 * (meta_window_drag_update_cursor() in compositor/meta-window-drag.c)
 * sets a cursor TYPE on the grab actor and invalidates the sprite
 * whenever an interactive move/resize grab is active - but confirmed via
 * direct logging that this never actually reaches this renderer's own
 * update_cursor() via its cursor_sprite argument in this rebuild
 * (cursor_sprite stayed NULL for the renderer's entire duration of a
 * live resize drag, reproducing exactly the "cursor reverts to a plain
 * arrow the moment button 1 goes down, for as long as it's held" bug).
 * This looks like a separate, pre-existing gap in this project's
 * reconstructed grab/cursor plumbing, independent of the
 * MUTTER_X11_FORCE_SW_CURSOR work itself. Query the actively-running
 * grab op directly instead of relying on cursor_sprite for this one
 * case (mirroring meta_cursor_for_grab_op()'s own mapping, which is
 * static/private to meta-window-drag.c) so the SW-cursor overlay shows
 * the right resize/move cursor for the whole duration of a drag - real
 * client-window per-widget cursors (the mutter-x11-frames helper's own
 * hover cursor, etc.) still take over via cursor_sprite/XFixes mirroring
 * whenever no grab is active. */
static ClutterCursorType
get_active_grab_op_cursor_type (MetaBackend *backend)
{
  MetaContext *context = meta_backend_get_context (backend);
  MetaDisplay *display;
  MetaCompositor *compositor;
  MetaWindowDrag *window_drag;
  MetaGrabOp op;

  /* This renderer is constructed (and update_cursor() called at least
   * once, to set the initial pointer position/sprite) well before
   * MetaDisplay exists yet - meta_backend_initable_init() sets up the
   * cursor renderer as part of backend init, which happens before
   * meta_context_main_setup() goes on to create the display at all.
   * Both meta_context_get_display() and meta_display_get_compositor()
   * can legitimately return NULL this early - guard both rather than
   * assume a grab (which can't exist yet either way) is ever active
   * before the display exists. */
  display = meta_context_get_display (context);
  if (!display)
    return CLUTTER_CURSOR_INHERIT;

  compositor = meta_display_get_compositor (display);
  if (!compositor)
    return CLUTTER_CURSOR_INHERIT;

  window_drag = meta_compositor_get_current_window_drag (compositor);
  if (!window_drag)
    return CLUTTER_CURSOR_INHERIT;

  op = meta_window_drag_get_grab_op (window_drag);
  op &= ~(META_GRAB_OP_WINDOW_FLAG_UNCONSTRAINED);

  switch (op)
    {
    case META_GRAB_OP_RESIZING_SE:
    case META_GRAB_OP_KEYBOARD_RESIZING_SE:
      return CLUTTER_CURSOR_SE_RESIZE;
    case META_GRAB_OP_RESIZING_S:
    case META_GRAB_OP_KEYBOARD_RESIZING_S:
      return CLUTTER_CURSOR_S_RESIZE;
    case META_GRAB_OP_RESIZING_SW:
    case META_GRAB_OP_KEYBOARD_RESIZING_SW:
      return CLUTTER_CURSOR_SW_RESIZE;
    case META_GRAB_OP_RESIZING_N:
    case META_GRAB_OP_KEYBOARD_RESIZING_N:
      return CLUTTER_CURSOR_N_RESIZE;
    case META_GRAB_OP_RESIZING_NE:
    case META_GRAB_OP_KEYBOARD_RESIZING_NE:
      return CLUTTER_CURSOR_NE_RESIZE;
    case META_GRAB_OP_RESIZING_NW:
    case META_GRAB_OP_KEYBOARD_RESIZING_NW:
      return CLUTTER_CURSOR_NW_RESIZE;
    case META_GRAB_OP_RESIZING_W:
    case META_GRAB_OP_KEYBOARD_RESIZING_W:
      return CLUTTER_CURSOR_W_RESIZE;
    case META_GRAB_OP_RESIZING_E:
    case META_GRAB_OP_KEYBOARD_RESIZING_E:
      return CLUTTER_CURSOR_E_RESIZE;
    case META_GRAB_OP_MOVING:
      return CLUTTER_CURSOR_DEFAULT;
    case META_GRAB_OP_KEYBOARD_MOVING:
    case META_GRAB_OP_KEYBOARD_RESIZING_UNKNOWN:
      return CLUTTER_CURSOR_MOVE;
    default:
      return CLUTTER_CURSOR_INHERIT;
    }
}

static gboolean
meta_cursor_renderer_x11_update_cursor (MetaCursorRenderer *renderer,
                                        ClutterCursor      *cursor_sprite)
{
  MetaCursorRendererX11 *x11 = META_CURSOR_RENDERER_X11 (renderer);
  MetaBackend *backend = meta_cursor_renderer_get_backend (renderer);
  MetaBackendX11 *backend_x11 = META_BACKEND_X11 (backend);
  Window xwindow = meta_backend_x11_get_xwindow (backend_x11);
  Display *xdisplay = meta_backend_x11_get_xdisplay (backend_x11);
  ClutterCursorType cursor = CLUTTER_CURSOR_INHERIT;

  if (xwindow == None)
    {
      if (cursor_sprite)
        clutter_cursor_realize_texture (cursor_sprite);
      return TRUE;
    }

  gboolean has_server_cursor = FALSE;

  if (x11->force_sw_cursor)
    cursor = get_active_grab_op_cursor_type (backend);

  if (cursor == CLUTTER_CURSOR_INHERIT &&
      cursor_sprite && META_IS_CURSOR_XCURSOR (cursor_sprite))
    {
      MetaCursorXcursor *sprite_xcursor =
        META_CURSOR_XCURSOR (cursor_sprite);

      cursor = meta_cursor_xcursor_get_cursor (sprite_xcursor);
    }

  if (cursor != CLUTTER_CURSOR_INHERIT)
    {
      Cursor xcursor;

      xcursor = create_x_cursor (xdisplay, cursor);
      if (xcursor)
        {
          XDefineCursor (xdisplay, xwindow, xcursor);
          XFlush (xdisplay);
          XFreeCursor (xdisplay, xcursor);

          has_server_cursor = TRUE;
        }
    }
  else if (x11->force_sw_cursor)
    {
      /* No active grab-op cursor to apply (the branch above), and the
       * xcursor-type-specific sprite fallback earlier in this function
       * never matches MetaCursorSpriteXfixes (see the comment on
       * create_x_cursor_from_xfixes_image() above) - this is the plain
       * hover case. Define a native cursor built straight from the
       * live XFixes cursor image anyway, so mutter's own window (the
       * only thing observed to actually influence this VM's hardware
       * pointer - see project_edge_resize_broken.md) reflects real
       * per-widget hover cursor changes too, not just active grabs. */
      Cursor xcursor = create_x_cursor_from_xfixes_image (xdisplay);

      if (xcursor)
        {
          XDefineCursor (xdisplay, xwindow, xcursor);
          XFlush (xdisplay);
          XFreeCursor (xdisplay, xcursor);

          has_server_cursor = TRUE;
        }
    }

  if (has_server_cursor != x11->server_cursor_visible)
    {
      if (has_server_cursor)
        XFixesShowCursor (xdisplay, xwindow);
      else
        XFixesHideCursor (xdisplay, xwindow);

      x11->server_cursor_visible = has_server_cursor;
    }

  if (cursor_sprite)
    clutter_cursor_realize_texture (cursor_sprite);

  if (x11->force_sw_cursor)
    {
      /* The XDefineCursor()/XFixesShowCursor() calls above still run
       * unconditionally - they're what let this renderer's own cursor
       * decisions (e.g. mutter's own grab cursor during an interactive
       * move/resize, which - unlike a plain per-widget hover cursor -
       * really is this compositor's own responsibility, not the
       * client's) reach the real X server cursor state at all. Mirror
       * whatever that state now is (which XFixesGetCursorImage reports
       * correctly regardless of whether we just changed it here, or
       * some other client - e.g. the mutter-x11-frames helper's own
       * per-window hover cursor - already had) into our own overlay,
       * instead of using this renderer's local cursor_sprite/rect math
       * for display, and suppress the base class's own separate
       * ClutterCursor-based overlay (return FALSE below) so the two
       * don't end up drawing two overlapping cursors. */
      if (x11->force_sw_cursor_poll_id == 0)
        {
          x11->force_sw_cursor_poll_id =
            g_timeout_add (16, force_sw_cursor_poll_cb, x11);
        }

      /* Whatever mechanism this VM's display stack uses to mirror the
       * X server's cursor to the actual viewer out-of-band (invisible
       * to any in-guest screenshot/query - see
       * project_edge_resize_broken.md) does not reliably reflect real
       * per-window hover cursor changes, and an earlier attempt to
       * suppress it via XFixesHideCursor() (a visibility TOGGLE) had no
       * effect - it appears to just keep showing whatever cursor IMAGE
       * is currently defined, regardless of show/hide state. Define an
       * actual blank cursor image on the root window instead (once),
       * so there is nothing for that mechanism to mirror at all -
       * this backend's own SW overlay above is now the sole visible
       * cursor for every case, matching kwin_x11's single-cursor
       * behavior via the opposite means (kwin lets native rendering
       * show through; this makes native rendering show nothing, since
       * it doesn't reliably reflect real cursor state here anyway). */
      if (!x11->root_cursor_blanked)
        {
          Cursor blank_cursor = create_blank_cursor (xdisplay);

          if (blank_cursor)
            {
              XDefineCursor (xdisplay, DefaultRootWindow (xdisplay),
                             blank_cursor);
              XFlush (xdisplay);
              XFreeCursor (xdisplay, blank_cursor);
            }
          x11->root_cursor_blanked = TRUE;
        }

      update_sw_cursor_overlay (x11, backend, xdisplay);

      return FALSE;
    }

  /* Never let MetaCursorRenderer's own generic ClutterCursor-based stage
   * overlay activate for this backend, regardless of whether a native
   * XDefineCursor() cursor is currently showing (has_server_cursor).
   * That overlay's position is driven by stage motion events - but this
   * project's own input-region hole-punching architecture
   * (js/ui/layout.js's _updateRegions(), see
   * project_x11_input_region_architecture_revert.md) deliberately routes
   * real pointer motion straight to whichever client window currently
   * owns that screen area, bypassing the stage entirely once the pointer
   * is over a real client window (or once it leaves the X11 screen's own
   * bounds altogether) - so the overlay's last-known position freezes at
   * whatever boundary the stage last saw motion for, producing a visible
   * "ghost" cursor stuck at that edge while the X server's own actual
   * cursor (native XDefineCursor rendering, or - out of this project's
   * control - the display's own client-side cursor mirror) keeps
   * tracking correctly. Confirmed via kwin_x11 on the same VM/hardware:
   * it does zero compositor-side cursor rendering of its own and shows a
   * single, correctly-tracking, correctly-shaped cursor throughout -
   * matching that (pure native rendering, no compositor-drawn overlay at
   * all) is the fix, not building a better compositor-drawn overlay. */
  return FALSE;
}

static void
meta_cursor_renderer_x11_finalize (GObject *object)
{
  MetaCursorRendererX11 *x11 = META_CURSOR_RENDERER_X11 (object);

  g_clear_handle_id (&x11->force_sw_cursor_poll_id, g_source_remove);

  if (x11->sw_cursor_overlay)
    {
      MetaBackend *backend =
        meta_cursor_renderer_get_backend (META_CURSOR_RENDERER (x11));
      ClutterActor *stage = meta_backend_get_stage (backend);

      meta_stage_remove_cursor_overlay (META_STAGE (stage),
                                       x11->sw_cursor_overlay);
    }

  G_OBJECT_CLASS (meta_cursor_renderer_x11_parent_class)->finalize (object);
}

static void
meta_cursor_renderer_x11_class_init (MetaCursorRendererX11Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MetaCursorRendererClass *renderer_class = META_CURSOR_RENDERER_CLASS (klass);

  object_class->finalize = meta_cursor_renderer_x11_finalize;
  renderer_class->update_cursor = meta_cursor_renderer_x11_update_cursor;
}

static void
meta_cursor_renderer_x11_init (MetaCursorRendererX11 *x11)
{
  /* XFixes has no way to retrieve the current cursor visibility. */
  x11->server_cursor_visible = TRUE;

  /* XDefineCursor()/XFixesShowCursor() "succeed" and update the X
   * server's own cursor-state bookkeeping even on a display device with
   * no real hardware cursor plane at all (e.g. QEMU's plain "std VGA",
   * bound to the bochs-drm kernel driver, which exposes exactly one DRM
   * plane total) - nothing ever actually renders the cursor on screen in
   * that case, since there's no hardware sprite for the X server to hand
   * it to.
   *
   * Originally gated behind the MUTTER_X11_FORCE_SW_CURSOR env var,
   * opt-in only, on the assumption that MetaCursorRenderer's own
   * generic stage overlay (meta_cursor_renderer_update_stage_overlay())
   * was an equivalent, always-correct fallback for the non-opted-in
   * case. That assumption turned out to be wrong: the generic overlay
   * positions itself from MetaCursorRenderer's own priv->current_x/
   * current_y, which for the X11-CM backend is only ever refreshed by
   * MetaSpriteX11's ClutterFocus::update_from_event - i.e. real Clutter
   * STAGE events - which this project's own input-region architecture
   * (see project_x11_input_region_architecture_revert.md) deliberately
   * routes away from the stage once the pointer is over a real client
   * window. That overlay's position freezes at whatever boundary the
   * stage last saw motion for - the "ghost cursor stuck at a border"
   * bug (see project_edge_resize_broken.md) - which is why this
   * renderer's update_cursor() now unconditionally suppresses it
   * (returns FALSE unconditionally, further down).
   *
   * update_sw_cursor_overlay() below has no such dependency: it
   * recomputes its destination rect directly from a fresh
   * XFixesGetCursorImage() call's own x/y/xhot/yhot fields every time
   * it runs (driven by its own poll timer, not stage events), so it
   * stays correct regardless of which window currently owns real
   * pointer routing. Given the generic overlay can't be trusted at all
   * on this backend, this is now the only correctly-working
   * compositor-side cursor path for X11-CM - make it unconditional
   * rather than opt-in. */
  x11->force_sw_cursor = TRUE;
}
