/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * focus-caret-tracker.c: Reports the focus and text caret changes of the
 * accessible applications, through AT-SPI. Port of js/ui/focusCaretTracker.js.
 *
 * Copyright 2012 Inclusive Design Research Centre, OCAD University.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author:
 *   Joseph Scheuhammer <clown@alum.mit.edu>
 * Contributor:
 *   Magdalen Berns <m.berns@sms.ed.ac.uk>
 */

#include "config.h"

#include <atspi/atspi.h>

#include "ui/focus-caret-tracker.h"

#define CARET_MOVED "object:text-caret-moved"
#define STATE_CHANGED "object:state-changed"

#define ATSPI_TIMEOUT 250

enum
{
  FOCUS_CHANGED,
  CARET_MOVED_SIGNAL,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

struct _ShellFocusCaretTracker
{
  GObject parent;

  AtspiEventListener *atspi_listener;

  gboolean atspi_inited;
  gboolean focus_listener_registered;
  gboolean caret_listener_registered;
};

G_DEFINE_FINAL_TYPE (ShellFocusCaretTracker, shell_focus_caret_tracker,
                     G_TYPE_OBJECT)

/* The AT-SPI listener keeps its user data until it is finalized, which can
 * happen after the tracker is gone: it gets a weak reference. */
static void
listener_data_free (gpointer data)
{
  GWeakRef *ref = data;

  g_weak_ref_clear (ref);
  g_free (ref);
}

static void
on_atspi_event (AtspiEvent *event,
                gpointer    user_data)
{
  GWeakRef *ref = user_data;
  g_autoptr (ShellFocusCaretTracker) self = g_weak_ref_get (ref);

  if (self == NULL || event->type == NULL)
    return;

  if (g_str_has_prefix (event->type, STATE_CHANGED))
    g_signal_emit (self, signals[FOCUS_CHANGED], 0, event);
  else if (g_strcmp0 (event->type, CARET_MOVED) == 0)
    g_signal_emit (self, signals[CARET_MOVED_SIGNAL], 0, event);
}

static gboolean
init_atspi (ShellFocusCaretTracker *self)
{
  if (!self->atspi_inited)
    {
      /* atspi_init () returns 0 on success and 1 when AT-SPI was already
       * initialized, which is the case for every tracker but the first one. */
      if (atspi_init () <= 1)
        {
          atspi_set_timeout (ATSPI_TIMEOUT, ATSPI_TIMEOUT);
          self->atspi_inited = TRUE;
        }
    }

  return self->atspi_inited;
}

static gboolean
register_event (ShellFocusCaretTracker *self,
                const char             *event_type)
{
  g_autoptr (GError) error = NULL;

  if (atspi_event_listener_register (self->atspi_listener, event_type, &error))
    return TRUE;

  g_warning ("%s: Failed to register for '%s': %s", G_STRFUNC, event_type,
             error != NULL ? error->message : "unknown error");
  return FALSE;
}

static void
deregister_event (ShellFocusCaretTracker *self,
                  const char             *event_type)
{
  g_autoptr (GError) error = NULL;

  if (!atspi_event_listener_deregister (self->atspi_listener, event_type, &error))
    {
      g_warning ("%s: Failed to deregister for '%s': %s", G_STRFUNC,
                 event_type, error != NULL ? error->message : "unknown error");
    }
}

/**
 * shell_focus_caret_tracker_register_focus_listener:
 * @self: a #ShellFocusCaretTracker
 *
 * Starts emitting #ShellFocusCaretTracker::focus-changed, initializing
 * AT-SPI if that was not done yet.
 */
void
shell_focus_caret_tracker_register_focus_listener (ShellFocusCaretTracker *self)
{
  g_return_if_fail (SHELL_IS_FOCUS_CARET_TRACKER (self));

  if (!init_atspi (self) || self->focus_listener_registered)
    return;

  if (!register_event (self, STATE_CHANGED ":focused") ||
      !register_event (self, STATE_CHANGED ":selected"))
    return;

  self->focus_listener_registered = TRUE;
}

/**
 * shell_focus_caret_tracker_register_caret_listener:
 * @self: a #ShellFocusCaretTracker
 *
 * Starts emitting #ShellFocusCaretTracker::caret-moved, initializing AT-SPI
 * if that was not done yet.
 */
void
shell_focus_caret_tracker_register_caret_listener (ShellFocusCaretTracker *self)
{
  g_return_if_fail (SHELL_IS_FOCUS_CARET_TRACKER (self));

  if (!init_atspi (self) || self->caret_listener_registered)
    return;

  if (!register_event (self, CARET_MOVED))
    return;

  self->caret_listener_registered = TRUE;
}

/**
 * shell_focus_caret_tracker_deregister_focus_listener:
 * @self: a #ShellFocusCaretTracker
 *
 * Stops emitting #ShellFocusCaretTracker::focus-changed.
 */
void
shell_focus_caret_tracker_deregister_focus_listener (ShellFocusCaretTracker *self)
{
  g_return_if_fail (SHELL_IS_FOCUS_CARET_TRACKER (self));

  if (!self->focus_listener_registered)
    return;

  deregister_event (self, STATE_CHANGED ":focused");
  deregister_event (self, STATE_CHANGED ":selected");
  self->focus_listener_registered = FALSE;
}

/**
 * shell_focus_caret_tracker_deregister_caret_listener:
 * @self: a #ShellFocusCaretTracker
 *
 * Stops emitting #ShellFocusCaretTracker::caret-moved.
 */
void
shell_focus_caret_tracker_deregister_caret_listener (ShellFocusCaretTracker *self)
{
  g_return_if_fail (SHELL_IS_FOCUS_CARET_TRACKER (self));

  if (!self->caret_listener_registered)
    return;

  deregister_event (self, CARET_MOVED);
  self->caret_listener_registered = FALSE;
}

static void
shell_focus_caret_tracker_dispose (GObject *object)
{
  ShellFocusCaretTracker *self = SHELL_FOCUS_CARET_TRACKER (object);

  if (self->atspi_listener != NULL)
    {
      /* quietly, this runs on teardown paths */
      if (self->focus_listener_registered)
        {
          atspi_event_listener_deregister (self->atspi_listener,
                                           STATE_CHANGED ":focused", NULL);
          atspi_event_listener_deregister (self->atspi_listener,
                                           STATE_CHANGED ":selected", NULL);
        }
      if (self->caret_listener_registered)
        atspi_event_listener_deregister (self->atspi_listener, CARET_MOVED, NULL);

      self->focus_listener_registered = FALSE;
      self->caret_listener_registered = FALSE;
      g_clear_object (&self->atspi_listener);
    }

  G_OBJECT_CLASS (shell_focus_caret_tracker_parent_class)->dispose (object);
}

static void
shell_focus_caret_tracker_class_init (ShellFocusCaretTrackerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_focus_caret_tracker_dispose;

  /**
   * ShellFocusCaretTracker::focus-changed:
   * @self: the #ShellFocusCaretTracker
   * @event: (type Atspi.Event): the `object:state-changed` event, for the
   *   `focused` and `selected` states
   *
   * Emitted when the focus or the selection changes in an accessible
   * application, while the focus listener is registered.
   */
  signals[FOCUS_CHANGED] =
    g_signal_new ("focus-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  ATSPI_TYPE_EVENT);

  /**
   * ShellFocusCaretTracker::caret-moved:
   * @self: the #ShellFocusCaretTracker
   * @event: (type Atspi.Event): the `object:text-caret-moved` event
   *
   * Emitted when the text caret moves in an accessible application, while
   * the caret listener is registered.
   */
  signals[CARET_MOVED_SIGNAL] =
    g_signal_new ("caret-moved",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  ATSPI_TYPE_EVENT);
}

static void
shell_focus_caret_tracker_init (ShellFocusCaretTracker *self)
{
  GWeakRef *ref = g_new0 (GWeakRef, 1);

  g_weak_ref_init (ref, self);
  self->atspi_listener = atspi_event_listener_new (on_atspi_event, ref,
                                                   listener_data_free);
}

/**
 * shell_focus_caret_tracker_new:
 *
 * Returns: (transfer full): a new #ShellFocusCaretTracker
 */
ShellFocusCaretTracker *
shell_focus_caret_tracker_new (void)
{
  return g_object_new (SHELL_TYPE_FOCUS_CARET_TRACKER, NULL);
}
