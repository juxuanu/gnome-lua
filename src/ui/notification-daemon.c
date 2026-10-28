/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * notification-daemon.c: The notification services of the shell. Port of
 * js/ui/notificationDaemon.js.
 *
 * Two D-Bus objects are exported on the session bus with gdbus-codegen
 * skeletons:
 *
 *  - /org/freedesktop/Notifications (org.freedesktop.Notifications): the
 *    notification helper process (src/dbus-services/notification-service.c) owns the bus
 *    name and forwards the calls of the applications to the shell, adding the
 *    `x-shell-sender` and `x-shell-sender-pid` hints. The signals are sent to
 *    the helper only.
 *  - /org/gtk/Notifications (org.gtk.Notifications), whose bus name the shell
 *    owns. The notifications of GApplication are saved in the persistent
 *    state `notifications` and restored at startup.
 *
 * The types of the JS file are private to this file, except
 * #ShellNotificationDaemon (`NotificationDaemon`), which owns the two
 * daemons:
 *
 *  - ShellFdoNotificationDaemon (`FdoNotificationDaemon`),
 *    ShellFdoNotificationDaemonSource (`FdoNotificationDaemonSource`);
 *  - ShellGtkNotificationDaemon (`GtkNotificationDaemon`),
 *    ShellGtkNotificationDaemonAppSource (`GtkNotificationDaemonAppSource`),
 *    ShellGtkNotificationDaemonNotification
 *    (`GtkNotificationDaemonNotification`).
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The message tray is the construct-only property
 *    #ShellNotificationDaemon:message-tray (`Main.messageTray`).
 *  - What the sources did through `Main.overview.hide ()` and
 *    `Main.panel.close_calendar ()` when they bring an application up is the
 *    signal #ShellNotificationDaemon::app-opened, which ShellMain connects.
 *  - `Notify` validates the `image-data` hint instead of passing it on and
 *    provoking a critical warning: invalid image data is ignored. A hint that
 *    has the wrong D-Bus type is ignored too (JS coerced it); the truthiness
 *    rules of JS are kept for `resident` and `transient`.
 *  - `AddNotification` without a string `title` fails with
 *    org.freedesktop.DBus.Error.InvalidArgs (the JS method threw and never
 *    replied). The notifications of the saved state that are invalid are
 *    skipped.
 *  - The error of `AddNotification` for an unknown application is returned as
 *    org.gtk.Notifications.Error.InvalidApp without going through the
 *    error domain of js/misc/dbusErrors.js.
 *  - The notifications that the Gtk source keeps, and the sources of the
 *    daemon, are in insertion order (the JS object listed integer-like
 *    notification ids first).
 *  - After ShellGlobal::shutdown the objects are unexported and the calls
 *    that still arrive fail.
 *  - A source of a disabled application (policy `enable` is false) that the
 *    daemon does not index (the Fdo sources without a pid) is only kept alive
 *    by its notifications, as the JS garbage collector would have collected
 *    it; destroying its last notification frees it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

#include "misc/dbus-utils.h"
#include "shell-app.h"
#include "shell-app-system.h"
#include "shell-global.h"
#include "shell-util.h"
#include "shell-window-tracker.h"
#include "ui/message-tray.h"
#include "ui/notification-daemon.h"

#include "notifications-dbus.h"

#define SERVER_NAME "gnome-shell"
#define SERVER_VENDOR "GNOME"
#define SERVER_SPEC_VERSION "1.2"

#define FDO_OBJECT_PATH "/org/freedesktop/Notifications"
#define GTK_OBJECT_PATH "/org/gtk/Notifications"
#define GTK_BUS_NAME "org.gtk.Notifications"
#define GTK_ERROR_INVALID_APP "org.gtk.Notifications.Error.InvalidApp"

#define NOTIFICATION_SERIALIZED_NAME "notifications"
#define NOTIFICATION_SERIALIZED_FORMAT "a(sa(sv))"

/* NotificationClosedReason */
typedef enum
{
  CLOSED_REASON_EXPIRED = 1,
  CLOSED_REASON_DISMISSED = 2,
  CLOSED_REASON_APP_CLOSED = 3,
  CLOSED_REASON_UNDEFINED = 4,
} NotificationClosedReason;

/* Urgency of the org.freedesktop.Notifications spec */
typedef enum
{
  FDO_URGENCY_LOW = 0,
  FDO_URGENCY_NORMAL = 1,
  FDO_URGENCY_CRITICAL = 2,
} FdoUrgency;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

/* Exports @skeleton on the session bus; returns the connection or %NULL */
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

static void
return_shutting_down (GDBusMethodInvocation *invocation)
{
  g_dbus_method_invocation_return_dbus_error (invocation,
                                              "org.freedesktop.DBus.Error.Failed",
                                              "The shell is shutting down");
}

/* The JS truthiness of a hint */
static gboolean
variant_is_truthy (GVariant *value)
{
  if (value == NULL)
    return FALSE;

  switch (g_variant_classify (value))
    {
    case G_VARIANT_CLASS_BOOLEAN:
      return g_variant_get_boolean (value);

    case G_VARIANT_CLASS_BYTE:
      return g_variant_get_byte (value) != 0;

    case G_VARIANT_CLASS_INT16:
      return g_variant_get_int16 (value) != 0;

    case G_VARIANT_CLASS_UINT16:
      return g_variant_get_uint16 (value) != 0;

    case G_VARIANT_CLASS_INT32:
      return g_variant_get_int32 (value) != 0;

    case G_VARIANT_CLASS_UINT32:
      return g_variant_get_uint32 (value) != 0;

    case G_VARIANT_CLASS_INT64:
      return g_variant_get_int64 (value) != 0;

    case G_VARIANT_CLASS_UINT64:
      return g_variant_get_uint64 (value) != 0;

    case G_VARIANT_CLASS_DOUBLE:
      return g_variant_get_double (value) != 0.0;

    case G_VARIANT_CLASS_STRING:
      return *g_variant_get_string (value, NULL) != '\0';

    default:
      return TRUE;
    }
}

/* Looks up @key in the hints; returns the value if it is not falsy */
static GVariant *
lookup_truthy (GVariant   *hints,
               const char *key)
{
  GVariant *value = g_variant_lookup_value (hints, key, NULL);

  if (!variant_is_truthy (value))
    g_clear_pointer (&value, g_variant_unref);

  return value;
}

static gboolean
lookup_hint_truthy (GVariant   *hints,
                    const char *key)
{
  g_autoptr (GVariant) value = g_variant_lookup_value (hints, key, NULL);

  return variant_is_truthy (value);
}

/* The value of a numeric hint of any integer type */
static gboolean
lookup_hint_number (GVariant   *hints,
                    const char *key,
                    gint64     *number)
{
  g_autoptr (GVariant) value = g_variant_lookup_value (hints, key, NULL);

  if (value == NULL)
    return FALSE;

  switch (g_variant_classify (value))
    {
    case G_VARIANT_CLASS_BYTE:
      *number = g_variant_get_byte (value);
      return TRUE;

    case G_VARIANT_CLASS_INT16:
      *number = g_variant_get_int16 (value);
      return TRUE;

    case G_VARIANT_CLASS_UINT16:
      *number = g_variant_get_uint16 (value);
      return TRUE;

    case G_VARIANT_CLASS_INT32:
      *number = g_variant_get_int32 (value);
      return TRUE;

    case G_VARIANT_CLASS_UINT32:
      *number = g_variant_get_uint32 (value);
      return TRUE;

    case G_VARIANT_CLASS_INT64:
      *number = g_variant_get_int64 (value);
      return TRUE;

    default:
      return FALSE;
    }
}

/* `_iconForNotificationData ()` */
static GIcon *
icon_for_notification_data (const char *icon)
{
  if (icon == NULL || *icon == '\0')
    return NULL;

  if (g_str_has_prefix (icon, "file://"))
    {
      g_autoptr (GFile) file = g_file_new_for_uri (icon);

      return G_ICON (g_file_icon_new (file));
    }
  else if (icon[0] == '/')
    {
      g_autoptr (GFile) file = g_file_new_for_path (icon);

      return G_ICON (g_file_icon_new (file));
    }

  return G_ICON (g_themed_icon_new (icon));
}

/* The pixbuf of an `image-data` hint, (iiibiiay), or %NULL if it is invalid */
static GdkPixbuf *
pixbuf_from_image_data (GVariant *image_data)
{
  gint32 width, height, rowstride, bits_per_sample, n_channels;
  gboolean has_alpha;
  g_autoptr (GVariant) data = NULL;
  const guint8 *bytes;
  guint64 row_min, required;
  gsize len;
  guint8 *copy;
  GdkPixbuf *pixbuf;

  if (!g_variant_is_of_type (image_data, G_VARIANT_TYPE ("(iiibiiay)")))
    return NULL;

  g_variant_get (image_data, "(iiibii@ay)",
                 &width, &height, &rowstride, &has_alpha,
                 &bits_per_sample, &n_channels, &data);

  if (bits_per_sample != 8 || width <= 0 || height <= 0 || rowstride < 0)
    return NULL;

  row_min = (guint64) width * (has_alpha ? 4 : 3);
  if ((guint64) rowstride < row_min)
    return NULL;

  required = (guint64) (height - 1) * (guint64) rowstride + row_min;
  bytes = g_variant_get_fixed_array (data, &len, sizeof (guint8));
  if (bytes == NULL || len < required)
    return NULL;

  copy = g_memdup2 (bytes, len);
  pixbuf = shell_util_create_pixbuf_from_data (copy, len, GDK_COLORSPACE_RGB,
                                               has_alpha, bits_per_sample,
                                               width, height, rowstride);
  if (pixbuf == NULL)
    g_free (copy);

  return pixbuf;
}

/* `_imageForNotificationData ()`, with the compatibility with the hints of
 * the older versions of the spec */
static GIcon *
image_for_notification_data (GVariant *hints)
{
  g_autoptr (GVariant) image_path = lookup_truthy (hints, "image-path");
  g_autoptr (GVariant) image_data = lookup_truthy (hints, "image-data");

  /* version 1.1 of the spec */
  if (image_path == NULL)
    image_path = lookup_truthy (hints, "image_path");

  if (image_data == NULL)
    {
      image_data = lookup_truthy (hints, "image_data");

      /* early versions of the spec; 'icon_data' should only be used if
       * 'image-path' is not available */
      if (image_data == NULL && image_path == NULL)
        image_data = lookup_truthy (hints, "icon_data");
    }

  if (image_data != NULL)
    {
      g_autoptr (GdkPixbuf) pixbuf = pixbuf_from_image_data (image_data);

      return pixbuf != NULL ? G_ICON (g_object_ref (pixbuf)) : NULL;
    }
  else if (image_path != NULL &&
           g_variant_is_of_type (image_path, G_VARIANT_TYPE_STRING))
    {
      return icon_for_notification_data (g_variant_get_string (image_path, NULL));
    }

  return NULL;
}

/* ------------------------------------------------------------------------ */
/* ShellFdoNotificationDaemonSource                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_FDO_NOTIFICATION_DAEMON_SOURCE (shell_fdo_notification_daemon_source_get_type ())
G_DECLARE_FINAL_TYPE (ShellFdoNotificationDaemonSource,
                      shell_fdo_notification_daemon_source,
                      SHELL, FDO_NOTIFICATION_DAEMON_SOURCE, ShellSource)

enum
{
  FDO_SOURCE_PROP_0,

  FDO_SOURCE_PROP_APP,
  FDO_SOURCE_PROP_SENDER,

  FDO_SOURCE_N_PROPS
};

static GParamSpec *fdo_source_props[FDO_SOURCE_N_PROPS] = { NULL, };

enum
{
  FDO_SOURCE_APP_OPENED,

  FDO_SOURCE_LAST_SIGNAL
};

static guint fdo_source_signals[FDO_SOURCE_LAST_SIGNAL] = { 0, };

struct _ShellFdoNotificationDaemonSource
{
  ShellSource parent;

  ShellApp *app;                    /* (nullable) */
  char *sender;                     /* (nullable) */
  char *pid_and_name_key;           /* (nullable): the key in the daemon's table */
  guint name_watcher_id;
};

G_DEFINE_FINAL_TYPE (ShellFdoNotificationDaemonSource,
                     shell_fdo_notification_daemon_source,
                     SHELL_TYPE_SOURCE)

static void
fdo_source_unwatch_sender (ShellFdoNotificationDaemonSource *self)
{
  if (self->name_watcher_id != 0)
    {
      g_bus_unwatch_name (self->name_watcher_id);
      self->name_watcher_id = 0;
    }
}

static void
fdo_source_on_shutdown (ShellGlobal                      *global,
                        ShellFdoNotificationDaemonSource *self)
{
  /* The senders may vanish while the shell is shutting down */
  fdo_source_unwatch_sender (self);
}

static void
fdo_source_name_vanished_cb (GDBusConnection *connection,
                             const char      *name,
                             gpointer         user_data)
{
  ShellFdoNotificationDaemonSource *self = user_data;

  /* Destroy the notification source when its sender is removed from DBus.
   * Only do so if the source has an app to avoid removing "notify-send"
   * sources, senders of which are removed from DBus immediately. Sender being
   * removed from DBus would normally result in a tray icon being removed, so
   * allow the code path that handles the tray icon being removed to handle
   * that case. */
  if (self->app != NULL)
    shell_source_destroy (SHELL_SOURCE (self), SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);
}

static void
fdo_source_open (ShellSource *source)
{
  ShellFdoNotificationDaemonSource *self = SHELL_FDO_NOTIFICATION_DAEMON_SOURCE (source);

  /* openApp () */
  if (self->app != NULL)
    {
      shell_app_activate (self->app);
      g_signal_emit (self, fdo_source_signals[FDO_SOURCE_APP_OPENED], 0);
    }

  shell_source_destroy_non_resident_notifications (source);
}

static void
fdo_source_destroy (ShellSource                      *source,
                    ShellNotificationDestroyedReason  reason)
{
  ShellFdoNotificationDaemonSource *self = SHELL_FDO_NOTIFICATION_DAEMON_SOURCE (source);
  ShellSourceClass *parent_class = SHELL_SOURCE_CLASS (shell_fdo_notification_daemon_source_parent_class);

  fdo_source_unwatch_sender (self);

  parent_class->destroy (source, reason);
}

/* `processNotification ()` */
static void
fdo_source_process_notification (ShellFdoNotificationDaemonSource *self,
                                 ShellNotification                *notification,
                                 const char                       *app_name,
                                 GIcon                            *app_icon)
{
  ShellMessageSource *message_source = SHELL_MESSAGE_SOURCE (self);
  ShellWindowTracker *tracker;

  if (self->app == NULL && app_name != NULL && *app_name != '\0')
    shell_message_source_set_title (message_source, app_name);

  if (self->app == NULL && app_icon != NULL)
    shell_message_source_set_icon (message_source, app_icon);

  tracker = shell_window_tracker_get_default ();

  /* Acknowledge notifications that are resident and their app has the
   * current focus so that we don't show a banner. */
  if (shell_notification_get_resident (notification) && self->app != NULL &&
      shell_window_tracker_get_focus_app (tracker) == self->app)
    shell_notification_set_acknowledged (notification, TRUE);

  shell_source_add_notification (SHELL_SOURCE (self), notification);
}

static void
shell_fdo_notification_daemon_source_get_property (GObject    *object,
                                                   guint       prop_id,
                                                   GValue     *value,
                                                   GParamSpec *pspec)
{
  ShellFdoNotificationDaemonSource *self = SHELL_FDO_NOTIFICATION_DAEMON_SOURCE (object);

  switch (prop_id)
    {
    case FDO_SOURCE_PROP_APP:
      g_value_set_object (value, self->app);
      break;

    case FDO_SOURCE_PROP_SENDER:
      g_value_set_string (value, self->sender);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_fdo_notification_daemon_source_set_property (GObject      *object,
                                                   guint         prop_id,
                                                   const GValue *value,
                                                   GParamSpec   *pspec)
{
  ShellFdoNotificationDaemonSource *self = SHELL_FDO_NOTIFICATION_DAEMON_SOURCE (object);

  switch (prop_id)
    {
    case FDO_SOURCE_PROP_APP:
      g_set_object (&self->app, g_value_get_object (value));
      break;

    case FDO_SOURCE_PROP_SENDER:
      g_free (self->sender);
      self->sender = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_fdo_notification_daemon_source_constructed (GObject *object)
{
  ShellFdoNotificationDaemonSource *self = SHELL_FDO_NOTIFICATION_DAEMON_SOURCE (object);
  ShellMessageSource *message_source = SHELL_MESSAGE_SOURCE (self);

  G_OBJECT_CLASS (shell_fdo_notification_daemon_source_parent_class)->constructed (object);

  if (self->app != NULL)
    {
      shell_message_source_set_title (message_source, shell_app_get_name (self->app));
      shell_message_source_set_icon (message_source, shell_app_get_icon (self->app));
    }

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (fdo_source_on_shutdown), self, 0);

  if (self->sender != NULL && g_dbus_is_name (self->sender))
    {
      self->name_watcher_id = g_bus_watch_name (G_BUS_TYPE_SESSION,
                                                self->sender,
                                                G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                NULL,
                                                fdo_source_name_vanished_cb,
                                                self, NULL);
    }
}

static void
shell_fdo_notification_daemon_source_dispose (GObject *object)
{
  ShellFdoNotificationDaemonSource *self = SHELL_FDO_NOTIFICATION_DAEMON_SOURCE (object);

  fdo_source_unwatch_sender (self);
  g_clear_object (&self->app);

  G_OBJECT_CLASS (shell_fdo_notification_daemon_source_parent_class)->dispose (object);
}

static void
shell_fdo_notification_daemon_source_finalize (GObject *object)
{
  ShellFdoNotificationDaemonSource *self = SHELL_FDO_NOTIFICATION_DAEMON_SOURCE (object);

  g_clear_pointer (&self->sender, g_free);
  g_clear_pointer (&self->pid_and_name_key, g_free);

  G_OBJECT_CLASS (shell_fdo_notification_daemon_source_parent_class)->finalize (object);
}

static void
shell_fdo_notification_daemon_source_class_init (ShellFdoNotificationDaemonSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellSourceClass *source_class = SHELL_SOURCE_CLASS (klass);

  object_class->get_property = shell_fdo_notification_daemon_source_get_property;
  object_class->set_property = shell_fdo_notification_daemon_source_set_property;
  object_class->constructed = shell_fdo_notification_daemon_source_constructed;
  object_class->dispose = shell_fdo_notification_daemon_source_dispose;
  object_class->finalize = shell_fdo_notification_daemon_source_finalize;

  source_class->open = fdo_source_open;
  source_class->destroy = fdo_source_destroy;

  fdo_source_props[FDO_SOURCE_PROP_APP] =
    g_param_spec_object ("app", NULL, NULL,
                         SHELL_TYPE_APP,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  fdo_source_props[FDO_SOURCE_PROP_SENDER] =
    g_param_spec_string ("sender", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, FDO_SOURCE_N_PROPS, fdo_source_props);

  fdo_source_signals[FDO_SOURCE_APP_OPENED] =
    g_signal_new ("app-opened",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_fdo_notification_daemon_source_init (ShellFdoNotificationDaemonSource *self)
{
}

static ShellFdoNotificationDaemonSource *
fdo_source_new (const char *sender,
                ShellApp   *app)
{
  g_autoptr (ShellNotificationPolicy) policy = shell_notification_policy_new_for_app (app);

  return g_object_new (SHELL_TYPE_FDO_NOTIFICATION_DAEMON_SOURCE,
                       "policy", policy,
                       "app", app,
                       "sender", sender,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellFdoNotificationDaemon                                                */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_FDO_NOTIFICATION_DAEMON (shell_fdo_notification_daemon_get_type ())
G_DECLARE_FINAL_TYPE (ShellFdoNotificationDaemon, shell_fdo_notification_daemon,
                      SHELL, FDO_NOTIFICATION_DAEMON, GObject)

enum
{
  FDO_PROP_0,

  FDO_PROP_MESSAGE_TRAY,

  FDO_N_PROPS
};

static GParamSpec *fdo_props[FDO_N_PROPS] = { NULL, };

enum
{
  FDO_APP_OPENED,

  FDO_LAST_SIGNAL
};

static guint fdo_signals[FDO_LAST_SIGNAL] = { 0, };

struct _ShellFdoNotificationDaemon
{
  GObject parent;

  ShellMessageTray *message_tray;                 /* weak */
  ShellFreedesktopNotifications *skeleton;

  GHashTable *sources_for_app;                    /* ShellApp -> source */
  GHashTable *sources_for_pid_and_name;           /* char * -> source */
  GHashTable *notifications;                      /* id -> FdoNotificationEntry */
  guint next_notification_id;
  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellFdoNotificationDaemon, shell_fdo_notification_daemon, G_TYPE_OBJECT)

typedef struct
{
  ShellFdoNotificationDaemon *daemon;  /* handlers are disconnected in dispose () */
  guint id;
  ShellNotification *notification;
  char *sender;                        /* the sender of the first Notify () */
  gulong activated_id;
} FdoNotificationEntry;

static void
fdo_notification_entry_free (FdoNotificationEntry *entry)
{
  g_clear_object (&entry->notification);
  g_clear_pointer (&entry->sender, g_free);
  g_free (entry);
}

/* What the action handlers need to answer to the sender of Notify () */
typedef struct
{
  ShellFreedesktopNotifications *skeleton;
  guint id;
  char *action_id;
  char *sender;
} FdoActionData;

static FdoActionData *
fdo_action_data_new (ShellFreedesktopNotifications *skeleton,
                     guint                          id,
                     const char                    *action_id,
                     const char                    *sender)
{
  FdoActionData *data = g_new0 (FdoActionData, 1);

  data->skeleton = g_object_ref (skeleton);
  data->id = id;
  data->action_id = g_strdup (action_id);
  data->sender = g_strdup (sender);
  return data;
}

static void
fdo_action_data_free (FdoActionData *data)
{
  g_clear_object (&data->skeleton);
  g_free (data->action_id);
  g_free (data->sender);
  g_free (data);
}

static void
fdo_emit_notification_closed (ShellFreedesktopNotifications *skeleton,
                              const char                    *sender,
                              guint                          id,
                              guint                          reason)
{
  shell_dbus_emit_signal_to_destination (G_DBUS_INTERFACE_SKELETON (skeleton), sender,
                                         "NotificationClosed",
                                         g_variant_new ("(uu)", id, reason));
}

static void
fdo_emit_action_invoked (FdoActionData *data)
{
  shell_dbus_emit_signal_to_destination (G_DBUS_INTERFACE_SKELETON (data->skeleton),
                                         data->sender, "ActionInvoked",
                                         g_variant_new ("(us)", data->id, data->action_id));
}

static void
fdo_emit_activation_token (FdoActionData *data)
{
  g_autoptr (GAppLaunchContext) context =
    shell_global_create_app_launch_context (shell_global_get (), 0, -1);
  g_autofree char *token = g_app_launch_context_get_startup_notify_id (context, NULL, NULL);

  shell_dbus_emit_signal_to_destination (G_DBUS_INTERFACE_SKELETON (data->skeleton),
                                         data->sender, "ActivationToken",
                                         g_variant_new ("(us)", data->id,
                                                        token != NULL ? token : ""));
}

static void
fdo_action_cb (ShellNotification *notification,
               gpointer           user_data)
{
  FdoActionData *data = user_data;

  fdo_emit_activation_token (data);
  fdo_emit_action_invoked (data);
}

static void
fdo_open_source_cb (ShellNotification *notification,
                    ShellSource       *source)
{
  shell_source_open (source);
}

static void
fdo_notification_destroy_cb (ShellNotification                *notification,
                             ShellNotificationDestroyedReason  reason,
                             FdoNotificationEntry             *entry)
{
  ShellFdoNotificationDaemon *self = entry->daemon;
  g_autoptr (ShellFreedesktopNotifications) skeleton = g_object_ref (self->skeleton);
  g_autofree char *sender = g_strdup (entry->sender);
  guint id = entry->id;
  NotificationClosedReason closed_reason;

  switch (reason)
    {
    case SHELL_NOTIFICATION_DESTROYED_REASON_EXPIRED:
      closed_reason = CLOSED_REASON_EXPIRED;
      break;

    case SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED:
      closed_reason = CLOSED_REASON_DISMISSED;
      break;

    case SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED:
      closed_reason = CLOSED_REASON_APP_CLOSED;
      break;

    case SHELL_NOTIFICATION_DESTROYED_REASON_REPLACED:
    default:
      closed_reason = CLOSED_REASON_UNDEFINED;
      break;
    }

  /* Frees the entry, and drops its reference to the notification */
  g_hash_table_remove (self->notifications, GUINT_TO_POINTER (id));

  fdo_emit_notification_closed (skeleton, sender, id, closed_reason);
}

/* `_getApp ()` */
static ShellApp *
fdo_get_app (int         pid,
             const char *app_id,
             const char *app_name)
{
  ShellAppSystem *app_system = shell_app_system_get_default ();
  ShellApp *app;

  app = shell_window_tracker_get_app_from_pid (shell_window_tracker_get_default (), pid);
  if (app == NULL && app_id != NULL && *app_id != '\0')
    {
      g_autofree char *desktop_id = g_strconcat (app_id, ".desktop", NULL);

      app = shell_app_system_lookup_app (app_system, desktop_id);
    }

  if (app == NULL)
    {
      g_autofree char *desktop_id = g_strconcat (app_name, ".desktop", NULL);

      app = shell_app_system_lookup_app (app_system, desktop_id);
    }

  return app;
}

static void
fdo_source_app_opened_cb (ShellFdoNotificationDaemonSource *source,
                          ShellFdoNotificationDaemon        *self)
{
  g_signal_emit (self, fdo_signals[FDO_APP_OPENED], 0);
}

static void
fdo_source_for_app_destroy_cb (ShellSource                      *source,
                               ShellNotificationDestroyedReason  reason,
                               ShellFdoNotificationDaemon       *self)
{
  ShellFdoNotificationDaemonSource *fdo_source = SHELL_FDO_NOTIFICATION_DAEMON_SOURCE (source);

  if (fdo_source->app != NULL &&
      g_hash_table_lookup (self->sources_for_app, fdo_source->app) == source)
    g_hash_table_remove (self->sources_for_app, fdo_source->app);
}

static void
fdo_source_for_pid_and_name_destroy_cb (ShellSource                      *source,
                                        ShellNotificationDestroyedReason  reason,
                                        ShellFdoNotificationDaemon       *self)
{
  ShellFdoNotificationDaemonSource *fdo_source = SHELL_FDO_NOTIFICATION_DAEMON_SOURCE (source);

  if (fdo_source->pid_and_name_key != NULL &&
      g_hash_table_lookup (self->sources_for_pid_and_name, fdo_source->pid_and_name_key) == source)
    g_hash_table_remove (self->sources_for_pid_and_name, fdo_source->pid_and_name_key);
}

/* Returns the source associated with an app (transfer full). If no existing
 * source is found a new one is created. */
static ShellFdoNotificationDaemonSource *
fdo_get_source_for_app (ShellFdoNotificationDaemon *self,
                        const char                 *sender,
                        ShellApp                   *app)
{
  ShellFdoNotificationDaemonSource *source = g_hash_table_lookup (self->sources_for_app, app);

  if (source != NULL)
    return g_object_ref (source);

  source = fdo_source_new (sender, app);
  g_signal_connect_object (source, "app-opened",
                           G_CALLBACK (fdo_source_app_opened_cb), self, 0);

  g_hash_table_insert (self->sources_for_app, g_object_ref (app), g_object_ref (source));
  g_signal_connect (source, "destroy",
                    G_CALLBACK (fdo_source_for_app_destroy_cb), self);

  shell_message_tray_add (self->message_tray, SHELL_SOURCE (source));
  return g_object_ref (source);
}

/* Returns the source associated with a pid and the app name (transfer full).
 * If no existing source is found, a new one is created. */
static ShellFdoNotificationDaemonSource *
fdo_get_source_for_pid_and_name (ShellFdoNotificationDaemon *self,
                                 const char                 *sender,
                                 guint                       pid,
                                 const char                 *app_name)
{
  g_autofree char *key = g_strdup_printf ("%u%s", pid, app_name);
  ShellFdoNotificationDaemonSource *source = g_hash_table_lookup (self->sources_for_pid_and_name, key);

  if (source != NULL)
    return g_object_ref (source);

  source = fdo_source_new (sender, NULL);
  g_signal_connect_object (source, "app-opened",
                           G_CALLBACK (fdo_source_app_opened_cb), self, 0);

  /* Only check whether we have a PID since it's enough to identify uniquely
   * an app and "" is a valid app name. */
  if (pid != 0)
    {
      source->pid_and_name_key = g_strdup (key);
      g_hash_table_insert (self->sources_for_pid_and_name, g_strdup (key),
                           g_object_ref (source));
      g_signal_connect (source, "destroy",
                        G_CALLBACK (fdo_source_for_pid_and_name_destroy_cb), self);
    }

  /* Without a pid nothing indexes the source: the message tray and the
   * notifications keep it alive */
  shell_message_tray_add (self->message_tray, SHELL_SOURCE (source));

  return source;
}

static void
fdo_notification_set_actions (ShellFdoNotificationDaemon *self,
                              ShellNotification          *notification,
                              guint                       id,
                              const char * const         *actions,
                              const char                 *sender,
                              gboolean                   *has_default_action)
{
  guint n_actions = g_strv_length ((char **) actions);
  guint i;

  *has_default_action = FALSE;

  for (i = 0; i + 1 < n_actions; i += 2)
    {
      const char *action_id = actions[i];
      const char *label = actions[i + 1];

      if (g_strcmp0 (action_id, "default") == 0)
        {
          *has_default_action = TRUE;
        }
      else
        {
          shell_notification_add_action (notification, label,
                                         fdo_action_cb,
                                         fdo_action_data_new (self->skeleton, id,
                                                              action_id, sender),
                                         (GDestroyNotify) fdo_action_data_free);
        }
    }
}

static gboolean
fdo_on_handle_notify (ShellFreedesktopNotifications *skeleton,
                      GDBusMethodInvocation         *invocation,
                      const char                    *app_name,
                      guint                          replaces_id,
                      const char                    *app_icon,
                      const char                    *summary,
                      const char                    *body,
                      const char * const            *actions,
                      GVariant                      *hints,
                      int                            timeout,
                      ShellFdoNotificationDaemon    *self)
{
  const char *invocation_sender = g_dbus_method_invocation_get_sender (invocation);
  FdoNotificationEntry *entry = NULL;
  g_autoptr (ShellNotification) notification = NULL;
  g_autoptr (GIcon) gicon = NULL;
  g_autoptr (GIcon) source_gicon = NULL;
  g_autoptr (GFile) sound_file = NULL;
  g_autoptr (ShellNotificationSound) sound = NULL;
  g_autoptr (ShellFdoNotificationDaemonSource) source = NULL;
  const char *sound_file_path = NULL;
  const char *sound_name = NULL;
  const char *privacy_scope = NULL;
  gboolean has_default_action;
  gint64 urgency = FDO_URGENCY_NORMAL;
  guint id;

  if (self->shutting_down || self->message_tray == NULL)
    {
      return_shutting_down (invocation);
      return TRUE;
    }

  if (replaces_id != 0)
    entry = g_hash_table_lookup (self->notifications, GUINT_TO_POINTER (replaces_id));

  if (entry != NULL)
    {
      ShellSource *notification_source;

      notification = g_object_ref (entry->notification);
      notification_source = shell_notification_get_source (notification);
      if (notification_source != NULL)
        source = g_object_ref (SHELL_FDO_NOTIFICATION_DAEMON_SOURCE (notification_source));
      id = replaces_id;
    }

  if (entry == NULL || source == NULL)
    {
      const char *sender = NULL;
      const char *app_id = NULL;
      gint64 pid = 0;
      ShellApp *app;

      if (entry != NULL)
        {
          /* The notification lost its source: start over */
          g_signal_handlers_disconnect_by_data (entry->notification, entry);
          g_hash_table_remove (self->notifications, GUINT_TO_POINTER (replaces_id));
          entry = NULL;
          g_clear_object (&notification);
        }

      g_variant_lookup (hints, "x-shell-sender", "&s", &sender);
      g_variant_lookup (hints, "desktop-entry", "&s", &app_id);
      lookup_hint_number (hints, "x-shell-sender-pid", &pid);

      app = fdo_get_app ((int) pid, app_id, app_name);

      id = self->next_notification_id++;
      if (self->next_notification_id == 0)
        self->next_notification_id = 1;

      if (app != NULL)
        source = fdo_get_source_for_app (self, sender, app);
      else
        source = fdo_get_source_for_pid_and_name (self, sender, (guint) pid, app_name);

      notification = shell_notification_new (SHELL_SOURCE (source), NULL, NULL);

      entry = g_new0 (FdoNotificationEntry, 1);
      entry->daemon = self;
      entry->id = id;
      entry->notification = g_object_ref (notification);
      entry->sender = g_strdup (invocation_sender);
      g_hash_table_insert (self->notifications, GUINT_TO_POINTER (id), entry);
      g_signal_connect (notification, "destroy",
                        G_CALLBACK (fdo_notification_destroy_cb), entry);
    }

  gicon = image_for_notification_data (hints);

  if (g_variant_lookup (hints, "sound-file", "&s", &sound_file_path))
    sound_file = g_file_new_for_path (sound_file_path);
  g_variant_lookup (hints, "sound-name", "&s", &sound_name);
  sound = shell_notification_sound_new (sound_file, sound_name);

  shell_notification_set_title (notification, summary);
  shell_notification_set_body (notification, body);
  shell_notification_set_gicon (notification, gicon);
  shell_notification_set_use_body_markup (notification, TRUE);
  shell_notification_set_sound (notification, sound);
  shell_notification_set_acknowledged (notification, FALSE);

  shell_notification_clear_actions (notification);
  if (entry->activated_id != 0)
    {
      g_signal_handler_disconnect (notification, entry->activated_id);
      entry->activated_id = 0;
    }

  fdo_notification_set_actions (self, notification, id, actions,
                                invocation_sender, &has_default_action);

  if (has_default_action)
    {
      entry->activated_id =
        g_signal_connect_data (notification, "activated",
                               G_CALLBACK (fdo_action_cb),
                               fdo_action_data_new (self->skeleton, id, "default",
                                                    invocation_sender),
                               (GClosureNotify) fdo_action_data_free, 0);
    }
  else
    {
      entry->activated_id =
        g_signal_connect_object (notification, "activated",
                                 G_CALLBACK (fdo_open_source_cb), source, 0);
    }

  lookup_hint_number (hints, "urgency", &urgency);
  switch (urgency)
    {
    case FDO_URGENCY_LOW:
      shell_notification_set_urgency (notification, SHELL_NOTIFICATION_URGENCY_LOW);
      break;

    case FDO_URGENCY_NORMAL:
      shell_notification_set_urgency (notification, SHELL_NOTIFICATION_URGENCY_NORMAL);
      break;

    case FDO_URGENCY_CRITICAL:
      shell_notification_set_urgency (notification, SHELL_NOTIFICATION_URGENCY_CRITICAL);
      break;

    default:
      break;
    }

  shell_notification_set_resident (notification, lookup_hint_truthy (hints, "resident"));
  shell_notification_set_is_transient (notification, lookup_hint_truthy (hints, "transient"));

  {
    g_autoptr (GVariant) scope = lookup_truthy (hints, "x-gnome-privacy-scope");

    if (scope != NULL && g_variant_is_of_type (scope, G_VARIANT_TYPE_STRING))
      privacy_scope = g_variant_get_string (scope, NULL);

    shell_notification_set_privacy_scope (notification,
                                          g_strcmp0 (privacy_scope, "system") == 0
                                          ? SHELL_NOTIFICATION_PRIVACY_SCOPE_SYSTEM
                                          : SHELL_NOTIFICATION_PRIVACY_SCOPE_USER);
  }

  /* Only fallback to 'app-icon' when the source doesn't have a valid app */
  if (source->app == NULL)
    source_gicon = icon_for_notification_data (app_icon);
  fdo_source_process_notification (source, notification, app_name, source_gicon);

  shell_freedesktop_notifications_complete_notify (skeleton, invocation, id);
  return TRUE;
}

static gboolean
fdo_on_handle_close_notification (ShellFreedesktopNotifications *skeleton,
                                  GDBusMethodInvocation         *invocation,
                                  guint                          id,
                                  ShellFdoNotificationDaemon    *self)
{
  FdoNotificationEntry *entry;

  if (self->shutting_down)
    {
      return_shutting_down (invocation);
      return TRUE;
    }

  entry = g_hash_table_lookup (self->notifications, GUINT_TO_POINTER (id));
  if (entry != NULL)
    {
      g_autoptr (ShellNotification) notification = g_object_ref (entry->notification);

      shell_notification_destroy (notification,
                                  SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED);
    }

  shell_freedesktop_notifications_complete_close_notification (skeleton, invocation);
  return TRUE;
}

static gboolean
fdo_on_handle_get_capabilities (ShellFreedesktopNotifications *skeleton,
                                GDBusMethodInvocation         *invocation,
                                ShellFdoNotificationDaemon    *self)
{
  static const char * const capabilities[] = {
    "actions",
    /* "action-icons", */
    "body",
    /* "body-hyperlinks", */
    /* "body-images", */
    "body-markup",
    /* "icon-multi", */
    "icon-static",
    "persistence",
    "sound",
    NULL
  };

  shell_freedesktop_notifications_complete_get_capabilities (skeleton, invocation,
                                                             capabilities);
  return TRUE;
}

static gboolean
fdo_on_handle_get_server_information (ShellFreedesktopNotifications *skeleton,
                                      GDBusMethodInvocation         *invocation,
                                      ShellFdoNotificationDaemon    *self)
{
  shell_freedesktop_notifications_complete_get_server_information (skeleton, invocation,
                                                                   SERVER_NAME,
                                                                   SERVER_VENDOR,
                                                                   PACKAGE_VERSION,
                                                                   SERVER_SPEC_VERSION);
  return TRUE;
}

static void
fdo_disconnect_all (ShellFdoNotificationDaemon *self)
{
  GHashTableIter iter;
  gpointer key, value;

  if (self->sources_for_app != NULL)
    {
      g_hash_table_iter_init (&iter, self->sources_for_app);
      while (g_hash_table_iter_next (&iter, &key, &value))
        g_signal_handlers_disconnect_by_data (value, self);
    }

  if (self->sources_for_pid_and_name != NULL)
    {
      g_hash_table_iter_init (&iter, self->sources_for_pid_and_name);
      while (g_hash_table_iter_next (&iter, &key, &value))
        g_signal_handlers_disconnect_by_data (value, self);
    }

  if (self->notifications != NULL)
    {
      g_hash_table_iter_init (&iter, self->notifications);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          FdoNotificationEntry *entry = value;

          g_signal_handlers_disconnect_by_data (entry->notification, entry);
        }
    }
}

static void
fdo_on_shutdown (ShellGlobal                *global,
                 ShellFdoNotificationDaemon *self)
{
  self->shutting_down = TRUE;
  unexport_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton), self);
}

static void
shell_fdo_notification_daemon_get_property (GObject    *object,
                                            guint       prop_id,
                                            GValue     *value,
                                            GParamSpec *pspec)
{
  ShellFdoNotificationDaemon *self = SHELL_FDO_NOTIFICATION_DAEMON (object);

  switch (prop_id)
    {
    case FDO_PROP_MESSAGE_TRAY:
      g_value_set_object (value, self->message_tray);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_fdo_notification_daemon_set_property (GObject      *object,
                                            guint         prop_id,
                                            const GValue *value,
                                            GParamSpec   *pspec)
{
  ShellFdoNotificationDaemon *self = SHELL_FDO_NOTIFICATION_DAEMON (object);

  switch (prop_id)
    {
    case FDO_PROP_MESSAGE_TRAY:
      g_set_weak_pointer (&self->message_tray, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_fdo_notification_daemon_constructed (GObject *object)
{
  ShellFdoNotificationDaemon *self = SHELL_FDO_NOTIFICATION_DAEMON (object);
  g_autoptr (GDBusConnection) connection = NULL;

  G_OBJECT_CLASS (shell_fdo_notification_daemon_parent_class)->constructed (object);

  self->skeleton = shell_freedesktop_notifications_skeleton_new ();
  g_signal_connect (self->skeleton, "handle-notify",
                    G_CALLBACK (fdo_on_handle_notify), self);
  g_signal_connect (self->skeleton, "handle-close-notification",
                    G_CALLBACK (fdo_on_handle_close_notification), self);
  g_signal_connect (self->skeleton, "handle-get-capabilities",
                    G_CALLBACK (fdo_on_handle_get_capabilities), self);
  g_signal_connect (self->skeleton, "handle-get-server-information",
                    G_CALLBACK (fdo_on_handle_get_server_information), self);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (fdo_on_shutdown), self, 0);

  connection = export_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                FDO_OBJECT_PATH);
}

static void
shell_fdo_notification_daemon_dispose (GObject *object)
{
  ShellFdoNotificationDaemon *self = SHELL_FDO_NOTIFICATION_DAEMON (object);

  self->shutting_down = TRUE;

  fdo_disconnect_all (self);
  g_clear_pointer (&self->sources_for_app, g_hash_table_unref);
  g_clear_pointer (&self->sources_for_pid_and_name, g_hash_table_unref);
  g_clear_pointer (&self->notifications, g_hash_table_unref);

  if (self->skeleton != NULL)
    {
      unexport_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton), self);
      g_clear_object (&self->skeleton);
    }

  g_clear_weak_pointer (&self->message_tray);

  G_OBJECT_CLASS (shell_fdo_notification_daemon_parent_class)->dispose (object);
}

static void
shell_fdo_notification_daemon_class_init (ShellFdoNotificationDaemonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_fdo_notification_daemon_get_property;
  object_class->set_property = shell_fdo_notification_daemon_set_property;
  object_class->constructed = shell_fdo_notification_daemon_constructed;
  object_class->dispose = shell_fdo_notification_daemon_dispose;

  fdo_props[FDO_PROP_MESSAGE_TRAY] =
    g_param_spec_object ("message-tray", NULL, NULL,
                         SHELL_TYPE_MESSAGE_TRAY,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, FDO_N_PROPS, fdo_props);

  fdo_signals[FDO_APP_OPENED] =
    g_signal_new ("app-opened",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_fdo_notification_daemon_init (ShellFdoNotificationDaemon *self)
{
  /* The sources and notifications are removed from the tables when they are
   * destroyed; the tables keep them alive until then */
  self->sources_for_app = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                 g_object_unref, g_object_unref);
  self->sources_for_pid_and_name = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                          g_free, g_object_unref);
  self->notifications = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                               NULL,
                                               (GDestroyNotify) fdo_notification_entry_free);
  self->next_notification_id = 1;
}

/* ------------------------------------------------------------------------ */
/* ShellGtkNotificationDaemonNotification                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_GTK_NOTIFICATION_DAEMON_APP_SOURCE (shell_gtk_notification_daemon_app_source_get_type ())
G_DECLARE_FINAL_TYPE (ShellGtkNotificationDaemonAppSource,
                      shell_gtk_notification_daemon_app_source,
                      SHELL, GTK_NOTIFICATION_DAEMON_APP_SOURCE, ShellSource)

#define SHELL_TYPE_GTK_NOTIFICATION_DAEMON_NOTIFICATION (shell_gtk_notification_daemon_notification_get_type ())
G_DECLARE_FINAL_TYPE (ShellGtkNotificationDaemonNotification,
                      shell_gtk_notification_daemon_notification,
                      SHELL, GTK_NOTIFICATION_DAEMON_NOTIFICATION, ShellNotification)

struct _ShellGtkNotificationDaemonNotification
{
  ShellNotification parent;

  char *id;
  GVariant *serialized;                   /* a{sv} */
  char *default_action;                   /* (nullable) */
  GVariant *default_action_target;        /* (nullable) */
};

G_DEFINE_FINAL_TYPE (ShellGtkNotificationDaemonNotification,
                     shell_gtk_notification_daemon_notification,
                     SHELL_TYPE_NOTIFICATION)

static void gtk_app_source_activate_action (ShellGtkNotificationDaemonAppSource *self,
                                            const char                          *action_id,
                                            GVariant                            *target);
static void gtk_app_source_emit_action_invoked (ShellGtkNotificationDaemonAppSource *self,
                                                const char                          *notification_id,
                                                const char                          *action_id,
                                                GVariant                            *target);
static void gtk_app_source_emit_app_opened (ShellGtkNotificationDaemonAppSource *self);

static ShellNotificationUrgency
gtk_priority_to_urgency (const char *priority)
{
  if (g_strcmp0 (priority, "low") == 0)
    return SHELL_NOTIFICATION_URGENCY_LOW;
  else if (g_strcmp0 (priority, "high") == 0)
    return SHELL_NOTIFICATION_URGENCY_HIGH;
  else if (g_strcmp0 (priority, "urgent") == 0)
    return SHELL_NOTIFICATION_URGENCY_CRITICAL;

  return SHELL_NOTIFICATION_URGENCY_NORMAL;
}

static void
gtk_notification_activate_action (ShellGtkNotificationDaemonNotification *self,
                                  const char                             *action_id,
                                  GVariant                               *target)
{
  ShellSource *source = shell_notification_get_source (SHELL_NOTIFICATION (self));
  ShellGtkNotificationDaemonAppSource *app_source;

  if (source == NULL)
    return;

  app_source = SHELL_GTK_NOTIFICATION_DAEMON_APP_SOURCE (source);

  if (g_str_has_prefix (action_id, "app."))
    gtk_app_source_activate_action (app_source, action_id + strlen ("app."), target);
  else
    gtk_app_source_emit_action_invoked (app_source, self->id, action_id, target);

  gtk_app_source_emit_app_opened (app_source);
}

typedef struct
{
  char *action;
  GVariant *target;   /* (nullable) */
} GtkButtonData;

static void
gtk_button_data_free (GtkButtonData *data)
{
  g_free (data->action);
  g_clear_pointer (&data->target, g_variant_unref);
  g_free (data);
}

static void
gtk_button_clicked_cb (ShellNotification *notification,
                       gpointer           user_data)
{
  GtkButtonData *data = user_data;

  gtk_notification_activate_action (SHELL_GTK_NOTIFICATION_DAEMON_NOTIFICATION (notification),
                                    data->action, data->target);
}

static void
gtk_notification_real_activate (ShellNotification *notification)
{
  ShellGtkNotificationDaemonNotification *self =
    SHELL_GTK_NOTIFICATION_DAEMON_NOTIFICATION (notification);
  ShellNotificationClass *parent_class =
    SHELL_NOTIFICATION_CLASS (shell_gtk_notification_daemon_notification_parent_class);

  if (self->default_action != NULL && *self->default_action != '\0')
    {
      gtk_notification_activate_action (self, self->default_action,
                                        self->default_action_target);
    }
  else
    {
      ShellSource *source = shell_notification_get_source (notification);

      if (source != NULL)
        shell_source_open (source);
    }

  parent_class->activate (notification);
}

static void
shell_gtk_notification_daemon_notification_finalize (GObject *object)
{
  ShellGtkNotificationDaemonNotification *self =
    SHELL_GTK_NOTIFICATION_DAEMON_NOTIFICATION (object);

  g_clear_pointer (&self->id, g_free);
  g_clear_pointer (&self->serialized, g_variant_unref);
  g_clear_pointer (&self->default_action, g_free);
  g_clear_pointer (&self->default_action_target, g_variant_unref);

  G_OBJECT_CLASS (shell_gtk_notification_daemon_notification_parent_class)->finalize (object);
}

static void
shell_gtk_notification_daemon_notification_class_init (ShellGtkNotificationDaemonNotificationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellNotificationClass *notification_class = SHELL_NOTIFICATION_CLASS (klass);

  object_class->finalize = shell_gtk_notification_daemon_notification_finalize;

  notification_class->activate = gtk_notification_real_activate;
}

static void
shell_gtk_notification_daemon_notification_init (ShellGtkNotificationDaemonNotification *self)
{
}

/* Whether @notification, the a{sv} of org.gtk.Notifications, can be shown */
static gboolean
gtk_notification_data_is_valid (GVariant *notification)
{
  g_autoptr (GVariant) title = NULL;

  if (!g_variant_is_of_type (notification, G_VARIANT_TYPE_VARDICT))
    return FALSE;

  title = g_variant_lookup_value (notification, "title", G_VARIANT_TYPE_STRING);
  return title != NULL;
}

/* Creates the notification of @source; @notification must be valid */
static ShellGtkNotificationDaemonNotification *
gtk_notification_new (ShellGtkNotificationDaemonAppSource *source,
                      const char                          *id,
                      GVariant                            *notification)
{
  ShellGtkNotificationDaemonNotification *self;
  ShellNotification *base;
  g_autoptr (GVariant) buttons = NULL;
  g_autoptr (GVariant) icon = NULL;
  g_autoptr (GIcon) gicon = NULL;
  g_autoptr (GDateTime) datetime = NULL;
  const char *title = NULL;
  const char *body = NULL;
  const char *priority = NULL;
  const char *default_action = NULL;
  gboolean urgent = FALSE;
  gint64 timestamp = 0;

  self = g_object_new (SHELL_TYPE_GTK_NOTIFICATION_DAEMON_NOTIFICATION,
                       "source", source,
                       NULL);
  base = SHELL_NOTIFICATION (self);

  self->id = g_strdup (id);
  self->serialized = g_variant_ref (notification);

  g_variant_lookup (notification, "title", "&s", &title);
  g_variant_lookup (notification, "body", "&s", &body);

  if (g_variant_lookup (notification, "priority", "&s", &priority))
    shell_notification_set_urgency (base, gtk_priority_to_urgency (priority));
  else if (g_variant_lookup (notification, "urgent", "b", &urgent))
    shell_notification_set_urgency (base, urgent
                                    ? SHELL_NOTIFICATION_URGENCY_CRITICAL
                                    : SHELL_NOTIFICATION_URGENCY_NORMAL);
  else
    shell_notification_set_urgency (base, SHELL_NOTIFICATION_URGENCY_NORMAL);

  buttons = g_variant_lookup_value (notification, "buttons", G_VARIANT_TYPE ("aa{sv}"));
  if (buttons != NULL)
    {
      GVariantIter iter;
      GVariant *button;

      g_variant_iter_init (&iter, buttons);
      while ((button = g_variant_iter_next_value (&iter)) != NULL)
        {
          g_autoptr (GVariant) owned_button = button;
          const char *label = NULL;
          const char *action = NULL;
          GtkButtonData *data;

          if (!g_variant_lookup (owned_button, "label", "&s", &label) ||
              !g_variant_lookup (owned_button, "action", "&s", &action))
            continue;

          data = g_new0 (GtkButtonData, 1);
          data->action = g_strdup (action);
          data->target = g_variant_lookup_value (owned_button, "target", NULL);
          shell_notification_add_action (base, label, gtk_button_clicked_cb,
                                         data, (GDestroyNotify) gtk_button_data_free);
        }
    }

  if (g_variant_lookup (notification, "default-action", "&s", &default_action))
    self->default_action = g_strdup (default_action);
  self->default_action_target = g_variant_lookup_value (notification,
                                                        "default-action-target",
                                                        NULL);

  icon = g_variant_lookup_value (notification, "icon", NULL);
  if (icon != NULL)
    gicon = g_icon_deserialize (icon);

  if (g_variant_lookup (notification, "timestamp", "x", &timestamp))
    datetime = g_date_time_new_from_unix_local (timestamp);

  shell_notification_set_title (base, title);
  shell_notification_set_body (base, body);
  shell_notification_set_gicon (base, gicon);
  if (datetime != NULL)
    shell_notification_set_datetime (base, datetime);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellGtkNotificationDaemonAppSource                                       */
/* ------------------------------------------------------------------------ */

enum
{
  GTK_SOURCE_PROP_0,

  GTK_SOURCE_PROP_APP_ID,
  GTK_SOURCE_PROP_APP,
  GTK_SOURCE_PROP_SKELETON,

  GTK_SOURCE_N_PROPS
};

static GParamSpec *gtk_source_props[GTK_SOURCE_N_PROPS] = { NULL, };

enum
{
  GTK_SOURCE_APP_OPENED,

  GTK_SOURCE_LAST_SIGNAL
};

static guint gtk_source_signals[GTK_SOURCE_LAST_SIGNAL] = { 0, };

struct _ShellGtkNotificationDaemonAppSource
{
  ShellSource parent;

  char *app_id;
  ShellApp *app;
  ShellGtkNotifications *skeleton;
  GPtrArray *notifications;               /* of ShellGtkNotificationDaemonNotification, in order */
  gboolean notification_pending;
};

G_DEFINE_FINAL_TYPE (ShellGtkNotificationDaemonAppSource,
                     shell_gtk_notification_daemon_app_source,
                     SHELL_TYPE_SOURCE)

static void
gtk_activate_action_cb (GObject      *source_object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  g_autofree char *app_id = user_data;
  g_autoptr (GError) error = NULL;

  if (!shell_app_activate_action_finish (SHELL_APP (source_object), res, &error))
    g_warning ("Failed to activate action for %s: %s", app_id, error->message);
}

static void
gtk_app_source_activate_action (ShellGtkNotificationDaemonAppSource *self,
                                const char                          *action_id,
                                GVariant                            *target)
{
  g_autoptr (GVariant) params = NULL;

  if (*action_id == '\0')
    return;

  if (target != NULL)
    {
      GVariantBuilder builder;

      g_variant_builder_init (&builder, G_VARIANT_TYPE ("av"));
      g_variant_builder_add (&builder, "v", target);
      params = g_variant_ref_sink (g_variant_builder_end (&builder));
    }

  shell_app_activate_action (self->app, action_id, params, 0, -1, NULL,
                             gtk_activate_action_cb, g_strdup (self->app_id));
}

static void
gtk_app_source_emit_action_invoked (ShellGtkNotificationDaemonAppSource *self,
                                    const char                          *notification_id,
                                    const char                          *action_id,
                                    GVariant                            *target)
{
  g_autoptr (GAppLaunchContext) context =
    shell_global_create_app_launch_context (shell_global_get (), 0, -1);
  GDesktopAppInfo *info = shell_app_get_app_info (self->app);
  g_autofree char *token = NULL;
  GVariantBuilder parameter;
  GVariantBuilder platform_data;

  token = g_app_launch_context_get_startup_notify_id (context,
                                                      info != NULL ? G_APP_INFO (info) : NULL,
                                                      NULL);

  g_variant_builder_init (&parameter, G_VARIANT_TYPE ("av"));
  if (target != NULL)
    g_variant_builder_add (&parameter, "v", target);

  g_variant_builder_init (&platform_data, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&platform_data, "{sv}", "activation-token",
                         g_variant_new_string (token != NULL ? token : ""));

  shell_gtk_notifications_emit_action_invoked (self->skeleton,
                                               self->app_id, notification_id, action_id,
                                               g_variant_builder_end (&parameter),
                                               g_variant_builder_end (&platform_data));
}

static void
gtk_app_source_emit_app_opened (ShellGtkNotificationDaemonAppSource *self)
{
  g_signal_emit (self, gtk_source_signals[GTK_SOURCE_APP_OPENED], 0);
}

static void
gtk_app_source_open (ShellSource *source)
{
  ShellGtkNotificationDaemonAppSource *self = SHELL_GTK_NOTIFICATION_DAEMON_APP_SOURCE (source);

  shell_app_activate (self->app);
  gtk_app_source_emit_app_opened (self);
}

static void
gtk_app_source_destroy (ShellSource                      *source,
                        ShellNotificationDestroyedReason  reason)
{
  ShellGtkNotificationDaemonAppSource *self = SHELL_GTK_NOTIFICATION_DAEMON_APP_SOURCE (source);
  ShellSourceClass *parent_class =
    SHELL_SOURCE_CLASS (shell_gtk_notification_daemon_app_source_parent_class);

  if (self->notification_pending)
    return;

  parent_class->destroy (source, reason);
}

static void
gtk_app_source_notification_destroy_cb (ShellNotification                *notification,
                                        ShellNotificationDestroyedReason  reason,
                                        ShellGtkNotificationDaemonAppSource *self)
{
  g_ptr_array_remove (self->notifications, notification);
}

static ShellGtkNotificationDaemonNotification *
gtk_app_source_find_notification (ShellGtkNotificationDaemonAppSource *self,
                                  const char                          *id)
{
  guint i;

  for (i = 0; i < self->notifications->len; i++)
    {
      ShellGtkNotificationDaemonNotification *notification =
        g_ptr_array_index (self->notifications, i);

      if (g_strcmp0 (notification->id, id) == 0)
        return notification;
    }

  return NULL;
}

/* `add_notification ()` */
static void
gtk_app_source_add_notification (ShellGtkNotificationDaemonAppSource *self,
                                 ShellGtkNotificationDaemonNotification *notification)
{
  ShellGtkNotificationDaemonNotification *existing;

  g_object_ref (self);
  g_object_ref (notification);

  self->notification_pending = TRUE;

  existing = gtk_app_source_find_notification (self, notification->id);
  if (existing != NULL)
    {
      g_autoptr (ShellGtkNotificationDaemonNotification) old = g_object_ref (existing);

      shell_notification_destroy (SHELL_NOTIFICATION (old),
                                  SHELL_NOTIFICATION_DESTROYED_REASON_REPLACED);
    }

  g_signal_connect (notification, "destroy",
                    G_CALLBACK (gtk_app_source_notification_destroy_cb), self);
  g_ptr_array_add (self->notifications, g_object_ref (notification));

  shell_source_add_notification (SHELL_SOURCE (self), SHELL_NOTIFICATION (notification));

  self->notification_pending = FALSE;

  g_object_unref (notification);
  g_object_unref (self);
}

static void
gtk_app_source_remove_notification (ShellGtkNotificationDaemonAppSource *self,
                                    const char                          *id)
{
  ShellGtkNotificationDaemonNotification *notification =
    gtk_app_source_find_notification (self, id);

  if (notification != NULL)
    {
      g_autoptr (ShellGtkNotificationDaemonNotification) keep_alive = g_object_ref (notification);

      shell_notification_destroy (SHELL_NOTIFICATION (keep_alive),
                                  SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED);
    }
}

/* `serialize ()`: (sa(sv)), floating */
static GVariant *
gtk_app_source_serialize (ShellGtkNotificationDaemonAppSource *self)
{
  GVariantBuilder builder;
  guint i;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sv)"));
  for (i = 0; i < self->notifications->len; i++)
    {
      ShellGtkNotificationDaemonNotification *notification =
        g_ptr_array_index (self->notifications, i);

      g_variant_builder_add (&builder, "(sv)", notification->id, notification->serialized);
    }

  return g_variant_new ("(s@a(sv))", self->app_id, g_variant_builder_end (&builder));
}

static void
shell_gtk_notification_daemon_app_source_get_property (GObject    *object,
                                                       guint       prop_id,
                                                       GValue     *value,
                                                       GParamSpec *pspec)
{
  ShellGtkNotificationDaemonAppSource *self = SHELL_GTK_NOTIFICATION_DAEMON_APP_SOURCE (object);

  switch (prop_id)
    {
    case GTK_SOURCE_PROP_APP_ID:
      g_value_set_string (value, self->app_id);
      break;

    case GTK_SOURCE_PROP_APP:
      g_value_set_object (value, self->app);
      break;

    case GTK_SOURCE_PROP_SKELETON:
      g_value_set_object (value, self->skeleton);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_gtk_notification_daemon_app_source_set_property (GObject      *object,
                                                       guint         prop_id,
                                                       const GValue *value,
                                                       GParamSpec   *pspec)
{
  ShellGtkNotificationDaemonAppSource *self = SHELL_GTK_NOTIFICATION_DAEMON_APP_SOURCE (object);

  switch (prop_id)
    {
    case GTK_SOURCE_PROP_APP_ID:
      g_free (self->app_id);
      self->app_id = g_value_dup_string (value);
      break;

    case GTK_SOURCE_PROP_APP:
      g_set_object (&self->app, g_value_get_object (value));
      break;

    case GTK_SOURCE_PROP_SKELETON:
      g_set_object (&self->skeleton, SHELL_GTK_NOTIFICATIONS (g_value_get_object (value)));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_gtk_notification_daemon_app_source_dispose (GObject *object)
{
  ShellGtkNotificationDaemonAppSource *self = SHELL_GTK_NOTIFICATION_DAEMON_APP_SOURCE (object);
  guint i;

  for (i = 0; i < self->notifications->len; i++)
    g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->notifications, i), self);
  g_ptr_array_set_size (self->notifications, 0);

  g_clear_object (&self->app);
  g_clear_object (&self->skeleton);

  G_OBJECT_CLASS (shell_gtk_notification_daemon_app_source_parent_class)->dispose (object);
}

static void
shell_gtk_notification_daemon_app_source_finalize (GObject *object)
{
  ShellGtkNotificationDaemonAppSource *self = SHELL_GTK_NOTIFICATION_DAEMON_APP_SOURCE (object);

  g_clear_pointer (&self->app_id, g_free);
  g_clear_pointer (&self->notifications, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_gtk_notification_daemon_app_source_parent_class)->finalize (object);
}

static void
shell_gtk_notification_daemon_app_source_class_init (ShellGtkNotificationDaemonAppSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellSourceClass *source_class = SHELL_SOURCE_CLASS (klass);

  object_class->get_property = shell_gtk_notification_daemon_app_source_get_property;
  object_class->set_property = shell_gtk_notification_daemon_app_source_set_property;
  object_class->dispose = shell_gtk_notification_daemon_app_source_dispose;
  object_class->finalize = shell_gtk_notification_daemon_app_source_finalize;

  source_class->open = gtk_app_source_open;
  source_class->destroy = gtk_app_source_destroy;

  gtk_source_props[GTK_SOURCE_PROP_APP_ID] =
    g_param_spec_string ("app-id", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  gtk_source_props[GTK_SOURCE_PROP_APP] =
    g_param_spec_object ("app", NULL, NULL,
                         SHELL_TYPE_APP,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  gtk_source_props[GTK_SOURCE_PROP_SKELETON] =
    g_param_spec_object ("skeleton", NULL, NULL,
                         G_TYPE_DBUS_INTERFACE_SKELETON,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, GTK_SOURCE_N_PROPS, gtk_source_props);

  gtk_source_signals[GTK_SOURCE_APP_OPENED] =
    g_signal_new ("app-opened",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_gtk_notification_daemon_app_source_init (ShellGtkNotificationDaemonAppSource *self)
{
  self->notifications = g_ptr_array_new_with_free_func (g_object_unref);
}

/* Returns %NULL if @app_id is not the id of an installed application */
static ShellGtkNotificationDaemonAppSource *
gtk_app_source_new (const char                    *app_id,
                    ShellGtkNotifications         *skeleton)
{
  g_autofree char *desktop_id = NULL;
  g_autoptr (ShellNotificationPolicy) policy = NULL;
  ShellApp *app;

  if (!g_application_id_is_valid (app_id))
    return NULL;

  desktop_id = g_strconcat (app_id, ".desktop", NULL);
  app = shell_app_system_lookup_app (shell_app_system_get_default (), desktop_id);
  if (app == NULL)
    return NULL;

  policy = SHELL_NOTIFICATION_POLICY (shell_notification_application_policy_new (app_id));

  return g_object_new (SHELL_TYPE_GTK_NOTIFICATION_DAEMON_APP_SOURCE,
                       "title", shell_app_get_name (app),
                       "icon", shell_app_get_icon (app),
                       "policy", policy,
                       "app-id", app_id,
                       "app", app,
                       "skeleton", skeleton,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellGtkNotificationDaemon                                                */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_GTK_NOTIFICATION_DAEMON (shell_gtk_notification_daemon_get_type ())
G_DECLARE_FINAL_TYPE (ShellGtkNotificationDaemon, shell_gtk_notification_daemon,
                      SHELL, GTK_NOTIFICATION_DAEMON, GObject)

enum
{
  GTK_PROP_0,

  GTK_PROP_MESSAGE_TRAY,

  GTK_N_PROPS
};

static GParamSpec *gtk_props[GTK_N_PROPS] = { NULL, };

enum
{
  GTK_APP_OPENED,

  GTK_LAST_SIGNAL
};

static guint gtk_signals[GTK_LAST_SIGNAL] = { 0, };

struct _ShellGtkNotificationDaemon
{
  GObject parent;

  ShellMessageTray *message_tray;                 /* weak */
  ShellGlobal *global;                            /* weak */
  ShellGtkNotifications *skeleton;
  GPtrArray *sources;                             /* of the app sources, in order */
  guint name_id;
  gboolean is_loading;
  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellGtkNotificationDaemon, shell_gtk_notification_daemon, G_TYPE_OBJECT)

static void
gtk_daemon_save_notifications (ShellGtkNotificationDaemon *self)
{
  g_autoptr (GVariant) state = NULL;
  GVariantBuilder builder;
  guint i;

  if (self->is_loading || self->global == NULL || self->sources == NULL)
    return;

  if (self->sources->len > 0)
    {
      g_variant_builder_init (&builder, G_VARIANT_TYPE (NOTIFICATION_SERIALIZED_FORMAT));
      for (i = 0; i < self->sources->len; i++)
        g_variant_builder_add_value (&builder,
                                     gtk_app_source_serialize (g_ptr_array_index (self->sources, i)));
      state = g_variant_ref_sink (g_variant_builder_end (&builder));
    }

  shell_global_set_persistent_state (self->global, NOTIFICATION_SERIALIZED_NAME, state);
}

static void
gtk_source_destroy_cb (ShellSource                      *source,
                       ShellNotificationDestroyedReason  reason,
                       ShellGtkNotificationDaemon       *self)
{
  g_signal_handlers_disconnect_by_data (source, self);
  g_ptr_array_remove (self->sources, source);
  gtk_daemon_save_notifications (self);
}

static void
gtk_source_count_notify_cb (ShellSource                *source,
                            GParamSpec                 *pspec,
                            ShellGtkNotificationDaemon *self)
{
  gtk_daemon_save_notifications (self);
}

static void
gtk_source_app_opened_cb (ShellGtkNotificationDaemonAppSource *source,
                          ShellGtkNotificationDaemon          *self)
{
  g_signal_emit (self, gtk_signals[GTK_APP_OPENED], 0);
}

static ShellGtkNotificationDaemonAppSource *
gtk_daemon_find_source (ShellGtkNotificationDaemon *self,
                        const char                 *app_id)
{
  guint i;

  for (i = 0; i < self->sources->len; i++)
    {
      ShellGtkNotificationDaemonAppSource *source = g_ptr_array_index (self->sources, i);

      if (g_strcmp0 (source->app_id, app_id) == 0)
        return source;
    }

  return NULL;
}

/* Returns the source of @app_id, or %NULL if it is not a valid application */
static ShellGtkNotificationDaemonAppSource *
gtk_daemon_ensure_app_source (ShellGtkNotificationDaemon *self,
                              const char                 *app_id)
{
  ShellGtkNotificationDaemonAppSource *source;

  source = gtk_daemon_find_source (self, app_id);
  if (source != NULL)
    return source;

  source = gtk_app_source_new (app_id, self->skeleton);
  if (source == NULL)
    return NULL;

  g_signal_connect (source, "destroy",
                    G_CALLBACK (gtk_source_destroy_cb), self);
  g_signal_connect (source, "notify::count",
                    G_CALLBACK (gtk_source_count_notify_cb), self);
  g_signal_connect (source, "app-opened",
                    G_CALLBACK (gtk_source_app_opened_cb), self);

  shell_message_tray_add (self->message_tray, SHELL_SOURCE (source));
  g_ptr_array_add (self->sources, source);

  return source;
}

static void
gtk_daemon_load_notifications (ShellGtkNotificationDaemon *self)
{
  g_autoptr (GVariant) value = NULL;
  GVariantIter sources_iter;
  const char *app_id;
  GVariant *notifications;

  self->is_loading = TRUE;

  value = shell_global_get_persistent_state (self->global,
                                             NOTIFICATION_SERIALIZED_FORMAT,
                                             NOTIFICATION_SERIALIZED_NAME);
  if (value != NULL)
    {
      g_variant_ref_sink (value);

      g_variant_iter_init (&sources_iter, value);
      while (g_variant_iter_next (&sources_iter, "(&s@a(sv))", &app_id, &notifications))
        {
          g_autoptr (GVariant) owned_notifications = notifications;
          ShellGtkNotificationDaemonAppSource *source;
          GVariantIter iter;
          const char *notification_id;
          GVariant *packed;

          if (g_variant_n_children (owned_notifications) == 0)
            continue;

          source = gtk_daemon_ensure_app_source (self, app_id);
          if (source == NULL)
            continue;

          g_variant_iter_init (&iter, owned_notifications);
          while (g_variant_iter_next (&iter, "(&sv)", &notification_id, &packed))
            {
              g_autoptr (GVariant) owned_packed = packed;
              g_autoptr (ShellGtkNotificationDaemonNotification) notification = NULL;

              if (!gtk_notification_data_is_valid (owned_packed))
                continue;

              notification = gtk_notification_new (source, notification_id, owned_packed);

              /* Acknowledge all stored notification so that we don't show a
               * banner again */
              shell_notification_set_acknowledged (SHELL_NOTIFICATION (notification), TRUE);
              gtk_app_source_add_notification (source, notification);
            }
        }
    }

  self->is_loading = FALSE;
}

static gboolean
gtk_on_handle_add_notification (ShellGtkNotifications       *skeleton,
                                GDBusMethodInvocation       *invocation,
                                const char                  *app_id,
                                const char                  *notification_id,
                                GVariant                    *notification_data,
                                ShellGtkNotificationDaemon  *self)
{
  ShellGtkNotificationDaemonAppSource *source;
  g_autoptr (ShellGtkNotificationDaemonNotification) notification = NULL;
  g_autoptr (GVariant) serialized = NULL;
  g_autoptr (GDateTime) now = NULL;
  GVariantDict dict;

  if (self->shutting_down || self->message_tray == NULL)
    {
      return_shutting_down (invocation);
      return TRUE;
    }

  if (!gtk_notification_data_is_valid (notification_data))
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_DBUS_ERROR,
                                                     G_DBUS_ERROR_INVALID_ARGS,
                                                     "The notification needs a title");
      return TRUE;
    }

  source = gtk_daemon_ensure_app_source (self, app_id);
  if (source == NULL)
    {
      g_autofree char *message =
        g_strdup_printf ("The app by ID \"%s\" could not be found", app_id);

      g_dbus_method_invocation_return_dbus_error (invocation, GTK_ERROR_INVALID_APP, message);
      return TRUE;
    }

  now = g_date_time_new_now_local ();
  g_variant_dict_init (&dict, notification_data);
  g_variant_dict_insert (&dict, "timestamp", "x", g_date_time_to_unix (now));
  serialized = g_variant_ref_sink (g_variant_dict_end (&dict));

  notification = gtk_notification_new (source, notification_id, serialized);
  gtk_app_source_add_notification (source, notification);

  shell_gtk_notifications_complete_add_notification (skeleton, invocation);
  return TRUE;
}

static gboolean
gtk_on_handle_remove_notification (ShellGtkNotifications      *skeleton,
                                   GDBusMethodInvocation      *invocation,
                                   const char                 *app_id,
                                   const char                 *notification_id,
                                   ShellGtkNotificationDaemon *self)
{
  ShellGtkNotificationDaemonAppSource *source;

  if (self->shutting_down)
    {
      return_shutting_down (invocation);
      return TRUE;
    }

  source = gtk_daemon_find_source (self, app_id);
  if (source != NULL)
    gtk_app_source_remove_notification (source, notification_id);

  shell_gtk_notifications_complete_remove_notification (skeleton, invocation);
  return TRUE;
}

static void
gtk_on_shutdown (ShellGlobal                *global,
                 ShellGtkNotificationDaemon *self)
{
  self->shutting_down = TRUE;
  unexport_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton), self);
}

static void
shell_gtk_notification_daemon_get_property (GObject    *object,
                                            guint       prop_id,
                                            GValue     *value,
                                            GParamSpec *pspec)
{
  ShellGtkNotificationDaemon *self = SHELL_GTK_NOTIFICATION_DAEMON (object);

  switch (prop_id)
    {
    case GTK_PROP_MESSAGE_TRAY:
      g_value_set_object (value, self->message_tray);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_gtk_notification_daemon_set_property (GObject      *object,
                                            guint         prop_id,
                                            const GValue *value,
                                            GParamSpec   *pspec)
{
  ShellGtkNotificationDaemon *self = SHELL_GTK_NOTIFICATION_DAEMON (object);

  switch (prop_id)
    {
    case GTK_PROP_MESSAGE_TRAY:
      g_set_weak_pointer (&self->message_tray, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_gtk_notification_daemon_constructed (GObject *object)
{
  ShellGtkNotificationDaemon *self = SHELL_GTK_NOTIFICATION_DAEMON (object);
  g_autoptr (GDBusConnection) connection = NULL;

  G_OBJECT_CLASS (shell_gtk_notification_daemon_parent_class)->constructed (object);

  g_set_weak_pointer (&self->global, shell_global_get ());

  self->skeleton = shell_gtk_notifications_skeleton_new ();
  g_signal_connect (self->skeleton, "handle-add-notification",
                    G_CALLBACK (gtk_on_handle_add_notification), self);
  g_signal_connect (self->skeleton, "handle-remove-notification",
                    G_CALLBACK (gtk_on_handle_remove_notification), self);

  g_signal_connect_object (self->global, "shutdown",
                           G_CALLBACK (gtk_on_shutdown), self, 0);

  connection = export_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                GTK_OBJECT_PATH);

  if (self->message_tray != NULL)
    gtk_daemon_load_notifications (self);

  if (connection != NULL)
    self->name_id = g_bus_own_name_on_connection (connection, GTK_BUS_NAME,
                                                  G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                                  NULL, NULL, NULL, NULL);
}

static void
shell_gtk_notification_daemon_dispose (GObject *object)
{
  ShellGtkNotificationDaemon *self = SHELL_GTK_NOTIFICATION_DAEMON (object);
  guint i;

  self->shutting_down = TRUE;

  if (self->name_id != 0)
    {
      g_bus_unown_name (self->name_id);
      self->name_id = 0;
    }

  if (self->sources != NULL)
    {
      for (i = 0; i < self->sources->len; i++)
        g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->sources, i), self);
      g_clear_pointer (&self->sources, g_ptr_array_unref);
    }

  if (self->skeleton != NULL)
    {
      unexport_skeleton (G_DBUS_INTERFACE_SKELETON (self->skeleton), self);
      g_clear_object (&self->skeleton);
    }

  g_clear_weak_pointer (&self->message_tray);
  g_clear_weak_pointer (&self->global);

  G_OBJECT_CLASS (shell_gtk_notification_daemon_parent_class)->dispose (object);
}

static void
shell_gtk_notification_daemon_class_init (ShellGtkNotificationDaemonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_gtk_notification_daemon_get_property;
  object_class->set_property = shell_gtk_notification_daemon_set_property;
  object_class->constructed = shell_gtk_notification_daemon_constructed;
  object_class->dispose = shell_gtk_notification_daemon_dispose;

  gtk_props[GTK_PROP_MESSAGE_TRAY] =
    g_param_spec_object ("message-tray", NULL, NULL,
                         SHELL_TYPE_MESSAGE_TRAY,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, GTK_N_PROPS, gtk_props);

  gtk_signals[GTK_APP_OPENED] =
    g_signal_new ("app-opened",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_gtk_notification_daemon_init (ShellGtkNotificationDaemon *self)
{
  /* The sources are removed when they are destroyed */
  self->sources = g_ptr_array_new_with_free_func (g_object_unref);
}

/* ------------------------------------------------------------------------ */
/* ShellNotificationDaemon                                                   */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_MESSAGE_TRAY,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  APP_OPENED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellNotificationDaemon
{
  GObject parent;

  ShellMessageTray *message_tray;              /* weak */
  ShellFdoNotificationDaemon *fdo_daemon;
  ShellGtkNotificationDaemon *gtk_daemon;
};

G_DEFINE_FINAL_TYPE (ShellNotificationDaemon, shell_notification_daemon, G_TYPE_OBJECT)

static void
daemon_app_opened_cb (GObject                 *daemon,
                      ShellNotificationDaemon *self)
{
  g_signal_emit (self, signals[APP_OPENED], 0);
}

static void
shell_notification_daemon_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellNotificationDaemon *self = SHELL_NOTIFICATION_DAEMON (object);

  switch (prop_id)
    {
    case PROP_MESSAGE_TRAY:
      g_value_set_object (value, self->message_tray);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_notification_daemon_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellNotificationDaemon *self = SHELL_NOTIFICATION_DAEMON (object);

  switch (prop_id)
    {
    case PROP_MESSAGE_TRAY:
      g_set_weak_pointer (&self->message_tray, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_notification_daemon_constructed (GObject *object)
{
  ShellNotificationDaemon *self = SHELL_NOTIFICATION_DAEMON (object);

  G_OBJECT_CLASS (shell_notification_daemon_parent_class)->constructed (object);

  self->fdo_daemon = g_object_new (SHELL_TYPE_FDO_NOTIFICATION_DAEMON,
                                  "message-tray", self->message_tray,
                                  NULL);
  g_signal_connect (self->fdo_daemon, "app-opened",
                    G_CALLBACK (daemon_app_opened_cb), self);

  self->gtk_daemon = g_object_new (SHELL_TYPE_GTK_NOTIFICATION_DAEMON,
                                   "message-tray", self->message_tray,
                                   NULL);
  g_signal_connect (self->gtk_daemon, "app-opened",
                    G_CALLBACK (daemon_app_opened_cb), self);
}

static void
shell_notification_daemon_dispose (GObject *object)
{
  ShellNotificationDaemon *self = SHELL_NOTIFICATION_DAEMON (object);

  if (self->fdo_daemon != NULL)
    g_signal_handlers_disconnect_by_data (self->fdo_daemon, self);
  if (self->gtk_daemon != NULL)
    g_signal_handlers_disconnect_by_data (self->gtk_daemon, self);

  g_clear_object (&self->fdo_daemon);
  g_clear_object (&self->gtk_daemon);
  g_clear_weak_pointer (&self->message_tray);

  G_OBJECT_CLASS (shell_notification_daemon_parent_class)->dispose (object);
}

static void
shell_notification_daemon_class_init (ShellNotificationDaemonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_notification_daemon_get_property;
  object_class->set_property = shell_notification_daemon_set_property;
  object_class->constructed = shell_notification_daemon_constructed;
  object_class->dispose = shell_notification_daemon_dispose;

  /**
   * ShellNotificationDaemon:message-tray:
   *
   * The message tray that gets the sources of the notifications
   * (`Main.messageTray`).
   */
  props[PROP_MESSAGE_TRAY] =
    g_param_spec_object ("message-tray", NULL, NULL,
                         SHELL_TYPE_MESSAGE_TRAY,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellNotificationDaemon::app-opened:
   * @self: the daemon
   *
   * Emitted after the activation of a notification, or of its source, brought
   * an application up. ShellMain hides the overview and closes the calendar
   * (what the sources did with `Main.overview.hide ()` and
   * `Main.panel.close_calendar ()`).
   */
  signals[APP_OPENED] =
    g_signal_new ("app-opened",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_notification_daemon_init (ShellNotificationDaemon *self)
{
}

/**
 * shell_notification_daemon_new:
 * @message_tray: the message tray to add the notification sources to
 *
 * Creates the notification daemons, exports them on the session bus and owns
 * org.gtk.Notifications (`new NotificationDaemon ()`). src/ui/main.c creates
 * the only instance.
 *
 * Returns: (transfer full): a new #ShellNotificationDaemon
 */
ShellNotificationDaemon *
shell_notification_daemon_new (ShellMessageTray *message_tray)
{
  g_return_val_if_fail (SHELL_IS_MESSAGE_TRAY (message_tray), NULL);

  return g_object_new (SHELL_TYPE_NOTIFICATION_DAEMON,
                       "message-tray", message_tray,
                       NULL);
}
