/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-dbus.c: The D-Bus interfaces the shell exports on the session bus.
 * Port of js/ui/shellDBus.js.
 *
 * Every interface has a gdbus-codegen skeleton (the XML files in data/dbus-interfaces,
 * generated as shell-dbus-generated.h) exported by one service object:
 *
 *  - #ShellDBusService: org.gnome.Shell at /org/gnome/Shell (what the JS
 *    called GnomeShell). The sender check (js/misc/util.js DBusSenderChecker)
 *    is a #ShellDBusSenderChecker, the accelerator grabbing is done here.
 *  - #ShellDBusExtensionsService: org.gnome.Shell.Extensions at
 *    /org/gnome/Shell (GnomeShellExtensions).
 *  - #ShellScreenSaverService: org.gnome.ScreenSaver at /org/gnome/ScreenSaver
 *    and the name org.gnome.Shell.ScreenShield (ScreenSaverDBus).
 *  - #ShellScreenTimeService: org.gnome.Shell.ScreenTime (ScreenTimeDBus).
 *  - #ShellBrightnessService: org.gnome.Shell.Brightness (BrightnessDBus).
 *
 * The overview and the window manager are JS: the JS code that reached them
 * is a signal of the service, connected by src/ui/main.c. The screen shield
 * (src/ui/screen-shield.c) connects to the signals of the ScreenSaver
 * service. The methods of those interfaces
 * that return a value use signals with a return value or, when the answer
 * comes later (Eval), hand the #GDBusMethodInvocation to the handler. The
 * extension service calls the extension manager of extension-system.c
 * directly. The JS also created the Screenshot service in the constructor of
 * GnomeShell, src/ui/main.c does it now.
 *
 * Differences with the JS version:
 *
 *  - the Mode property emits PropertiesChanged when the session mode changes
 *    (the JS getter did not notify);
 *  - org.gnome.Shell.Brightness does not emit PropertiesChanged for
 *    HasBrightnessControl when the service is created;
 *  - setting OverviewActive over D-Bus asks ShellMain to show or hide the
 *    overview, and the property only changes when the overview has; a
 *    skeleton property is written first by the D-Bus call, so it is put back
 *    to its previous value before the request (nothing is emitted for that),
 *    once the emission of notify::overview-active is over;
 *  - an accelerator signal whose activation token cannot be created is sent
 *    with an empty token (the JS threw and no signal was sent);
 *  - ReloadExtension () reloads the extension (it used to fail as not
 *    supported), and InstallRemoteExtension () fails with the errors of
 *    #ShellExtensionError;
 *  - Eval without a handler (or with the unsafe mode off) returns (false, "");
 *  - the sender checks fail after ShellGlobal::shutdown, and the services stop
 *    following the shell then.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <math.h>

#include <meta/display.h>
#include <meta/meta-backend.h>
#include <meta/meta-context.h>
#include <meta/meta-monitor-manager.h>
#include <meta/util.h>

#include "misc/dbus-utils.h"
#include "shell-app-system.h"
#include "shell-global.h"
#include "ui/extension-system.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/shell-dbus.h"

#include "shell-dbus-generated.h"

#define SHELL_OBJECT_PATH "/org/gnome/Shell"
#define SCREEN_SAVER_OBJECT_PATH "/org/gnome/ScreenSaver"
#define SCREEN_SAVER_BUS_NAME "org.gnome.Shell.ScreenShield"
#define SCREEN_TIME_OBJECT_PATH "/org/gnome/Shell/ScreenTime"
#define BRIGHTNESS_OBJECT_PATH "/org/gnome/Shell/Brightness"
#define BRIGHTNESS_BUS_NAME "org.gnome.Shell.Brightness"

#define DISABLE_USER_EXTENSIONS_KEY "disable-user-extensions"

static const char * const SENDER_ALLOWLIST[] = {
  "org.gnome.Settings",
  "org.gnome.SettingsDaemon.MediaKeys",
  "org.freedesktop.impl.portal.desktop.gnome",
  NULL
};

/* ------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------ */

/*
 * Exports @skeleton on the session bus; returns the connection, or %NULL
 * (with a warning) if the bus or the export failed.
 */
static GDBusConnection *
export_skeleton (GDBusInterfaceSkeleton *skeleton,
                 const char             *object_path)
{
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GError) error = NULL;

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (connection == NULL)
    {
      g_warning ("%s: %s", G_STRFUNC, error->message);
      return NULL;
    }

  if (!g_dbus_interface_skeleton_export (skeleton, connection, object_path, &error))
    {
      g_warning ("%s: %s", G_STRFUNC, error->message);
      return NULL;
    }

  return g_steal_pointer (&connection);
}

static void
unexport_skeleton (GDBusInterfaceSkeleton *skeleton,
                   gpointer                service)
{
  if (skeleton == NULL)
    return;

  g_signal_handlers_disconnect_by_data (skeleton, service);
  if (g_dbus_interface_skeleton_get_connection (skeleton) != NULL)
    g_dbus_interface_skeleton_unexport (skeleton);
}

static gboolean
is_unsafe_mode (void)
{
  gboolean unsafe_mode = FALSE;

  g_object_get (shell_global_get_context (shell_global_get ()),
                "unsafe-mode", &unsafe_mode, NULL);
  return unsafe_mode;
}

/* A number of a{sv}, whatever its D-Bus type (the JS deepUnpack ()ed them) */
static gboolean
lookup_double (GVariant   *dict,
               const char *key,
               double     *out)
{
  g_autoptr (GVariant) value = g_variant_lookup_value (dict, key, NULL);

  if (value == NULL)
    return FALSE;

  switch (g_variant_classify (value))
    {
    case G_VARIANT_CLASS_DOUBLE:
      *out = g_variant_get_double (value);
      return TRUE;
    case G_VARIANT_CLASS_BYTE:
      *out = g_variant_get_byte (value);
      return TRUE;
    case G_VARIANT_CLASS_INT16:
      *out = g_variant_get_int16 (value);
      return TRUE;
    case G_VARIANT_CLASS_UINT16:
      *out = g_variant_get_uint16 (value);
      return TRUE;
    case G_VARIANT_CLASS_INT32:
      *out = g_variant_get_int32 (value);
      return TRUE;
    case G_VARIANT_CLASS_UINT32:
      *out = g_variant_get_uint32 (value);
      return TRUE;
    case G_VARIANT_CLASS_INT64:
      *out = g_variant_get_int64 (value);
      return TRUE;
    case G_VARIANT_CLASS_UINT64:
      *out = g_variant_get_uint64 (value);
      return TRUE;
    default:
      return FALSE;
    }
}

/* ------------------------------------------------------------------------
 * ShellDBusService: org.gnome.Shell
 * ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_OSD_WINDOW_MANAGER,
  PROP_OSD_MONITOR_LABELER,
  PROP_OVERVIEW_ACTIVE,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  EVAL_REQUESTED,
  FOCUS_SEARCH_REQUESTED,
  FOCUS_APP_REQUESTED,
  SHOW_APPLICATIONS_REQUESTED,
  ALLOW_KEYBINDING_REQUESTED,
  OVERVIEW_ACTIVE_REQUESTED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

typedef enum
{
  METHOD_FOCUS_SEARCH,
  METHOD_SHOW_OSD,
  METHOD_FOCUS_APP,
  METHOD_SHOW_APPLICATIONS,
  METHOD_GRAB_ACCELERATOR,
  METHOD_GRAB_ACCELERATORS,
  METHOD_UNGRAB_ACCELERATOR,
  METHOD_UNGRAB_ACCELERATORS,
  METHOD_SCREEN_TRANSITION,
  METHOD_SHOW_MONITOR_LABELS,
  METHOD_HIDE_MONITOR_LABELS,
} Method;

struct _ShellDBusService
{
  GObject parent;

  ShellDBusShell *skeleton;
  ShellDBusSenderChecker *sender_checker;

  ShellOsdWindowManager *osd_window_manager;
  ShellOsdMonitorLabeler *osd_monitor_labeler;

  GHashTable *grabbed_accelerators;  /* action (guint) -> sender (char *) */
  GHashTable *grabbers;              /* sender -> bus watch id */

  gboolean overview_active;
  gboolean syncing_overview_active;
  gboolean requested_overview_active;
  guint overview_active_request_id;
  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellDBusService, shell_dbus_service, G_TYPE_OBJECT)

typedef struct
{
  ShellDBusService *self;
  GDBusMethodInvocation *invocation;
  Method method;
} CheckedCall;

static void
checked_call_free (CheckedCall *call)
{
  g_object_unref (call->self);
  g_object_unref (call->invocation);
  g_free (call);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CheckedCall, checked_call_free)

static void
sync_overview_active (ShellDBusService *self)
{
  self->syncing_overview_active = TRUE;
  shell_dbus_shell_set_overview_active (self->skeleton, self->overview_active);
  self->syncing_overview_active = FALSE;
}

/* Accelerators */

static void
emit_accelerator_signal (ShellDBusService *self,
                         const char       *signal_name,
                         guint             action,
                         ClutterInputDevice *device,
                         guint             timestamp)
{
  ShellGlobal *global = shell_global_get ();
  const char *destination;
  g_autoptr (GAppLaunchContext) context = NULL;
  g_autofree char *token = NULL;
  GVariantBuilder params;
  const char *device_node;

  if (self->shutting_down)
    return;

  destination = g_hash_table_lookup (self->grabbed_accelerators,
                                     GUINT_TO_POINTER (action));
  if (destination == NULL)
    return;

  context = shell_global_create_app_launch_context (global, 0, -1);
  token = g_app_launch_context_get_startup_notify_id (context, NULL, NULL);

  g_variant_builder_init (&params, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&params, "{sv}", "timestamp",
                         g_variant_new_uint32 (timestamp));
  g_variant_builder_add (&params, "{sv}", "action-mode",
                         g_variant_new_uint32 (shell_main_get_action_mode (shell_main_get_default ())));
  g_variant_builder_add (&params, "{sv}", "activation-token",
                         g_variant_new_string (token != NULL ? token : ""));

  device_node = device != NULL ? clutter_input_device_get_device_node (device) : NULL;
  if (device_node != NULL)
    g_variant_builder_add (&params, "{sv}", "device-node",
                           g_variant_new_string (device_node));

  shell_dbus_emit_signal_to_destination (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                         destination,
                                         signal_name,
                                         g_variant_new ("(ua{sv})", action, &params));
}

static void
on_accelerator_activated (MetaDisplay        *display,
                          guint               action,
                          ClutterInputDevice *device,
                          guint               timestamp,
                          ShellDBusService   *self)
{
  emit_accelerator_signal (self, "AcceleratorActivated", action, device, timestamp);
}

static void
on_accelerator_deactivated (MetaDisplay        *display,
                            guint               action,
                            ClutterInputDevice *device,
                            guint               timestamp,
                            ShellDBusService   *self)
{
  emit_accelerator_signal (self, "AcceleratorDeactivated", action, device, timestamp);
}

static gboolean
ungrab_accelerator (ShellDBusService *self,
                    guint             action)
{
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  gboolean ungrab_succeeded = meta_display_ungrab_accelerator (display, action);

  if (ungrab_succeeded)
    g_hash_table_remove (self->grabbed_accelerators, GUINT_TO_POINTER (action));

  return ungrab_succeeded;
}

static gboolean
ungrab_accelerator_for_sender (ShellDBusService *self,
                               guint             action,
                               const char       *sender)
{
  const char *grabbed_by = g_hash_table_lookup (self->grabbed_accelerators,
                                                GUINT_TO_POINTER (action));

  if (g_strcmp0 (sender, grabbed_by) != 0)
    return FALSE;

  return ungrab_accelerator (self, action);
}

static void
on_grabber_name_vanished (GDBusConnection *connection,
                          const char      *name,
                          gpointer         user_data)
{
  ShellDBusService *self = user_data;
  g_autoptr (GArray) actions = g_array_new (FALSE, FALSE, sizeof (guint));
  GHashTableIter iter;
  gpointer key, value;
  gpointer watch_id;
  guint i;

  g_hash_table_iter_init (&iter, self->grabbed_accelerators);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (g_strcmp0 (value, name) == 0)
        {
          guint action = GPOINTER_TO_UINT (key);

          g_array_append_val (actions, action);
        }
    }

  for (i = 0; i < actions->len; i++)
    ungrab_accelerator (self, g_array_index (actions, guint, i));

  if (g_hash_table_lookup_extended (self->grabbers, name, NULL, &watch_id))
    {
      g_bus_unwatch_name (GPOINTER_TO_UINT (watch_id));
      g_hash_table_remove (self->grabbers, name);
    }
}

static guint
grab_accelerator_for_sender (ShellDBusService *self,
                             const char       *accelerator,
                             guint             mode_flags,
                             guint             grab_flags,
                             const char       *sender)
{
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  g_autofree char *binding_name = NULL;
  guint binding_action;

  binding_action = meta_display_grab_accelerator (display, accelerator, grab_flags);
  if (binding_action == META_KEYBINDING_ACTION_NONE)
    return META_KEYBINDING_ACTION_NONE;

  binding_name = meta_external_binding_name_for_action (binding_action);
  g_signal_emit (self, signals[ALLOW_KEYBINDING_REQUESTED], 0, binding_name, mode_flags);

  g_hash_table_replace (self->grabbed_accelerators,
                        GUINT_TO_POINTER (binding_action), g_strdup (sender));

  if (!g_hash_table_contains (self->grabbers, sender))
    {
      guint id = g_bus_watch_name (G_BUS_TYPE_SESSION, sender,
                                   G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   NULL, on_grabber_name_vanished,
                                   self, NULL);

      g_hash_table_replace (self->grabbers, g_strdup (sender), GUINT_TO_POINTER (id));
    }

  return binding_action;
}

/* The methods */

static void
run_show_osd (ShellDBusService      *self,
              GDBusMethodInvocation *invocation,
              GVariant              *params)
{
  const char *connector = NULL, *label = NULL, *serialized_icon = NULL;
  g_autoptr (GIcon) icon = NULL;
  double level = NAN, max_level = 1;

  g_variant_lookup (params, "connector", "&s", &connector);
  g_variant_lookup (params, "label", "&s", &label);
  g_variant_lookup (params, "icon", "&s", &serialized_icon);
  lookup_double (params, "level", &level);
  lookup_double (params, "max_level", &max_level);

  if (serialized_icon != NULL && *serialized_icon != '\0')
    {
      g_autoptr (GError) error = NULL;

      icon = g_icon_new_for_string (serialized_icon, &error);
      if (icon == NULL)
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return;
        }
    }

  if (self->osd_window_manager != NULL)
    {
      if (connector != NULL && *connector != '\0')
        {
          MetaBackend *backend = shell_global_get_backend (shell_global_get ());
          MetaMonitorManager *monitor_manager = meta_backend_get_monitor_manager (backend);
          int monitor_index = meta_monitor_manager_get_monitor_for_connector (monitor_manager,
                                                                              connector);

          shell_osd_window_manager_show_one (self->osd_window_manager, monitor_index,
                                             icon, label, level, max_level);
        }
      else
        {
          shell_osd_window_manager_show_all (self->osd_window_manager,
                                             icon, label, level, max_level);
        }
    }

  shell_dbus_shell_complete_show_osd (self->skeleton, invocation);
}

static void
run_method (ShellDBusService      *self,
            GDBusMethodInvocation *invocation,
            Method                 method)
{
  GVariant *parameters = g_dbus_method_invocation_get_parameters (invocation);
  const char *sender = g_dbus_method_invocation_get_sender (invocation);

  switch (method)
    {
    case METHOD_FOCUS_SEARCH:
      g_signal_emit (self, signals[FOCUS_SEARCH_REQUESTED], 0);
      shell_dbus_shell_complete_focus_search (self->skeleton, invocation);
      break;

    case METHOD_SHOW_OSD:
      {
        g_autoptr (GVariant) params = NULL;

        g_variant_get (parameters, "(@a{sv})", &params);
        run_show_osd (self, invocation, params);
        break;
      }

    case METHOD_FOCUS_APP:
      {
        const char *id;

        g_variant_get (parameters, "(&s)", &id);
        if (shell_app_system_lookup_app (shell_app_system_get_default (), id) == NULL)
          {
            g_dbus_method_invocation_return_error (invocation,
                                                   G_DBUS_ERROR,
                                                   G_DBUS_ERROR_FILE_NOT_FOUND,
                                                   "No app with ID %s", id);
            break;
          }

        g_signal_emit (self, signals[FOCUS_APP_REQUESTED], 0, id);
        shell_dbus_shell_complete_focus_app (self->skeleton, invocation);
        break;
      }

    case METHOD_SHOW_APPLICATIONS:
      g_signal_emit (self, signals[SHOW_APPLICATIONS_REQUESTED], 0);
      shell_dbus_shell_complete_show_applications (self->skeleton, invocation);
      break;

    case METHOD_GRAB_ACCELERATOR:
      {
        const char *accelerator;
        guint mode_flags, grab_flags, binding_action;

        g_variant_get (parameters, "(&suu)", &accelerator, &mode_flags, &grab_flags);
        binding_action = grab_accelerator_for_sender (self, accelerator,
                                                      mode_flags, grab_flags, sender);
        shell_dbus_shell_complete_grab_accelerator (self->skeleton, invocation,
                                                    binding_action);
        break;
      }

    case METHOD_GRAB_ACCELERATORS:
      {
        g_autoptr (GVariantIter) iter = NULL;
        GVariantBuilder binding_actions;
        const char *accelerator;
        guint mode_flags, grab_flags;

        g_variant_get (parameters, "(a(suu))", &iter);
        g_variant_builder_init (&binding_actions, G_VARIANT_TYPE ("au"));
        while (g_variant_iter_next (iter, "(&suu)", &accelerator, &mode_flags, &grab_flags))
          g_variant_builder_add (&binding_actions, "u",
                                 grab_accelerator_for_sender (self, accelerator,
                                                              mode_flags, grab_flags,
                                                              sender));
        shell_dbus_shell_complete_grab_accelerators (self->skeleton, invocation,
                                                     g_variant_builder_end (&binding_actions));
        break;
      }

    case METHOD_UNGRAB_ACCELERATOR:
      {
        guint action;

        g_variant_get (parameters, "(u)", &action);
        shell_dbus_shell_complete_ungrab_accelerator (self->skeleton, invocation,
                                                      ungrab_accelerator_for_sender (self, action, sender));
        break;
      }

    case METHOD_UNGRAB_ACCELERATORS:
      {
        g_autoptr (GVariantIter) iter = NULL;
        gboolean ungrab_succeeded = TRUE;
        guint action;

        g_variant_get (parameters, "(au)", &iter);
        while (g_variant_iter_next (iter, "u", &action))
          ungrab_succeeded &= ungrab_accelerator_for_sender (self, action, sender);
        shell_dbus_shell_complete_ungrab_accelerators (self->skeleton, invocation,
                                                       ungrab_succeeded);
        break;
      }

    case METHOD_SCREEN_TRANSITION:
      {
        ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());

        if (layout_manager != NULL)
          shell_screen_transition_run (shell_layout_manager_get_screen_transition (layout_manager));
        shell_dbus_shell_complete_screen_transition (self->skeleton, invocation);
        break;
      }

    case METHOD_SHOW_MONITOR_LABELS:
      {
        g_autoptr (GVariant) params = NULL;

        g_variant_get (parameters, "(@a{sv})", &params);
        if (self->osd_monitor_labeler != NULL)
          shell_osd_monitor_labeler_show (self->osd_monitor_labeler, sender, params);
        shell_dbus_shell_complete_show_monitor_labels (self->skeleton, invocation);
        break;
      }

    case METHOD_HIDE_MONITOR_LABELS:
      if (self->osd_monitor_labeler != NULL)
        shell_osd_monitor_labeler_hide (self->osd_monitor_labeler, sender);
      shell_dbus_shell_complete_hide_monitor_labels (self->skeleton, invocation);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
on_sender_checked (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  g_autoptr (CheckedCall) call = user_data;
  g_autoptr (GError) error = NULL;

  if (!shell_dbus_sender_checker_check_invocation_finish (SHELL_DBUS_SENDER_CHECKER (source),
                                                          result, &error))
    {
      g_dbus_method_invocation_return_gerror (call->invocation, error);
      return;
    }

  run_method (call->self, call->invocation, call->method);
}

static gboolean
check_and_run (ShellDBusService      *self,
               GDBusMethodInvocation *invocation,
               Method                 method)
{
  CheckedCall *call = g_new0 (CheckedCall, 1);

  call->self = g_object_ref (self);
  call->invocation = g_object_ref (invocation);
  call->method = method;
  shell_dbus_sender_checker_check_invocation_async (self->sender_checker, invocation,
                                                    NULL, on_sender_checked, call);
  return TRUE;
}

static gboolean
on_handle_eval (ShellDBusShell        *skeleton,
                GDBusMethodInvocation *invocation,
                const char            *script,
                ShellDBusService      *self)
{
  if (!is_unsafe_mode () || self->shutting_down ||
      !g_signal_has_handler_pending (self, signals[EVAL_REQUESTED], 0, TRUE))
    {
      shell_dbus_shell_complete_eval (skeleton, invocation, FALSE, "");
      return TRUE;
    }

  g_signal_emit (self, signals[EVAL_REQUESTED], 0, script, invocation);
  return TRUE;
}

static gboolean
on_handle_focus_search (ShellDBusShell        *skeleton,
                        GDBusMethodInvocation *invocation,
                        ShellDBusService      *self)
{
  return check_and_run (self, invocation, METHOD_FOCUS_SEARCH);
}

static gboolean
on_handle_show_osd (ShellDBusShell        *skeleton,
                    GDBusMethodInvocation *invocation,
                    GVariant              *params,
                    ShellDBusService      *self)
{
  return check_and_run (self, invocation, METHOD_SHOW_OSD);
}

static gboolean
on_handle_focus_app (ShellDBusShell        *skeleton,
                     GDBusMethodInvocation *invocation,
                     const char            *id,
                     ShellDBusService      *self)
{
  return check_and_run (self, invocation, METHOD_FOCUS_APP);
}

static gboolean
on_handle_show_applications (ShellDBusShell        *skeleton,
                             GDBusMethodInvocation *invocation,
                             ShellDBusService      *self)
{
  return check_and_run (self, invocation, METHOD_SHOW_APPLICATIONS);
}

static gboolean
on_handle_grab_accelerator (ShellDBusShell        *skeleton,
                            GDBusMethodInvocation *invocation,
                            const char            *accelerator,
                            guint                  mode_flags,
                            guint                  grab_flags,
                            ShellDBusService      *self)
{
  return check_and_run (self, invocation, METHOD_GRAB_ACCELERATOR);
}

static gboolean
on_handle_grab_accelerators (ShellDBusShell        *skeleton,
                             GDBusMethodInvocation *invocation,
                             GVariant              *accelerators,
                             ShellDBusService      *self)
{
  return check_and_run (self, invocation, METHOD_GRAB_ACCELERATORS);
}

static gboolean
on_handle_ungrab_accelerator (ShellDBusShell        *skeleton,
                              GDBusMethodInvocation *invocation,
                              guint                  action,
                              ShellDBusService      *self)
{
  return check_and_run (self, invocation, METHOD_UNGRAB_ACCELERATOR);
}

static gboolean
on_handle_ungrab_accelerators (ShellDBusShell        *skeleton,
                               GDBusMethodInvocation *invocation,
                               GVariant              *actions,
                               ShellDBusService      *self)
{
  return check_and_run (self, invocation, METHOD_UNGRAB_ACCELERATORS);
}

static gboolean
on_handle_screen_transition (ShellDBusShell        *skeleton,
                             GDBusMethodInvocation *invocation,
                             ShellDBusService      *self)
{
  return check_and_run (self, invocation, METHOD_SCREEN_TRANSITION);
}

static gboolean
on_handle_show_monitor_labels (ShellDBusShell        *skeleton,
                               GDBusMethodInvocation *invocation,
                               GVariant              *params,
                               ShellDBusService      *self)
{
  return check_and_run (self, invocation, METHOD_SHOW_MONITOR_LABELS);
}

static gboolean
on_handle_hide_monitor_labels (ShellDBusShell        *skeleton,
                               GDBusMethodInvocation *invocation,
                               ShellDBusService      *self)
{
  return check_and_run (self, invocation, METHOD_HIDE_MONITOR_LABELS);
}

/* The OverviewActive property */

static gboolean
overview_active_request_cb (gpointer user_data)
{
  ShellDBusService *self = user_data;

  self->overview_active_request_id = 0;

  /* The property only changes when the overview does: restore the value
   * before asking for the change */
  sync_overview_active (self);

  if (!self->shutting_down)
    g_signal_emit (self, signals[OVERVIEW_ACTIVE_REQUESTED], 0,
                   self->requested_overview_active);

  return G_SOURCE_REMOVE;
}

static void
on_overview_active_notify (GObject          *skeleton,
                           GParamSpec       *pspec,
                           ShellDBusService *self)
{
  if (self->syncing_overview_active)
    return;

  /* A client wrote the property. Restoring it here would emit
   * notify::overview-active within this emission, and notify is a
   * G_SIGNAL_NO_RECURSE signal: GLib would restart this emission, and this
   * handler with it, for good. So it is restored once the emission is over */
  self->requested_overview_active = shell_dbus_shell_get_overview_active (self->skeleton);
  if (self->overview_active_request_id == 0)
    {
      /* Before the skeleton's PropertiesChanged, which then has nothing to
       * emit, and before the next D-Bus call is handled */
      self->overview_active_request_id =
        g_idle_add_full (G_PRIORITY_HIGH, overview_active_request_cb, self, NULL);
    }
}

static void
sync_mode (ShellDBusService *self)
{
  const char *mode = shell_global_get_session_mode (shell_global_get ());

  shell_dbus_shell_set_mode (self->skeleton, mode != NULL ? mode : "");
}

static void
on_session_mode_notify (ShellGlobal      *global,
                        GParamSpec       *pspec,
                        ShellDBusService *self)
{
  if (!self->shutting_down)
    sync_mode (self);
}

static void
on_shutdown (ShellGlobal      *global,
             ShellDBusService *self)
{
  self->shutting_down = TRUE;
}

/**
 * shell_dbus_service_complete_eval:
 * @self: a #ShellDBusService
 * @invocation: the invocation passed to #ShellDBusService::eval-requested
 * @success: whether the evaluation succeeded
 * @result: the JSON representation of the result, or the error
 *
 * Replies to a call of Eval ().
 */
void
shell_dbus_service_complete_eval (ShellDBusService      *self,
                                  GDBusMethodInvocation *invocation,
                                  gboolean               success,
                                  const char            *result)
{
  g_return_if_fail (SHELL_IS_DBUS_SERVICE (self));
  g_return_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation));

  shell_dbus_shell_complete_eval (self->skeleton, invocation, success,
                                  result != NULL ? result : "");
}

/**
 * shell_dbus_service_get_osd_window_manager:
 * @self: a #ShellDBusService
 *
 * Returns: (transfer none) (nullable): the OSD window manager ShowOSD () uses
 */
ShellOsdWindowManager *
shell_dbus_service_get_osd_window_manager (ShellDBusService *self)
{
  g_return_val_if_fail (SHELL_IS_DBUS_SERVICE (self), NULL);

  return self->osd_window_manager;
}

/**
 * shell_dbus_service_set_osd_window_manager:
 * @self: a #ShellDBusService
 * @manager: (nullable): the OSD window manager
 */
void
shell_dbus_service_set_osd_window_manager (ShellDBusService      *self,
                                           ShellOsdWindowManager *manager)
{
  g_return_if_fail (SHELL_IS_DBUS_SERVICE (self));
  g_return_if_fail (manager == NULL || SHELL_IS_OSD_WINDOW_MANAGER (manager));

  if (!g_set_object (&self->osd_window_manager, manager))
    return;

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OSD_WINDOW_MANAGER]);
}

/**
 * shell_dbus_service_get_osd_monitor_labeler:
 * @self: a #ShellDBusService
 *
 * Returns: (transfer none) (nullable): the monitor labeler ShowMonitorLabels () uses
 */
ShellOsdMonitorLabeler *
shell_dbus_service_get_osd_monitor_labeler (ShellDBusService *self)
{
  g_return_val_if_fail (SHELL_IS_DBUS_SERVICE (self), NULL);

  return self->osd_monitor_labeler;
}

/**
 * shell_dbus_service_set_osd_monitor_labeler:
 * @self: a #ShellDBusService
 * @labeler: (nullable): the monitor labeler
 */
void
shell_dbus_service_set_osd_monitor_labeler (ShellDBusService       *self,
                                            ShellOsdMonitorLabeler *labeler)
{
  g_return_if_fail (SHELL_IS_DBUS_SERVICE (self));
  g_return_if_fail (labeler == NULL || SHELL_IS_OSD_MONITOR_LABELER (labeler));

  if (!g_set_object (&self->osd_monitor_labeler, labeler))
    return;

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OSD_MONITOR_LABELER]);
}

/**
 * shell_dbus_service_get_overview_active:
 * @self: a #ShellDBusService
 *
 * Returns: whether the overview is visible, as far as the service was told
 */
gboolean
shell_dbus_service_get_overview_active (ShellDBusService *self)
{
  g_return_val_if_fail (SHELL_IS_DBUS_SERVICE (self), FALSE);

  return self->overview_active;
}

/**
 * shell_dbus_service_set_overview_active:
 * @self: a #ShellDBusService
 * @active: whether the overview is visible
 *
 * Updates the OverviewActive property; src/ui/main.c calls it when the
 * overview is showing or hidden.
 */
void
shell_dbus_service_set_overview_active (ShellDBusService *self,
                                        gboolean          active)
{
  g_return_if_fail (SHELL_IS_DBUS_SERVICE (self));

  active = !!active;
  if (self->overview_active == active)
    return;

  self->overview_active = active;
  sync_overview_active (self);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OVERVIEW_ACTIVE]);
}

static void
shell_dbus_service_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellDBusService *self = SHELL_DBUS_SERVICE (object);

  switch (prop_id)
    {
    case PROP_OSD_WINDOW_MANAGER:
      shell_dbus_service_set_osd_window_manager (self, g_value_get_object (value));
      break;

    case PROP_OSD_MONITOR_LABELER:
      shell_dbus_service_set_osd_monitor_labeler (self, g_value_get_object (value));
      break;

    case PROP_OVERVIEW_ACTIVE:
      shell_dbus_service_set_overview_active (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_dbus_service_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellDBusService *self = SHELL_DBUS_SERVICE (object);

  switch (prop_id)
    {
    case PROP_OSD_WINDOW_MANAGER:
      g_value_set_object (value, self->osd_window_manager);
      break;

    case PROP_OSD_MONITOR_LABELER:
      g_value_set_object (value, self->osd_monitor_labeler);
      break;

    case PROP_OVERVIEW_ACTIVE:
      g_value_set_boolean (value, self->overview_active);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_dbus_service_constructed (GObject *object)
{
  ShellDBusService *self = SHELL_DBUS_SERVICE (object);
  ShellGlobal *global = shell_global_get ();
  MetaDisplay *display = shell_global_get_display (global);
  g_autoptr (GDBusConnection) connection = NULL;

  G_OBJECT_CLASS (shell_dbus_service_parent_class)->constructed (object);

  self->sender_checker = shell_dbus_sender_checker_new (SENDER_ALLOWLIST);

  self->skeleton = shell_dbus_shell_skeleton_new ();
  shell_dbus_shell_set_shell_version (self->skeleton, PACKAGE_VERSION);
  sync_mode (self);
  sync_overview_active (self);

  g_signal_connect (self->skeleton, "handle-eval",
                    G_CALLBACK (on_handle_eval), self);
  g_signal_connect (self->skeleton, "handle-focus-search",
                    G_CALLBACK (on_handle_focus_search), self);
  g_signal_connect (self->skeleton, "handle-show-osd",
                    G_CALLBACK (on_handle_show_osd), self);
  g_signal_connect (self->skeleton, "handle-focus-app",
                    G_CALLBACK (on_handle_focus_app), self);
  g_signal_connect (self->skeleton, "handle-show-applications",
                    G_CALLBACK (on_handle_show_applications), self);
  g_signal_connect (self->skeleton, "handle-grab-accelerator",
                    G_CALLBACK (on_handle_grab_accelerator), self);
  g_signal_connect (self->skeleton, "handle-grab-accelerators",
                    G_CALLBACK (on_handle_grab_accelerators), self);
  g_signal_connect (self->skeleton, "handle-ungrab-accelerator",
                    G_CALLBACK (on_handle_ungrab_accelerator), self);
  g_signal_connect (self->skeleton, "handle-ungrab-accelerators",
                    G_CALLBACK (on_handle_ungrab_accelerators), self);
  g_signal_connect (self->skeleton, "handle-screen-transition",
                    G_CALLBACK (on_handle_screen_transition), self);
  g_signal_connect (self->skeleton, "handle-show-monitor-labels",
                    G_CALLBACK (on_handle_show_monitor_labels), self);
  g_signal_connect (self->skeleton, "handle-hide-monitor-labels",
                    G_CALLBACK (on_handle_hide_monitor_labels), self);
  g_signal_connect (self->skeleton, "notify::overview-active",
                    G_CALLBACK (on_overview_active_notify), self);

  g_signal_connect_object (display, "accelerator-activated",
                           G_CALLBACK (on_accelerator_activated), self, 0);
  g_signal_connect_object (display, "accelerator-deactivated",
                           G_CALLBACK (on_accelerator_deactivated), self, 0);
  g_signal_connect_object (global, "notify::session-mode",
                           G_CALLBACK (on_session_mode_notify), self, 0);
  g_signal_connect_object (global, "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  connection = export_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                SHELL_OBJECT_PATH);
}

static void
shell_dbus_service_dispose (GObject *object)
{
  ShellDBusService *self = SHELL_DBUS_SERVICE (object);
  GHashTableIter iter;
  gpointer watch_id;

  self->shutting_down = TRUE;

  g_clear_handle_id (&self->overview_active_request_id, g_source_remove);

  if (self->grabbers != NULL)
    {
      g_hash_table_iter_init (&iter, self->grabbers);
      while (g_hash_table_iter_next (&iter, NULL, &watch_id))
        g_bus_unwatch_name (GPOINTER_TO_UINT (watch_id));
      g_hash_table_remove_all (self->grabbers);
    }

  if (self->skeleton != NULL)
    {
      unexport_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton), self);
      g_clear_object (&self->skeleton);
    }

  g_clear_object (&self->sender_checker);
  g_clear_object (&self->osd_window_manager);
  g_clear_object (&self->osd_monitor_labeler);

  G_OBJECT_CLASS (shell_dbus_service_parent_class)->dispose (object);
}

static void
shell_dbus_service_finalize (GObject *object)
{
  ShellDBusService *self = SHELL_DBUS_SERVICE (object);

  g_clear_pointer (&self->grabbed_accelerators, g_hash_table_unref);
  g_clear_pointer (&self->grabbers, g_hash_table_unref);

  G_OBJECT_CLASS (shell_dbus_service_parent_class)->finalize (object);
}

static void
shell_dbus_service_class_init (ShellDBusServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = shell_dbus_service_set_property;
  object_class->get_property = shell_dbus_service_get_property;
  object_class->constructed = shell_dbus_service_constructed;
  object_class->dispose = shell_dbus_service_dispose;
  object_class->finalize = shell_dbus_service_finalize;

  /**
   * ShellDBusService:osd-window-manager:
   *
   * The OSD window manager ShowOSD () shows the OSD with, if any
   * (`Main.osdWindowManager`).
   */
  props[PROP_OSD_WINDOW_MANAGER] =
    g_param_spec_object ("osd-window-manager", NULL, NULL,
                         SHELL_TYPE_OSD_WINDOW_MANAGER,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDBusService:osd-monitor-labeler:
   *
   * The monitor labeler ShowMonitorLabels () and HideMonitorLabels () drive, if any
   * (`Main.osdMonitorLabeler`).
   */
  props[PROP_OSD_MONITOR_LABELER] =
    g_param_spec_object ("osd-monitor-labeler", NULL, NULL,
                         SHELL_TYPE_OSD_MONITOR_LABELER,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDBusService:overview-active:
   *
   * Whether the overview is visible: the OverviewActive property. Set it
   * when the overview is showing or hidden; a client writing the property
   * emits #ShellDBusService::overview-active-requested instead.
   */
  props[PROP_OVERVIEW_ACTIVE] =
    g_param_spec_boolean ("overview-active", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellDBusService::eval-requested:
   * @self: the #ShellDBusService
   * @code: the script of the call of Eval ()
   * @invocation: the method invocation
   *
   * Emitted when a client calls Eval () and the shell is in unsafe mode. The
   * handler evaluates @code and replies with
   * shell_dbus_service_complete_eval (), possibly later.
   */
  signals[EVAL_REQUESTED] =
    g_signal_new ("eval-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING, G_TYPE_DBUS_METHOD_INVOCATION);

  /**
   * ShellDBusService::focus-search-requested:
   * @self: the #ShellDBusService
   *
   * Emitted by FocusSearch (): focus the search entry of the overview.
   */
  signals[FOCUS_SEARCH_REQUESTED] =
    g_signal_new ("focus-search-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellDBusService::focus-app-requested:
   * @self: the #ShellDBusService
   * @app_id: the ID of an installed application
   *
   * Emitted by FocusApp (): select the application in the app grid of the
   * overview.
   */
  signals[FOCUS_APP_REQUESTED] =
    g_signal_new ("focus-app-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);

  /**
   * ShellDBusService::show-applications-requested:
   * @self: the #ShellDBusService
   *
   * Emitted by ShowApplications (): show the app grid of the overview.
   */
  signals[SHOW_APPLICATIONS_REQUESTED] =
    g_signal_new ("show-applications-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellDBusService::allow-keybinding-requested:
   * @self: the #ShellDBusService
   * @name: the name of the keybinding a client has grabbed
   * @mode_flags: the #ShellActionMode flags the keybinding is allowed in
   *
   * Emitted by GrabAccelerator () and GrabAccelerators (), for the window
   * manager to allow the keybinding (`Main.wm.allowKeybinding ()`).
   */
  signals[ALLOW_KEYBINDING_REQUESTED] =
    g_signal_new ("allow-keybinding-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING, G_TYPE_UINT);

  /**
   * ShellDBusService::overview-active-requested:
   * @self: the #ShellDBusService
   * @active: whether the client wants the overview to be visible
   *
   * Emitted when a client writes the OverviewActive property: show or hide
   * the overview. #ShellDBusService:overview-active follows the overview.
   */
  signals[OVERVIEW_ACTIVE_REQUESTED] =
    g_signal_new ("overview-active-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN);
}

static void
shell_dbus_service_init (ShellDBusService *self)
{
  self->grabbed_accelerators = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                      NULL, g_free);
  self->grabbers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

/**
 * shell_dbus_service_new:
 *
 * Creates the service and exports org.gnome.Shell at /org/gnome/Shell on the
 * session bus. src/ui/main.c creates the only instance.
 *
 * Returns: (transfer full): a new #ShellDBusService
 */
ShellDBusService *
shell_dbus_service_new (void)
{
  return g_object_new (SHELL_TYPE_DBUS_SERVICE, NULL);
}

/* ------------------------------------------------------------------------
 * ShellDBusExtensionsService: org.gnome.Shell.Extensions
 * ------------------------------------------------------------------------ */

enum
{
  EXT_PROP_0,
  EXT_PROP_MANAGER,
  EXT_N_PROPS
};

static GParamSpec *ext_props[EXT_N_PROPS] = { NULL, };

struct _ShellDBusExtensionsService
{
  GObject parent;

  ShellDBusShellExtensions *skeleton;
  ShellExtensionManager *manager;
  gboolean syncing_enabled;
};

G_DEFINE_FINAL_TYPE (ShellDBusExtensionsService, shell_dbus_extensions_service, G_TYPE_OBJECT)

static gboolean
on_handle_list_extensions (ShellDBusShellExtensions    *skeleton,
                           GDBusMethodInvocation       *invocation,
                           ShellDBusExtensionsService  *self)
{
  g_autoptr (GVariant) extensions = shell_extension_manager_list_extensions (self->manager);

  shell_dbus_shell_extensions_complete_list_extensions (skeleton, invocation, extensions);
  return TRUE;
}

static gboolean
on_handle_get_extension_info (ShellDBusShellExtensions   *skeleton,
                              GDBusMethodInvocation      *invocation,
                              const char                 *uuid,
                              ShellDBusExtensionsService *self)
{
  g_autoptr (GVariant) info = shell_extension_manager_get_extension_info (self->manager, uuid);

  shell_dbus_shell_extensions_complete_get_extension_info (skeleton, invocation, info);
  return TRUE;
}

static gboolean
on_handle_get_extension_errors (ShellDBusShellExtensions   *skeleton,
                                GDBusMethodInvocation      *invocation,
                                const char                 *uuid,
                                ShellDBusExtensionsService *self)
{
  g_autoptr (GVariant) errors = shell_extension_manager_get_extension_errors (self->manager, uuid);
  g_autofree const char **strv = g_variant_get_strv (errors, NULL);

  shell_dbus_shell_extensions_complete_get_extension_errors (skeleton, invocation, strv);
  return TRUE;
}

typedef struct
{
  ShellDBusExtensionsService *service;
  GDBusMethodInvocation *invocation;
} InstallCall;

static void
install_call_free (InstallCall *call)
{
  g_object_unref (call->service);
  g_free (call);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (InstallCall, install_call_free)

static void
install_remote_extension_cb (GObject      *source,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  g_autoptr (InstallCall) call = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *outcome = NULL;

  outcome = shell_extension_manager_install_extension_finish (SHELL_EXTENSION_MANAGER (source),
                                                              result, &error);
  if (outcome == NULL)
    g_dbus_method_invocation_return_gerror (call->invocation, error);
  else
    shell_dbus_shell_extensions_complete_install_remote_extension (call->service->skeleton,
                                                                   call->invocation, outcome);
}

static gboolean
on_handle_install_remote_extension (ShellDBusShellExtensions   *skeleton,
                                    GDBusMethodInvocation      *invocation,
                                    const char                 *uuid,
                                    ShellDBusExtensionsService *self)
{
  InstallCall *call = g_new0 (InstallCall, 1);

  call->service = g_object_ref (self);
  call->invocation = invocation;
  shell_extension_manager_install_extension_async (self->manager, uuid, NULL,
                                                   install_remote_extension_cb, call);
  return TRUE;
}

static gboolean
on_handle_uninstall_extension (ShellDBusShellExtensions   *skeleton,
                               GDBusMethodInvocation      *invocation,
                               const char                 *uuid,
                               ShellDBusExtensionsService *self)
{
  shell_dbus_shell_extensions_complete_uninstall_extension (skeleton, invocation,
                                                            shell_extension_manager_uninstall_extension (self->manager, uuid));
  return TRUE;
}

static gboolean
on_handle_reload_extension (ShellDBusShellExtensions   *skeleton,
                            GDBusMethodInvocation      *invocation,
                            const char                 *uuid,
                            ShellDBusExtensionsService *self)
{
  g_autoptr (GError) error = NULL;

  if (!shell_extension_manager_reload_extension (self->manager, uuid, &error))
    g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR,
                                                   G_DBUS_ERROR_FAILED, error->message);
  else
    shell_dbus_shell_extensions_complete_reload_extension (skeleton, invocation);

  return TRUE;
}

static gboolean
on_handle_enable_extension (ShellDBusShellExtensions   *skeleton,
                            GDBusMethodInvocation      *invocation,
                            const char                 *uuid,
                            ShellDBusExtensionsService *self)
{
  shell_dbus_shell_extensions_complete_enable_extension (skeleton, invocation,
                                                         shell_extension_manager_enable_extension (self->manager, uuid));
  return TRUE;
}

static gboolean
on_handle_disable_extension (ShellDBusShellExtensions   *skeleton,
                             GDBusMethodInvocation      *invocation,
                             const char                 *uuid,
                             ShellDBusExtensionsService *self)
{
  shell_dbus_shell_extensions_complete_disable_extension (skeleton, invocation,
                                                          shell_extension_manager_disable_extension (self->manager, uuid));
  return TRUE;
}

static gboolean
on_handle_launch_extension_prefs (ShellDBusShellExtensions   *skeleton,
                                  GDBusMethodInvocation      *invocation,
                                  const char                 *uuid,
                                  ShellDBusExtensionsService *self)
{
  shell_extension_manager_open_extension_prefs (self->manager, uuid, "", NULL);
  shell_dbus_shell_extensions_complete_launch_extension_prefs (skeleton, invocation);
  return TRUE;
}

static gboolean
on_handle_open_extension_prefs (ShellDBusShellExtensions   *skeleton,
                                GDBusMethodInvocation      *invocation,
                                const char                 *uuid,
                                const char                 *parent_window,
                                GVariant                   *options,
                                ShellDBusExtensionsService *self)
{
  shell_extension_manager_open_extension_prefs (self->manager, uuid, parent_window, options);
  shell_dbus_shell_extensions_complete_open_extension_prefs (skeleton, invocation);
  return TRUE;
}

static void
check_for_updates_cb (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  if (!shell_extension_manager_check_for_updates_finish (SHELL_EXTENSION_MANAGER (source),
                                                         result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("Update check failed: %s", error->message);
}

static gboolean
on_handle_check_for_updates (ShellDBusShellExtensions   *skeleton,
                             GDBusMethodInvocation      *invocation,
                             ShellDBusExtensionsService *self)
{
  /* The call returns at once, the check goes on in the background */
  shell_extension_manager_check_for_updates_async (self->manager, NULL,
                                                   check_for_updates_cb, NULL);
  shell_dbus_shell_extensions_complete_check_for_updates (skeleton, invocation);
  return TRUE;
}

static void
sync_user_extensions_enabled (ShellDBusExtensionsService *self)
{
  GSettings *settings = shell_global_get_settings (shell_global_get ());

  self->syncing_enabled = TRUE;
  shell_dbus_shell_extensions_set_user_extensions_enabled (self->skeleton,
                                                           !g_settings_get_boolean (settings,
                                                                                    DISABLE_USER_EXTENSIONS_KEY));
  self->syncing_enabled = FALSE;
}

static void
on_user_extensions_enabled_notify (GObject                    *skeleton,
                                   GParamSpec                 *pspec,
                                   ShellDBusExtensionsService *self)
{
  GSettings *settings;
  gboolean enable;

  if (self->syncing_enabled)
    return;

  /* A client wrote the property: it is the setting that holds the value */
  settings = shell_global_get_settings (shell_global_get ());
  enable = shell_dbus_shell_extensions_get_user_extensions_enabled (self->skeleton);
  g_settings_set_boolean (settings, DISABLE_USER_EXTENSIONS_KEY, !enable);
  sync_user_extensions_enabled (self);
}

static void
on_disable_user_extensions_changed (GSettings                  *settings,
                                    const char                 *key,
                                    ShellDBusExtensionsService *self)
{
  sync_user_extensions_enabled (self);
}

/* ExtensionStateChanged and ExtensionStatusChanged follow the manager */
static void
on_manager_extension_state_changed (ShellExtensionManager     *manager,
                                    ShellLuaExtension         *extension,
                                    ShellDBusExtensionsService *self)
{
  const char *uuid = shell_lua_extension_get_uuid (extension);
  const char *error = shell_lua_extension_get_error (extension);
  g_autoptr (GVariant) info = shell_extension_manager_serialize_extension (manager, extension);

  if (self->skeleton == NULL)
    return;

  shell_dbus_shell_extensions_emit_extension_state_changed (self->skeleton, uuid, info);
  shell_dbus_shell_extensions_emit_extension_status_changed (self->skeleton, uuid,
                                                             (int) shell_lua_extension_get_state (extension),
                                                             error != NULL ? error : "");
}

static void
shell_dbus_extensions_service_constructed (GObject *object)
{
  ShellDBusExtensionsService *self = SHELL_DBUS_EXTENSIONS_SERVICE (object);
  GSettings *settings = shell_global_get_settings (shell_global_get ());
  g_autoptr (GDBusConnection) connection = NULL;

  G_OBJECT_CLASS (shell_dbus_extensions_service_parent_class)->constructed (object);

  g_assert (self->manager != NULL);

  self->skeleton = shell_dbus_shell_extensions_skeleton_new ();
  shell_dbus_shell_extensions_set_shell_version (self->skeleton, PACKAGE_VERSION);
  sync_user_extensions_enabled (self);

  g_signal_connect (self->skeleton, "handle-list-extensions",
                    G_CALLBACK (on_handle_list_extensions), self);
  g_signal_connect (self->skeleton, "handle-get-extension-info",
                    G_CALLBACK (on_handle_get_extension_info), self);
  g_signal_connect (self->skeleton, "handle-get-extension-errors",
                    G_CALLBACK (on_handle_get_extension_errors), self);
  g_signal_connect (self->skeleton, "handle-install-remote-extension",
                    G_CALLBACK (on_handle_install_remote_extension), self);
  g_signal_connect (self->skeleton, "handle-uninstall-extension",
                    G_CALLBACK (on_handle_uninstall_extension), self);
  g_signal_connect (self->skeleton, "handle-reload-extension",
                    G_CALLBACK (on_handle_reload_extension), self);
  g_signal_connect (self->skeleton, "handle-enable-extension",
                    G_CALLBACK (on_handle_enable_extension), self);
  g_signal_connect (self->skeleton, "handle-disable-extension",
                    G_CALLBACK (on_handle_disable_extension), self);
  g_signal_connect (self->skeleton, "handle-launch-extension-prefs",
                    G_CALLBACK (on_handle_launch_extension_prefs), self);
  g_signal_connect (self->skeleton, "handle-open-extension-prefs",
                    G_CALLBACK (on_handle_open_extension_prefs), self);
  g_signal_connect (self->skeleton, "handle-check-for-updates",
                    G_CALLBACK (on_handle_check_for_updates), self);
  g_signal_connect (self->skeleton, "notify::user-extensions-enabled",
                    G_CALLBACK (on_user_extensions_enabled_notify), self);

  g_signal_connect_object (settings, "changed::" DISABLE_USER_EXTENSIONS_KEY,
                           G_CALLBACK (on_disable_user_extensions_changed), self, 0);
  g_signal_connect_object (self->manager, "extension-state-changed",
                           G_CALLBACK (on_manager_extension_state_changed), self, 0);

  connection = export_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                SHELL_OBJECT_PATH);
}

static void
shell_dbus_extensions_service_set_property (GObject      *object,
                                            guint         prop_id,
                                            const GValue *value,
                                            GParamSpec   *pspec)
{
  ShellDBusExtensionsService *self = SHELL_DBUS_EXTENSIONS_SERVICE (object);

  switch (prop_id)
    {
    case EXT_PROP_MANAGER:
      g_set_object (&self->manager, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_dbus_extensions_service_get_property (GObject    *object,
                                            guint       prop_id,
                                            GValue     *value,
                                            GParamSpec *pspec)
{
  ShellDBusExtensionsService *self = SHELL_DBUS_EXTENSIONS_SERVICE (object);

  switch (prop_id)
    {
    case EXT_PROP_MANAGER:
      g_value_set_object (value, self->manager);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_dbus_extensions_service_dispose (GObject *object)
{
  ShellDBusExtensionsService *self = SHELL_DBUS_EXTENSIONS_SERVICE (object);

  if (self->skeleton != NULL)
    {
      unexport_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton), self);
      g_clear_object (&self->skeleton);
    }

  g_clear_object (&self->manager);

  G_OBJECT_CLASS (shell_dbus_extensions_service_parent_class)->dispose (object);
}

static void
shell_dbus_extensions_service_class_init (ShellDBusExtensionsServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_dbus_extensions_service_constructed;
  object_class->set_property = shell_dbus_extensions_service_set_property;
  object_class->get_property = shell_dbus_extensions_service_get_property;
  object_class->dispose = shell_dbus_extensions_service_dispose;

  /**
   * ShellDBusExtensionsService:manager:
   *
   * The extension manager that answers the calls and whose changes are
   * reported with ExtensionStateChanged.
   */
  ext_props[EXT_PROP_MANAGER] =
    g_param_spec_object ("manager", NULL, NULL,
                         SHELL_TYPE_EXTENSION_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, EXT_N_PROPS, ext_props);
}

static void
shell_dbus_extensions_service_init (ShellDBusExtensionsService *self)
{
}

/**
 * shell_dbus_extensions_service_new:
 * @manager: the extension manager
 *
 * Creates the service and exports org.gnome.Shell.Extensions at
 * /org/gnome/Shell on the session bus. src/ui/main.c creates the only
 * instance.
 *
 * Returns: (transfer full): a new #ShellDBusExtensionsService
 */
ShellDBusExtensionsService *
shell_dbus_extensions_service_new (ShellExtensionManager *manager)
{
  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (manager), NULL);

  return g_object_new (SHELL_TYPE_DBUS_EXTENSIONS_SERVICE, "manager", manager, NULL);
}

/* ------------------------------------------------------------------------
 * ShellScreenSaverService: org.gnome.ScreenSaver
 * ------------------------------------------------------------------------ */

enum
{
  SS_LOCK_REQUESTED,
  SS_SET_ACTIVE_REQUESTED,
  SS_QUERY_ACTIVE,
  SS_QUERY_ACTIVATION_TIME,

  SS_LAST_SIGNAL
};

static guint ss_signals[SS_LAST_SIGNAL] = { 0, };

struct _ShellScreenSaverService
{
  GObject parent;

  ShellDBusScreenSaver *skeleton;
  guint name_id;
  GQueue pending_locks;  /* of GDBusMethodInvocation, waiting for lock_screen_shown () */
};

G_DEFINE_FINAL_TYPE (ShellScreenSaverService, shell_screen_saver_service, G_TYPE_OBJECT)

static gboolean
on_handle_lock (ShellDBusScreenSaver      *skeleton,
                GDBusMethodInvocation     *invocation,
                ShellScreenSaverService   *self)
{
  if (!g_signal_has_handler_pending (self, ss_signals[SS_LOCK_REQUESTED], 0, TRUE))
    {
      shell_dbus_screen_saver_complete_lock (skeleton, invocation);
      return TRUE;
    }

  /* The reply is the lock screen being shown, which can happen while lock ()
   * runs */
  g_queue_push_tail (&self->pending_locks, g_object_ref (invocation));
  g_signal_emit (self, ss_signals[SS_LOCK_REQUESTED], 0);
  return TRUE;
}

static gboolean
on_handle_set_active (ShellDBusScreenSaver      *skeleton,
                      GDBusMethodInvocation     *invocation,
                      gboolean                   value,
                      ShellScreenSaverService   *self)
{
  g_signal_emit (self, ss_signals[SS_SET_ACTIVE_REQUESTED], 0, value);
  shell_dbus_screen_saver_complete_set_active (skeleton, invocation);
  return TRUE;
}

static gboolean
on_handle_get_active (ShellDBusScreenSaver      *skeleton,
                      GDBusMethodInvocation     *invocation,
                      ShellScreenSaverService   *self)
{
  gboolean active = FALSE;

  g_signal_emit (self, ss_signals[SS_QUERY_ACTIVE], 0, &active);
  shell_dbus_screen_saver_complete_get_active (skeleton, invocation, active);
  return TRUE;
}

static gboolean
on_handle_get_active_time (ShellDBusScreenSaver      *skeleton,
                           GDBusMethodInvocation     *invocation,
                           ShellScreenSaverService   *self)
{
  gint64 started = 0;
  guint32 active_time = 0;

  g_signal_emit (self, ss_signals[SS_QUERY_ACTIVATION_TIME], 0, &started);
  if (started > 0)
    active_time = (g_get_monotonic_time () - started) / G_USEC_PER_SEC;

  shell_dbus_screen_saver_complete_get_active_time (skeleton, invocation, active_time);
  return TRUE;
}

static void
fail_pending_locks (ShellScreenSaverService *self)
{
  GDBusMethodInvocation *invocation;

  while ((invocation = g_queue_pop_head (&self->pending_locks)) != NULL)
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_DBUS_ERROR,
                                                     G_DBUS_ERROR_FAILED,
                                                     "The shell is shutting down");
      g_object_unref (invocation);
    }
}

static void
on_screen_saver_shutdown (ShellGlobal             *global,
                          ShellScreenSaverService *self)
{
  fail_pending_locks (self);
}

/**
 * shell_screen_saver_service_emit_active_changed:
 * @self: a #ShellScreenSaverService
 * @active: whether the screen shield is active
 *
 * Emits the ActiveChanged signal; src/ui/screen-shield.c calls it for its
 * `active-changed`.
 */
void
shell_screen_saver_service_emit_active_changed (ShellScreenSaverService *self,
                                                gboolean                 active)
{
  g_return_if_fail (SHELL_IS_SCREEN_SAVER_SERVICE (self));

  shell_dbus_screen_saver_emit_active_changed (self->skeleton, active);
}

/**
 * shell_screen_saver_service_emit_wake_up_screen:
 * @self: a #ShellScreenSaverService
 *
 * Emits the WakeUpScreen signal; src/ui/screen-shield.c calls it for its
 * `wake-up-screen`.
 */
void
shell_screen_saver_service_emit_wake_up_screen (ShellScreenSaverService *self)
{
  g_return_if_fail (SHELL_IS_SCREEN_SAVER_SERVICE (self));

  shell_dbus_screen_saver_emit_wake_up_screen (self->skeleton);
}

/**
 * shell_screen_saver_service_lock_screen_shown:
 * @self: a #ShellScreenSaverService
 *
 * Replies to the calls of Lock () that wait for the lock screen;
 * src/ui/screen-shield.c calls it for its `lock-screen-shown`.
 */
void
shell_screen_saver_service_lock_screen_shown (ShellScreenSaverService *self)
{
  GDBusMethodInvocation *invocation;

  g_return_if_fail (SHELL_IS_SCREEN_SAVER_SERVICE (self));

  while ((invocation = g_queue_pop_head (&self->pending_locks)) != NULL)
    {
      shell_dbus_screen_saver_complete_lock (self->skeleton, invocation);
      g_object_unref (invocation);
    }
}

static void
shell_screen_saver_service_constructed (GObject *object)
{
  ShellScreenSaverService *self = SHELL_SCREEN_SAVER_SERVICE (object);
  g_autoptr (GDBusConnection) connection = NULL;

  G_OBJECT_CLASS (shell_screen_saver_service_parent_class)->constructed (object);

  self->skeleton = shell_dbus_screen_saver_skeleton_new ();
  g_signal_connect (self->skeleton, "handle-lock",
                    G_CALLBACK (on_handle_lock), self);
  g_signal_connect (self->skeleton, "handle-set-active",
                    G_CALLBACK (on_handle_set_active), self);
  g_signal_connect (self->skeleton, "handle-get-active",
                    G_CALLBACK (on_handle_get_active), self);
  g_signal_connect (self->skeleton, "handle-get-active-time",
                    G_CALLBACK (on_handle_get_active_time), self);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_screen_saver_shutdown), self, 0);

  connection = export_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                SCREEN_SAVER_OBJECT_PATH);
  if (connection != NULL)
    self->name_id = g_bus_own_name_on_connection (connection, SCREEN_SAVER_BUS_NAME,
                                                  G_BUS_NAME_OWNER_FLAGS_NONE,
                                                  NULL, NULL, NULL, NULL);
}

static void
shell_screen_saver_service_dispose (GObject *object)
{
  ShellScreenSaverService *self = SHELL_SCREEN_SAVER_SERVICE (object);

  fail_pending_locks (self);

  if (self->name_id != 0)
    {
      g_bus_unown_name (self->name_id);
      self->name_id = 0;
    }

  if (self->skeleton != NULL)
    {
      unexport_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton), self);
      g_clear_object (&self->skeleton);
    }

  G_OBJECT_CLASS (shell_screen_saver_service_parent_class)->dispose (object);
}

static void
shell_screen_saver_service_class_init (ShellScreenSaverServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_screen_saver_service_constructed;
  object_class->dispose = shell_screen_saver_service_dispose;

  /**
   * ShellScreenSaverService::lock-requested:
   * @self: the #ShellScreenSaverService
   *
   * Emitted by Lock (): lock the screen. The call is answered by
   * shell_screen_saver_service_lock_screen_shown ().
   */
  ss_signals[SS_LOCK_REQUESTED] =
    g_signal_new ("lock-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellScreenSaverService::set-active-requested:
   * @self: the #ShellScreenSaverService
   * @active: whether to activate or deactivate the screen shield
   *
   * Emitted by SetActive ().
   */
  ss_signals[SS_SET_ACTIVE_REQUESTED] =
    g_signal_new ("set-active-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN);

  /**
   * ShellScreenSaverService::query-active:
   * @self: the #ShellScreenSaverService
   *
   * Emitted by GetActive ().
   *
   * Returns: whether the screen shield is active
   */
  ss_signals[SS_QUERY_ACTIVE] =
    g_signal_new ("query-active",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, g_signal_accumulator_first_wins, NULL, NULL,
                  G_TYPE_BOOLEAN, 0);

  /**
   * ShellScreenSaverService::query-activation-time:
   * @self: the #ShellScreenSaverService
   *
   * Emitted by GetActiveTime ().
   *
   * Returns: the monotonic time (see g_get_monotonic_time ()) the screen
   *   shield was activated at, or 0 if it is not
   */
  ss_signals[SS_QUERY_ACTIVATION_TIME] =
    g_signal_new ("query-activation-time",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, g_signal_accumulator_first_wins, NULL, NULL,
                  G_TYPE_INT64, 0);
}

static void
shell_screen_saver_service_init (ShellScreenSaverService *self)
{
  g_queue_init (&self->pending_locks);
}

/**
 * shell_screen_saver_service_new:
 *
 * Creates the service, exports org.gnome.ScreenSaver at /org/gnome/ScreenSaver
 * and owns org.gnome.Shell.ScreenShield on the session bus.
 * src/ui/screen-shield.c creates the only instance.
 *
 * Returns: (transfer full): a new #ShellScreenSaverService
 */
ShellScreenSaverService *
shell_screen_saver_service_new (void)
{
  return g_object_new (SHELL_TYPE_SCREEN_SAVER_SERVICE, NULL);
}

/* ------------------------------------------------------------------------
 * ShellScreenTimeService: org.gnome.Shell.ScreenTime
 * ------------------------------------------------------------------------ */

enum
{
  ST_PROP_0,

  ST_PROP_MANAGER,

  ST_N_PROPS
};

static GParamSpec *st_props[ST_N_PROPS] = { NULL, };

struct _ShellScreenTimeService
{
  GObject parent;

  ShellDBusShellScreenTime *skeleton;
  GObject *manager;
};

G_DEFINE_FINAL_TYPE (ShellScreenTimeService, shell_screen_time_service, G_TYPE_OBJECT)

static void
sync_screen_time (ShellScreenTimeService *self)
{
  int state = 0;
  guint64 last_break_end_time = 0;

  g_object_get (self->manager,
                "state", &state,
                "last-break-end-time", &last_break_end_time,
                NULL);

  /* Both values are always notified together, as clients need both: the
   * skeleton combines the two changes in one PropertiesChanged */
  shell_dbus_shell_screen_time_set_state (self->skeleton, state);
  shell_dbus_shell_screen_time_set_last_break_end_time (self->skeleton, last_break_end_time);
}

static void
on_manager_notify (GObject                *manager,
                   GParamSpec             *pspec,
                   ShellScreenTimeService *self)
{
  sync_screen_time (self);
}

static void
shell_screen_time_service_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellScreenTimeService *self = SHELL_SCREEN_TIME_SERVICE (object);

  switch (prop_id)
    {
    case ST_PROP_MANAGER:
      self->manager = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_screen_time_service_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellScreenTimeService *self = SHELL_SCREEN_TIME_SERVICE (object);

  switch (prop_id)
    {
    case ST_PROP_MANAGER:
      g_value_set_object (value, self->manager);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_screen_time_service_constructed (GObject *object)
{
  ShellScreenTimeService *self = SHELL_SCREEN_TIME_SERVICE (object);
  g_autoptr (GDBusConnection) connection = NULL;

  G_OBJECT_CLASS (shell_screen_time_service_parent_class)->constructed (object);

  self->skeleton = shell_dbus_shell_screen_time_skeleton_new ();

  if (self->manager != NULL)
    {
      g_signal_connect_object (self->manager, "notify::state",
                               G_CALLBACK (on_manager_notify), self, 0);
      g_signal_connect_object (self->manager, "notify::last-break-end-time",
                               G_CALLBACK (on_manager_notify), self, 0);
      sync_screen_time (self);
    }

  connection = export_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                SCREEN_TIME_OBJECT_PATH);
}

static void
shell_screen_time_service_dispose (GObject *object)
{
  ShellScreenTimeService *self = SHELL_SCREEN_TIME_SERVICE (object);

  if (self->skeleton != NULL)
    {
      unexport_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton), self);
      g_clear_object (&self->skeleton);
    }

  g_clear_object (&self->manager);

  G_OBJECT_CLASS (shell_screen_time_service_parent_class)->dispose (object);
}

static void
shell_screen_time_service_class_init (ShellScreenTimeServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = shell_screen_time_service_set_property;
  object_class->get_property = shell_screen_time_service_get_property;
  object_class->constructed = shell_screen_time_service_constructed;
  object_class->dispose = shell_screen_time_service_dispose;

  /**
   * ShellScreenTimeService:manager:
   *
   * The break manager: a #GObject with the `state` (int) and
   * `last-break-end-time` (uint64) properties.
   */
  st_props[ST_PROP_MANAGER] =
    g_param_spec_object ("manager", NULL, NULL,
                         G_TYPE_OBJECT,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, ST_N_PROPS, st_props);
}

static void
shell_screen_time_service_init (ShellScreenTimeService *self)
{
}

/**
 * shell_screen_time_service_new:
 * @manager: the break manager, a #GObject with the `state` (int) and
 *   `last-break-end-time` (uint64) properties
 *
 * Creates the service and exports org.gnome.Shell.ScreenTime at
 * /org/gnome/Shell/ScreenTime on the session bus. src/ui/main.c creates the
 * only instance.
 *
 * Returns: (transfer full): a new #ShellScreenTimeService
 */
ShellScreenTimeService *
shell_screen_time_service_new (GObject *manager)
{
  g_return_val_if_fail (G_IS_OBJECT (manager), NULL);

  return g_object_new (SHELL_TYPE_SCREEN_TIME_SERVICE,
                       "manager", manager,
                       NULL);
}

/* ------------------------------------------------------------------------
 * ShellBrightnessService: org.gnome.Shell.Brightness
 * ------------------------------------------------------------------------ */

enum
{
  BR_PROP_0,

  BR_PROP_MANAGER,

  BR_N_PROPS
};

static GParamSpec *br_props[BR_N_PROPS] = { NULL, };

struct _ShellBrightnessService
{
  GObject parent;

  ShellDBusShellBrightness *skeleton;
  ShellBrightnessManager *manager;
  guint name_id;
};

G_DEFINE_FINAL_TYPE (ShellBrightnessService, shell_brightness_service, G_TYPE_OBJECT)

static void
sync_has_brightness_control (ShellBrightnessService *self)
{
  shell_dbus_shell_brightness_set_has_brightness_control (self->skeleton,
                                                          shell_brightness_manager_get_global_scale (self->manager) != NULL);
}

static void
on_manager_changed (ShellBrightnessManager *manager,
                    ShellBrightnessService *self)
{
  sync_has_brightness_control (self);
}

static void
on_manager_user_update (ShellBrightnessManager *manager,
                        ShellBrightnessService *self)
{
  shell_dbus_shell_brightness_emit_brightness_changed (self->skeleton);
}

static gboolean
on_handle_set_dimming (ShellDBusShellBrightness *skeleton,
                       GDBusMethodInvocation    *invocation,
                       gboolean                  enable,
                       ShellBrightnessService   *self)
{
  shell_brightness_manager_set_dimming (self->manager, enable);
  shell_dbus_shell_brightness_complete_set_dimming (skeleton, invocation);
  return TRUE;
}

static gboolean
on_handle_set_auto_brightness_target (ShellDBusShellBrightness *skeleton,
                                      GDBusMethodInvocation    *invocation,
                                      double                    target,
                                      ShellBrightnessService   *self)
{
  shell_brightness_manager_set_auto_brightness_target (self->manager, target);
  shell_dbus_shell_brightness_complete_set_auto_brightness_target (skeleton, invocation);
  return TRUE;
}

static void
shell_brightness_service_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellBrightnessService *self = SHELL_BRIGHTNESS_SERVICE (object);

  switch (prop_id)
    {
    case BR_PROP_MANAGER:
      self->manager = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_brightness_service_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellBrightnessService *self = SHELL_BRIGHTNESS_SERVICE (object);

  switch (prop_id)
    {
    case BR_PROP_MANAGER:
      g_value_set_object (value, self->manager);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_brightness_service_constructed (GObject *object)
{
  ShellBrightnessService *self = SHELL_BRIGHTNESS_SERVICE (object);
  g_autoptr (GDBusConnection) connection = NULL;

  G_OBJECT_CLASS (shell_brightness_service_parent_class)->constructed (object);

  self->skeleton = shell_dbus_shell_brightness_skeleton_new ();
  g_signal_connect (self->skeleton, "handle-set-dimming",
                    G_CALLBACK (on_handle_set_dimming), self);
  g_signal_connect (self->skeleton, "handle-set-auto-brightness-target",
                    G_CALLBACK (on_handle_set_auto_brightness_target), self);

  g_signal_connect_object (self->manager, "changed",
                           G_CALLBACK (on_manager_changed), self, 0);
  g_signal_connect_object (self->manager, "user-update",
                           G_CALLBACK (on_manager_user_update), self, 0);
  sync_has_brightness_control (self);

  connection = export_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                BRIGHTNESS_OBJECT_PATH);
  if (connection != NULL)
    self->name_id = g_bus_own_name_on_connection (connection, BRIGHTNESS_BUS_NAME,
                                                  G_BUS_NAME_OWNER_FLAGS_NONE,
                                                  NULL, NULL, NULL, NULL);
}

static void
shell_brightness_service_dispose (GObject *object)
{
  ShellBrightnessService *self = SHELL_BRIGHTNESS_SERVICE (object);

  if (self->name_id != 0)
    {
      g_bus_unown_name (self->name_id);
      self->name_id = 0;
    }

  if (self->skeleton != NULL)
    {
      unexport_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton), self);
      g_clear_object (&self->skeleton);
    }

  g_clear_object (&self->manager);

  G_OBJECT_CLASS (shell_brightness_service_parent_class)->dispose (object);
}

static void
shell_brightness_service_class_init (ShellBrightnessServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = shell_brightness_service_set_property;
  object_class->get_property = shell_brightness_service_get_property;
  object_class->constructed = shell_brightness_service_constructed;
  object_class->dispose = shell_brightness_service_dispose;

  /**
   * ShellBrightnessService:manager:
   *
   * The brightness manager the interface controls.
   */
  br_props[BR_PROP_MANAGER] =
    g_param_spec_object ("manager", NULL, NULL,
                         SHELL_TYPE_BRIGHTNESS_MANAGER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, BR_N_PROPS, br_props);
}

static void
shell_brightness_service_init (ShellBrightnessService *self)
{
}

/**
 * shell_brightness_service_new:
 * @manager: the brightness manager
 *
 * Creates the service, exports org.gnome.Shell.Brightness at
 * /org/gnome/Shell/Brightness and owns org.gnome.Shell.Brightness on the
 * session bus. src/ui/main.c creates the only instance.
 *
 * Returns: (transfer full): a new #ShellBrightnessService
 */
ShellBrightnessService *
shell_brightness_service_new (ShellBrightnessManager *manager)
{
  g_return_val_if_fail (SHELL_IS_BRIGHTNESS_MANAGER (manager), NULL);

  return g_object_new (SHELL_TYPE_BRIGHTNESS_SERVICE,
                       "manager", manager,
                       NULL);
}
