/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * status-private.h: Helpers shared by the C files of ui/status that are
 * ports of the modules of js/ui/status (the quick settings indicators).
 */

#pragma once

#include <clutter/clutter.h>
#include <gio/gio.h>

#include "ui/main.h"
#include "ui/session-mode.h"

G_BEGIN_DECLS

/*
 * new Gio.Settings ({schema_id}) throws for a schema that is not installed,
 * where g_settings_new () would abort. The indicators of the status area are
 * created in every session, so a missing schema (those of
 * gnome-settings-daemon are the likely ones) leaves the setting out
 * quietly: NULL is returned and a message is logged.
 */
static inline GSettings *
shell_status_settings_new (const char *schema_id)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = NULL;

  if (source != NULL)
    schema = g_settings_schema_source_lookup (source, schema_id, TRUE);

  if (schema == NULL)
    {
      g_message ("GSettings schema %s is not installed", schema_id);
      return NULL;
    }

  return g_settings_new_full (schema, NULL, NULL);
}

/* actor.visible = visible, which also shows and hides the actor */
static inline void
shell_status_set_visible (gpointer actor,
                          gboolean visible)
{
  if (visible)
    clutter_actor_show (CLUTTER_ACTOR (actor));
  else
    clutter_actor_hide (CLUTTER_ACTOR (actor));
}

/* Main.sessionMode, NULL until main.js has set it */
static inline ShellSessionMode *
shell_status_get_session_mode (void)
{
  return shell_main_get_session_mode (shell_main_get_default ());
}

/*
 * Whether a D-Bus proxy creation error only says that the service is not
 * there (not installed, not activatable, or not answering in time). That is
 * the normal state of a session without the daemon, so it is not logged as
 * anything louder than a debug message.
 */
static inline gboolean
shell_status_is_service_missing_error (const GError *error)
{
  return g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
         g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
         g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY) ||
         g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_TIMED_OUT) ||
         g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_TIMEOUT) ||
         g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
         g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

G_END_DECLS
