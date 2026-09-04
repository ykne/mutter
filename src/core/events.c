/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2001 Havoc Pennington
 * Copyright (C) 2002, 2003, 2004 Red Hat, Inc.
 * Copyright (C) 2003, 2004 Rob Adams
 * Copyright (C) 2004-2006 Elijah Newren
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
 */

#include "config.h"

#include "core/events.h"

#include "backends/meta-a11y-manager.h"
#include "backends/meta-cursor-tracker-private.h"
#include "backends/meta-dnd-private.h"
#include "backends/meta-idle-manager.h"
#include "compositor/compositor-private.h"
#include "compositor/meta-window-actor-private.h"
#include "core/display-private.h"
#include "core/window-private.h"
#include "meta/meta-backend.h"
#include "wayland/meta-wayland-private.h"

#ifdef HAVE_NATIVE_BACKEND
#include "backends/native/meta-backend-native.h"
#endif

#ifdef HAVE_X11
#include "backends/x11/meta-backend-x11.h"
#endif

#define IS_KEY_EVENT(et) ((et) == CLUTTER_KEY_PRESS || \
                          (et) == CLUTTER_KEY_RELEASE)

static ClutterStage *
stage_from_display (MetaDisplay *display)
{
  MetaContext *context = meta_display_get_context (display);
  MetaBackend *backend = meta_context_get_backend (context);

  return CLUTTER_STAGE (meta_backend_get_stage (backend));
}

static gboolean
stage_has_key_focus (MetaDisplay *display)
{
  ClutterStage *stage = stage_from_display (display);

  return clutter_stage_get_key_focus (stage) == NULL;
}

static gboolean
stage_has_grab (MetaDisplay *display)
{
  ClutterStage *stage = stage_from_display (display);

  /* This used to also exclude grab_actor == CLUTTER_ACTOR (stage), on
   * the theory that Clutter falls back to an "implicit" grab on the
   * stage itself when a button press isn't claimed by any reactive
   * actor. That premise doesn't hold: ClutterStage's topmost_grab is
   * only ever set by clutter_grab_activate(), called from
   * clutter_stage_grab()/clutter_stage_grab_inactive() - both of which
   * require an explicit actor argument from the caller. There is no
   * Clutter-internal path that grabs the stage on its own.
   *
   * grab_actor == stage in practice means a real, deliberate whole-stage
   * modal grab - gnome-shell's Main.pushModal(global.stage, ...) (which
   * calls exactly clutter_stage_grab(stage, stage)) is exactly this,
   * and it's not a rare case: the overview (overview.js), the GDM login
   * screen (loginDialog.js), and workspace-switch animations
   * (workspaceAnimation.js) all use it. The exclusion made
   * get_window_for_event() and the unmodified-click handling below
   * treat all of those as "no grab", letting clicks fall through to
   * mutter's normal per-window focus/raise/move handling instead of
   * being properly captured by the active modal grab - confirmed by
   * mutter's own test harness (src/tests/meta-test-shell.c) using the
   * identical clutter_stage_grab(stage, CLUTTER_ACTOR (stage)) pattern
   * to simulate the overview's grab. */
  return clutter_stage_get_grab_actor (stage) != NULL;
}

static MetaWindow *
get_window_for_event (MetaDisplay        *display,
                      const ClutterEvent *event,
                      ClutterActor       *event_actor)
{
  MetaWindowActor *window_actor;

  if (stage_has_grab (display))
    return NULL;

  /* Always use the key focused window for key events. */
  if (IS_KEY_EVENT (clutter_event_type (event)))
    {
      return stage_has_key_focus (display) ? display->focus_window
        : NULL;
    }

  window_actor = meta_window_actor_from_actor (event_actor);

  if (window_actor)
    return meta_window_actor_get_meta_window (window_actor);
  else
    return NULL;
}

static void
handle_idletime_for_event (MetaDisplay        *display,
                           const ClutterEvent *event)
{
  MetaContext *context = meta_display_get_context (display);
  MetaBackend *backend = meta_context_get_backend (context);
  MetaIdleManager *idle_manager;
  ClutterEventType event_type;
  ClutterEventFlags flags;

  flags = clutter_event_get_flags (event);
  event_type = clutter_event_type (event);

  if (flags & CLUTTER_EVENT_FLAG_SYNTHETIC ||
      event_type == CLUTTER_ENTER ||
      event_type == CLUTTER_LEAVE)
    return;

  idle_manager = meta_backend_get_idle_manager (backend);
  meta_idle_manager_reset_idle_time (idle_manager);
}

static gboolean
meta_display_handle_event (MetaDisplay        *display,
                           const ClutterEvent *event,
                           ClutterActor       *event_actor)
{
  MetaContext *context = meta_display_get_context (display);
  MetaBackend *backend = meta_context_get_backend (context);
  MetaA11yManager *a11y_manager = meta_backend_get_a11y_manager (backend);
  MetaCompositor *compositor = meta_display_get_compositor (display);
  ClutterBackend *clutter_backend = meta_backend_get_clutter_backend (backend);
  ClutterSeat *seat = clutter_backend_get_default_seat (clutter_backend);
  ClutterInputDevice *source_device;
  MetaWindow *window = NULL;
  ClutterEventType event_type;
  gboolean has_grab;
  gboolean a11y_grabbed;
  MetaTabletActionMapper *mapper;
  MetaWaylandCompositor *wayland_compositor;
  MetaWaylandTextInput *wayland_text_input = NULL;
  uint32_t time_ms;

  /* No Wayland compositor role exists under the X11 backend (see
   * meta_context_start()) - text-input routing is a Wayland protocol
   * concept, wayland_text_input just stays NULL, same as its
   * declaration default. */
  wayland_compositor = meta_context_get_wayland_compositor (context);
  if (wayland_compositor)
    {
      wayland_text_input =
        meta_wayland_compositor_get_text_input (wayland_compositor);
    }

  COGL_TRACE_BEGIN_SCOPED (MetaDisplayHandleEvent,
                           "Meta::Display::handle_event()");
  COGL_TRACE_DESCRIBE (MetaDisplayHandleEvent,
                       clutter_event_get_name (event));

  has_grab = stage_has_grab (display);

  event_type = clutter_event_type (event);

  if (meta_display_process_captured_input (display, event))
    {
      /* A globally-keybound keycode's passive XIGrabModeSync grab
       * freezes the keyboard device the instant X delivers the matching
       * KEY_PRESS, regardless of which mutter code path ends up
       * consuming it - see the THAW/REPLAY comment further down in this
       * function (08c1b797f). A captured-input grab (e.g. a gesture or
       * eavesdrop grab) can consume a key event before that later logic
       * ever runs, leaving the device frozen just like the original bug
       * this function already fixed on its normal path. THAW here is
       * always correct (never REPLAY): we're returning STOP, keeping
       * this event as our own. XIAllowEvents() is a no-op if this
       * particular event didn't actually engage a SYNC freeze. */
#ifdef HAVE_X11
      if (META_IS_BACKEND_X11 (backend) && IS_KEY_EVENT (event_type))
        {
          meta_backend_x11_allow_events (META_BACKEND_X11 (backend), event,
                                         META_EVENT_MODE_THAW);
        }
#endif
      return CLUTTER_EVENT_STOP;
    }

  if (IS_KEY_EVENT (event_type))
    {
      a11y_grabbed = meta_a11y_manager_notify_clients (a11y_manager, event);
      if (a11y_grabbed)
        {
          /* Same reasoning as the captured-input case above: an AT-SPI
           * keystroke listener (e.g. Orca) can consume a globally-bound
           * key before the THAW/REPLAY logic further down ever runs. */
#ifdef HAVE_X11
          if (META_IS_BACKEND_X11 (backend))
            {
              meta_backend_x11_allow_events (META_BACKEND_X11 (backend), event,
                                             META_EVENT_MODE_THAW);
            }
#endif
          return CLUTTER_EVENT_STOP;
        }
    }
  else if (event_type == CLUTTER_MOTION &&
           !clutter_event_get_device_tool (event))
    {
      meta_a11y_manager_maybe_notify_motion (a11y_manager);
    }

  source_device = clutter_event_get_source_device (event);
  clutter_seat_a11y_update (seat, event);

  if (wayland_text_input &&
      !meta_compositor_get_current_window_drag (compositor) &&
      meta_wayland_text_input_update (wayland_text_input, event))
    return CLUTTER_EVENT_STOP;

  if (event_type == CLUTTER_MOTION &&
      !clutter_event_get_device_tool (event))
    meta_display_handle_sticky_mouse_focus_event (display, event);

  if (wayland_compositor)
    meta_wayland_compositor_update (wayland_compositor, event);

  if (event_type == CLUTTER_PAD_BUTTON_PRESS ||
      event_type == CLUTTER_PAD_BUTTON_RELEASE ||
      event_type == CLUTTER_PAD_RING ||
      event_type == CLUTTER_PAD_STRIP ||
      event_type == CLUTTER_PAD_DIAL)
    {
      gboolean handle_pad_event;
      gboolean is_mode_switch = FALSE;

      if (event_type == CLUTTER_PAD_BUTTON_PRESS ||
          event_type == CLUTTER_PAD_BUTTON_RELEASE)
        {
          ClutterInputDevice *pad;
          uint32_t button;

          pad = clutter_event_get_source_device (event);
          button = clutter_event_get_button (event);

          is_mode_switch =
            clutter_input_device_get_mode_switch_button_group (pad, button) >= 0;
        }

      handle_pad_event = !display->current_pad_osd || is_mode_switch;
      mapper = META_TABLET_ACTION_MAPPER (display->pad_action_mapper);

      if (handle_pad_event &&
          meta_tablet_action_mapper_handle_event (mapper, event))
        return CLUTTER_EVENT_STOP;
    }
  else if (event_type == CLUTTER_BUTTON_PRESS ||
           event_type == CLUTTER_BUTTON_RELEASE)
    {
      mapper = META_TABLET_ACTION_MAPPER (display->tool_action_mapper);
      if ((!!(clutter_input_device_get_capabilities (source_device) &
              CLUTTER_INPUT_CAPABILITY_TABLET_TOOL) &&
           meta_tablet_action_mapper_handle_event (mapper, event)) ||
          clutter_event_get_button (event) == 0)
        return CLUTTER_EVENT_STOP;
    }

  if (event_type != CLUTTER_DEVICE_ADDED &&
      event_type != CLUTTER_DEVICE_REMOVED)
    {
      handle_idletime_for_event (display, event);
    }
  else
    {
      mapper = META_TABLET_ACTION_MAPPER (display->pad_action_mapper);
      meta_tablet_action_mapper_handle_event (mapper, event);
      mapper = META_TABLET_ACTION_MAPPER (display->tool_action_mapper);
      meta_tablet_action_mapper_handle_event (mapper, event);
    }

  window = get_window_for_event (display, event, event_actor);

  if (window && !window->override_redirect &&
      (event_type == CLUTTER_KEY_PRESS ||
       event_type == CLUTTER_BUTTON_PRESS ||
       event_type == CLUTTER_TOUCH_BEGIN))
    {
      if (META_CURRENT_TIME == display->current_time)
        {
          /* We can't use missing (i.e. invalid) timestamps to set user time,
           * nor do we want to use them to sanity check other timestamps.
           * See bug 313490 for more details.
           */
          meta_topic (META_DEBUG_X11,
                      "Event has no timestamp! You may be using a program "
                      "injecting events with invalid timestamps.");
        }
      else
        {
          meta_window_set_user_time (window, display->current_time);
          meta_display_sanity_check_timestamps (display, display->current_time);
        }
    }

  /* For key events, it's important to enforce single-handling, or
   * we can get into a confused state. So if a keybinding is
   * handled (because it's one of our hot-keys, or because we are
   * in a keyboard-grabbed mode like moving a window, we don't
   * want to pass the key event to the compositor or Wayland at all.
   */
  if (!meta_compositor_get_current_window_drag (compositor))
    {
      gboolean keybinding_handled =
        meta_keybindings_process_event (display, window, event);

      /* Every keycode bound to a global keybinding (see
       * meta_compositor_x11_change_keygrab()) is passively grabbed with
       * XIGrabModeSync, which freezes the virtual keyboard device the
       * instant a matching key is pressed - X won't deliver any further
       * key events for that device, bound or not, until something calls
       * XIAllowEvents(). Nothing ever did on this path, so the very
       * first global shortcut in a session (the bare overlay key, Print,
       * Super+Left tiling, ...) froze the keyboard for good. Thaw it
       * here - REPLAY sends the event on to the client when we didn't
       * handle it (mirrors the analogous fix for the passive button
       * grab in meta_window_handle_ungrabbed_event()), THAW just
       * unfreezes when we did. */
#ifdef HAVE_X11
      if (META_IS_BACKEND_X11 (backend) && IS_KEY_EVENT (event_type))
        {
          MetaEventMode event_mode;

          event_mode = keybinding_handled ?
            META_EVENT_MODE_THAW : META_EVENT_MODE_REPLAY;

          /* A PROPAGATE here for a KEY_PRESS matching the overlay-key or
           * locate-pointer-key's own keycode isn't "not ours, let the
           * client see it" - it's process_special_modifier_key() (see
           * keybindings.c) tentatively arming its "waiting for a lone
           * release" flag, deliberately deferring the real decision
           * until that release arrives (or a different key's press
           * proves this wasn't a lone tap - see that function's own
           * comment). REPLAY-ing this press anyway - which only matters
           * when nothing is focused, since a focused client's own
           * per-window grab otherwise gives the sequence a second
           * chance - loses the device's claim on the matching
           * KEY_RELEASE that's still to come: it never reaches this
           * handler at all, so the tap is silently swallowed and the
           * flag is left stuck armed. Confirmed live: with no window
           * focused, a bare Super tap consistently did nothing at all;
           * a second tap then worked, because process_special_modifier_key()
           * took its "repeat press, already armed" branch instead - a
           * STOP, which THAWs correctly. THAW here instead: we're
           * keeping this event as our own regardless of how the
           * sequence resolves, so there's nothing to replay to anyone. */
          if (event_mode == META_EVENT_MODE_REPLAY &&
              event_type == CLUTTER_KEY_PRESS)
            {
              MetaKeyBindingManager *keys = &display->key_binding_manager;
              uint32_t keycode = clutter_event_get_key_code (event);
              MetaResolvedKeyCombo *combos[] = {
                &keys->overlay_resolved_key_combo,
                &keys->locate_pointer_resolved_key_combo,
              };
              int i, j;

              for (i = 0; i < (int) G_N_ELEMENTS (combos); i++)
                {
                  for (j = 0; j < combos[i]->len; j++)
                    {
                      if (combos[i]->keycodes[j] == keycode)
                        {
                          event_mode = META_EVENT_MODE_THAW;
                          break;
                        }
                    }
                }
            }

          meta_backend_x11_allow_events (META_BACKEND_X11 (backend), event,
                                         event_mode);
        }
#endif

      if (keybinding_handled)
        return CLUTTER_EVENT_STOP;
    }

  /* Do not pass keyboard events to Wayland if key focus is not on the
   * stage in normal mode (e.g. during keynav in the panel)
   */
  if (!has_grab)
    {
      if (IS_KEY_EVENT (event_type) && !stage_has_key_focus (display))
        return CLUTTER_EVENT_PROPAGATE;
    }

  if (event_type == CLUTTER_SCROLL &&
      meta_prefs_get_mouse_button_mods () > 0)
    {
      ClutterModifierType grab_mods;

      grab_mods = meta_display_get_compositor_modifiers (display);
      if ((clutter_event_get_state (event) & grab_mods) == grab_mods)
        return CLUTTER_EVENT_PROPAGATE;
    }

  if (display->current_pad_osd)
    return CLUTTER_EVENT_PROPAGATE;

  if (stage_has_grab (display))
    return CLUTTER_EVENT_PROPAGATE;

  if (window)
    {
      if (meta_window_handle_ungrabbed_event (window, event))
        return CLUTTER_EVENT_STOP;

      /* If the focus window has an active close dialog let clutter
       * events go through, so fancy clutter dialogs can get to handle
       * all events.
       */
      if (window->close_dialog &&
          meta_close_dialog_is_visible (window->close_dialog))
        return CLUTTER_EVENT_PROPAGATE;
    }

  time_ms = clutter_event_get_time (event);
  if (window && event_type == CLUTTER_MOTION &&
      time_ms != CLUTTER_CURRENT_TIME)
    meta_window_check_alive_on_event (window, time_ms);

  if (wayland_compositor &&
      meta_wayland_compositor_handle_event (wayland_compositor, event))
    return CLUTTER_EVENT_STOP;

  return CLUTTER_EVENT_PROPAGATE;
}

static gboolean
event_callback (const ClutterEvent *event,
                ClutterActor       *event_actor,
                gpointer            data)
{
  MetaDisplay *display = data;
  gboolean retval;

  display->current_time = clutter_event_get_time (event);
  retval = meta_display_handle_event (display, event, event_actor);
  display->current_time = META_CURRENT_TIME;

  return retval;
}

void
meta_display_init_events (MetaDisplay *display)
{
  display->clutter_event_filter = clutter_event_add_filter (NULL,
                                                            event_callback,
                                                            NULL,
                                                            display);
}

void
meta_display_free_events (MetaDisplay *display)
{
  clutter_event_remove_filter (display->clutter_event_filter);
  display->clutter_event_filter = 0;
}
