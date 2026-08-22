/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright 2013 Red Hat, Inc.
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
 * Author: Giovanni Campagna <gcampagn@redhat.com>
 */

#pragma once

#include "backends/meta-cursor-renderer.h"
#include "meta/meta-cursor-tracker.h"

struct _MetaCursorTrackerClass
{
  GObjectClass parent_class;

  ClutterCursor * (* get_sprite) (MetaCursorTracker *tracker);
};

void meta_cursor_tracker_set_current_cursor (MetaCursorTracker *tracker,
                                             ClutterCursor     *cursor);

void meta_cursor_tracker_invalidate_position (MetaCursorTracker *tracker);

/* Restored alongside the X11 backend: still used internally (still
 * 'static' there), made non-static so the X11 tracker's XFixesCursorNotify
 * handling can call it directly, same as before X11 support was removed. */
void meta_cursor_tracker_notify_cursor_changed (MetaCursorTracker *tracker);

MetaBackend * meta_cursor_tracker_get_backend (MetaCursorTracker *tracker);

void meta_cursor_tracker_destroy (MetaCursorTracker *tracker);
