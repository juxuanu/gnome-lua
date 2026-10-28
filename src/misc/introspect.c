/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * introspect.c: The org.gnome.Shell.Introspect D-Bus service, which tells the
 * desktop portal backends about the running applications and windows.
 * Port of js/misc/introspect.js.
 *
 * The interface is exported at /org/gnome/Shell/Introspect on the session
 * bus, with a gdbus-codegen skeleton (#ShellIntrospect). The methods are
 * only available to the well-known names of APP_ALLOWLIST (what the JS
 * DBusSenderChecker of js/misc/util.js did) unless the shell runs in unsafe
 * mode; calls that arrive before the owners of all those names are known wait
 * for them.
 *
 * Differences with the JS version:
 *
 *  - the sender check is private to this file, js/misc/util.js still has its
 *    own DBusSenderChecker for the other services;
 *  - GetWindows lists the windows after the sender check has passed, the JS
 *    took the focus window and the list of running applications before it
 *    (which only differs for a call that had to wait for the check);
 *  - after ShellGlobal::shutdown the service stops following the shell, and
 *    the calls that still wait for the sender check fail.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <meta/display.h>
#include <meta/meta-backend.h>
#include <meta/meta-context.h>
#include <meta/meta-monitor-manager.h>
#include <meta/window.h>

#include "misc/introspect.h"
#include "shell-app.h"
#include "shell-app-system.h"
#include "shell-global.h"
#include "shell-window-tracker.h"
#include "st.h"

#include "introspect-dbus.h"

#define INTROSPECT_BUS_NAME "org.gnome.Shell.Introspect"
#define INTROSPECT_OBJECT_PATH "/org/gnome/Shell/Introspect"

#define INTROSPECT_DBUS_API_VERSION 3

#define SEAT_NAME "seat0"

static const char * const APP_ALLOWLIST[] = {
  "org.freedesktop.impl.portal.desktop.gtk",
  "org.freedesktop.impl.portal.desktop.gnome",
  NULL
};

typedef enum
{
  METHOD_GET_RUNNING_APPLICATIONS,
  METHOD_GET_WINDOWS,
} Method;

typedef struct
{
  GDBusMethodInvocation *invocation;
  Method method;
} PendingCall;

struct _ShellIntrospectService
{
  GObject parent;

  ShellIntrospect *skeleton;
  guint name_id;

  GVariant *running_applications;  /* a{sa{sv}} */
  gboolean running_applications_dirty;
  char *active_application;
  gboolean active_application_dirty;

  ShellAppSystem *app_system;
  ShellWindowTracker *tracker;
  StSettings *st_settings;
  MetaMonitorManager *monitor_manager;

  int screen_width;
  int screen_height;
  gboolean screen_size_set;

  /* The sender check */
  GHashTable *allowlist_map;        /* well-known name -> owner */
  GHashTable *uninitialized_names;  /* set of well-known names */
  GArray *watch_ids;
  GQueue pending_calls;             /* of PendingCall */

  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellIntrospectService, shell_introspect_service, G_TYPE_OBJECT)

static void
pending_call_free (PendingCall *call)
{
  g_object_unref (call->invocation);
  g_free (call);
}

static gboolean
is_standalone_app (ShellApp *app)
{
  g_autoptr (GSList) windows = shell_app_get_windows (app);
  GSList *l;

  for (l = windows; l; l = l->next)
    {
      if (meta_window_get_transient_for (l->data) == NULL)
        return TRUE;
    }

  return FALSE;
}

static const char *
get_sandboxed_app_id (ShellApp *app)
{
  g_autoptr (GSList) windows = shell_app_get_windows (app);
  GSList *l;

  for (l = windows; l; l = l->next)
    {
      const char *id = meta_window_get_sandboxed_app_id (l->data);

      if (id != NULL)
        return id;
    }

  return NULL;
}

static void
sync_running_applications (ShellIntrospectService *self)
{
  g_autoptr (GSList) apps = shell_app_system_get_running (self->app_system);
  ShellApp *focused_app = shell_window_tracker_get_focus_app (self->tracker);
  GVariantBuilder builder;
  g_autofree char *new_active_application = NULL;
  GSList *l;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sa{sv}}"));

  for (l = apps; l; l = l->next)
    {
      ShellApp *app = l->data;
      GVariantBuilder app_info;
      const char *sandboxed_app_id;

      if (!is_standalone_app (app))
        continue;

      g_variant_builder_init (&app_info, G_VARIANT_TYPE ("a{sv}"));

      if (focused_app == app)
        {
          const char * const seats[] = { SEAT_NAME, NULL };

          g_variant_builder_add (&app_info, "{sv}", "active-on-seats",
                                 g_variant_new_strv (seats, -1));
          g_free (new_active_application);
          new_active_application = g_strdup (shell_app_get_id (app));
        }

      sandboxed_app_id = get_sandboxed_app_id (app);
      if (sandboxed_app_id != NULL)
        g_variant_builder_add (&app_info, "{sv}", "sandboxed-app-id",
                               g_variant_new_string (sandboxed_app_id));

      g_variant_builder_add (&builder, "{s@a{sv}}", shell_app_get_id (app),
                             g_variant_builder_end (&app_info));
    }

  if (self->running_applications_dirty ||
      (self->active_application_dirty &&
       g_strcmp0 (self->active_application, new_active_application) != 0))
    {
      g_clear_pointer (&self->running_applications, g_variant_unref);
      self->running_applications = g_variant_ref_sink (g_variant_builder_end (&builder));

      g_free (self->active_application);
      self->active_application = g_steal_pointer (&new_active_application);

      shell_introspect_emit_running_applications_changed (self->skeleton);
    }
  else
    {
      g_variant_builder_clear (&builder);
    }

  self->running_applications_dirty = FALSE;
  self->active_application_dirty = FALSE;
}

static gboolean
is_eligible_window (MetaWindow *window)
{
  MetaWindowType type;

  if (meta_window_is_override_redirect (window))
    return FALSE;

  type = meta_window_get_window_type (window);
  return type == META_WINDOW_NORMAL ||
         type == META_WINDOW_DIALOG ||
         type == META_WINDOW_MODAL_DIALOG ||
         type == META_WINDOW_UTILITY;
}

static void
reply_get_running_applications (ShellIntrospectService *self,
                                GDBusMethodInvocation  *invocation)
{
  shell_introspect_complete_get_running_applications (self->skeleton, invocation,
                                                      self->running_applications);
}

static void
reply_get_windows (ShellIntrospectService *self,
                   GDBusMethodInvocation  *invocation)
{
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  MetaWindow *focus_window = meta_display_get_focus_window (display);
  g_autoptr (GSList) apps = shell_app_system_get_running (self->app_system);
  GVariantBuilder windows_list;
  GSList *l;

  g_variant_builder_init (&windows_list, G_VARIANT_TYPE ("a{ta{sv}}"));

  for (l = apps; l; l = l->next)
    {
      ShellApp *app = l->data;
      g_autoptr (GSList) windows = shell_app_get_windows (app);
      GSList *w;

      for (w = windows; w; w = w->next)
        {
          MetaWindow *window = w->data;
          GVariantBuilder props;
          MtkRectangle frame_rect;
          const char *title, *wm_class, *sandboxed_app_id;

          if (!is_eligible_window (window))
            continue;

          meta_window_get_frame_rect (window, &frame_rect);
          title = meta_window_get_title (window);
          wm_class = meta_window_get_wm_class (window);
          sandboxed_app_id = meta_window_get_sandboxed_app_id (window);

          g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
          g_variant_builder_add (&props, "{sv}", "app-id",
                                 g_variant_new_string (shell_app_get_id (app)));
          g_variant_builder_add (&props, "{sv}", "client-type",
                                 g_variant_new_uint32 (meta_window_get_client_type (window)));
          g_variant_builder_add (&props, "{sv}", "is-hidden",
                                 g_variant_new_boolean (meta_window_is_hidden (window)));
          g_variant_builder_add (&props, "{sv}", "has-focus",
                                 g_variant_new_boolean (window == focus_window));
          g_variant_builder_add (&props, "{sv}", "width",
                                 g_variant_new_uint32 (frame_rect.width));
          g_variant_builder_add (&props, "{sv}", "height",
                                 g_variant_new_uint32 (frame_rect.height));

          /* These properties may not be available for all windows: */
          if (title != NULL)
            g_variant_builder_add (&props, "{sv}", "title",
                                   g_variant_new_string (title));

          if (wm_class != NULL)
            g_variant_builder_add (&props, "{sv}", "wm-class",
                                   g_variant_new_string (wm_class));

          if (sandboxed_app_id != NULL)
            g_variant_builder_add (&props, "{sv}", "sandboxed-app-id",
                                   g_variant_new_string (sandboxed_app_id));

          g_variant_builder_add (&windows_list, "{t@a{sv}}",
                                 meta_window_get_id (window),
                                 g_variant_builder_end (&props));
        }
    }

  shell_introspect_complete_get_windows (self->skeleton, invocation,
                                         g_variant_builder_end (&windows_list));
}

static void
run_call (ShellIntrospectService *self,
          GDBusMethodInvocation  *invocation,
          Method                  method)
{
  switch (method)
    {
    case METHOD_GET_RUNNING_APPLICATIONS:
      reply_get_running_applications (self, invocation);
      break;

    case METHOD_GET_WINDOWS:
      reply_get_windows (self, invocation);
      break;

    default:
      g_assert_not_reached ();
    }
}

/* The sender check: DBusSenderChecker.checkInvocation () */

static gboolean
is_sender_allowed (ShellIntrospectService *self,
                   const char             *sender)
{
  GHashTableIter iter;
  gpointer owner;

  g_hash_table_iter_init (&iter, self->allowlist_map);
  while (g_hash_table_iter_next (&iter, NULL, &owner))
    {
      if (g_strcmp0 (owner, sender) == 0)
        return TRUE;
    }

  return FALSE;
}

static gboolean
is_unsafe_mode (void)
{
  gboolean unsafe_mode = FALSE;

  g_object_get (shell_global_get_context (shell_global_get ()),
                "unsafe-mode", &unsafe_mode, NULL);
  return unsafe_mode;
}

static void
check_and_run_call (ShellIntrospectService *self,
                    GDBusMethodInvocation  *invocation,
                    Method                  method)
{
  if (is_unsafe_mode () ||
      is_sender_allowed (self, g_dbus_method_invocation_get_sender (invocation)))
    {
      run_call (self, invocation, method);
      return;
    }

  g_dbus_method_invocation_return_error (invocation,
                                         G_DBUS_ERROR,
                                         G_DBUS_ERROR_ACCESS_DENIED,
                                         "%s is not allowed",
                                         g_dbus_method_invocation_get_method_name (invocation));
}

static void
queue_or_check_call (ShellIntrospectService *self,
                     GDBusMethodInvocation  *invocation,
                     Method                  method)
{
  if (is_unsafe_mode ())
    {
      run_call (self, invocation, method);
    }
  else if (self->shutting_down)
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_DBUS_ERROR,
                                                     G_DBUS_ERROR_FAILED,
                                                     "The shell is shutting down");
    }
  else if (g_hash_table_size (self->uninitialized_names) > 0)
    {
      PendingCall *call = g_new0 (PendingCall, 1);

      call->invocation = g_object_ref (invocation);
      call->method = method;
      g_queue_push_tail (&self->pending_calls, call);
    }
  else
    {
      check_and_run_call (self, invocation, method);
    }
}

static void
flush_pending_calls (ShellIntrospectService *self)
{
  PendingCall *call;

  while ((call = g_queue_pop_head (&self->pending_calls)) != NULL)
    {
      if (self->shutting_down)
        g_dbus_method_invocation_return_error_literal (call->invocation,
                                                       G_DBUS_ERROR,
                                                       G_DBUS_ERROR_FAILED,
                                                       "The shell is shutting down");
      else
        check_and_run_call (self, call->invocation, call->method);

      pending_call_free (call);
    }
}

static void
check_and_resolve_initialized (ShellIntrospectService *self,
                               const char             *name)
{
  if (g_hash_table_remove (self->uninitialized_names, name) &&
      g_hash_table_size (self->uninitialized_names) == 0)
    flush_pending_calls (self);
}

static void
on_allowlist_name_appeared (GDBusConnection *connection,
                            const char      *name,
                            const char      *name_owner,
                            gpointer         user_data)
{
  ShellIntrospectService *self = user_data;

  g_hash_table_replace (self->allowlist_map, g_strdup (name), g_strdup (name_owner));
  check_and_resolve_initialized (self, name);
}

static void
on_allowlist_name_vanished (GDBusConnection *connection,
                            const char      *name,
                            gpointer         user_data)
{
  ShellIntrospectService *self = user_data;

  g_hash_table_remove (self->allowlist_map, name);
  check_and_resolve_initialized (self, name);
}

static gboolean
on_handle_get_running_applications (ShellIntrospect        *skeleton,
                                    GDBusMethodInvocation  *invocation,
                                    ShellIntrospectService *self)
{
  queue_or_check_call (self, invocation, METHOD_GET_RUNNING_APPLICATIONS);
  return TRUE;
}

static gboolean
on_handle_get_windows (ShellIntrospect        *skeleton,
                       GDBusMethodInvocation  *invocation,
                       ShellIntrospectService *self)
{
  queue_or_check_call (self, invocation, METHOD_GET_WINDOWS);
  return TRUE;
}

static void
sync_animations_enabled (ShellIntrospectService *self)
{
  shell_introspect_set_animations_enabled (self->skeleton,
                                           st_settings_get_enable_animations (self->st_settings));
}

static void
sync_screen_size (ShellIntrospectService *self)
{
  ShellGlobal *global = shell_global_get ();
  int width = shell_global_get_screen_width (global);
  int height = shell_global_get_screen_height (global);

  if (self->screen_size_set &&
      self->screen_width == width && self->screen_height == height)
    return;

  self->screen_size_set = TRUE;
  self->screen_width = width;
  self->screen_height = height;
  shell_introspect_set_screen_size (self->skeleton,
                                    g_variant_new ("(ii)", width, height));
}

static void
on_app_state_changed (ShellAppSystem         *app_system,
                      ShellApp               *app,
                      ShellIntrospectService *self)
{
  self->running_applications_dirty = TRUE;
  sync_running_applications (self);
}

static void
on_focus_app_notify (ShellWindowTracker    *tracker,
                     GParamSpec            *pspec,
                     ShellIntrospectService *self)
{
  self->active_application_dirty = TRUE;
  sync_running_applications (self);
}

static void
on_tracked_windows_changed (ShellWindowTracker    *tracker,
                            ShellIntrospectService *self)
{
  shell_introspect_emit_windows_changed (self->skeleton);
}

static void
disconnect_from_shell (ShellIntrospectService *self)
{
  if (self->app_system != NULL)
    g_signal_handlers_disconnect_by_data (self->app_system, self);
  if (self->tracker != NULL)
    g_signal_handlers_disconnect_by_data (self->tracker, self);
  if (self->st_settings != NULL)
    g_signal_handlers_disconnect_by_data (self->st_settings, self);
  if (self->monitor_manager != NULL)
    g_signal_handlers_disconnect_by_data (self->monitor_manager, self);
}

static void
on_shutdown (ShellGlobal           *global,
             ShellIntrospectService *self)
{
  self->shutting_down = TRUE;

  disconnect_from_shell (self);
  flush_pending_calls (self);
}

static void
export_and_own_name (ShellIntrospectService *self)
{
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GError) error = NULL;

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (connection == NULL)
    {
      g_warning ("%s: %s", G_STRFUNC, error->message);
      return;
    }

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                         connection, INTROSPECT_OBJECT_PATH,
                                         &error))
    {
      g_warning ("%s: %s", G_STRFUNC, error->message);
      return;
    }

  self->name_id = g_bus_own_name_on_connection (connection, INTROSPECT_BUS_NAME,
                                                G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                                NULL, NULL, NULL, NULL);
}

static void
shell_introspect_service_constructed (GObject *object)
{
  ShellIntrospectService *self = SHELL_INTROSPECT_SERVICE (object);
  ShellGlobal *global = shell_global_get ();
  guint i;

  G_OBJECT_CLASS (shell_introspect_service_parent_class)->constructed (object);

  self->skeleton = shell_introspect_skeleton_new ();
  shell_introspect_set_version (self->skeleton, INTROSPECT_DBUS_API_VERSION);
  shell_introspect_set_animations_enabled (self->skeleton, TRUE);
  g_signal_connect (self->skeleton, "handle-get-running-applications",
                    G_CALLBACK (on_handle_get_running_applications), self);
  g_signal_connect (self->skeleton, "handle-get-windows",
                    G_CALLBACK (on_handle_get_windows), self);

  self->running_applications_dirty = TRUE;
  self->active_application_dirty = TRUE;
  self->running_applications = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("{sa{sv}}"), NULL, 0));

  self->app_system = shell_app_system_get_default ();
  g_signal_connect (self->app_system, "app-state-changed",
                    G_CALLBACK (on_app_state_changed), self);

  self->tracker = shell_window_tracker_get_default ();
  g_signal_connect (self->tracker, "notify::focus-app",
                    G_CALLBACK (on_focus_app_notify), self);
  g_signal_connect (self->tracker, "tracked-windows-changed",
                    G_CALLBACK (on_tracked_windows_changed), self);

  sync_running_applications (self);

  /* The senders that may call the methods; the check waits until the owners
   * of all of the names are known */
  self->allowlist_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->uninitialized_names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->watch_ids = g_array_new (FALSE, FALSE, sizeof (guint));
  for (i = 0; APP_ALLOWLIST[i] != NULL; i++)
    g_hash_table_add (self->uninitialized_names, g_strdup (APP_ALLOWLIST[i]));
  for (i = 0; APP_ALLOWLIST[i] != NULL; i++)
    {
      guint watch_id = g_bus_watch_name (G_BUS_TYPE_SESSION, APP_ALLOWLIST[i],
                                         G_BUS_NAME_WATCHER_FLAGS_NONE,
                                         on_allowlist_name_appeared,
                                         on_allowlist_name_vanished,
                                         self, NULL);

      g_array_append_val (self->watch_ids, watch_id);
    }

  self->st_settings = st_settings_get ();
  g_signal_connect_swapped (self->st_settings, "notify::enable-animations",
                            G_CALLBACK (sync_animations_enabled), self);
  sync_animations_enabled (self);

  self->monitor_manager = meta_backend_get_monitor_manager (shell_global_get_backend (global));
  g_signal_connect_swapped (self->monitor_manager, "monitors-changed",
                            G_CALLBACK (sync_screen_size), self);
  sync_screen_size (self);

  g_signal_connect_object (global, "shutdown", G_CALLBACK (on_shutdown), self, 0);

  export_and_own_name (self);
}

static void
shell_introspect_service_dispose (GObject *object)
{
  ShellIntrospectService *self = SHELL_INTROSPECT_SERVICE (object);
  guint i;

  disconnect_from_shell (self);
  self->app_system = NULL;
  self->tracker = NULL;
  self->st_settings = NULL;
  self->monitor_manager = NULL;

  self->shutting_down = TRUE;
  flush_pending_calls (self);

  if (self->watch_ids != NULL)
    {
      for (i = 0; i < self->watch_ids->len; i++)
        g_bus_unwatch_name (g_array_index (self->watch_ids, guint, i));
      g_clear_pointer (&self->watch_ids, g_array_unref);
    }

  if (self->name_id != 0)
    {
      g_bus_unown_name (self->name_id);
      self->name_id = 0;
    }

  if (self->skeleton != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->skeleton, self);
      if (g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (self->skeleton)) != NULL)
        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->skeleton));
      g_clear_object (&self->skeleton);
    }

  g_clear_pointer (&self->allowlist_map, g_hash_table_unref);
  g_clear_pointer (&self->uninitialized_names, g_hash_table_unref);

  G_OBJECT_CLASS (shell_introspect_service_parent_class)->dispose (object);
}

static void
shell_introspect_service_finalize (GObject *object)
{
  ShellIntrospectService *self = SHELL_INTROSPECT_SERVICE (object);

  g_clear_pointer (&self->running_applications, g_variant_unref);
  g_clear_pointer (&self->active_application, g_free);

  G_OBJECT_CLASS (shell_introspect_service_parent_class)->finalize (object);
}

static void
shell_introspect_service_class_init (ShellIntrospectServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_introspect_service_constructed;
  object_class->dispose = shell_introspect_service_dispose;
  object_class->finalize = shell_introspect_service_finalize;
}

static void
shell_introspect_service_init (ShellIntrospectService *self)
{
  g_queue_init (&self->pending_calls);
}

/**
 * shell_introspect_service_new:
 *
 * Creates the service, exports it on the session bus and owns
 * org.gnome.Shell.Introspect. src/ui/main.c creates the only instance.
 *
 * Returns: (transfer full): a new #ShellIntrospectService
 */
ShellIntrospectService *
shell_introspect_service_new (void)
{
  return g_object_new (SHELL_TYPE_INTROSPECT_SERVICE, NULL);
}
