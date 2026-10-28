/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-lua-api-notify.c: `shell.notify`, notifications through the message
 * tray of the shell (docs/api/notify.md).
 *
 * Handle types:
 *
 *   shell.Notification        ShellNotification
 *   shell.NotificationSource  ShellLuaNotifySource (private, below)
 *
 * A #ShellSource destroys itself when its last notification goes, and cannot
 * be used again (it is unregistered from the tray and disposed). The handle of
 * a `shell.NotificationSource` has to stay valid while the source has no
 * notification, so it wraps a #ShellLuaNotifySource: a title and an icon, and
 * the #ShellSource that is currently registered with the tray, if any. The
 * first `notify` creates one, the destruction of the last notification
 * drops it, the next `notify` creates a new one. The default source of
 * `shell.notify{...}` is the same thing without a handle for the extension.
 * Both are owned by the extension context.
 *
 * A notification is an object owned by the extension context
 * (shell_lua_own_object ()). The shell destroys it in its own ways (it
 * expires, the user closes it, the source closes), which is reported through
 * `on_dismissed`; what the extension does itself (`destroy ()`, the `destroy
 * ()` or `clear ()` of its source, disable) is not: those kill the callbacks
 * first, or set DISMISSAL_SUPPRESSED_KEY before the destruction.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "shell-global.h"
#include "shell-lua-api-private.h"
#include "shell-lua-api-resolve.h"
#include "ui/main.h"
#include "ui/message-tray.h"

#define MAX_ACTIONS 3

#define DISMISSAL_SUPPRESSED_KEY "shell-lua-notify-suppressed"
#define STATE_KEY "shell-lua-notify-state"

#define DEFAULT_SOURCE_ICON "application-x-addon-symbolic"

static char default_source_key;

/* ------------------------------------------------------------------------ */
/* ShellLuaNotifySource                                                      */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LUA_NOTIFY_SOURCE (shell_lua_notify_source_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaNotifySource, shell_lua_notify_source,
                      SHELL, LUA_NOTIFY_SOURCE, GObject)

enum
{
  NS_PROP_0,

  NS_PROP_TITLE,
  NS_PROP_ICON_NAME,
  NS_PROP_COUNT,

  NS_N_PROPS
};

static GParamSpec *ns_props[NS_N_PROPS] = { NULL, };

struct _ShellLuaNotifySource
{
  GObject parent;

  char *title;
  char *icon_name;
  GIcon *icon;     /* from an icon_file */
  char *owner;     /* uuid of the extension */

  ShellSource *source; /* strong, NULL while no notification is live */
};

G_DEFINE_FINAL_TYPE (ShellLuaNotifySource, shell_lua_notify_source, G_TYPE_OBJECT)

static void
notify_source_apply_icon (ShellLuaNotifySource *self)
{
  ShellMessageSource *source;

  if (self->source == NULL)
    return;

  source = SHELL_MESSAGE_SOURCE (self->source);
  if (self->icon != NULL)
    shell_message_source_set_icon (source, self->icon);
  else
    shell_message_source_set_icon_name (source, self->icon_name);
}

static void
notify_source_destroy_cb (ShellSource                      *source,
                          ShellNotificationDestroyedReason  reason,
                          ShellLuaNotifySource             *self)
{
  if (self->source == source)
    g_clear_object (&self->source);
}

static ShellSource *
notify_source_ensure (ShellLuaNotifySource *self,
                      ShellMessageTray     *tray)
{
  if (self->source != NULL)
    return self->source;

  self->source = g_object_new (SHELL_TYPE_SOURCE, NULL);
  shell_message_source_set_title (SHELL_MESSAGE_SOURCE (self->source), self->title);
  notify_source_apply_icon (self);
  shell_source_set_owner (self->source, self->owner);
  g_signal_connect_object (self->source, "destroy",
                           G_CALLBACK (notify_source_destroy_cb), self, 0);

  shell_message_tray_add (tray, self->source);
  return self->source;
}

/* The extension asks for the end of its notifications: no `on_dismissed` */
static void
suppress_dismissal (ShellNotification *notification)
{
  g_object_set_data (G_OBJECT (notification), DISMISSAL_SUPPRESSED_KEY, GINT_TO_POINTER (1));
}

static void
notify_source_suppress_all (ShellLuaNotifySource *self,
                            gboolean              only_non_resident)
{
  GList *notifications, *l;

  if (self->source == NULL)
    return;

  notifications = shell_source_get_notifications (self->source);
  for (l = notifications; l != NULL; l = l->next)
    {
      if (!only_non_resident || !shell_notification_get_resident (l->data))
        suppress_dismissal (l->data);
    }
  g_list_free (notifications);
}

/* ShellLuaOwnOps.destroy: `src:destroy ()` and disable */
static void
notify_source_destroy (GObject *object)
{
  ShellLuaNotifySource *self = SHELL_LUA_NOTIFY_SOURCE (object);

  if (self->source != NULL)
    {
      g_autoptr (ShellSource) source = g_object_ref (self->source);

      notify_source_suppress_all (self, FALSE);
      shell_source_destroy (source, SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED);
      g_clear_object (&self->source);
    }
}

static const ShellLuaOwnOps notify_source_ops = {
  .destroy = notify_source_destroy,
};

static void
shell_lua_notify_source_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  ShellLuaNotifySource *self = SHELL_LUA_NOTIFY_SOURCE (object);

  switch (prop_id)
    {
    case NS_PROP_TITLE:
      g_value_set_string (value, self->title);
      break;

    case NS_PROP_ICON_NAME:
      g_value_set_string (value, self->icon_name);
      break;

    case NS_PROP_COUNT:
      g_value_set_int (value, self->source != NULL ? shell_source_get_count (self->source) : 0);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_lua_notify_source_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  ShellLuaNotifySource *self = SHELL_LUA_NOTIFY_SOURCE (object);

  switch (prop_id)
    {
    case NS_PROP_TITLE:
      g_free (self->title);
      self->title = g_value_dup_string (value);
      if (self->source != NULL)
        shell_message_source_set_title (SHELL_MESSAGE_SOURCE (self->source), self->title);
      break;

    case NS_PROP_ICON_NAME:
      g_free (self->icon_name);
      self->icon_name = g_value_dup_string (value);
      g_clear_object (&self->icon);
      notify_source_apply_icon (self);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_lua_notify_source_dispose (GObject *object)
{
  ShellLuaNotifySource *self = SHELL_LUA_NOTIFY_SOURCE (object);

  g_clear_object (&self->source);

  G_OBJECT_CLASS (shell_lua_notify_source_parent_class)->dispose (object);
}

static void
shell_lua_notify_source_finalize (GObject *object)
{
  ShellLuaNotifySource *self = SHELL_LUA_NOTIFY_SOURCE (object);

  g_free (self->title);
  g_free (self->icon_name);
  g_free (self->owner);
  g_clear_object (&self->icon);

  G_OBJECT_CLASS (shell_lua_notify_source_parent_class)->finalize (object);
}

static void
shell_lua_notify_source_class_init (ShellLuaNotifySourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_lua_notify_source_get_property;
  object_class->set_property = shell_lua_notify_source_set_property;
  object_class->dispose = shell_lua_notify_source_dispose;
  object_class->finalize = shell_lua_notify_source_finalize;

  ns_props[NS_PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL, NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  ns_props[NS_PROP_ICON_NAME] =
    g_param_spec_string ("icon-name", NULL, NULL, NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  ns_props[NS_PROP_COUNT] =
    g_param_spec_int ("count", NULL, NULL, 0, G_MAXINT, 0,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NS_N_PROPS, ns_props);
}

static void
shell_lua_notify_source_init (ShellLuaNotifySource *self)
{
}

static void __attribute__ ((constructor))
shell_lua_api_notify_init (void);
static void shell_lua_api_notify_register (ShellLuaState *state,
                                           lua_State     *L);

static void
shell_lua_api_notify_init (void)
{
  shell_lua_api_register ("notify", shell_lua_api_notify_register);
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static const char *
get_owner (lua_State *L)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);

  return extension != NULL ? shell_lua_extension_get_uuid (extension) : NULL;
}

static gboolean
field_present (lua_State  *L,
               int         tbl,
               const char *field)
{
  gboolean present = lua_getfield (L, tbl, field) != LUA_TNIL;

  lua_pop (L, 1);
  return present;
}

/* Raises unless the field is absent or callable */
static void
check_callable_field (lua_State  *L,
                      int         tbl,
                      const char *field)
{
  if (lua_getfield (L, tbl, field) != LUA_TNIL && lua_type (L, -1) != LUA_TFUNCTION)
    {
      if (luaL_getmetafield (L, -1, "__call") == LUA_TNIL)
        luaL_error (L, "field '%s' must be a function, got %s", field, luaL_typename (L, -1));
      lua_pop (L, 1);
    }
  lua_pop (L, 1);
}

static int
ref_function_arg (lua_State *L,
                  int        idx)
{
  if (lua_type (L, idx) != LUA_TFUNCTION)
    {
      if (luaL_getmetafield (L, idx, "__call") == LUA_TNIL)
        return luaL_typeerror (L, idx, "function");
      lua_pop (L, 1);
    }

  lua_pushvalue (L, idx);
  return luaL_ref (L, LUA_REGISTRYINDEX);
}

static GIcon *
icon_from_file (lua_State  *L,
                const char *field,
                const char *rel)
{
  GFile *file;
  GIcon *icon;

  shell_lua_api_push_extension_file (L, field, rel);
  file = g_file_new_for_path (lua_tostring (L, -1));
  lua_pop (L, 1);

  icon = g_file_icon_new (file);
  g_object_unref (file);
  return icon;
}

static ShellMessageTray *
get_tray (void)
{
  ShellMain *shell_main = shell_main_get_default ();

  return shell_main != NULL ? shell_main_get_message_tray (shell_main) : NULL;
}

/* ------------------------------------------------------------------------ */
/* Urgency, reasons                                                          */
/* ------------------------------------------------------------------------ */

static const char *
urgency_to_string (ShellNotificationUrgency urgency)
{
  switch (urgency)
    {
    case SHELL_NOTIFICATION_URGENCY_LOW:
      return "low";
    case SHELL_NOTIFICATION_URGENCY_HIGH:
      return "high";
    case SHELL_NOTIFICATION_URGENCY_CRITICAL:
      /* Not settable in API 1.0, but a notification of the shell can be */
      return "critical";
    case SHELL_NOTIFICATION_URGENCY_NORMAL:
    default:
      return "normal";
    }
}

/* Raises unless @idx is "low", "normal" or "high" ("critical" is not part
 * of API 1.0) */
static ShellNotificationUrgency
check_urgency (lua_State  *L,
               int         idx,
               const char *what)
{
  const char *value;

  if (lua_type (L, idx) != LUA_TSTRING)
    luaL_error (L, "%s must be \"low\", \"normal\" or \"high\", got %s",
                what, luaL_typename (L, idx));

  value = lua_tostring (L, idx);
  if (strcmp (value, "low") == 0)
    return SHELL_NOTIFICATION_URGENCY_LOW;
  if (strcmp (value, "normal") == 0)
    return SHELL_NOTIFICATION_URGENCY_NORMAL;
  if (strcmp (value, "high") == 0)
    return SHELL_NOTIFICATION_URGENCY_HIGH;

  luaL_error (L, "%s must be \"low\", \"normal\" or \"high\", got \"%s\"", what, value);
  return SHELL_NOTIFICATION_URGENCY_NORMAL;
}

static const char *
reason_to_string (ShellNotificationDestroyedReason reason)
{
  switch (reason)
    {
    case SHELL_NOTIFICATION_DESTROYED_REASON_EXPIRED:
      return "expired";
    case SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED:
      return "source-closed";
    case SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED:
    case SHELL_NOTIFICATION_DESTROYED_REASON_REPLACED:
    default:
      return "dismissed";
    }
}

/* ------------------------------------------------------------------------ */
/* Notification events                                                       */
/* ------------------------------------------------------------------------ */

typedef struct
{
  GWeakRef state;
} NotificationData;

static void
notification_data_free (gpointer data)
{
  NotificationData *d = data;

  g_weak_ref_clear (&d->state);
  g_free (d);
}

static void
push_reason (lua_State *L,
             gpointer   user_data)
{
  lua_pushstring (L, reason_to_string (GPOINTER_TO_INT (user_data)));
}

static void
notification_activated_cb (ShellNotification *notification,
                           ShellLuaCallback  *cb)
{
  shell_lua_callback_emit (cb, 0, NULL, NULL);
}

static void
notification_dismissed_cb (ShellNotification                *notification,
                           ShellNotificationDestroyedReason  reason,
                           ShellLuaCallback                 *cb)
{
  if (g_object_get_data (G_OBJECT (notification), DISMISSAL_SUPPRESSED_KEY) != NULL)
    return;

  shell_lua_callback_emit (cb, 1, push_reason, GINT_TO_POINTER (reason));
}

/* The notification went away without the extension asking for it: free its
 * slot (after the `on_dismissed` callbacks, which are connected before) */
static void
notification_gone_cb (ShellNotification                *notification,
                      ShellNotificationDestroyedReason  reason,
                      gpointer                          user_data)
{
  NotificationData *d = g_object_get_data (G_OBJECT (notification), STATE_KEY);
  ShellLuaState *state = d != NULL ? g_weak_ref_get (&d->state) : NULL;

  if (state == NULL)
    return;

  if (shell_lua_state_is_alive (state))
    shell_lua_destroy_object (shell_lua_state_get_L (state), G_OBJECT (notification));

  g_object_unref (state);
}

static void
notification_action_cb (ShellNotification *notification,
                        gpointer           user_data)
{
  shell_lua_callback_emit (user_data, 0, NULL, NULL);
}

/* ShellLuaOwnOps.destroy: `n:destroy ()`, the source being destroyed, disable */
static void
notification_destroy (GObject *object)
{
  suppress_dismissal (SHELL_NOTIFICATION (object));
  shell_notification_destroy (SHELL_NOTIFICATION (object),
                              SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED);
}

static const ShellLuaOwnOps notification_ops = {
  .destroy = notification_destroy,
};

/* n:on_activate(fn) */
static int
l_notification_on_activate (lua_State *L)
{
  GObject *notification = shell_lua_handle_check (L, 1, "shell.Notification");
  ShellLuaCallback *cb;
  gulong id;
  int ref;

  ref = ref_function_arg (L, 2);
  cb = shell_lua_callback_new (L, notification, ref);
  id = shell_lua_callback_connect (cb, notification, "activated",
                                   G_CALLBACK (notification_activated_cb));
  return shell_lua_callback_push_connection (L, cb, notification, id);
}

/* n:on_dismissed(fn) */
static int
l_notification_on_dismissed (lua_State *L)
{
  GObject *notification = shell_lua_handle_check (L, 1, "shell.Notification");
  ShellLuaCallback *cb;
  gulong id;
  int ref;

  ref = ref_function_arg (L, 2);
  cb = shell_lua_callback_new (L, notification, ref);
  id = shell_lua_callback_connect (cb, notification, "destroy",
                                   G_CALLBACK (notification_dismissed_cb));
  return shell_lua_callback_push_connection (L, cb, notification, id);
}

/* n:show(): asks for the banner again; the message list ignores it for low */
static int
l_notification_show (lua_State *L)
{
  ShellNotification *notification =
    SHELL_NOTIFICATION (shell_lua_handle_check (L, 1, "shell.Notification"));

  /* The change to FALSE is what asks the tray for a banner */
  shell_notification_set_acknowledged (notification, TRUE);
  shell_notification_set_acknowledged (notification, FALSE);
  return 0;
}

static void
get_urgency (lua_State *L,
             GObject   *object)
{
  lua_pushstring (L, urgency_to_string (shell_notification_get_urgency (SHELL_NOTIFICATION (object))));
}

static void
set_urgency (lua_State *L,
             GObject   *object,
             int        idx)
{
  shell_notification_set_urgency (SHELL_NOTIFICATION (object),
                                  check_urgency (L, idx, "urgency"));
}

static const luaL_Reg notification_methods[] = {
  { "show", l_notification_show },
  { "on_activate", l_notification_on_activate },
  { "on_dismissed", l_notification_on_dismissed },
  { NULL, NULL }
};

static const ShellLuaHandleMethod notification_methods_ex[] = {
  { "destroy", shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty notification_properties[] = {
  { "title", "title", TRUE, TRUE },
  { "body", "body", TRUE, TRUE },
  { "icon_name", "icon-name", TRUE, TRUE },
  { "resident", "resident", TRUE, TRUE },
  { "transient", "is-transient", TRUE, TRUE },
  { "acknowledged", "acknowledged", TRUE, TRUE },
  { NULL }
};

static const ShellLuaHandleVirtual notification_virtuals[] = {
  { "urgency", get_urgency, set_urgency },
  { NULL }
};

static ShellLuaHandleType notification_type = {
  .name = "shell.Notification",
  .methods = notification_methods,
  .methods_ex = notification_methods_ex,
  .properties = notification_properties,
  .virtuals = notification_virtuals,
};

/* ------------------------------------------------------------------------ */
/* Posting                                                                   */
/* ------------------------------------------------------------------------ */

static const char * const notify_fields[] = {
  "title", "body", "markup", "icon_name", "icon_file", "urgency", "resident", "transient",
  "actions", "on_activate", "on_dismissed", NULL
};

static const char * const action_fields[] = { "label", "on_activate", NULL };

/* Raises unless the options table at @tbl describes a notification */
static void
check_notify_options (lua_State *L,
                      int        tbl)
{
  shell_lua_check_fields (L, tbl, notify_fields);

  if (shell_lua_opt_string (L, tbl, "title", NULL) == NULL)
    luaL_error (L, "missing required field 'title' (a string)");
  shell_lua_opt_string (L, tbl, "body", NULL);
  shell_lua_opt_boolean (L, tbl, "markup", FALSE);
  shell_lua_opt_string (L, tbl, "icon_name", NULL);
  shell_lua_opt_string (L, tbl, "icon_file", NULL);
  if (field_present (L, tbl, "icon_name") && field_present (L, tbl, "icon_file"))
    luaL_error (L, "fields 'icon_name' and 'icon_file' are mutually exclusive");
  shell_lua_opt_boolean (L, tbl, "resident", FALSE);
  shell_lua_opt_boolean (L, tbl, "transient", FALSE);

  if (field_present (L, tbl, "urgency"))
    {
      lua_getfield (L, tbl, "urgency");
      check_urgency (L, -1, "field 'urgency'");
      lua_pop (L, 1);
    }

  check_callable_field (L, tbl, "on_activate");
  check_callable_field (L, tbl, "on_dismissed");

  if (field_present (L, tbl, "actions"))
    {
      lua_Integer n, i;

      lua_getfield (L, tbl, "actions");
      if (lua_type (L, -1) != LUA_TTABLE)
        luaL_error (L, "field 'actions' must be a sequence of tables, got %s", luaL_typename (L, -1));

      n = (lua_Integer) lua_rawlen (L, -1);
      if (n > MAX_ACTIONS)
        luaL_error (L, "field 'actions' has %d entries, at most %d are allowed",
                    (int) n, MAX_ACTIONS);

      for (i = 1; i <= n; i++)
        {
          int action;

          lua_rawgeti (L, -1, i);
          action = lua_gettop (L);
          if (lua_type (L, action) != LUA_TTABLE)
            luaL_error (L, "field 'actions' entry %d must be a table with 'label' and 'on_activate'",
                        (int) i);

          shell_lua_check_fields (L, action, action_fields);
          if (shell_lua_opt_string (L, action, "label", NULL) == NULL)
            luaL_error (L, "action %d: missing required field 'label' (a string)", (int) i);
          if (!field_present (L, action, "on_activate"))
            luaL_error (L, "action %d: missing required field 'on_activate' (a function)", (int) i);
          check_callable_field (L, action, "on_activate");
          lua_pop (L, 1);
        }
      lua_pop (L, 1);
    }
}

static ShellLuaNotifySource *
notify_source_new_owned (lua_State  *L,
                         const char *title,
                         const char *icon_name,
                         GIcon      *icon)
{
  const char *owner = get_owner (L);
  ShellLuaNotifySource *self;

  shell_lua_check_owned_capacity (L);

  self = g_object_new (SHELL_TYPE_LUA_NOTIFY_SOURCE, NULL);
  self->title = g_strdup (title);
  self->icon_name = g_strdup (icon_name);
  self->icon = icon != NULL ? g_object_ref (icon) : NULL;
  self->owner = g_strdup (owner);

  shell_lua_own_object (L, G_OBJECT (self), &notify_source_ops);
  g_object_unref (self);   /* the context holds it */
  return self;
}

/* The source `shell.notify{...}` posts to; created on first use */
static ShellLuaNotifySource *
get_default_source (lua_State *L)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);
  ShellLuaNotifySource *self = NULL;
  const char *title = "(inspector)";

  lua_rawgetp (L, LUA_REGISTRYINDEX, &default_source_key);
  if (!lua_isnil (L, -1) && shell_lua_handle_is_alive (L, -1))
    self = SHELL_LUA_NOTIFY_SOURCE (shell_lua_handle_check (L, -1, "shell.NotificationSource"));
  lua_pop (L, 1);
  if (self != NULL)
    return self;

  if (extension != NULL)
    {
      JsonObject *metadata = shell_lua_extension_get_metadata (extension);

      if (json_object_has_member (metadata, "name"))
        title = json_object_get_string_member (metadata, "name");
      else
        title = shell_lua_extension_get_uuid (extension);
    }

  self = notify_source_new_owned (L, title, DEFAULT_SOURCE_ICON, NULL);
  shell_lua_handle_push (L, "shell.NotificationSource", G_OBJECT (self));
  lua_rawsetp (L, LUA_REGISTRYINDEX, &default_source_key);
  return self;
}

/* Posts the notification described by the options table at @tbl, which
 * check_notify_options () accepted, to @self; pushes its handle */
static void
post_notification (lua_State            *L,
                   ShellLuaNotifySource *self,
                   int                   tbl)
{
  ShellMessageTray *tray = get_tray ();
  ShellSource *source;
  ShellNotification *notification;
  NotificationData *data;
  const char *title, *body, *icon_name, *icon_file;
  gboolean has_urgency;
  int on_activate_ref, on_dismissed_ref;

  GIcon *icon = NULL;

  tbl = lua_absindex (L, tbl);

  if (tray == NULL)
    {
      luaL_error (L, "the message tray does not exist");
      return;
    }

  title = shell_lua_opt_string (L, tbl, "title", NULL);
  body = shell_lua_opt_string (L, tbl, "body", NULL);
  icon_name = shell_lua_opt_string (L, tbl, "icon_name", NULL);
  icon_file = shell_lua_opt_string (L, tbl, "icon_file", NULL);
  has_urgency = field_present (L, tbl, "urgency");

  /* Everything that can fail comes before anything is created */
  if (icon_file != NULL)
    icon = icon_from_file (L, "icon_file", icon_file);
  shell_lua_check_owned_capacity (L);

  source = notify_source_ensure (self, tray);
  notification = shell_notification_new (source, title, body);
  shell_lua_own_object (L, G_OBJECT (notification), &notification_ops);
  g_object_unref (notification);   /* the context holds it */

  data = g_new0 (NotificationData, 1);
  g_weak_ref_init (&data->state, shell_lua_state_from_L (L));
  g_object_set_data_full (G_OBJECT (notification), STATE_KEY, data, notification_data_free);

  shell_notification_set_use_body_markup (notification,
                                          shell_lua_opt_boolean (L, tbl, "markup", FALSE));
  shell_notification_set_resident (notification,
                                   shell_lua_opt_boolean (L, tbl, "resident", FALSE));
  shell_notification_set_is_transient (notification,
                                       shell_lua_opt_boolean (L, tbl, "transient", FALSE));
  if (has_urgency)
    {
      lua_getfield (L, tbl, "urgency");
      shell_notification_set_urgency (notification, check_urgency (L, -1, "urgency"));
      lua_pop (L, 1);
    }
  if (icon_name != NULL)
    {
      shell_notification_set_icon_name (notification, icon_name);
    }
  else if (icon != NULL)
    {
      shell_notification_set_gicon (notification, icon);
    }
  g_clear_object (&icon);

  /* The destroy handlers: the callbacks of on_dismissed are connected before
   * the one that frees the slot, which runs last */
  on_dismissed_ref = shell_lua_opt_callback (L, tbl, "on_dismissed", FALSE);
  if (on_dismissed_ref != LUA_NOREF)
    {
      ShellLuaCallback *cb = shell_lua_callback_new (L, G_OBJECT (notification), on_dismissed_ref);

      shell_lua_callback_connect (cb, notification, "destroy",
                                  G_CALLBACK (notification_dismissed_cb));
    }
  g_signal_connect_after (notification, "destroy",
                          G_CALLBACK (notification_gone_cb), NULL);

  on_activate_ref = shell_lua_opt_callback (L, tbl, "on_activate", FALSE);
  if (on_activate_ref != LUA_NOREF)
    {
      ShellLuaCallback *cb = shell_lua_callback_new (L, G_OBJECT (notification), on_activate_ref);

      shell_lua_callback_connect (cb, notification, "activated",
                                  G_CALLBACK (notification_activated_cb));
    }

  if (field_present (L, tbl, "actions"))
    {
      lua_Integer n, i;

      lua_getfield (L, tbl, "actions");
      n = (lua_Integer) lua_rawlen (L, -1);
      for (i = 1; i <= n; i++)
        {
          ShellLuaCallback *cb;
          const char *label;
          int action, ref;

          lua_rawgeti (L, -1, i);
          action = lua_gettop (L);
          label = shell_lua_opt_string (L, action, "label", NULL);
          ref = shell_lua_opt_callback (L, action, "on_activate", TRUE);
          cb = shell_lua_callback_new (L, G_OBJECT (notification), ref);

          shell_notification_add_action (notification, label, notification_action_cb,
                                         shell_lua_callback_ref (cb),
                                         (GDestroyNotify) shell_lua_callback_unref);
          lua_pop (L, 1);
        }
      lua_pop (L, 1);
    }

  /* The handle first: the banner may already be a reason to call back */
  shell_lua_handle_push (L, "shell.Notification", G_OBJECT (notification));
  shell_source_add_notification (source, notification);
}

/* shell.notify{...}: a call of the table, so the table comes first */
static int
l_notify (lua_State *L)
{
  lua_remove (L, 1);
  luaL_checktype (L, 1, LUA_TTABLE);
  check_notify_options (L, 1);
  post_notification (L, get_default_source (L), 1);
  return 1;
}

/* ------------------------------------------------------------------------ */
/* shell.notify.source{...}, shell.NotificationSource                        */
/* ------------------------------------------------------------------------ */

static const char * const source_fields[] = { "title", "icon_name", "icon_file", NULL };

static int
l_notify_source (lua_State *L)
{
  ShellLuaNotifySource *self;
  const char *title, *icon_name, *icon_file;
  GIcon *icon = NULL;

  shell_lua_check_fields (L, 1, source_fields);
  title = shell_lua_opt_string (L, 1, "title", NULL);
  if (title == NULL)
    return luaL_error (L, "missing required field 'title' (a string)");
  icon_name = shell_lua_opt_string (L, 1, "icon_name", NULL);
  icon_file = shell_lua_opt_string (L, 1, "icon_file", NULL);
  if (icon_name != NULL && icon_file != NULL)
    return luaL_error (L, "fields 'icon_name' and 'icon_file' are mutually exclusive");

  shell_lua_check_owned_capacity (L);

  if (icon_file != NULL)
    icon = icon_from_file (L, "icon_file", icon_file);

  self = notify_source_new_owned (L, title, icon_name, icon);
  g_clear_object (&icon);

  shell_lua_handle_push (L, "shell.NotificationSource", G_OBJECT (self));
  return 1;
}

/* src:notify{...} */
static int
l_source_notify (lua_State *L)
{
  ShellLuaNotifySource *self =
    SHELL_LUA_NOTIFY_SOURCE (shell_lua_handle_check (L, 1, "shell.NotificationSource"));

  luaL_checktype (L, 2, LUA_TTABLE);
  check_notify_options (L, 2);
  post_notification (L, self, 2);
  return 1;
}

/* src:clear(): destroys the non-resident notifications, without on_dismissed */
static int
l_source_clear (lua_State *L)
{
  ShellLuaNotifySource *self =
    SHELL_LUA_NOTIFY_SOURCE (shell_lua_handle_check (L, 1, "shell.NotificationSource"));

  if (self->source != NULL)
    {
      g_autoptr (ShellSource) source = g_object_ref (self->source);

      notify_source_suppress_all (self, TRUE);
      shell_source_destroy_non_resident_notifications (source);
    }

  return 0;
}

static const luaL_Reg source_methods[] = {
  { "notify", l_source_notify },
  { "clear", l_source_clear },
  { NULL, NULL }
};

static const ShellLuaHandleMethod source_methods_ex[] = {
  { "destroy", shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty source_properties[] = {
  { "title", "title", TRUE, TRUE },
  { "icon_name", "icon-name", TRUE, TRUE },
  { "count", "count", TRUE, FALSE },
  { NULL }
};

static ShellLuaHandleType source_type = {
  .name = "shell.NotificationSource",
  .methods = source_methods,
  .methods_ex = source_methods_ex,
  .properties = source_properties,
};

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

static void
shell_lua_api_notify_register (ShellLuaState *state,
                               lua_State     *L)
{
  int shell = lua_absindex (L, -1);

  luaL_checkstack (L, 6, "shell.notify");

  /* The class declares the notification.* hooks */
  g_type_ensure (SHELL_TYPE_NOTIFICATION);

  notification_type.gtype = SHELL_TYPE_NOTIFICATION;
  source_type.gtype = SHELL_TYPE_LUA_NOTIFY_SOURCE;
  shell_lua_handle_register_type (L, &notification_type);
  shell_lua_handle_register_type (L, &source_type);

  /* shell.notify(opts) == a call of the table; shell.notify.source(opts) */
  lua_newtable (L);
  lua_pushcfunction (L, l_notify_source);
  lua_setfield (L, -2, "source");

  lua_newtable (L);
  lua_pushcfunction (L, l_notify);
  lua_setfield (L, -2, "__call");
  lua_pushboolean (L, 0);
  lua_setfield (L, -2, "__metatable");
  lua_setmetatable (L, -2);

  lua_setfield (L, shell, "notify");
}
