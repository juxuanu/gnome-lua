/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * extensions-service.c: org.gnome.Shell.Extensions for the clients of the
 * extensions (gnome-extensions, the Extensions application, the browser
 * connector). Port of js/dbusServices/extensions/extensionsService.js and
 * main.js.
 *
 * The service exports org.gnome.Shell.Extensions at /org/gnome/Shell/Extensions
 * and forwards the calls, the signal and the properties to the extension
 * manager of the shell, which exports the same interface at /org/gnome/Shell
 * (src/ui/shell-dbus.c). What it adds is the window of the preferences of an
 * extension: OpenExtensionPrefs () and LaunchExtensionPrefs () open it here,
 * in a process of its own, so that no toolkit is loaded into the compositor
 * and a crash of the window does not take the session down.
 *
 * Differences from the JS version:
 *
 *  - The window is made from the declarative `prefs.lua` of the extension
 *    (extension-prefs-window.c); no code of the extension runs here but the
 *    sandboxed file that returns the data.
 *  - GTK is initialized when the first window is opened, not at startup, so
 *    that the service answers the other calls on a session without a display.
 *  - A call that arrives while org.gnome.Shell is not on the bus is answered
 *    with the error of the bus instead of waiting for the shell.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <limits.h>

#include <glib/gi18n.h>
#include <shew-external-window.h>

#include "extension-prefs-window.h"
#include "extensions-service.h"
#include "extensions-service-dbus.h"
#include "shell-lua-extension-private.h"

#define SHELL_BUS_NAME "org.gnome.Shell"
#define SHELL_OBJECT_PATH "/org/gnome/Shell"
#define EXTENSIONS_OBJECT_PATH "/org/gnome/Shell/Extensions"

struct _ExtService
{
  ShellDBusService parent;

  ShellDBusExtensions *skeleton;     /* borrowed from the base class */
  ShellDBusExtensions *proxy;
  GCancellable *cancellable;
  gboolean syncing;                  /* the skeleton is being updated from the proxy */

  ExtPrefsWindow *prefs_window;      /* weak */
  gboolean opening_prefs;
};

G_DEFINE_FINAL_TYPE (ExtService, ext_service, SHELL_TYPE_DBUS_SERVICE)

typedef struct
{
  ExtService *service;
  GDBusMethodInvocation *invocation;
} CallData;

static CallData *
call_data_new (ExtService            *self,
               GDBusMethodInvocation *invocation)
{
  CallData *data = g_new0 (CallData, 1);

  data->service = g_object_ref (self);
  data->invocation = invocation;

  return data;
}

static void
call_data_free (CallData *data)
{
  g_object_unref (data->service);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CallData, call_data_free)

/* ------------------------------------------------------------------------ */
/* Forwarding to the shell                                                   */
/* ------------------------------------------------------------------------ */

static void
forward_cb (GObject      *source,
            GAsyncResult *result,
            gpointer      user_data)
{
  g_autoptr (CallData) data = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = NULL;

  reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
  if (reply == NULL)
    {
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (data->service),
                                       data->invocation, error);
      return;
    }

  /* The reply of the shell has the signature of the reply of the service */
  shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (data->service),
                                       data->invocation);
  g_dbus_method_invocation_return_value (data->invocation, reply);
}

static void
forward_call (ExtService            *self,
              GDBusMethodInvocation *invocation,
              const char            *method,
              GVariant              *parameters,
              int                    timeout_msec)
{
  g_dbus_proxy_call (G_DBUS_PROXY (self->proxy), method, parameters,
                     G_DBUS_CALL_FLAGS_NONE, timeout_msec, self->cancellable,
                     forward_cb, call_data_new (self, invocation));
}

#define FORWARD_NO_ARGS(function, method)                                      \
  static gboolean                                                              \
  handle_##function (ShellDBusExtensions    *skeleton,                         \
                     GDBusMethodInvocation  *invocation,                       \
                     ExtService             *self)                             \
  {                                                                            \
    forward_call (self, invocation, method, NULL, -1);                         \
    return TRUE;                                                               \
  }

#define FORWARD_UUID(function, method, timeout)                                \
  static gboolean                                                              \
  handle_##function (ShellDBusExtensions    *skeleton,                         \
                     GDBusMethodInvocation  *invocation,                       \
                     const char             *uuid,                             \
                     ExtService             *self)                             \
  {                                                                            \
    forward_call (self, invocation, method, g_variant_new ("(s)", uuid),       \
                  timeout);                                                    \
    return TRUE;                                                               \
  }

FORWARD_NO_ARGS (list_extensions, "ListExtensions")
FORWARD_NO_ARGS (check_for_updates, "CheckForUpdates")
FORWARD_UUID (get_extension_info, "GetExtensionInfo", -1)
FORWARD_UUID (get_extension_errors, "GetExtensionErrors", -1)
/* The user answers a dialog of the shell: wait for it as long as needed */
FORWARD_UUID (install_remote_extension, "InstallRemoteExtension", G_MAXINT)
FORWARD_UUID (uninstall_extension, "UninstallExtension", -1)
FORWARD_UUID (reload_extension, "ReloadExtension", -1)
FORWARD_UUID (enable_extension, "EnableExtension", -1)
FORWARD_UUID (disable_extension, "DisableExtension", -1)

/* ------------------------------------------------------------------------ */
/* The window of the preferences                                             */
/* ------------------------------------------------------------------------ */

typedef struct
{
  CallData *call;
  char *parent_window;
  gboolean modal;
  gboolean launch;     /* LaunchExtensionPrefs (), not OpenExtensionPrefs () */
} PrefsRequest;

static void
prefs_request_free (PrefsRequest *request)
{
  call_data_free (request->call);
  g_free (request->parent_window);
  g_free (request);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PrefsRequest, prefs_request_free)

static void
return_prefs_reply (PrefsRequest *request)
{
  ExtService *self = request->call->service;

  shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (self), request->call->invocation);
  if (request->launch)
    shell_dbus_extensions_complete_launch_extension_prefs (self->skeleton, request->call->invocation);
  else
    shell_dbus_extensions_complete_open_extension_prefs (self->skeleton, request->call->invocation);
}

static gboolean
on_prefs_close_request (GtkWindow  *window,
                        ExtService *self)
{
  shell_dbus_service_release (SHELL_DBUS_SERVICE (self));
  g_clear_weak_pointer (&self->prefs_window);

  return FALSE;
}

static void
on_prefs_realize (GtkWidget *widget,
                  gpointer   user_data)
{
  ShewExternalWindow *external = g_object_get_data (G_OBJECT (widget), "external-window");
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (widget));

  if (external != NULL && surface != NULL)
    shew_external_window_set_parent_of (external, surface);
}

static void
show_prefs_window (PrefsRequest      *request,
                   ShellLuaExtension *extension)
{
  ExtService *self = request->call->service;
  ExtPrefsWindow *window = ext_prefs_window_new (extension);

  g_set_weak_pointer (&self->prefs_window, window);

  if (request->parent_window != NULL && *request->parent_window != '\0')
    {
      ShewExternalWindow *external = shew_external_window_new_from_handle (request->parent_window);

      if (external != NULL)
        {
          g_object_set_data_full (G_OBJECT (window), "external-window", external, g_object_unref);
          g_signal_connect (window, "realize", G_CALLBACK (on_prefs_realize), NULL);
        }
    }

  if (request->modal)
    gtk_window_set_modal (GTK_WINDOW (window), TRUE);

  g_signal_connect (window, "close-request", G_CALLBACK (on_prefs_close_request), self);
  shell_dbus_service_hold (SHELL_DBUS_SERVICE (self));

  gtk_window_present (GTK_WINDOW (window));
}

static void
get_info_for_prefs_cb (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr (PrefsRequest) request = user_data;
  ExtService *self = request->call->service;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) info = NULL;
  g_autoptr (ShellLuaExtension) extension = NULL;
  g_autoptr (GFile) dir = NULL;
  const char *path = NULL;
  double type = SHELL_LUA_EXTENSION_TYPE_PER_USER;

  self->opening_prefs = FALSE;

  if (!shell_dbus_extensions_call_get_extension_info_finish (SHELL_DBUS_EXTENSIONS (source),
                                                             &info, result, &error))
    {
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self), request->call->invocation, error);
      return;
    }

  g_variant_lookup (info, "path", "&s", &path);
  g_variant_lookup (info, "type", "d", &type);
  if (path == NULL || *path == '\0')
    {
      g_set_error_literal (&error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                           "The extension is not installed");
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self), request->call->invocation, error);
      return;
    }

  dir = g_file_new_for_path (path);
  extension = shell_lua_extension_new_from_dir (dir, (ShellLuaExtensionType) type, &error);
  if (extension == NULL)
    {
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self), request->call->invocation, error);
      return;
    }

  show_prefs_window (request, extension);
  return_prefs_reply (request);
}

static void
open_prefs (ExtService            *self,
            GDBusMethodInvocation *invocation,
            const char            *uuid,
            const char            *parent_window,
            GVariant              *options,
            gboolean               launch)
{
  g_autoptr (GError) error = NULL;
  PrefsRequest *request;
  gboolean modal = FALSE;

  if (self->prefs_window != NULL || self->opening_prefs)
    {
      g_set_error_literal (&error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                           "Already showing a prefs dialog");
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self), invocation, error);
      return;
    }

  /* GTK is only needed from here on */
  if (!gtk_init_check ())
    {
      g_set_error_literal (&error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                           "Cannot open the display to show the preferences");
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self), invocation, error);
      return;
    }
  adw_init ();

  if (options != NULL)
    g_variant_lookup (options, "modal", "b", &modal);

  request = g_new0 (PrefsRequest, 1);
  request->call = call_data_new (self, invocation);
  request->parent_window = g_strdup (parent_window);
  request->modal = modal;
  request->launch = launch;

  self->opening_prefs = TRUE;
  shell_dbus_extensions_call_get_extension_info (self->proxy, uuid, self->cancellable,
                                                 get_info_for_prefs_cb, request);
}

static gboolean
handle_launch_extension_prefs (ShellDBusExtensions   *skeleton,
                               GDBusMethodInvocation *invocation,
                               const char            *uuid,
                               ExtService            *self)
{
  open_prefs (self, invocation, uuid, "", NULL, TRUE);
  return TRUE;
}

static gboolean
handle_open_extension_prefs (ShellDBusExtensions   *skeleton,
                             GDBusMethodInvocation *invocation,
                             const char            *uuid,
                             const char            *parent_window,
                             GVariant              *options,
                             ExtService            *self)
{
  open_prefs (self, invocation, uuid, parent_window, options, FALSE);
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Properties and signals                                                    */
/* ------------------------------------------------------------------------ */

static void
sync_skeleton_from_proxy (ExtService *self)
{
  self->syncing = TRUE;
  shell_dbus_extensions_set_shell_version (self->skeleton,
                                           shell_dbus_extensions_get_shell_version (self->proxy));
  shell_dbus_extensions_set_user_extensions_enabled (self->skeleton,
                                                     shell_dbus_extensions_get_user_extensions_enabled (self->proxy));
  self->syncing = FALSE;
}

static void
on_proxy_property_notify (GObject    *proxy,
                          GParamSpec *pspec,
                          ExtService *self)
{
  sync_skeleton_from_proxy (self);
}

/* A client wrote UserExtensionsEnabled: the shell holds the value */
static void
on_skeleton_user_extensions_enabled_notify (GObject    *skeleton,
                                            GParamSpec *pspec,
                                            ExtService *self)
{
  if (self->syncing)
    return;

  shell_dbus_extensions_set_user_extensions_enabled (self->proxy,
                                                     shell_dbus_extensions_get_user_extensions_enabled (self->skeleton));
}

static void
on_proxy_extension_state_changed (ShellDBusExtensions *proxy,
                                  const char          *uuid,
                                  GVariant            *state,
                                  ExtService          *self)
{
  shell_dbus_extensions_emit_extension_state_changed (self->skeleton, uuid, state);
}

/* ------------------------------------------------------------------------ */
/* Object                                                                    */
/* ------------------------------------------------------------------------ */

static void
ext_service_constructed (GObject *object)
{
  ExtService *self = EXT_SERVICE (object);
  g_autoptr (GError) error = NULL;

  G_OBJECT_CLASS (ext_service_parent_class)->constructed (object);

  self->skeleton = SHELL_DBUS_EXTENSIONS (shell_dbus_service_get_skeleton (SHELL_DBUS_SERVICE (self)));
  self->cancellable = g_cancellable_new ();

  self->proxy = shell_dbus_extensions_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                              G_DBUS_PROXY_FLAGS_NONE,
                                                              SHELL_BUS_NAME,
                                                              SHELL_OBJECT_PATH,
                                                              NULL, &error);
  if (self->proxy == NULL)
    {
      /* The service is of no use without the shell: it exits when it is idle */
      g_warning ("Failed to create the proxy of the extension manager: %s", error->message);
      return;
    }

  sync_skeleton_from_proxy (self);
  g_signal_connect (self->proxy, "notify::shell-version",
                    G_CALLBACK (on_proxy_property_notify), self);
  g_signal_connect (self->proxy, "notify::user-extensions-enabled",
                    G_CALLBACK (on_proxy_property_notify), self);
  g_signal_connect (self->proxy, "extension-state-changed",
                    G_CALLBACK (on_proxy_extension_state_changed), self);

  g_signal_connect (self->skeleton, "notify::user-extensions-enabled",
                    G_CALLBACK (on_skeleton_user_extensions_enabled_notify), self);
  g_signal_connect (self->skeleton, "handle-list-extensions",
                    G_CALLBACK (handle_list_extensions), self);
  g_signal_connect (self->skeleton, "handle-get-extension-info",
                    G_CALLBACK (handle_get_extension_info), self);
  g_signal_connect (self->skeleton, "handle-get-extension-errors",
                    G_CALLBACK (handle_get_extension_errors), self);
  g_signal_connect (self->skeleton, "handle-install-remote-extension",
                    G_CALLBACK (handle_install_remote_extension), self);
  g_signal_connect (self->skeleton, "handle-uninstall-extension",
                    G_CALLBACK (handle_uninstall_extension), self);
  g_signal_connect (self->skeleton, "handle-reload-extension",
                    G_CALLBACK (handle_reload_extension), self);
  g_signal_connect (self->skeleton, "handle-enable-extension",
                    G_CALLBACK (handle_enable_extension), self);
  g_signal_connect (self->skeleton, "handle-disable-extension",
                    G_CALLBACK (handle_disable_extension), self);
  g_signal_connect (self->skeleton, "handle-launch-extension-prefs",
                    G_CALLBACK (handle_launch_extension_prefs), self);
  g_signal_connect (self->skeleton, "handle-open-extension-prefs",
                    G_CALLBACK (handle_open_extension_prefs), self);
  g_signal_connect (self->skeleton, "handle-check-for-updates",
                    G_CALLBACK (handle_check_for_updates), self);
}

static void
ext_service_dispose (GObject *object)
{
  ExtService *self = EXT_SERVICE (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  if (self->skeleton != NULL)
    g_signal_handlers_disconnect_by_data (self->skeleton, self);
  self->skeleton = NULL;

  if (self->proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->proxy, self);
  g_clear_object (&self->proxy);

  g_clear_weak_pointer (&self->prefs_window);

  G_OBJECT_CLASS (ext_service_parent_class)->dispose (object);
}

static void
ext_service_class_init (ExtServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = ext_service_constructed;
  object_class->dispose = ext_service_dispose;
}

static void
ext_service_init (ExtService *self)
{
}

/**
 * ext_service_new:
 *
 * Creates the service. The proxy of the extension manager of the shell is made
 * synchronously.
 *
 * Returns: (transfer full): a new #ExtService
 */
ExtService *
ext_service_new (void)
{
  g_autoptr (ShellDBusExtensions) skeleton = shell_dbus_extensions_skeleton_new ();

  return g_object_new (EXT_TYPE_SERVICE,
                       "skeleton", skeleton,
                       "object-path", EXTENSIONS_OBJECT_PATH,
                       NULL);
}
