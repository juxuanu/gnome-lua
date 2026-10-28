/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * message-tray.c: Notifications and the tray that shows their banners. Port of
 * js/ui/messageTray.js and of the Source class of js/ui/messageList.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `destroy ()` of #ShellNotification and #ShellSource, and `activate ()` of
 *    #ShellNotification and `open ()` of #ShellSource, are virtual functions:
 *    the code of this file calls them through the class, as the JS code
 *    dispatched `this.destroy ()` to the subclass.
 *  - The properties `title` and `icon` of #ShellMessageSource are plain
 *    properties. JS subclasses that overrode their getters set them.
 *  - `Main.overview` and `Main.xdndHandler` drag signals are the property
 *    #ShellMessageTray:dragging, and the `focus-active-notification`
 *    keybinding of `Main.wm` calls
 *    shell_message_tray_expand_active_notification (); ShellMain connects both.
 *  - `getSystemSource ()` is a method of the tray, as the singleton needs the
 *    tray to register the source.
 *  - Hooks (docs/api/hooks.md): `notification.showing` (cancellable) before a
 *    banner is queued or updated, `notification.shown` when a banner starts to
 *    show and `notification.activated` when the user activates a notification.
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n-lib.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-backend.h>
#include <meta/meta-idle-monitor.h>
#include <meta/meta-sound-player.h>

#include "misc/gnome-session.h"
#include "shell-global.h"
#include "shell-enum-types.h"
#include "shell-hooks.h"
#include "shell-test-runner.h"
#include "shell-util.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/message-list.h"
#include "ui/message-tray.h"
#include "ui/session-mode.h"

#define NOTIFICATION_TIMEOUT 4000

#define HIDE_TIMEOUT 200
#define LONGER_HIDE_TIMEOUT 600

#define MAX_NOTIFICATIONS_IN_QUEUE 3
#define MAX_NOTIFICATIONS_PER_SOURCE 10

/* We delay hiding of the tray if the mouse is within MOUSE_LEFT_ACTOR_THRESHOLD
 * range from the point where it left the tray. */
#define MOUSE_LEFT_ACTOR_THRESHOLD 20

#define IDLE_TIME 1000

/* ------------------------------------------------------------------------ */
/* Hooks                                                                     */
/* ------------------------------------------------------------------------ */

static void
set_actor_visible (ClutterActor *actor,
                   gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

static void
declare_hooks (void)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  const char *schema = "notification-id:u source:s urgency:s resident:b "
                       "transient:b owner:s app-id:s";

  shell_hook_registry_declare (registry, "notification.showing", schema, TRUE,
                               "A notification asks for a banner; cancelling "
                               "shows neither the banner nor plays the sound");
  shell_hook_registry_declare (registry, "notification.shown", schema, FALSE,
                               "A banner starts to show");
  shell_hook_registry_declare (registry, "notification.activated", schema, FALSE,
                               "The user activated a notification");
}

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
      return "critical";
    case SHELL_NOTIFICATION_URGENCY_NORMAL:
    default:
      return "normal";
    }
}

static GVariant *
hook_payload (ShellNotification *notification)
{
  ShellSource *source = shell_notification_get_source (notification);
  GVariantBuilder builder;
  const char *title = NULL;
  const char *app_id = NULL;
  const char *owner = NULL;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "notification-id",
                         g_variant_new_uint32 (shell_notification_get_id (notification)));

  if (source != NULL)
    {
      ShellNotificationPolicy *policy = shell_source_get_policy (source);

      title = shell_message_source_get_title (SHELL_MESSAGE_SOURCE (source));
      owner = shell_source_get_owner (source);
      if (policy != NULL && SHELL_IS_NOTIFICATION_APPLICATION_POLICY (policy))
        app_id = shell_notification_policy_get_id (policy);
    }

  g_variant_builder_add (&builder, "{sv}", "source",
                         g_variant_new_string (title != NULL ? title : ""));
  g_variant_builder_add (&builder, "{sv}", "urgency",
                         g_variant_new_string (urgency_to_string (shell_notification_get_urgency (notification))));
  g_variant_builder_add (&builder, "{sv}", "resident",
                         g_variant_new_boolean (shell_notification_get_resident (notification)));
  g_variant_builder_add (&builder, "{sv}", "transient",
                         g_variant_new_boolean (shell_notification_get_is_transient (notification)));
  g_variant_builder_add (&builder, "{sv}", "owner",
                         g_variant_new_string (owner != NULL ? owner : ""));
  if (app_id != NULL)
    g_variant_builder_add (&builder, "{sv}", "app-id", g_variant_new_string (app_id));

  return g_variant_builder_end (&builder);
}

static void
emit_hook_after (const char        *name,
                 ShellNotification *notification)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();

  if (!shell_hook_registry_is_declared (registry, name))
    return;

  shell_hook_registry_emit_after (registry, name, hook_payload (notification));
}

static gboolean
emit_hook_before (const char        *name,
                  ShellNotification *notification)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  ShellHookVerdict verdict;

  if (!shell_hook_registry_is_declared (registry, name))
    return TRUE;

  verdict = shell_hook_registry_emit_before (registry, name,
                                             hook_payload (notification), NULL);

  return verdict != SHELL_HOOK_VERDICT_CANCEL;
}

/* ------------------------------------------------------------------------ */
/* ShellMessageSource                                                        */
/* ------------------------------------------------------------------------ */

enum
{
  SOURCE_PROP_0,

  SOURCE_PROP_TITLE,
  SOURCE_PROP_ICON,
  SOURCE_PROP_ICON_NAME,

  SOURCE_N_PROPS
};

static GParamSpec *message_source_props[SOURCE_N_PROPS] = { NULL, };

typedef struct
{
  char  *title;
  GIcon *icon;
} ShellMessageSourcePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellMessageSource, shell_message_source, G_TYPE_OBJECT)

/**
 * shell_message_source_new:
 *
 * Creates a source without a title or an icon.
 *
 * Returns: (transfer full): a new #ShellMessageSource
 */
ShellMessageSource *
shell_message_source_new (void)
{
  return g_object_new (SHELL_TYPE_MESSAGE_SOURCE, NULL);
}

/**
 * shell_message_source_get_title:
 * @self: a #ShellMessageSource
 *
 * Returns: (nullable): #ShellMessageSource:title
 */
const char *
shell_message_source_get_title (ShellMessageSource *self)
{
  ShellMessageSourcePrivate *priv;

  g_return_val_if_fail (SHELL_IS_MESSAGE_SOURCE (self), NULL);

  priv = shell_message_source_get_instance_private (self);
  return priv->title;
}

/**
 * shell_message_source_set_title:
 * @self: a #ShellMessageSource
 * @title: (nullable): the new title
 *
 * Sets #ShellMessageSource:title.
 */
void
shell_message_source_set_title (ShellMessageSource *self,
                                const char         *title)
{
  ShellMessageSourcePrivate *priv;

  g_return_if_fail (SHELL_IS_MESSAGE_SOURCE (self));

  priv = shell_message_source_get_instance_private (self);

  g_free (priv->title);
  priv->title = g_strdup (title);
  g_object_notify_by_pspec (G_OBJECT (self), message_source_props[SOURCE_PROP_TITLE]);
}

/**
 * shell_message_source_get_icon:
 * @self: a #ShellMessageSource
 *
 * Returns: (transfer none) (nullable): #ShellMessageSource:icon
 */
GIcon *
shell_message_source_get_icon (ShellMessageSource *self)
{
  ShellMessageSourcePrivate *priv;

  g_return_val_if_fail (SHELL_IS_MESSAGE_SOURCE (self), NULL);

  priv = shell_message_source_get_instance_private (self);
  return priv->icon;
}

/**
 * shell_message_source_set_icon:
 * @self: a #ShellMessageSource
 * @icon: (nullable): the new icon
 *
 * Sets #ShellMessageSource:icon.
 */
void
shell_message_source_set_icon (ShellMessageSource *self,
                               GIcon              *icon)
{
  ShellMessageSourcePrivate *priv;

  g_return_if_fail (SHELL_IS_MESSAGE_SOURCE (self));
  g_return_if_fail (icon == NULL || G_IS_ICON (icon));

  priv = shell_message_source_get_instance_private (self);

  g_set_object (&priv->icon, icon);
  g_object_notify_by_pspec (G_OBJECT (self), message_source_props[SOURCE_PROP_ICON]);
  g_object_notify_by_pspec (G_OBJECT (self), message_source_props[SOURCE_PROP_ICON_NAME]);
}

/**
 * shell_message_source_get_icon_name:
 * @self: a #ShellMessageSource
 *
 * Returns: (nullable): the name of the icon if it is a themed icon
 */
const char *
shell_message_source_get_icon_name (ShellMessageSource *self)
{
  ShellMessageSourcePrivate *priv;

  g_return_val_if_fail (SHELL_IS_MESSAGE_SOURCE (self), NULL);

  priv = shell_message_source_get_instance_private (self);

  if (priv->icon != NULL && G_IS_THEMED_ICON (priv->icon))
    {
      const char * const *names = g_themed_icon_get_names (G_THEMED_ICON (priv->icon));

      return names != NULL ? names[0] : NULL;
    }

  return NULL;
}

/**
 * shell_message_source_set_icon_name:
 * @self: a #ShellMessageSource
 * @icon_name: (nullable): the name of a themed icon
 *
 * Sets #ShellMessageSource:icon to the themed icon @icon_name.
 */
void
shell_message_source_set_icon_name (ShellMessageSource *self,
                                    const char         *icon_name)
{
  g_autoptr (GIcon) icon = NULL;

  g_return_if_fail (SHELL_IS_MESSAGE_SOURCE (self));

  if (icon_name != NULL)
    icon = g_themed_icon_new (icon_name);

  shell_message_source_set_icon (self, icon);
}

static void
shell_message_source_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  ShellMessageSource *self = SHELL_MESSAGE_SOURCE (object);

  switch (prop_id)
    {
    case SOURCE_PROP_TITLE:
      g_value_set_string (value, shell_message_source_get_title (self));
      break;

    case SOURCE_PROP_ICON:
      g_value_set_object (value, shell_message_source_get_icon (self));
      break;

    case SOURCE_PROP_ICON_NAME:
      g_value_set_string (value, shell_message_source_get_icon_name (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_message_source_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  ShellMessageSource *self = SHELL_MESSAGE_SOURCE (object);

  switch (prop_id)
    {
    case SOURCE_PROP_TITLE:
      shell_message_source_set_title (self, g_value_get_string (value));
      break;

    case SOURCE_PROP_ICON:
      shell_message_source_set_icon (self, g_value_get_object (value));
      break;

    case SOURCE_PROP_ICON_NAME:
      shell_message_source_set_icon_name (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_message_source_dispose (GObject *object)
{
  ShellMessageSource *self = SHELL_MESSAGE_SOURCE (object);
  ShellMessageSourcePrivate *priv = shell_message_source_get_instance_private (self);

  g_clear_object (&priv->icon);

  G_OBJECT_CLASS (shell_message_source_parent_class)->dispose (object);
}

static void
shell_message_source_finalize (GObject *object)
{
  ShellMessageSource *self = SHELL_MESSAGE_SOURCE (object);
  ShellMessageSourcePrivate *priv = shell_message_source_get_instance_private (self);

  g_clear_pointer (&priv->title, g_free);

  G_OBJECT_CLASS (shell_message_source_parent_class)->finalize (object);
}

static void
shell_message_source_class_init (ShellMessageSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_message_source_get_property;
  object_class->set_property = shell_message_source_set_property;
  object_class->dispose = shell_message_source_dispose;
  object_class->finalize = shell_message_source_finalize;

  /**
   * ShellMessageSource:title:
   *
   * The name of the source, shown in the header of its messages.
   */
  message_source_props[SOURCE_PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMessageSource:icon:
   *
   * The icon of the source.
   */
  message_source_props[SOURCE_PROP_ICON] =
    g_param_spec_object ("icon", NULL, NULL,
                         G_TYPE_ICON,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMessageSource:icon-name:
   *
   * The name of the icon of the source, if it is a themed icon. Setting it
   * replaces #ShellMessageSource:icon.
   */
  message_source_props[SOURCE_PROP_ICON_NAME] =
    g_param_spec_string ("icon-name", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, SOURCE_N_PROPS, message_source_props);
}

static void
shell_message_source_init (ShellMessageSource *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellNotificationPolicy                                                   */
/* ------------------------------------------------------------------------ */

enum
{
  POLICY_PROP_0,

  POLICY_PROP_ID,
  POLICY_PROP_ENABLE,
  POLICY_PROP_ENABLE_SOUND,
  POLICY_PROP_SHOW_BANNERS,
  POLICY_PROP_FORCE_EXPANDED,
  POLICY_PROP_SHOW_IN_LOCK_SCREEN,
  POLICY_PROP_DETAILS_IN_LOCK_SCREEN,

  POLICY_N_PROPS
};

static GParamSpec *policy_props[POLICY_N_PROPS] = { NULL, };

typedef struct
{
  char *id;
} ShellNotificationPolicyPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (ShellNotificationPolicy, shell_notification_policy, G_TYPE_OBJECT)

/**
 * shell_notification_policy_new_for_app:
 * @app: (nullable): the application, or %NULL for the generic policy
 *
 * Creates a new policy for @app. This will be a
 * #ShellNotificationApplicationPolicy for valid apps, or a
 * #ShellNotificationGenericPolicy otherwise (`NotificationPolicy.newForApp ()`).
 *
 * Returns: (transfer full): a new #ShellNotificationPolicy
 */
ShellNotificationPolicy *
shell_notification_policy_new_for_app (ShellApp *app)
{
  g_return_val_if_fail (app == NULL || SHELL_IS_APP (app), NULL);

  /* fallback to generic policy */
  if (app == NULL || shell_app_get_app_info (app) == NULL)
    return SHELL_NOTIFICATION_POLICY (shell_notification_generic_policy_new ());
  else
    {
      g_autofree char *id = g_strdup (shell_app_get_id (app));

      if (g_str_has_suffix (id, ".desktop"))
        id[strlen (id) - strlen (".desktop")] = '\0';

      return SHELL_NOTIFICATION_POLICY (shell_notification_application_policy_new (id));
    }
}

/**
 * shell_notification_policy_get_id:
 * @self: a #ShellNotificationPolicy
 *
 * Returns: (nullable): #ShellNotificationPolicy:id
 */
const char *
shell_notification_policy_get_id (ShellNotificationPolicy *self)
{
  ShellNotificationPolicyPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION_POLICY (self), NULL);

  priv = shell_notification_policy_get_instance_private (self);
  return priv->id;
}

/**
 * shell_notification_policy_store:
 * @self: a #ShellNotificationPolicy
 *
 * Registers that a notification of the policy was received, so that the
 * settings application lists it (`store ()`). Does nothing for the generic
 * policy.
 */
void
shell_notification_policy_store (ShellNotificationPolicy *self)
{
  ShellNotificationPolicyClass *klass;

  g_return_if_fail (SHELL_IS_NOTIFICATION_POLICY (self));

  klass = SHELL_NOTIFICATION_POLICY_GET_CLASS (self);
  if (klass->store != NULL)
    klass->store (self);
}

/**
 * shell_notification_policy_destroy:
 * @self: a #ShellNotificationPolicy
 *
 * Disposes the policy, which stops it from watching the settings.
 */
void
shell_notification_policy_destroy (ShellNotificationPolicy *self)
{
  g_return_if_fail (SHELL_IS_NOTIFICATION_POLICY (self));

  g_object_run_dispose (G_OBJECT (self));
}

#define POLICY_GETTER(name)                                                     \
  gboolean                                                                      \
  shell_notification_policy_get_##name (ShellNotificationPolicy *self)          \
  {                                                                             \
    ShellNotificationPolicyClass *klass;                                        \
                                                                                \
    g_return_val_if_fail (SHELL_IS_NOTIFICATION_POLICY (self), FALSE);          \
                                                                                \
    klass = SHELL_NOTIFICATION_POLICY_GET_CLASS (self);                         \
    return klass->get_##name (self);                                            \
  }

/**
 * shell_notification_policy_get_enable:
 * @self: a #ShellNotificationPolicy
 *
 * Returns: #ShellNotificationPolicy:enable
 */
POLICY_GETTER (enable)

/**
 * shell_notification_policy_get_enable_sound:
 * @self: a #ShellNotificationPolicy
 *
 * Returns: #ShellNotificationPolicy:enable-sound
 */
POLICY_GETTER (enable_sound)

/**
 * shell_notification_policy_get_show_banners:
 * @self: a #ShellNotificationPolicy
 *
 * Returns: #ShellNotificationPolicy:show-banners
 */
POLICY_GETTER (show_banners)

/**
 * shell_notification_policy_get_force_expanded:
 * @self: a #ShellNotificationPolicy
 *
 * Returns: #ShellNotificationPolicy:force-expanded
 */
POLICY_GETTER (force_expanded)

/**
 * shell_notification_policy_get_show_in_lock_screen:
 * @self: a #ShellNotificationPolicy
 *
 * Returns: #ShellNotificationPolicy:show-in-lock-screen
 */
POLICY_GETTER (show_in_lock_screen)

/**
 * shell_notification_policy_get_details_in_lock_screen:
 * @self: a #ShellNotificationPolicy
 *
 * Returns: #ShellNotificationPolicy:details-in-lock-screen
 */
POLICY_GETTER (details_in_lock_screen)

static gboolean policy_default_true  (ShellNotificationPolicy *self) { return TRUE; }
static gboolean policy_default_false (ShellNotificationPolicy *self) { return FALSE; }

static void
policy_default_store (ShellNotificationPolicy *self)
{
  /* Do nothing for the default policy. This is only useful for the GSettings
   * policy. */
}

static void
shell_notification_policy_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellNotificationPolicy *self = SHELL_NOTIFICATION_POLICY (object);

  switch (prop_id)
    {
    case POLICY_PROP_ID:
      g_value_set_string (value, shell_notification_policy_get_id (self));
      break;

    case POLICY_PROP_ENABLE:
      g_value_set_boolean (value, shell_notification_policy_get_enable (self));
      break;

    case POLICY_PROP_ENABLE_SOUND:
      g_value_set_boolean (value, shell_notification_policy_get_enable_sound (self));
      break;

    case POLICY_PROP_SHOW_BANNERS:
      g_value_set_boolean (value, shell_notification_policy_get_show_banners (self));
      break;

    case POLICY_PROP_FORCE_EXPANDED:
      g_value_set_boolean (value, shell_notification_policy_get_force_expanded (self));
      break;

    case POLICY_PROP_SHOW_IN_LOCK_SCREEN:
      g_value_set_boolean (value, shell_notification_policy_get_show_in_lock_screen (self));
      break;

    case POLICY_PROP_DETAILS_IN_LOCK_SCREEN:
      g_value_set_boolean (value, shell_notification_policy_get_details_in_lock_screen (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_notification_policy_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellNotificationPolicy *self = SHELL_NOTIFICATION_POLICY (object);
  ShellNotificationPolicyPrivate *priv = shell_notification_policy_get_instance_private (self);

  switch (prop_id)
    {
    case POLICY_PROP_ID:
      g_free (priv->id);
      priv->id = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_notification_policy_finalize (GObject *object)
{
  ShellNotificationPolicy *self = SHELL_NOTIFICATION_POLICY (object);
  ShellNotificationPolicyPrivate *priv = shell_notification_policy_get_instance_private (self);

  g_clear_pointer (&priv->id, g_free);

  G_OBJECT_CLASS (shell_notification_policy_parent_class)->finalize (object);
}

static void
shell_notification_policy_class_init (ShellNotificationPolicyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_notification_policy_get_property;
  object_class->set_property = shell_notification_policy_set_property;
  object_class->finalize = shell_notification_policy_finalize;

  klass->store = policy_default_store;
  klass->get_enable = policy_default_true;
  klass->get_enable_sound = policy_default_true;
  klass->get_show_banners = policy_default_true;
  klass->get_force_expanded = policy_default_false;
  klass->get_show_in_lock_screen = policy_default_false;
  klass->get_details_in_lock_screen = policy_default_false;

  /**
   * ShellNotificationPolicy:id:
   *
   * The identifier of the policy: the application id without ".desktop" for
   * an application policy, "generic" for the generic one.
   */
  policy_props[POLICY_PROP_ID] =
    g_param_spec_string ("id", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellNotificationPolicy:enable:
   *
   * Whether the notifications of the source are enabled.
   */
  policy_props[POLICY_PROP_ENABLE] =
    g_param_spec_boolean ("enable", NULL, NULL,
                          TRUE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellNotificationPolicy:enable-sound:
   *
   * Whether the sound of the notifications is played.
   */
  policy_props[POLICY_PROP_ENABLE_SOUND] =
    g_param_spec_boolean ("enable-sound", NULL, NULL,
                          TRUE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellNotificationPolicy:show-banners:
   *
   * Whether the notifications are shown as banners.
   */
  policy_props[POLICY_PROP_SHOW_BANNERS] =
    g_param_spec_boolean ("show-banners", NULL, NULL,
                          TRUE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellNotificationPolicy:force-expanded:
   *
   * Whether the banners are always expanded.
   */
  policy_props[POLICY_PROP_FORCE_EXPANDED] =
    g_param_spec_boolean ("force-expanded", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellNotificationPolicy:show-in-lock-screen:
   *
   * Whether the notifications are shown on the lock screen.
   */
  policy_props[POLICY_PROP_SHOW_IN_LOCK_SCREEN] =
    g_param_spec_boolean ("show-in-lock-screen", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellNotificationPolicy:details-in-lock-screen:
   *
   * Whether the title and body of the notifications are shown on the lock
   * screen.
   */
  policy_props[POLICY_PROP_DETAILS_IN_LOCK_SCREEN] =
    g_param_spec_boolean ("details-in-lock-screen", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, POLICY_N_PROPS, policy_props);
}

static void
shell_notification_policy_init (ShellNotificationPolicy *self)
{
}

static void
policy_set_default_id (ShellNotificationPolicy *self,
                       const char              *id)
{
  ShellNotificationPolicyPrivate *priv = shell_notification_policy_get_instance_private (self);

  if (priv->id == NULL)
    priv->id = g_strdup (id);
}

static void
policy_settings_changed_cb (GSettings *settings,
                            const char *key,
                            gpointer    user_data)
{
  GObject *policy = user_data;

  if (g_object_class_find_property (G_OBJECT_GET_CLASS (policy), key) != NULL)
    g_object_notify (policy, key);
}

/* ------------------------------------------------------------------------ */
/* ShellNotificationGenericPolicy                                            */
/* ------------------------------------------------------------------------ */

struct _ShellNotificationGenericPolicy
{
  ShellNotificationPolicy parent;

  GSettings *master_settings;
};

G_DEFINE_FINAL_TYPE (ShellNotificationGenericPolicy, shell_notification_generic_policy,
                     SHELL_TYPE_NOTIFICATION_POLICY)

/**
 * shell_notification_generic_policy_new:
 *
 * Creates the policy that follows the notification settings of the desktop
 * and has no application.
 *
 * Returns: (transfer full): a new #ShellNotificationGenericPolicy
 */
ShellNotificationGenericPolicy *
shell_notification_generic_policy_new (void)
{
  return g_object_new (SHELL_TYPE_NOTIFICATION_GENERIC_POLICY, NULL);
}

static gboolean
generic_policy_get_show_banners (ShellNotificationPolicy *policy)
{
  ShellNotificationGenericPolicy *self = SHELL_NOTIFICATION_GENERIC_POLICY (policy);

  if (self->master_settings == NULL)
    return TRUE;

  return g_settings_get_boolean (self->master_settings, "show-banners");
}

static gboolean
generic_policy_get_show_in_lock_screen (ShellNotificationPolicy *policy)
{
  ShellNotificationGenericPolicy *self = SHELL_NOTIFICATION_GENERIC_POLICY (policy);

  if (self->master_settings == NULL)
    return FALSE;

  return g_settings_get_boolean (self->master_settings, "show-in-lock-screen");
}

static void
shell_notification_generic_policy_constructed (GObject *object)
{
  ShellNotificationGenericPolicy *self = SHELL_NOTIFICATION_GENERIC_POLICY (object);

  G_OBJECT_CLASS (shell_notification_generic_policy_parent_class)->constructed (object);

  policy_set_default_id (SHELL_NOTIFICATION_POLICY (self), "generic");

  self->master_settings = g_settings_new ("org.gnome.desktop.notifications");
  g_signal_connect (self->master_settings, "changed",
                    G_CALLBACK (policy_settings_changed_cb), self);
}

static void
shell_notification_generic_policy_dispose (GObject *object)
{
  ShellNotificationGenericPolicy *self = SHELL_NOTIFICATION_GENERIC_POLICY (object);

  if (self->master_settings != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->master_settings, self);
      g_clear_object (&self->master_settings);
    }

  G_OBJECT_CLASS (shell_notification_generic_policy_parent_class)->dispose (object);
}

static void
shell_notification_generic_policy_class_init (ShellNotificationGenericPolicyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellNotificationPolicyClass *policy_class = SHELL_NOTIFICATION_POLICY_CLASS (klass);

  object_class->constructed = shell_notification_generic_policy_constructed;
  object_class->dispose = shell_notification_generic_policy_dispose;

  policy_class->get_show_banners = generic_policy_get_show_banners;
  policy_class->get_show_in_lock_screen = generic_policy_get_show_in_lock_screen;
}

static void
shell_notification_generic_policy_init (ShellNotificationGenericPolicy *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellNotificationApplicationPolicy                                        */
/* ------------------------------------------------------------------------ */

struct _ShellNotificationApplicationPolicy
{
  ShellNotificationPolicy parent;

  char *canonical_id;
  GSettings *master_settings;
  GSettings *settings;
};

G_DEFINE_FINAL_TYPE (ShellNotificationApplicationPolicy, shell_notification_application_policy,
                     SHELL_TYPE_NOTIFICATION_POLICY)

/**
 * shell_notification_application_policy_new:
 * @id: the application id, without ".desktop"
 *
 * Creates the policy that follows the notification settings of an
 * application.
 *
 * Returns: (transfer full): a new #ShellNotificationApplicationPolicy
 */
ShellNotificationApplicationPolicy *
shell_notification_application_policy_new (const char *id)
{
  g_return_val_if_fail (id != NULL, NULL);

  return g_object_new (SHELL_TYPE_NOTIFICATION_APPLICATION_POLICY,
                       "id", id,
                       NULL);
}

/* Keys are restricted to lowercase alphanumeric characters and dash, and two
 * dashes cannot be in succession */
static char *
canonicalize_id (const char *id)
{
  g_autofree char *lower = g_utf8_strdown (id, -1);
  GString *canonical = g_string_new (NULL);
  const char *p;

  for (p = lower; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      char out;

      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
        out = (char) c;
      else
        out = '-';

      if (out == '-' && canonical->len > 0 && canonical->str[canonical->len - 1] == '-')
        continue;

      g_string_append_c (canonical, out);
    }

  return g_string_free (canonical, FALSE);
}

static void
application_policy_store (ShellNotificationPolicy *policy)
{
  ShellNotificationApplicationPolicy *self = SHELL_NOTIFICATION_APPLICATION_POLICY (policy);
  g_autofree char *application_id = NULL;
  g_auto (GStrv) apps = NULL;

  if (self->settings == NULL || self->master_settings == NULL)
    return;

  application_id = g_strdup_printf ("%s.desktop",
                                    shell_notification_policy_get_id (policy));
  g_settings_set_string (self->settings, "application-id", application_id);

  apps = g_settings_get_strv (self->master_settings, "application-children");
  if (!g_strv_contains ((const char * const *) apps, self->canonical_id))
    {
      g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();

      g_strv_builder_addv (builder, (const char **) apps);
      g_strv_builder_add (builder, self->canonical_id);

      {
        g_auto (GStrv) new_apps = g_strv_builder_end (builder);

        g_settings_set_strv (self->master_settings, "application-children",
                             (const char * const *) new_apps);
      }
    }
}

static gboolean
application_policy_get_enable (ShellNotificationPolicy *policy)
{
  ShellNotificationApplicationPolicy *self = SHELL_NOTIFICATION_APPLICATION_POLICY (policy);

  if (self->settings == NULL)
    return TRUE;

  return g_settings_get_boolean (self->settings, "enable");
}

static gboolean
application_policy_get_enable_sound (ShellNotificationPolicy *policy)
{
  ShellNotificationApplicationPolicy *self = SHELL_NOTIFICATION_APPLICATION_POLICY (policy);

  if (self->settings == NULL)
    return TRUE;

  return g_settings_get_boolean (self->settings, "enable-sound-alerts");
}

static gboolean
application_policy_get_show_banners (ShellNotificationPolicy *policy)
{
  ShellNotificationApplicationPolicy *self = SHELL_NOTIFICATION_APPLICATION_POLICY (policy);

  if (self->settings == NULL || self->master_settings == NULL)
    return TRUE;

  return g_settings_get_boolean (self->master_settings, "show-banners") &&
         g_settings_get_boolean (self->settings, "show-banners");
}

static gboolean
application_policy_get_force_expanded (ShellNotificationPolicy *policy)
{
  ShellNotificationApplicationPolicy *self = SHELL_NOTIFICATION_APPLICATION_POLICY (policy);

  if (self->settings == NULL)
    return FALSE;

  return g_settings_get_boolean (self->settings, "force-expanded");
}

static gboolean
application_policy_get_show_in_lock_screen (ShellNotificationPolicy *policy)
{
  ShellNotificationApplicationPolicy *self = SHELL_NOTIFICATION_APPLICATION_POLICY (policy);

  if (self->settings == NULL || self->master_settings == NULL)
    return FALSE;

  return g_settings_get_boolean (self->master_settings, "show-in-lock-screen") &&
         g_settings_get_boolean (self->settings, "show-in-lock-screen");
}

static gboolean
application_policy_get_details_in_lock_screen (ShellNotificationPolicy *policy)
{
  ShellNotificationApplicationPolicy *self = SHELL_NOTIFICATION_APPLICATION_POLICY (policy);

  if (self->settings == NULL)
    return FALSE;

  return g_settings_get_boolean (self->settings, "details-in-lock-screen");
}

static void
shell_notification_application_policy_constructed (GObject *object)
{
  ShellNotificationApplicationPolicy *self = SHELL_NOTIFICATION_APPLICATION_POLICY (object);
  const char *id;
  g_autofree char *path = NULL;

  G_OBJECT_CLASS (shell_notification_application_policy_parent_class)->constructed (object);

  id = shell_notification_policy_get_id (SHELL_NOTIFICATION_POLICY (self));
  if (id == NULL)
    id = "";

  self->canonical_id = canonicalize_id (id);

  self->master_settings = g_settings_new ("org.gnome.desktop.notifications");
  path = g_strdup_printf ("/org/gnome/desktop/notifications/application/%s/",
                          self->canonical_id);
  self->settings = g_settings_new_with_path ("org.gnome.desktop.notifications.application",
                                             path);

  g_signal_connect (self->master_settings, "changed",
                    G_CALLBACK (policy_settings_changed_cb), self);
  g_signal_connect (self->settings, "changed",
                    G_CALLBACK (policy_settings_changed_cb), self);
}

static void
shell_notification_application_policy_dispose (GObject *object)
{
  ShellNotificationApplicationPolicy *self = SHELL_NOTIFICATION_APPLICATION_POLICY (object);

  if (self->master_settings != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->master_settings, self);
      g_clear_object (&self->master_settings);
    }

  if (self->settings != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->settings, self);
      g_clear_object (&self->settings);
    }

  G_OBJECT_CLASS (shell_notification_application_policy_parent_class)->dispose (object);
}

static void
shell_notification_application_policy_finalize (GObject *object)
{
  ShellNotificationApplicationPolicy *self = SHELL_NOTIFICATION_APPLICATION_POLICY (object);

  g_clear_pointer (&self->canonical_id, g_free);

  G_OBJECT_CLASS (shell_notification_application_policy_parent_class)->finalize (object);
}

static void
shell_notification_application_policy_class_init (ShellNotificationApplicationPolicyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellNotificationPolicyClass *policy_class = SHELL_NOTIFICATION_POLICY_CLASS (klass);

  object_class->constructed = shell_notification_application_policy_constructed;
  object_class->dispose = shell_notification_application_policy_dispose;
  object_class->finalize = shell_notification_application_policy_finalize;

  policy_class->store = application_policy_store;
  policy_class->get_enable = application_policy_get_enable;
  policy_class->get_enable_sound = application_policy_get_enable_sound;
  policy_class->get_show_banners = application_policy_get_show_banners;
  policy_class->get_force_expanded = application_policy_get_force_expanded;
  policy_class->get_show_in_lock_screen = application_policy_get_show_in_lock_screen;
  policy_class->get_details_in_lock_screen = application_policy_get_details_in_lock_screen;
}

static void
shell_notification_application_policy_init (ShellNotificationApplicationPolicy *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellNotificationSound                                                    */
/* ------------------------------------------------------------------------ */

enum
{
  SOUND_PROP_0,

  SOUND_PROP_FILE,
  SOUND_PROP_NAME,

  SOUND_N_PROPS
};

static GParamSpec *sound_props[SOUND_N_PROPS] = { NULL, };

struct _ShellNotificationSound
{
  GObject parent;

  GFile *file;
  char *name;
};

G_DEFINE_FINAL_TYPE (ShellNotificationSound, shell_notification_sound, G_TYPE_OBJECT)

/**
 * shell_notification_sound_new:
 * @file: (nullable): a sound file
 * @name: (nullable): the name of a sound of the sound theme, which is used in
 *   preference to @file
 *
 * Creates a sound for a notification (`new Sound (file, themedName)`).
 *
 * Returns: (transfer full): a new #ShellNotificationSound
 */
ShellNotificationSound *
shell_notification_sound_new (GFile      *file,
                              const char *name)
{
  g_return_val_if_fail (file == NULL || G_IS_FILE (file), NULL);

  return g_object_new (SHELL_TYPE_NOTIFICATION_SOUND,
                       "file", file,
                       "name", name,
                       NULL);
}

/**
 * shell_notification_sound_play:
 * @self: a #ShellNotificationSound
 *
 * Plays the sound.
 */
void
shell_notification_sound_play (ShellNotificationSound *self)
{
  MetaSoundPlayer *player;

  g_return_if_fail (SHELL_IS_NOTIFICATION_SOUND (self));

  if (self->name == NULL && self->file == NULL)
    return;

  player = meta_display_get_sound_player (shell_global_get_display (shell_global_get ()));
  if (player == NULL)
    return;

  if (self->name != NULL)
    meta_sound_player_play_from_theme (player, self->name, _("Notification sound"), NULL);
  else
    meta_sound_player_play_from_file (player, self->file, _("Notification sound"), NULL);
}

static void
shell_notification_sound_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellNotificationSound *self = SHELL_NOTIFICATION_SOUND (object);

  switch (prop_id)
    {
    case SOUND_PROP_FILE:
      g_value_set_object (value, self->file);
      break;

    case SOUND_PROP_NAME:
      g_value_set_string (value, self->name);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_notification_sound_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellNotificationSound *self = SHELL_NOTIFICATION_SOUND (object);

  switch (prop_id)
    {
    case SOUND_PROP_FILE:
      g_set_object (&self->file, g_value_get_object (value));
      break;

    case SOUND_PROP_NAME:
      g_free (self->name);
      self->name = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_notification_sound_finalize (GObject *object)
{
  ShellNotificationSound *self = SHELL_NOTIFICATION_SOUND (object);

  g_clear_object (&self->file);
  g_clear_pointer (&self->name, g_free);

  G_OBJECT_CLASS (shell_notification_sound_parent_class)->finalize (object);
}

static void
shell_notification_sound_class_init (ShellNotificationSoundClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_notification_sound_get_property;
  object_class->set_property = shell_notification_sound_set_property;
  object_class->finalize = shell_notification_sound_finalize;

  /**
   * ShellNotificationSound:file:
   *
   * The sound file, used when there is no #ShellNotificationSound:name.
   */
  sound_props[SOUND_PROP_FILE] =
    g_param_spec_object ("file", NULL, NULL,
                         G_TYPE_FILE,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellNotificationSound:name:
   *
   * The name of a sound of the sound theme.
   */
  sound_props[SOUND_PROP_NAME] =
    g_param_spec_string ("name", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, SOUND_N_PROPS, sound_props);
}

static void
shell_notification_sound_init (ShellNotificationSound *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellNotificationAction                                                   */
/* ------------------------------------------------------------------------ */

enum
{
  ACTION_PROP_0,

  ACTION_PROP_LABEL,

  ACTION_N_PROPS
};

static GParamSpec *action_props[ACTION_N_PROPS] = { NULL, };

struct _ShellNotificationAction
{
  GObject parent;

  char *label;
  ShellNotification *notification; /* weak */
  ShellNotificationActionFunc callback;
  gpointer user_data;
  GDestroyNotify destroy;
};

G_DEFINE_FINAL_TYPE (ShellNotificationAction, shell_notification_action, G_TYPE_OBJECT)

/**
 * shell_notification_action_get_label:
 * @self: a #ShellNotificationAction
 *
 * Returns: #ShellNotificationAction:label
 */
const char *
shell_notification_action_get_label (ShellNotificationAction *self)
{
  g_return_val_if_fail (SHELL_IS_NOTIFICATION_ACTION (self), NULL);

  return self->label;
}

/**
 * shell_notification_action_activate:
 * @self: a #ShellNotificationAction
 *
 * Runs the callback of the action. Unless the notification is resident it is
 * destroyed afterwards: a resident notification commonly updates itself with
 * new information after the action, and the updated information is displayed
 * in place rather than in a new pop-up.
 */
void
shell_notification_action_activate (ShellNotificationAction *self)
{
  g_autoptr (ShellNotification) notification = NULL;

  g_return_if_fail (SHELL_IS_NOTIFICATION_ACTION (self));

  g_object_ref (self);

  if (self->notification != NULL)
    notification = g_object_ref (self->notification);

  if (self->callback != NULL)
    self->callback (notification, self->user_data);

  if (notification != NULL && !shell_notification_get_resident (notification))
    shell_notification_destroy (notification, SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);

  g_object_unref (self);
}

static void
action_notification_notify_cb (gpointer  data,
                               GObject  *where_the_object_was)
{
  ShellNotificationAction *self = data;

  self->notification = NULL;
}

static void
shell_notification_action_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellNotificationAction *self = SHELL_NOTIFICATION_ACTION (object);

  switch (prop_id)
    {
    case ACTION_PROP_LABEL:
      g_value_set_string (value, self->label);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_notification_action_dispose (GObject *object)
{
  ShellNotificationAction *self = SHELL_NOTIFICATION_ACTION (object);

  if (self->notification != NULL)
    {
      g_object_weak_unref (G_OBJECT (self->notification),
                           action_notification_notify_cb, self);
      self->notification = NULL;
    }

  G_OBJECT_CLASS (shell_notification_action_parent_class)->dispose (object);
}

static void
shell_notification_action_finalize (GObject *object)
{
  ShellNotificationAction *self = SHELL_NOTIFICATION_ACTION (object);

  if (self->destroy != NULL)
    self->destroy (self->user_data);

  g_clear_pointer (&self->label, g_free);

  G_OBJECT_CLASS (shell_notification_action_parent_class)->finalize (object);
}

static void
shell_notification_action_class_init (ShellNotificationActionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_notification_action_get_property;
  object_class->dispose = shell_notification_action_dispose;
  object_class->finalize = shell_notification_action_finalize;

  /**
   * ShellNotificationAction:label:
   *
   * The label of the button of the action.
   */
  action_props[ACTION_PROP_LABEL] =
    g_param_spec_string ("label", NULL, NULL,
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, ACTION_N_PROPS, action_props);
}

static void
shell_notification_action_init (ShellNotificationAction *self)
{
}

static ShellNotificationAction *
shell_notification_action_new (ShellNotification           *notification,
                               const char                  *label,
                               ShellNotificationActionFunc  callback,
                               gpointer                     user_data,
                               GDestroyNotify               destroy)
{
  ShellNotificationAction *self = g_object_new (SHELL_TYPE_NOTIFICATION_ACTION, NULL);

  self->label = g_strdup (label);
  self->callback = callback;
  self->user_data = user_data;
  self->destroy = destroy;

  self->notification = notification;
  g_object_weak_ref (G_OBJECT (notification), action_notification_notify_cb, self);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellNotification                                                         */
/* ------------------------------------------------------------------------ */

enum
{
  NOTIFICATION_PROP_0,

  NOTIFICATION_PROP_SOURCE,
  NOTIFICATION_PROP_TITLE,
  NOTIFICATION_PROP_BODY,
  NOTIFICATION_PROP_USE_BODY_MARKUP,
  NOTIFICATION_PROP_GICON,
  NOTIFICATION_PROP_ICON_NAME,
  NOTIFICATION_PROP_SOUND,
  NOTIFICATION_PROP_DATETIME,
  NOTIFICATION_PROP_PRIVACY_SCOPE,
  NOTIFICATION_PROP_URGENCY,
  NOTIFICATION_PROP_ACKNOWLEDGED,
  NOTIFICATION_PROP_RESIDENT,
  NOTIFICATION_PROP_FOR_FEEDBACK,
  NOTIFICATION_PROP_IS_TRANSIENT,

  NOTIFICATION_N_PROPS
};

static GParamSpec *notification_props[NOTIFICATION_N_PROPS] = { NULL, };

enum
{
  NOTIFICATION_ACTION_ADDED,
  NOTIFICATION_ACTION_REMOVED,
  NOTIFICATION_ACTIVATED,
  NOTIFICATION_DESTROY,

  NOTIFICATION_LAST_SIGNAL
};

static guint notification_signals[NOTIFICATION_LAST_SIGNAL] = { 0, };

typedef struct
{
  guint id;

  ShellSource *source;
  char *title;
  char *body;
  gboolean use_body_markup;
  GIcon *gicon;
  ShellNotificationSound *sound;
  GDateTime *datetime;
  ShellNotificationPrivacyScope privacy_scope;
  ShellNotificationUrgency urgency;
  gboolean acknowledged;
  gboolean resident;
  gboolean for_feedback;
  gboolean is_transient;

  GPtrArray *actions; /* of ShellNotificationAction */

  guint update_datetime_id;
  gboolean destroying;
  gboolean destroyed;

  /* The properties set while the notification is created and set up do not
   * update #ShellNotification:datetime: the JS class only started to watch for
   * changes when its constructor was over */
  gboolean initializing;
} ShellNotificationPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellNotification, shell_notification, G_TYPE_OBJECT)

static guint last_notification_id = 0;

/**
 * shell_notification_new:
 * @source: the source of the notification
 * @title: (nullable): the title
 * @body: (nullable): the body
 *
 * Creates a notification. It is shown once it is added to @source with
 * shell_source_add_notification ().
 *
 * Returns: (transfer full): a new #ShellNotification
 */
ShellNotification *
shell_notification_new (ShellSource *source,
                        const char  *title,
                        const char  *body)
{
  g_return_val_if_fail (SHELL_IS_SOURCE (source), NULL);

  return g_object_new (SHELL_TYPE_NOTIFICATION,
                       "source", source,
                       "title", title,
                       "body", body,
                       NULL);
}

/**
 * shell_notification_get_source:
 * @self: a #ShellNotification
 *
 * Returns: (transfer none) (nullable): #ShellNotification:source, which is
 *   %NULL once the notification is destroyed
 */
ShellSource *
shell_notification_get_source (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), NULL);

  priv = shell_notification_get_instance_private (self);
  return priv->source;
}

/**
 * shell_notification_get_id:
 * @self: a #ShellNotification
 *
 * Returns: the number the shell gave to the notification, unique for the
 *   session; it is the `notification-id` of the notification hooks
 */
guint
shell_notification_get_id (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), 0);

  priv = shell_notification_get_instance_private (self);
  return priv->id;
}

#define NOTIFICATION_NOTIFY(prop) \
  g_object_notify_by_pspec (G_OBJECT (self), notification_props[NOTIFICATION_PROP_##prop])

/**
 * shell_notification_get_title:
 * @self: a #ShellNotification
 *
 * Returns: (nullable): #ShellNotification:title
 */
const char *
shell_notification_get_title (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), NULL);

  priv = shell_notification_get_instance_private (self);
  return priv->title;
}

/**
 * shell_notification_set_title:
 * @self: a #ShellNotification
 * @title: (nullable): the new title
 *
 * Sets #ShellNotification:title.
 */
void
shell_notification_set_title (ShellNotification *self,
                              const char        *title)
{
  ShellNotificationPrivate *priv;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));

  priv = shell_notification_get_instance_private (self);

  g_free (priv->title);
  priv->title = g_strdup (title);
  NOTIFICATION_NOTIFY (TITLE);
}

/**
 * shell_notification_get_body:
 * @self: a #ShellNotification
 *
 * Returns: (nullable): #ShellNotification:body
 */
const char *
shell_notification_get_body (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), NULL);

  priv = shell_notification_get_instance_private (self);
  return priv->body;
}

/**
 * shell_notification_set_body:
 * @self: a #ShellNotification
 * @body: (nullable): the new body
 *
 * Sets #ShellNotification:body.
 */
void
shell_notification_set_body (ShellNotification *self,
                             const char        *body)
{
  ShellNotificationPrivate *priv;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));

  priv = shell_notification_get_instance_private (self);

  g_free (priv->body);
  priv->body = g_strdup (body);
  NOTIFICATION_NOTIFY (BODY);
}

/**
 * shell_notification_get_use_body_markup:
 * @self: a #ShellNotification
 *
 * Returns: #ShellNotification:use-body-markup
 */
gboolean
shell_notification_get_use_body_markup (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), FALSE);

  priv = shell_notification_get_instance_private (self);
  return priv->use_body_markup;
}

/**
 * shell_notification_set_use_body_markup:
 * @self: a #ShellNotification
 * @use_body_markup: whether the body contains markup
 *
 * Sets #ShellNotification:use-body-markup.
 */
void
shell_notification_set_use_body_markup (ShellNotification *self,
                                        gboolean           use_body_markup)
{
  ShellNotificationPrivate *priv;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));

  priv = shell_notification_get_instance_private (self);

  priv->use_body_markup = !!use_body_markup;
  NOTIFICATION_NOTIFY (USE_BODY_MARKUP);
}

/**
 * shell_notification_get_gicon:
 * @self: a #ShellNotification
 *
 * Returns: (transfer none) (nullable): #ShellNotification:gicon
 */
GIcon *
shell_notification_get_gicon (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), NULL);

  priv = shell_notification_get_instance_private (self);
  return priv->gicon;
}

/**
 * shell_notification_set_gicon:
 * @self: a #ShellNotification
 * @gicon: (nullable): the new icon
 *
 * Sets #ShellNotification:gicon.
 */
void
shell_notification_set_gicon (ShellNotification *self,
                              GIcon             *gicon)
{
  ShellNotificationPrivate *priv;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));
  g_return_if_fail (gicon == NULL || G_IS_ICON (gicon));

  priv = shell_notification_get_instance_private (self);

  g_set_object (&priv->gicon, gicon);
  NOTIFICATION_NOTIFY (GICON);
  NOTIFICATION_NOTIFY (ICON_NAME);
}

/**
 * shell_notification_get_icon_name:
 * @self: a #ShellNotification
 *
 * Returns: (nullable): the name of the icon if it is a themed icon
 */
const char *
shell_notification_get_icon_name (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), NULL);

  priv = shell_notification_get_instance_private (self);

  if (priv->gicon != NULL && G_IS_THEMED_ICON (priv->gicon))
    {
      const char * const *names = g_themed_icon_get_names (G_THEMED_ICON (priv->gicon));

      return names != NULL ? names[0] : NULL;
    }

  return NULL;
}

/**
 * shell_notification_set_icon_name:
 * @self: a #ShellNotification
 * @icon_name: (nullable): the name of a themed icon
 *
 * Sets #ShellNotification:gicon to the themed icon @icon_name.
 */
void
shell_notification_set_icon_name (ShellNotification *self,
                                  const char        *icon_name)
{
  g_autoptr (GIcon) icon = NULL;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));

  if (icon_name != NULL)
    icon = g_themed_icon_new (icon_name);

  shell_notification_set_gicon (self, icon);
}

/**
 * shell_notification_get_sound:
 * @self: a #ShellNotification
 *
 * Returns: (transfer none) (nullable): #ShellNotification:sound
 */
ShellNotificationSound *
shell_notification_get_sound (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), NULL);

  priv = shell_notification_get_instance_private (self);
  return priv->sound;
}

/**
 * shell_notification_set_sound:
 * @self: a #ShellNotification
 * @sound: (nullable): the sound to play when the banner shows
 *
 * Sets #ShellNotification:sound.
 */
void
shell_notification_set_sound (ShellNotification      *self,
                              ShellNotificationSound *sound)
{
  ShellNotificationPrivate *priv;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));
  g_return_if_fail (sound == NULL || SHELL_IS_NOTIFICATION_SOUND (sound));

  priv = shell_notification_get_instance_private (self);

  g_set_object (&priv->sound, sound);
  NOTIFICATION_NOTIFY (SOUND);
}

/**
 * shell_notification_get_datetime:
 * @self: a #ShellNotification
 *
 * Returns: (transfer none) (nullable): #ShellNotification:datetime
 */
GDateTime *
shell_notification_get_datetime (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), NULL);

  priv = shell_notification_get_instance_private (self);
  return priv->datetime;
}

/**
 * shell_notification_set_datetime:
 * @self: a #ShellNotification
 * @datetime: (nullable): the time of the notification
 *
 * Sets #ShellNotification:datetime.
 */
void
shell_notification_set_datetime (ShellNotification *self,
                                 GDateTime         *datetime)
{
  ShellNotificationPrivate *priv;
  GDateTime *old;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));

  priv = shell_notification_get_instance_private (self);

  old = priv->datetime;
  priv->datetime = datetime != NULL ? g_date_time_ref (datetime) : NULL;
  g_clear_pointer (&old, g_date_time_unref);

  NOTIFICATION_NOTIFY (DATETIME);
}

/**
 * shell_notification_get_privacy_scope:
 * @self: a #ShellNotification
 *
 * Returns: #ShellNotification:privacy-scope
 */
ShellNotificationPrivacyScope
shell_notification_get_privacy_scope (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), SHELL_NOTIFICATION_PRIVACY_SCOPE_USER);

  priv = shell_notification_get_instance_private (self);
  return priv->privacy_scope;
}

/**
 * shell_notification_set_privacy_scope:
 * @self: a #ShellNotification
 * @privacy_scope: the new privacy scope
 *
 * Sets #ShellNotification:privacy-scope.
 */
void
shell_notification_set_privacy_scope (ShellNotification            *self,
                                      ShellNotificationPrivacyScope  privacy_scope)
{
  ShellNotificationPrivate *priv;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));
  g_return_if_fail (privacy_scope == SHELL_NOTIFICATION_PRIVACY_SCOPE_USER ||
                    privacy_scope == SHELL_NOTIFICATION_PRIVACY_SCOPE_SYSTEM);

  priv = shell_notification_get_instance_private (self);

  if (priv->privacy_scope == privacy_scope)
    return;

  priv->privacy_scope = privacy_scope;
  NOTIFICATION_NOTIFY (PRIVACY_SCOPE);
}

/**
 * shell_notification_get_urgency:
 * @self: a #ShellNotification
 *
 * Returns: #ShellNotification:urgency
 */
ShellNotificationUrgency
shell_notification_get_urgency (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), SHELL_NOTIFICATION_URGENCY_NORMAL);

  priv = shell_notification_get_instance_private (self);
  return priv->urgency;
}

/**
 * shell_notification_set_urgency:
 * @self: a #ShellNotification
 * @urgency: the new urgency
 *
 * Sets #ShellNotification:urgency.
 */
void
shell_notification_set_urgency (ShellNotification       *self,
                                ShellNotificationUrgency urgency)
{
  ShellNotificationPrivate *priv;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));
  g_return_if_fail (urgency >= SHELL_NOTIFICATION_URGENCY_LOW &&
                    urgency <= SHELL_NOTIFICATION_URGENCY_CRITICAL);

  priv = shell_notification_get_instance_private (self);

  if (priv->urgency == urgency)
    return;

  priv->urgency = urgency;
  NOTIFICATION_NOTIFY (URGENCY);
}

/**
 * shell_notification_get_acknowledged:
 * @self: a #ShellNotification
 *
 * Returns: #ShellNotification:acknowledged
 */
gboolean
shell_notification_get_acknowledged (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), FALSE);

  priv = shell_notification_get_instance_private (self);
  return priv->acknowledged;
}

/**
 * shell_notification_set_acknowledged:
 * @self: a #ShellNotification
 * @acknowledged: whether the user has seen the notification
 *
 * Sets #ShellNotification:acknowledged. Like every property of the
 * notification it notifies even when the value stays the same: setting it to
 * %FALSE asks for the banner again.
 */
void
shell_notification_set_acknowledged (ShellNotification *self,
                                     gboolean           acknowledged)
{
  ShellNotificationPrivate *priv;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));

  priv = shell_notification_get_instance_private (self);

  priv->acknowledged = !!acknowledged;
  NOTIFICATION_NOTIFY (ACKNOWLEDGED);
}

/**
 * shell_notification_get_resident:
 * @self: a #ShellNotification
 *
 * Returns: #ShellNotification:resident
 */
gboolean
shell_notification_get_resident (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), FALSE);

  priv = shell_notification_get_instance_private (self);
  return priv->resident;
}

/**
 * shell_notification_set_resident:
 * @self: a #ShellNotification
 * @resident: whether the notification stays after activation or an action
 *
 * Sets #ShellNotification:resident.
 */
void
shell_notification_set_resident (ShellNotification *self,
                                 gboolean           resident)
{
  ShellNotificationPrivate *priv;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));

  priv = shell_notification_get_instance_private (self);

  priv->resident = !!resident;
  NOTIFICATION_NOTIFY (RESIDENT);
}

/**
 * shell_notification_get_for_feedback:
 * @self: a #ShellNotification
 *
 * Returns: #ShellNotification:for-feedback
 */
gboolean
shell_notification_get_for_feedback (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), FALSE);

  priv = shell_notification_get_instance_private (self);
  return priv->for_feedback;
}

/**
 * shell_notification_set_for_feedback:
 * @self: a #ShellNotification
 * @for_feedback: whether the notification is the feedback of an action of
 *   the user, which is shown even while the user is busy
 *
 * Sets #ShellNotification:for-feedback.
 */
void
shell_notification_set_for_feedback (ShellNotification *self,
                                     gboolean           for_feedback)
{
  ShellNotificationPrivate *priv;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));

  priv = shell_notification_get_instance_private (self);

  priv->for_feedback = !!for_feedback;
  NOTIFICATION_NOTIFY (FOR_FEEDBACK);
}

/**
 * shell_notification_get_is_transient:
 * @self: a #ShellNotification
 *
 * Returns: #ShellNotification:is-transient
 */
gboolean
shell_notification_get_is_transient (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), FALSE);

  priv = shell_notification_get_instance_private (self);
  return priv->is_transient;
}

/**
 * shell_notification_set_is_transient:
 * @self: a #ShellNotification
 * @is_transient: whether the notification is removed after its banner expires
 *   instead of staying in the list
 *
 * Sets #ShellNotification:is-transient.
 */
void
shell_notification_set_is_transient (ShellNotification *self,
                                     gboolean           is_transient)
{
  ShellNotificationPrivate *priv;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));

  priv = shell_notification_get_instance_private (self);

  priv->is_transient = !!is_transient;
  NOTIFICATION_NOTIFY (IS_TRANSIENT);
}

/**
 * shell_notification_get_actions:
 * @self: a #ShellNotification
 *
 * Returns: (transfer container) (element-type ShellNotificationAction): the
 *   actions of the notification, in the order they were added
 */
GList *
shell_notification_get_actions (ShellNotification *self)
{
  ShellNotificationPrivate *priv;
  GList *actions = NULL;
  guint i;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), NULL);

  priv = shell_notification_get_instance_private (self);

  for (i = priv->actions->len; i > 0; i--)
    actions = g_list_prepend (actions, g_ptr_array_index (priv->actions, i - 1));

  return actions;
}

/**
 * shell_notification_add_action:
 * @self: a #ShellNotification
 * @label: the label for the button of the action
 * @callback: (scope notified) (closure user_data) (destroy destroy): the
 *   callback for the action
 * @user_data: data for @callback
 * @destroy: called on @user_data when the action is gone
 *
 * Adds an action to the notification and emits
 * #ShellNotification::action-added (`addAction ()`). When the user invokes the
 * action @callback runs and the notification is destroyed, unless it is
 * resident.
 *
 * Returns: (transfer none): the new action
 */
ShellNotificationAction *
shell_notification_add_action (ShellNotification           *self,
                               const char                  *label,
                               ShellNotificationActionFunc  callback,
                               gpointer                     user_data,
                               GDestroyNotify               destroy)
{
  ShellNotificationPrivate *priv;
  ShellNotificationAction *action;

  g_return_val_if_fail (SHELL_IS_NOTIFICATION (self), NULL);
  g_return_val_if_fail (label != NULL, NULL);

  priv = shell_notification_get_instance_private (self);

  action = shell_notification_action_new (self, label, callback, user_data, destroy);
  g_ptr_array_add (priv->actions, action);
  g_signal_emit (self, notification_signals[NOTIFICATION_ACTION_ADDED], 0, action);

  return action;
}

/**
 * shell_notification_remove_action:
 * @self: a #ShellNotification
 * @action: an action of the notification
 *
 * Removes @action from the notification and emits
 * #ShellNotification::action-removed. Does nothing if it was removed before.
 */
void
shell_notification_remove_action (ShellNotification       *self,
                                  ShellNotificationAction *action)
{
  ShellNotificationPrivate *priv;
  guint index;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));
  g_return_if_fail (SHELL_IS_NOTIFICATION_ACTION (action));

  priv = shell_notification_get_instance_private (self);

  if (!g_ptr_array_find (priv->actions, action, &index))
    return;

  g_object_ref (action);
  g_ptr_array_remove_index (priv->actions, index);
  g_signal_emit (self, notification_signals[NOTIFICATION_ACTION_REMOVED], 0, action);
  g_object_unref (action);
}

/**
 * shell_notification_clear_actions:
 * @self: a #ShellNotification
 *
 * Removes all the actions of the notification, emitting
 * #ShellNotification::action-removed for each.
 */
void
shell_notification_clear_actions (ShellNotification *self)
{
  ShellNotificationPrivate *priv;
  g_autoptr (GPtrArray) removed = NULL;
  guint i;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));

  priv = shell_notification_get_instance_private (self);

  if (priv->actions->len == 0)
    return;

  removed = g_ptr_array_new_with_free_func (g_object_unref);
  for (i = 0; i < priv->actions->len; i++)
    g_ptr_array_add (removed, g_object_ref (g_ptr_array_index (priv->actions, i)));

  for (i = 0; i < removed->len; i++)
    g_signal_emit (self, notification_signals[NOTIFICATION_ACTION_REMOVED], 0,
                   g_ptr_array_index (removed, i));

  g_ptr_array_set_size (priv->actions, 0);
}

/**
 * shell_notification_play_sound:
 * @self: a #ShellNotification
 *
 * Plays #ShellNotification:sound, if the policy of the source allows it.
 */
void
shell_notification_play_sound (ShellNotification *self)
{
  ShellNotificationPrivate *priv;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));

  priv = shell_notification_get_instance_private (self);

  if (priv->source == NULL ||
      !shell_notification_policy_get_enable_sound (shell_source_get_policy (priv->source)))
    return;

  if (priv->sound != NULL)
    shell_notification_sound_play (priv->sound);
}

static void
shell_notification_real_activate (ShellNotification *self)
{
  ShellNotificationPrivate *priv = shell_notification_get_instance_private (self);

  if (priv->destroyed)
    return;

  g_object_ref (self);

  emit_hook_after ("notification.activated", self);
  g_signal_emit (self, notification_signals[NOTIFICATION_ACTIVATED], 0);

  /* Avoid double destruction after activation. We don't hide a resident
   * notification when the user invokes one of its actions, because it is
   * common for such notifications to update themselves with new information
   * based on the action. We'd like to display the updated information in
   * place, rather than pop-up a new notification. */
  if (!priv->destroyed && !priv->resident)
    shell_notification_destroy (self, SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);

  g_object_unref (self);
}

/**
 * shell_notification_activate:
 * @self: a #ShellNotification
 *
 * The user activated the notification (`activate ()`): emits
 * #ShellNotification::activated and destroys the notification unless it is
 * resident.
 */
void
shell_notification_activate (ShellNotification *self)
{
  ShellNotificationClass *klass;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));

  klass = SHELL_NOTIFICATION_GET_CLASS (self);
  klass->activate (self);
}

static void
shell_notification_real_destroy (ShellNotification                *self,
                                 ShellNotificationDestroyedReason  reason)
{
  ShellNotificationPrivate *priv = shell_notification_get_instance_private (self);

  if (priv->destroyed || priv->destroying)
    return;

  g_object_ref (self);

  priv->destroying = TRUE;
  g_signal_emit (self, notification_signals[NOTIFICATION_DESTROY], 0, reason);
  priv->destroying = FALSE;

  priv->destroyed = TRUE;
  g_clear_handle_id (&priv->update_datetime_id, g_source_remove);

  g_object_run_dispose (G_OBJECT (self));

  g_object_unref (self);
}

/**
 * shell_notification_destroy:
 * @self: a #ShellNotification
 * @reason: why the notification is destroyed
 *
 * Destroys the notification (`destroy (reason)`): emits
 * #ShellNotification::destroy, then disposes the object. The default reason of
 * the JS method was %SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED.
 */
void
shell_notification_destroy (ShellNotification                *self,
                            ShellNotificationDestroyedReason  reason)
{
  ShellNotificationClass *klass;

  g_return_if_fail (SHELL_IS_NOTIFICATION (self));

  klass = SHELL_NOTIFICATION_GET_CLASS (self);
  klass->destroy (self, reason);
}

static void
update_datetime_cb (gpointer user_data)
{
  ShellNotification *self = user_data;
  ShellNotificationPrivate *priv = shell_notification_get_instance_private (self);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();

  priv->update_datetime_id = 0;
  shell_notification_set_datetime (self, now);
}

static gboolean
finish_initialization_idle_cb (gpointer user_data)
{
  ShellNotification *self = user_data;
  ShellNotificationPrivate *priv = shell_notification_get_instance_private (self);

  priv->initializing = FALSE;

  return G_SOURCE_REMOVE;
}

/* Automatically update the datetime property when the notification is
 * updated. */
static void
shell_notification_notify (GObject    *object,
                           GParamSpec *pspec)
{
  ShellNotification *self = SHELL_NOTIFICATION (object);
  ShellNotificationPrivate *priv = shell_notification_get_instance_private (self);

  if (priv->initializing)
    {
      /* Nothing to do while the notification is being set up */
    }
  else if (g_strcmp0 (pspec->name, "acknowledged") == 0)
    {
      /* Don't update datetime property */
    }
  else if (g_strcmp0 (pspec->name, "datetime") == 0)
    {
      g_clear_handle_id (&priv->update_datetime_id, g_source_remove);
    }
  else if (priv->update_datetime_id == 0 && !priv->destroyed)
    {
      priv->update_datetime_id = g_idle_add_once (update_datetime_cb, self);
    }

  if (G_OBJECT_CLASS (shell_notification_parent_class)->notify != NULL)
    G_OBJECT_CLASS (shell_notification_parent_class)->notify (object, pspec);
}

static void
shell_notification_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellNotification *self = SHELL_NOTIFICATION (object);

  switch (prop_id)
    {
    case NOTIFICATION_PROP_SOURCE:
      g_value_set_object (value, shell_notification_get_source (self));
      break;

    case NOTIFICATION_PROP_TITLE:
      g_value_set_string (value, shell_notification_get_title (self));
      break;

    case NOTIFICATION_PROP_BODY:
      g_value_set_string (value, shell_notification_get_body (self));
      break;

    case NOTIFICATION_PROP_USE_BODY_MARKUP:
      g_value_set_boolean (value, shell_notification_get_use_body_markup (self));
      break;

    case NOTIFICATION_PROP_GICON:
      g_value_set_object (value, shell_notification_get_gicon (self));
      break;

    case NOTIFICATION_PROP_ICON_NAME:
      g_value_set_string (value, shell_notification_get_icon_name (self));
      break;

    case NOTIFICATION_PROP_SOUND:
      g_value_set_object (value, shell_notification_get_sound (self));
      break;

    case NOTIFICATION_PROP_DATETIME:
      g_value_set_boxed (value, shell_notification_get_datetime (self));
      break;

    case NOTIFICATION_PROP_PRIVACY_SCOPE:
      g_value_set_enum (value, shell_notification_get_privacy_scope (self));
      break;

    case NOTIFICATION_PROP_URGENCY:
      g_value_set_enum (value, shell_notification_get_urgency (self));
      break;

    case NOTIFICATION_PROP_ACKNOWLEDGED:
      g_value_set_boolean (value, shell_notification_get_acknowledged (self));
      break;

    case NOTIFICATION_PROP_RESIDENT:
      g_value_set_boolean (value, shell_notification_get_resident (self));
      break;

    case NOTIFICATION_PROP_FOR_FEEDBACK:
      g_value_set_boolean (value, shell_notification_get_for_feedback (self));
      break;

    case NOTIFICATION_PROP_IS_TRANSIENT:
      g_value_set_boolean (value, shell_notification_get_is_transient (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_notification_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellNotification *self = SHELL_NOTIFICATION (object);
  ShellNotificationPrivate *priv = shell_notification_get_instance_private (self);

  switch (prop_id)
    {
    case NOTIFICATION_PROP_SOURCE:
      g_set_object (&priv->source, g_value_get_object (value));
      break;

    case NOTIFICATION_PROP_TITLE:
      shell_notification_set_title (self, g_value_get_string (value));
      break;

    case NOTIFICATION_PROP_BODY:
      shell_notification_set_body (self, g_value_get_string (value));
      break;

    case NOTIFICATION_PROP_USE_BODY_MARKUP:
      shell_notification_set_use_body_markup (self, g_value_get_boolean (value));
      break;

    case NOTIFICATION_PROP_GICON:
      shell_notification_set_gicon (self, g_value_get_object (value));
      break;

    case NOTIFICATION_PROP_ICON_NAME:
      shell_notification_set_icon_name (self, g_value_get_string (value));
      break;

    case NOTIFICATION_PROP_SOUND:
      shell_notification_set_sound (self, g_value_get_object (value));
      break;

    case NOTIFICATION_PROP_DATETIME:
      shell_notification_set_datetime (self, g_value_get_boxed (value));
      break;

    case NOTIFICATION_PROP_PRIVACY_SCOPE:
      shell_notification_set_privacy_scope (self, g_value_get_enum (value));
      break;

    case NOTIFICATION_PROP_URGENCY:
      shell_notification_set_urgency (self, g_value_get_enum (value));
      break;

    case NOTIFICATION_PROP_ACKNOWLEDGED:
      shell_notification_set_acknowledged (self, g_value_get_boolean (value));
      break;

    case NOTIFICATION_PROP_RESIDENT:
      shell_notification_set_resident (self, g_value_get_boolean (value));
      break;

    case NOTIFICATION_PROP_FOR_FEEDBACK:
      shell_notification_set_for_feedback (self, g_value_get_boolean (value));
      break;

    case NOTIFICATION_PROP_IS_TRANSIENT:
      shell_notification_set_is_transient (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_notification_constructed (GObject *object)
{
  ShellNotification *self = SHELL_NOTIFICATION (object);
  ShellNotificationPrivate *priv = shell_notification_get_instance_private (self);

  G_OBJECT_CLASS (shell_notification_parent_class)->constructed (object);

  if (priv->datetime == NULL)
    priv->datetime = g_date_time_new_now_local ();

  /* The properties that are given to g_object_new () are set, and their
   * notifications dispatched, after this: end the initialization once the
   * code that creates the notification returns to the main loop */
  g_idle_add_full (G_PRIORITY_HIGH, G_SOURCE_FUNC (finish_initialization_idle_cb),
                   g_object_ref (self), g_object_unref);
}

static void
shell_notification_dispose (GObject *object)
{
  ShellNotification *self = SHELL_NOTIFICATION (object);
  ShellNotificationPrivate *priv = shell_notification_get_instance_private (self);

  g_clear_handle_id (&priv->update_datetime_id, g_source_remove);

  g_ptr_array_set_size (priv->actions, 0);
  g_clear_object (&priv->sound);
  g_clear_object (&priv->gicon);
  g_clear_object (&priv->source);

  G_OBJECT_CLASS (shell_notification_parent_class)->dispose (object);
}

static void
shell_notification_finalize (GObject *object)
{
  ShellNotification *self = SHELL_NOTIFICATION (object);
  ShellNotificationPrivate *priv = shell_notification_get_instance_private (self);

  g_clear_pointer (&priv->actions, g_ptr_array_unref);
  g_clear_pointer (&priv->datetime, g_date_time_unref);
  g_clear_pointer (&priv->title, g_free);
  g_clear_pointer (&priv->body, g_free);

  G_OBJECT_CLASS (shell_notification_parent_class)->finalize (object);
}

static void
shell_notification_class_init (ShellNotificationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  const GParamFlags rw_flags = G_PARAM_READWRITE |
                               G_PARAM_EXPLICIT_NOTIFY |
                               G_PARAM_STATIC_STRINGS;

  object_class->get_property = shell_notification_get_property;
  object_class->set_property = shell_notification_set_property;
  object_class->constructed = shell_notification_constructed;
  object_class->dispose = shell_notification_dispose;
  object_class->finalize = shell_notification_finalize;
  object_class->notify = shell_notification_notify;

  klass->activate = shell_notification_real_activate;
  klass->destroy = shell_notification_real_destroy;

  /**
   * ShellNotification:source:
   *
   * The source of the notification.
   */
  notification_props[NOTIFICATION_PROP_SOURCE] =
    g_param_spec_object ("source", NULL, NULL,
                         SHELL_TYPE_SOURCE,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellNotification:title:
   *
   * The title of the notification.
   */
  notification_props[NOTIFICATION_PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL, NULL, rw_flags);

  /**
   * ShellNotification:body:
   *
   * The body of the notification.
   */
  notification_props[NOTIFICATION_PROP_BODY] =
    g_param_spec_string ("body", NULL, NULL, NULL, rw_flags);

  /**
   * ShellNotification:use-body-markup:
   *
   * Whether the body contains Pango markup (only bold, italic and underline
   * are kept).
   */
  notification_props[NOTIFICATION_PROP_USE_BODY_MARKUP] =
    g_param_spec_boolean ("use-body-markup", NULL, NULL, FALSE, rw_flags);

  /**
   * ShellNotification:gicon:
   *
   * The icon of the notification.
   */
  notification_props[NOTIFICATION_PROP_GICON] =
    g_param_spec_object ("gicon", NULL, NULL, G_TYPE_ICON, rw_flags);

  /**
   * ShellNotification:icon-name:
   *
   * The name of the icon of the notification, if it is a themed icon. Setting
   * it replaces #ShellNotification:gicon.
   */
  notification_props[NOTIFICATION_PROP_ICON_NAME] =
    g_param_spec_string ("icon-name", NULL, NULL, NULL, rw_flags);

  /**
   * ShellNotification:sound:
   *
   * The sound to play when the banner shows.
   */
  notification_props[NOTIFICATION_PROP_SOUND] =
    g_param_spec_object ("sound", NULL, NULL, SHELL_TYPE_NOTIFICATION_SOUND, rw_flags);

  /**
   * ShellNotification:datetime:
   *
   * The time of the notification, which follows its updates.
   */
  notification_props[NOTIFICATION_PROP_DATETIME] =
    g_param_spec_boxed ("datetime", NULL, NULL, G_TYPE_DATE_TIME, rw_flags);

  /**
   * ShellNotification:privacy-scope:
   *
   * Whether the details of the notification are private to the user or to
   * the system.
   */
  notification_props[NOTIFICATION_PROP_PRIVACY_SCOPE] =
    g_param_spec_enum ("privacy-scope", NULL, NULL,
                       SHELL_TYPE_NOTIFICATION_PRIVACY_SCOPE,
                       SHELL_NOTIFICATION_PRIVACY_SCOPE_USER,
                       rw_flags | G_PARAM_CONSTRUCT);

  /**
   * ShellNotification:urgency:
   *
   * How urgent the notification is.
   */
  notification_props[NOTIFICATION_PROP_URGENCY] =
    g_param_spec_enum ("urgency", NULL, NULL,
                       SHELL_TYPE_NOTIFICATION_URGENCY,
                       SHELL_NOTIFICATION_URGENCY_NORMAL,
                       rw_flags | G_PARAM_CONSTRUCT);

  /**
   * ShellNotification:acknowledged:
   *
   * Whether the user has seen the notification. Assigning %FALSE asks for the
   * banner again.
   */
  notification_props[NOTIFICATION_PROP_ACKNOWLEDGED] =
    g_param_spec_boolean ("acknowledged", NULL, NULL, FALSE, rw_flags);

  /**
   * ShellNotification:resident:
   *
   * Whether the notification stays after it was activated or one of its
   * actions was invoked.
   */
  notification_props[NOTIFICATION_PROP_RESIDENT] =
    g_param_spec_boolean ("resident", NULL, NULL, FALSE, rw_flags);

  /**
   * ShellNotification:for-feedback:
   *
   * Whether the notification is the feedback of an action of the user.
   */
  notification_props[NOTIFICATION_PROP_FOR_FEEDBACK] =
    g_param_spec_boolean ("for-feedback", NULL, NULL, FALSE, rw_flags);

  /**
   * ShellNotification:is-transient:
   *
   * Whether the notification is removed after its banner expires.
   */
  notification_props[NOTIFICATION_PROP_IS_TRANSIENT] =
    g_param_spec_boolean ("is-transient", NULL, NULL, FALSE, rw_flags);

  g_object_class_install_properties (object_class, NOTIFICATION_N_PROPS,
                                     notification_props);

  /**
   * ShellNotification::action-added:
   * @self: the notification
   * @action: the new action
   */
  notification_signals[NOTIFICATION_ACTION_ADDED] =
    g_signal_new ("action-added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_NOTIFICATION_ACTION);

  /**
   * ShellNotification::action-removed:
   * @self: the notification
   * @action: the action that was removed
   */
  notification_signals[NOTIFICATION_ACTION_REMOVED] =
    g_signal_new ("action-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_NOTIFICATION_ACTION);

  /**
   * ShellNotification::activated:
   * @self: the notification
   *
   * The user activated the notification.
   */
  notification_signals[NOTIFICATION_ACTIVATED] =
    g_signal_new ("activated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellNotification::destroy:
   * @self: the notification
   * @reason: why the notification is destroyed
   *
   * The notification is destroyed.
   */
  notification_signals[NOTIFICATION_DESTROY] =
    g_signal_new ("destroy",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_NOTIFICATION_DESTROYED_REASON);

  declare_hooks ();
}

static void
shell_notification_init (ShellNotification *self)
{
  ShellNotificationPrivate *priv = shell_notification_get_instance_private (self);

  priv->id = ++last_notification_id;
  priv->initializing = TRUE;
  priv->actions = g_ptr_array_new_with_free_func (g_object_unref);
  priv->privacy_scope = SHELL_NOTIFICATION_PRIVACY_SCOPE_USER;
  priv->urgency = SHELL_NOTIFICATION_URGENCY_NORMAL;
}

/* ------------------------------------------------------------------------ */
/* ShellSource                                                               */
/* ------------------------------------------------------------------------ */

enum
{
  SRC_PROP_0,

  SRC_PROP_COUNT,
  SRC_PROP_COUNT_VISIBLE,
  SRC_PROP_UNSEEN_COUNT,
  SRC_PROP_POLICY,

  SRC_N_PROPS
};

static GParamSpec *source_props[SRC_N_PROPS] = { NULL, };

enum
{
  SOURCE_DESTROY,
  SOURCE_NOTIFICATION_ADDED,
  SOURCE_NOTIFICATION_REMOVED,
  SOURCE_NOTIFICATION_REQUEST_BANNER,

  SOURCE_LAST_SIGNAL
};

static guint source_signals[SOURCE_LAST_SIGNAL] = { 0, };

typedef struct
{
  ShellNotificationPolicy *policy;
  GPtrArray *notifications; /* of ShellNotification */
  char *owner;
  gboolean in_destruction;
  gboolean destroying;
} ShellSourcePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellSource, shell_source, SHELL_TYPE_MESSAGE_SOURCE)

/**
 * shell_source_get_policy:
 * @self: a #ShellSource
 *
 * Returns: (transfer none): #ShellSource:policy
 */
ShellNotificationPolicy *
shell_source_get_policy (ShellSource *self)
{
  ShellSourcePrivate *priv;

  g_return_val_if_fail (SHELL_IS_SOURCE (self), NULL);

  priv = shell_source_get_instance_private (self);
  return priv->policy;
}

/**
 * shell_source_set_policy:
 * @self: a #ShellSource
 * @policy: (transfer none): the new policy; the old one is destroyed
 *
 * Sets #ShellSource:policy.
 */
void
shell_source_set_policy (ShellSource             *self,
                         ShellNotificationPolicy *policy)
{
  ShellSourcePrivate *priv;

  g_return_if_fail (SHELL_IS_SOURCE (self));
  g_return_if_fail (policy == NULL || SHELL_IS_NOTIFICATION_POLICY (policy));

  priv = shell_source_get_instance_private (self);

  if (priv->policy == policy)
    return;

  if (priv->policy != NULL)
    shell_notification_policy_destroy (priv->policy);

  g_set_object (&priv->policy, policy);
}

/**
 * shell_source_get_owner:
 * @self: a #ShellSource
 *
 * Returns: (nullable): the uuid of the Lua extension that created the source
 *   through `shell.notify`, or %NULL for the sources of the shell and of
 *   applications. It is the `owner` of the notification hooks.
 */
const char *
shell_source_get_owner (ShellSource *self)
{
  ShellSourcePrivate *priv;

  g_return_val_if_fail (SHELL_IS_SOURCE (self), NULL);

  priv = shell_source_get_instance_private (self);
  return priv->owner;
}

/**
 * shell_source_set_owner:
 * @self: a #ShellSource
 * @owner: (nullable): the uuid of the extension that owns the source
 *
 * Sets the extension that created the source (see
 * shell_source_get_owner ()).
 */
void
shell_source_set_owner (ShellSource *self,
                        const char  *owner)
{
  ShellSourcePrivate *priv;

  g_return_if_fail (SHELL_IS_SOURCE (self));

  priv = shell_source_get_instance_private (self);
  g_free (priv->owner);
  priv->owner = g_strdup (owner);
}

/**
 * shell_source_get_count:
 * @self: a #ShellSource
 *
 * Returns: #ShellSource:count
 */
int
shell_source_get_count (ShellSource *self)
{
  ShellSourcePrivate *priv;

  g_return_val_if_fail (SHELL_IS_SOURCE (self), 0);

  priv = shell_source_get_instance_private (self);
  return priv->notifications->len;
}

/**
 * shell_source_get_unseen_count:
 * @self: a #ShellSource
 *
 * Returns: #ShellSource:unseen-count
 */
int
shell_source_get_unseen_count (ShellSource *self)
{
  ShellSourcePrivate *priv;
  int count = 0;
  guint i;

  g_return_val_if_fail (SHELL_IS_SOURCE (self), 0);

  priv = shell_source_get_instance_private (self);

  for (i = 0; i < priv->notifications->len; i++)
    {
      if (!shell_notification_get_acknowledged (g_ptr_array_index (priv->notifications, i)))
        count++;
    }

  return count;
}

/**
 * shell_source_get_count_visible:
 * @self: a #ShellSource
 *
 * Returns: #ShellSource:count-visible
 */
gboolean
shell_source_get_count_visible (ShellSource *self)
{
  g_return_val_if_fail (SHELL_IS_SOURCE (self), FALSE);

  return shell_source_get_count (self) > 1;
}

/**
 * shell_source_get_narrowest_privacy_scope:
 * @self: a #ShellSource
 *
 * Returns: %SHELL_NOTIFICATION_PRIVACY_SCOPE_SYSTEM if all the notifications
 *   of the source have the system scope, and
 *   %SHELL_NOTIFICATION_PRIVACY_SCOPE_USER otherwise
 */
ShellNotificationPrivacyScope
shell_source_get_narrowest_privacy_scope (ShellSource *self)
{
  ShellSourcePrivate *priv;
  guint i;

  g_return_val_if_fail (SHELL_IS_SOURCE (self), SHELL_NOTIFICATION_PRIVACY_SCOPE_USER);

  priv = shell_source_get_instance_private (self);

  for (i = 0; i < priv->notifications->len; i++)
    {
      ShellNotification *notification = g_ptr_array_index (priv->notifications, i);

      if (shell_notification_get_privacy_scope (notification) != SHELL_NOTIFICATION_PRIVACY_SCOPE_SYSTEM)
        return SHELL_NOTIFICATION_PRIVACY_SCOPE_USER;
    }

  return SHELL_NOTIFICATION_PRIVACY_SCOPE_SYSTEM;
}

/**
 * shell_source_get_notifications:
 * @self: a #ShellSource
 *
 * Returns: (transfer container) (element-type ShellNotification): the
 *   notifications of the source, the oldest first
 */
GList *
shell_source_get_notifications (ShellSource *self)
{
  ShellSourcePrivate *priv;
  GList *notifications = NULL;
  guint i;

  g_return_val_if_fail (SHELL_IS_SOURCE (self), NULL);

  priv = shell_source_get_instance_private (self);

  for (i = priv->notifications->len; i > 0; i--)
    notifications = g_list_prepend (notifications, g_ptr_array_index (priv->notifications, i - 1));

  return notifications;
}

/**
 * shell_source_count_updated:
 * @self: a #ShellSource
 *
 * Notifies #ShellSource:count and the properties that follow it
 * (`countUpdated ()`).
 */
void
shell_source_count_updated (ShellSource *self)
{
  g_return_if_fail (SHELL_IS_SOURCE (self));

  g_object_freeze_notify (G_OBJECT (self));
  g_object_notify_by_pspec (G_OBJECT (self), source_props[SRC_PROP_COUNT]);
  g_object_notify_by_pspec (G_OBJECT (self), source_props[SRC_PROP_COUNT_VISIBLE]);
  g_object_notify_by_pspec (G_OBJECT (self), source_props[SRC_PROP_UNSEEN_COUNT]);
  g_object_thaw_notify (G_OBJECT (self));
}

static void
source_disconnect_notification (ShellSource       *self,
                                ShellNotification *notification)
{
  g_signal_handlers_disconnect_by_data (notification, self);
}

static void
source_notification_destroy_cb (ShellNotification                *notification,
                                ShellNotificationDestroyedReason  reason,
                                ShellSource                      *self)
{
  ShellSourcePrivate *priv = shell_source_get_instance_private (self);
  g_autoptr (ShellNotification) keep_alive = NULL;
  guint index;

  if (!g_ptr_array_find (priv->notifications, notification, &index))
    return;

  g_object_ref (self);
  keep_alive = g_object_ref (notification);

  source_disconnect_notification (self, notification);
  g_ptr_array_remove_index (priv->notifications, index);

  g_signal_emit (self, source_signals[SOURCE_NOTIFICATION_REMOVED], 0, notification);
  shell_source_count_updated (self);

  if (!priv->in_destruction && priv->notifications->len == 0)
    shell_source_destroy (self, SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);

  g_object_unref (self);
}

static void
source_notification_acknowledged_cb (ShellNotification *notification,
                                     GParamSpec        *pspec,
                                     ShellSource       *self)
{
  shell_source_count_updated (self);

  /* If acknowledged was set to false try to show the notification again */
  if (!shell_notification_get_acknowledged (notification))
    g_signal_emit (self, source_signals[SOURCE_NOTIFICATION_REQUEST_BANNER], 0, notification);
}

/**
 * shell_source_add_notification:
 * @self: a #ShellSource
 * @notification: a notification of this source
 *
 * Adds @notification to the source and asks the tray for a banner
 * (`addNotification ()`). When the source holds more than ten notifications
 * the oldest are destroyed. Does nothing if @notification was added before.
 */
void
shell_source_add_notification (ShellSource       *self,
                               ShellNotification *notification)
{
  ShellSourcePrivate *priv;

  g_return_if_fail (SHELL_IS_SOURCE (self));
  g_return_if_fail (SHELL_IS_NOTIFICATION (notification));

  priv = shell_source_get_instance_private (self);

  if (g_ptr_array_find (priv->notifications, notification, NULL))
    return;

  g_object_ref (self);
  g_object_ref (notification);

  while (priv->notifications->len >= MAX_NOTIFICATIONS_PER_SOURCE)
    {
      ShellNotification *oldest = g_ptr_array_index (priv->notifications, 0);
      guint n_notifications = priv->notifications->len;

      shell_notification_destroy (oldest, SHELL_NOTIFICATION_DESTROYED_REASON_EXPIRED);

      /* A subclass that does not destroy the notification must not hang us */
      if (priv->notifications->len == n_notifications)
        {
          source_disconnect_notification (self, oldest);
          g_ptr_array_remove_index (priv->notifications, 0);
        }
    }

  g_signal_connect (notification, "destroy",
                    G_CALLBACK (source_notification_destroy_cb), self);
  g_signal_connect (notification, "notify::acknowledged",
                    G_CALLBACK (source_notification_acknowledged_cb), self);
  g_ptr_array_add (priv->notifications, g_object_ref (notification));

  g_signal_emit (self, source_signals[SOURCE_NOTIFICATION_ADDED], 0, notification);
  g_signal_emit (self, source_signals[SOURCE_NOTIFICATION_REQUEST_BANNER], 0, notification);
  shell_source_count_updated (self);

  g_object_unref (notification);
  g_object_unref (self);
}

static void
shell_source_real_destroy (ShellSource                      *self,
                           ShellNotificationDestroyedReason  reason)
{
  ShellSourcePrivate *priv = shell_source_get_instance_private (self);

  if (priv->destroying)
    return;

  g_object_ref (self);

  priv->destroying = TRUE;
  priv->in_destruction = TRUE;

  while (priv->notifications->len > 0)
    {
      ShellNotification *oldest = g_ptr_array_index (priv->notifications, 0);
      guint n_notifications = priv->notifications->len;

      shell_notification_destroy (oldest, reason);

      /* A subclass that does not destroy the notification must not hang us */
      if (priv->notifications->len == n_notifications)
        {
          source_disconnect_notification (self, oldest);
          g_ptr_array_remove_index (priv->notifications, 0);
        }
    }

  g_signal_emit (self, source_signals[SOURCE_DESTROY], 0, reason);

  if (priv->policy != NULL)
    shell_notification_policy_destroy (priv->policy);

  g_object_run_dispose (G_OBJECT (self));

  g_object_unref (self);
}

/**
 * shell_source_destroy:
 * @self: a #ShellSource
 * @reason: why the source is destroyed
 *
 * Destroys the source and its notifications with @reason
 * (`destroy (reason)`), emits #ShellSource::destroy and disposes the object.
 * The default reason of the JS method was
 * %SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED.
 */
void
shell_source_destroy (ShellSource                      *self,
                      ShellNotificationDestroyedReason  reason)
{
  ShellSourceClass *klass;

  g_return_if_fail (SHELL_IS_SOURCE (self));

  klass = SHELL_SOURCE_GET_CLASS (self);
  klass->destroy (self, reason);
}

static void
shell_source_real_open (ShellSource *self)
{
  /* To be overridden by subclasses */
}

/**
 * shell_source_open:
 * @self: a #ShellSource
 *
 * Opens the application of the source (`open ()`); subclasses override it.
 */
void
shell_source_open (ShellSource *self)
{
  ShellSourceClass *klass;

  g_return_if_fail (SHELL_IS_SOURCE (self));

  klass = SHELL_SOURCE_GET_CLASS (self);
  klass->open (self);
}

/**
 * shell_source_destroy_non_resident_notifications:
 * @self: a #ShellSource
 *
 * Destroys the notifications of the source that are not resident.
 */
void
shell_source_destroy_non_resident_notifications (ShellSource *self)
{
  ShellSourcePrivate *priv;
  g_autoptr (GPtrArray) notifications = NULL;
  guint i;

  g_return_if_fail (SHELL_IS_SOURCE (self));

  priv = shell_source_get_instance_private (self);

  /* Newest first, as the JS loop; the references keep the notifications
   * alive while the array changes under us */
  notifications = g_ptr_array_new_with_free_func (g_object_unref);
  for (i = priv->notifications->len; i > 0; i--)
    g_ptr_array_add (notifications, g_object_ref (g_ptr_array_index (priv->notifications, i - 1)));

  for (i = 0; i < notifications->len; i++)
    {
      ShellNotification *notification = g_ptr_array_index (notifications, i);

      if (!shell_notification_get_resident (notification))
        shell_notification_destroy (notification, SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);
    }
}

static void
shell_source_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  ShellSource *self = SHELL_SOURCE (object);

  switch (prop_id)
    {
    case SRC_PROP_COUNT:
      g_value_set_int (value, shell_source_get_count (self));
      break;

    case SRC_PROP_COUNT_VISIBLE:
      g_value_set_boolean (value, shell_source_get_count_visible (self));
      break;

    case SRC_PROP_UNSEEN_COUNT:
      g_value_set_int (value, shell_source_get_unseen_count (self));
      break;

    case SRC_PROP_POLICY:
      g_value_set_object (value, shell_source_get_policy (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_source_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  ShellSource *self = SHELL_SOURCE (object);

  switch (prop_id)
    {
    case SRC_PROP_POLICY:
      shell_source_set_policy (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_source_constructed (GObject *object)
{
  ShellSource *self = SHELL_SOURCE (object);
  ShellSourcePrivate *priv = shell_source_get_instance_private (self);

  G_OBJECT_CLASS (shell_source_parent_class)->constructed (object);

  if (priv->policy == NULL)
    priv->policy = SHELL_NOTIFICATION_POLICY (shell_notification_generic_policy_new ());
}

static void
shell_source_dispose (GObject *object)
{
  ShellSource *self = SHELL_SOURCE (object);
  ShellSourcePrivate *priv = shell_source_get_instance_private (self);
  guint i;

  for (i = 0; i < priv->notifications->len; i++)
    source_disconnect_notification (self, g_ptr_array_index (priv->notifications, i));
  g_ptr_array_set_size (priv->notifications, 0);

  g_clear_object (&priv->policy);

  G_OBJECT_CLASS (shell_source_parent_class)->dispose (object);
}

static void
shell_source_finalize (GObject *object)
{
  ShellSource *self = SHELL_SOURCE (object);
  ShellSourcePrivate *priv = shell_source_get_instance_private (self);

  g_clear_pointer (&priv->notifications, g_ptr_array_unref);
  g_clear_pointer (&priv->owner, g_free);

  G_OBJECT_CLASS (shell_source_parent_class)->finalize (object);
}

static void
shell_source_class_init (ShellSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_source_get_property;
  object_class->set_property = shell_source_set_property;
  object_class->constructed = shell_source_constructed;
  object_class->dispose = shell_source_dispose;
  object_class->finalize = shell_source_finalize;

  klass->open = shell_source_real_open;
  klass->destroy = shell_source_real_destroy;

  /**
   * ShellSource:count:
   *
   * The number of notifications of the source.
   */
  source_props[SRC_PROP_COUNT] =
    g_param_spec_int ("count", NULL, NULL,
                      0, G_MAXINT32, 0,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellSource:count-visible:
   *
   * Whether the count is worth showing: there is more than one notification.
   */
  source_props[SRC_PROP_COUNT_VISIBLE] =
    g_param_spec_boolean ("count-visible", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellSource:unseen-count:
   *
   * The number of notifications that the user has not seen yet.
   */
  source_props[SRC_PROP_UNSEEN_COUNT] =
    g_param_spec_int ("unseen-count", NULL, NULL,
                      0, G_MAXINT32, 0,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellSource:policy:
   *
   * What the user configured for the notifications of the source. A generic
   * policy unless one is given at construction.
   */
  source_props[SRC_PROP_POLICY] =
    g_param_spec_object ("policy", NULL, NULL,
                         SHELL_TYPE_NOTIFICATION_POLICY,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, SRC_N_PROPS, source_props);

  /**
   * ShellSource::destroy:
   * @self: the source
   * @reason: why the source is destroyed
   *
   * The source is destroyed.
   */
  source_signals[SOURCE_DESTROY] =
    g_signal_new ("destroy",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_NOTIFICATION_DESTROYED_REASON);

  /**
   * ShellSource::notification-added:
   * @self: the source
   * @notification: the new notification
   */
  source_signals[SOURCE_NOTIFICATION_ADDED] =
    g_signal_new ("notification-added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_NOTIFICATION);

  /**
   * ShellSource::notification-removed:
   * @self: the source
   * @notification: the notification that was destroyed
   */
  source_signals[SOURCE_NOTIFICATION_REMOVED] =
    g_signal_new ("notification-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_NOTIFICATION);

  /**
   * ShellSource::notification-request-banner:
   * @self: the source
   * @notification: the notification that asks for a banner
   */
  source_signals[SOURCE_NOTIFICATION_REQUEST_BANNER] =
    g_signal_new ("notification-request-banner",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_NOTIFICATION);
}

static void
shell_source_init (ShellSource *self)
{
  ShellSourcePrivate *priv = shell_source_get_instance_private (self);

  priv->notifications = g_ptr_array_new_with_free_func (g_object_unref);
}

/* ------------------------------------------------------------------------ */
/* ShellMessageTray                                                          */
/* ------------------------------------------------------------------------ */

enum
{
  TRAY_PROP_0,

  TRAY_PROP_BANNER_ALIGNMENT,
  TRAY_PROP_BANNER_BLOCKED,
  TRAY_PROP_QUEUE_COUNT,
  TRAY_PROP_DRAGGING,

  TRAY_N_PROPS
};

static GParamSpec *tray_props[TRAY_N_PROPS] = { NULL, };

enum
{
  TRAY_QUEUE_CHANGED,
  TRAY_SOURCE_ADDED,
  TRAY_SOURCE_REMOVED,

  TRAY_LAST_SIGNAL
};

static guint tray_signals[TRAY_LAST_SIGNAL] = { 0, };

/*
 * Moves the key focus into the banner while the user interacts with it, and
 * gives it back afterwards (`FocusGrabber` of the JS module).
 */
typedef struct
{
  ClutterActor *actor;                /* the banner bin, owned by the tray */
  ClutterStage *stage;                /* weak */
  ClutterActor *prev_key_focus_actor; /* weak, cleared when destroyed */
  gulong prev_destroy_id;
  gulong key_focus_id;
  gboolean focused;
} FocusGrabber;

struct _ShellMessageTray
{
  StWidget parent;

  GCancellable *cancellable;
  ShellSessionManagerPresence *presence;
  gboolean busy;
  gboolean banner_blocked;

  StWidget *banner_bin;
  FocusGrabber focus_grabber;
  GPtrArray *notification_queue; /* of ShellNotification */
  ShellNotification *notification;
  ClutterActor *banner;          /* a ShellNotificationMessage, child of banner_bin */

  gboolean user_active_while_notification_shown;

  ClutterStage *stage;           /* weak; the objects of the shell the tray */
  MetaDisplay *display;          /* weak; uses are kept as weak pointers so */
  MetaCompositor *compositor;    /* weak; teardown never needs ShellGlobal */
  MetaIdleMonitor *idle_monitor; /* weak */
  guint user_active_watch_id;

  gboolean use_longer_notification_left_timeout;

  /* pointer_in_notification is sort of a misnomer -- it tracks whether a
   * message tray notification should expand. The value is partially driven
   * by the hover state of the notification, but has a lot of complex state
   * related to timeouts and the current state of the pointer when a
   * notification pops up. */
  gboolean pointer_in_notification;

  /* This tracks banner_bin hover and is used to fizzle out non-changing
   * hover notifications in banner_bin_hover_changed () */
  gboolean notification_hovered;

  ShellNotificationState notification_state;
  guint notification_timeout_id;
  gboolean notification_removed;
  gboolean notification_expired;
  gboolean updating_state;

  guint notification_left_timeout_id;
  int notification_left_mouse_x;
  int notification_left_mouse_y;
  int show_notification_mouse_x;
  int show_notification_mouse_y;
  int last_seen_mouse_x;
  int last_seen_mouse_y;

  GPtrArray *sources;            /* of ShellSource */
  ShellSource *system_source;

  gboolean dragging;
  gboolean disposed;
};

G_DEFINE_FINAL_TYPE (ShellMessageTray, shell_message_tray, ST_TYPE_WIDGET)

static void tray_update_state (ShellMessageTray *self);

static void
init_ease_params (ShellEaseParams     *params,
                  int                  duration,
                  ClutterAnimationMode mode)
{
  params->duration = duration;
  params->delay = -1;
  params->mode = mode;
  params->repeat_count = 0;
  params->auto_reverse = FALSE;
  params->animation_required = FALSE;
}

static void
emit_queue_changed (ShellMessageTray *self)
{
  g_signal_emit (self, tray_signals[TRAY_QUEUE_CHANGED], 0);
  g_object_notify_by_pspec (G_OBJECT (self), tray_props[TRAY_PROP_QUEUE_COUNT]);
}

/* ------------------------------------------------------------------------ */
/* Focus                                                                     */
/* ------------------------------------------------------------------------ */

static gboolean focus_grabber_focus_ungrabbed (FocusGrabber *grabber);

static void
focus_grabber_prev_destroy_cb (ClutterActor *actor,
                               FocusGrabber *grabber)
{
  grabber->prev_destroy_id = 0;
  g_clear_weak_pointer (&grabber->prev_key_focus_actor);
}

static void
focus_grabber_focus_changed_cb (ClutterStage *stage,
                                GParamSpec   *pspec,
                                FocusGrabber *grabber)
{
  ClutterActor *focused_actor = clutter_stage_get_key_focus (stage);

  if (focused_actor == NULL || !clutter_actor_contains (grabber->actor, focused_actor))
    focus_grabber_focus_ungrabbed (grabber);
}

static void
focus_grabber_grab_focus (FocusGrabber *grabber)
{
  if (grabber->focused || grabber->stage == NULL)
    return;

  g_set_weak_pointer (&grabber->prev_key_focus_actor,
                      clutter_stage_get_key_focus (grabber->stage));

  if (grabber->prev_key_focus_actor != NULL)
    {
      grabber->prev_destroy_id =
        g_signal_connect (grabber->prev_key_focus_actor, "destroy",
                          G_CALLBACK (focus_grabber_prev_destroy_cb), grabber);
    }

  grabber->key_focus_id =
    g_signal_connect (grabber->stage, "notify::key-focus",
                      G_CALLBACK (focus_grabber_focus_changed_cb), grabber);

  if (!st_widget_navigate_focus (ST_WIDGET (grabber->actor), NULL,
                                 ST_DIR_TAB_FORWARD, FALSE))
    clutter_actor_grab_key_focus (grabber->actor);

  grabber->focused = TRUE;
}

static gboolean
focus_grabber_focus_ungrabbed (FocusGrabber *grabber)
{
  if (!grabber->focused)
    return FALSE;

  if (grabber->prev_key_focus_actor != NULL && grabber->prev_destroy_id != 0)
    g_clear_signal_handler (&grabber->prev_destroy_id, grabber->prev_key_focus_actor);
  grabber->prev_destroy_id = 0;

  if (grabber->stage != NULL)
    g_clear_signal_handler (&grabber->key_focus_id, grabber->stage);
  grabber->key_focus_id = 0;

  grabber->focused = FALSE;
  return TRUE;
}

static void
focus_grabber_ungrab_focus (FocusGrabber *grabber)
{
  if (!focus_grabber_focus_ungrabbed (grabber))
    return;

  if (grabber->stage == NULL)
    {
      g_clear_weak_pointer (&grabber->prev_key_focus_actor);
      return;
    }

  if (grabber->prev_key_focus_actor != NULL)
    {
      clutter_stage_set_key_focus (grabber->stage, grabber->prev_key_focus_actor);
      g_clear_weak_pointer (&grabber->prev_key_focus_actor);
    }
  else
    {
      ClutterActor *focused_actor = clutter_stage_get_key_focus (grabber->stage);

      if (focused_actor != NULL && clutter_actor_contains (grabber->actor, focused_actor))
        clutter_stage_set_key_focus (grabber->stage, NULL);
    }
}

static void
focus_grabber_clear (FocusGrabber *grabber)
{
  focus_grabber_focus_ungrabbed (grabber);
  g_clear_weak_pointer (&grabber->prev_key_focus_actor);
  g_clear_weak_pointer (&grabber->stage);
}

/* ------------------------------------------------------------------------ */
/* Sources                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct
{
  GWeakRef tray;
  GWeakRef source;
} PolicyWatch;

static void
policy_watch_free (PolicyWatch *watch)
{
  g_weak_ref_clear (&watch->tray);
  g_weak_ref_clear (&watch->source);
  g_free (watch);
}

static void tray_on_source_enable_changed (ShellMessageTray *self,
                                           ShellNotificationPolicy *policy,
                                           ShellSource *source);

static void
policy_enable_notify_cb (ShellNotificationPolicy *policy,
                         GParamSpec              *pspec,
                         PolicyWatch             *watch)
{
  g_autoptr (ShellMessageTray) self = g_weak_ref_get (&watch->tray);
  g_autoptr (ShellSource) source = g_weak_ref_get (&watch->source);

  if (self == NULL || source == NULL)
    return;

  tray_on_source_enable_changed (self, policy, source);
}

static void
policy_notify_cb (ShellNotificationPolicy *policy,
                  GParamSpec              *pspec,
                  PolicyWatch             *watch)
{
  g_autoptr (ShellMessageTray) self = g_weak_ref_get (&watch->tray);

  if (self == NULL)
    return;

  tray_update_state (self);
}

static void
tray_on_notification_removed (ShellSource       *source,
                              ShellNotification *notification,
                              ShellMessageTray  *self)
{
  if (self->notification == notification)
    {
      self->notification_removed = TRUE;
      if (self->notification_state == SHELL_NOTIFICATION_STATE_SHOWN ||
          self->notification_state == SHELL_NOTIFICATION_STATE_SHOWING)
        {
          self->pointer_in_notification = FALSE;
          /* _updateNotificationTimeout (0) */
          g_clear_handle_id (&self->notification_timeout_id, g_source_remove);
          tray_update_state (self);
        }
    }
  else
    {
      guint index;

      if (g_ptr_array_find (self->notification_queue, notification, &index))
        {
          g_ptr_array_remove_index (self->notification_queue, index);
          emit_queue_changed (self);
        }
    }
}

static void
queue_insert_sorted (ShellMessageTray *self,
                     ShellNotification *notification)
{
  GPtrArray *queue = self->notification_queue;
  guint i;

  g_ptr_array_add (queue, g_object_ref (notification));

  /* A stable sort, by decreasing urgency, as Array.sort () does */
  for (i = 1; i < queue->len; i++)
    {
      ShellNotification *current = g_ptr_array_index (queue, i);
      guint j = i;

      while (j > 0 &&
             shell_notification_get_urgency (g_ptr_array_index (queue, j - 1)) <
             shell_notification_get_urgency (current))
        {
          queue->pdata[j] = queue->pdata[j - 1];
          j--;
        }
      queue->pdata[j] = current;
    }
}

static void tray_update_showing_notification (ShellMessageTray *self);

static void
tray_on_notification_request_banner (ShellSource       *source,
                                     ShellNotification *notification,
                                     ShellMessageTray  *self)
{
  /* We never display a banner for already acknowledged notifications */
  if (shell_notification_get_acknowledged (notification))
    return;

  if (shell_notification_get_urgency (notification) == SHELL_NOTIFICATION_URGENCY_LOW)
    return;

  if (!shell_notification_policy_get_show_banners (shell_source_get_policy (source)) &&
      shell_notification_get_urgency (notification) != SHELL_NOTIFICATION_URGENCY_CRITICAL)
    return;

  /* Extensions can veto the banner (and its sound); the notification still
   * goes to the message list */
  if (!emit_hook_before ("notification.showing", notification))
    return;

  if (self->notification == notification)
    {
      /* If a notification that is being shown is updated, we update how it is
       * shown and extend the time until it auto-hides. If a new notification
       * is updated while it is being hidden, we stop hiding it and show it
       * again. */
      tray_update_showing_notification (self);
    }
  else if (!g_ptr_array_find (self->notification_queue, notification, NULL))
    {
      /* If the queue is "full", we skip banner mode and just show a small
       * indicator in the panel; however do make an exception for CRITICAL
       * notifications, as only banner mode allows expansion. */
      guint banner_count = self->notification != NULL ? 1 : 0;
      gboolean full = self->notification_queue->len + banner_count >= MAX_NOTIFICATIONS_IN_QUEUE;

      if (!full || shell_notification_get_urgency (notification) == SHELL_NOTIFICATION_URGENCY_CRITICAL)
        {
          queue_insert_sorted (self, notification);
          emit_queue_changed (self);
        }
    }

  tray_update_state (self);
}

static void
tray_remove_source (ShellMessageTray *self,
                    ShellSource      *source)
{
  g_autoptr (ShellSource) keep_alive = NULL;
  guint index;

  if (!g_ptr_array_find (self->sources, source, &index))
    return;

  keep_alive = g_object_ref (source);

  g_ptr_array_remove_index (self->sources, index);
  g_signal_handlers_disconnect_by_data (source, self);
  g_signal_emit (self, tray_signals[TRAY_SOURCE_REMOVED], 0, source);
}

static void
tray_on_source_destroy (ShellSource                      *source,
                        ShellNotificationDestroyedReason  reason,
                        ShellMessageTray                 *self)
{
  tray_remove_source (self, source);
}

static void
system_source_destroy_cb (ShellSource                      *source,
                          ShellNotificationDestroyedReason  reason,
                          ShellMessageTray                 *self)
{
  if (self->system_source == source)
    {
      g_signal_handlers_disconnect_by_func (source, system_source_destroy_cb, self);
      g_clear_object (&self->system_source);
    }
}

static void
tray_add_source (ShellMessageTray *self,
                 ShellSource      *source)
{
  g_ptr_array_add (self->sources, g_object_ref (source));

  g_signal_connect_object (source, "notification-request-banner",
                           G_CALLBACK (tray_on_notification_request_banner), self, 0);
  g_signal_connect_object (source, "notification-removed",
                           G_CALLBACK (tray_on_notification_removed), self, 0);
  g_signal_connect_object (source, "destroy",
                           G_CALLBACK (tray_on_source_destroy), self, 0);

  g_signal_emit (self, tray_signals[TRAY_SOURCE_ADDED], 0, source);
}

static void
tray_on_source_enable_changed (ShellMessageTray       *self,
                               ShellNotificationPolicy *policy,
                               ShellSource            *source)
{
  gboolean was_enabled = shell_message_tray_has_source (self, source);
  gboolean should_be_enabled = shell_notification_policy_get_enable (policy);

  if (was_enabled != should_be_enabled)
    {
      if (should_be_enabled)
        tray_add_source (self, source);
      else
        tray_remove_source (self, source);
    }
}

/**
 * shell_message_tray_add:
 * @self: a #ShellMessageTray
 * @source: a source
 *
 * Registers @source with the tray, if the policy of the source enables it
 * (`add ()`). It is registered again when the policy enables it later.
 */
void
shell_message_tray_add (ShellMessageTray *self,
                        ShellSource      *source)
{
  ShellNotificationPolicy *policy;
  PolicyWatch *watch;

  g_return_if_fail (SHELL_IS_MESSAGE_TRAY (self));
  g_return_if_fail (SHELL_IS_SOURCE (source));

  if (shell_message_tray_has_source (self, source))
    {
      g_message ("Trying to re-add source %s",
                 shell_message_source_get_title (SHELL_MESSAGE_SOURCE (source)));
      return;
    }

  policy = shell_source_get_policy (source);

  /* Register that we got a notification for this source */
  shell_notification_policy_store (policy);

  watch = g_new0 (PolicyWatch, 1);
  g_weak_ref_init (&watch->tray, self);
  g_weak_ref_init (&watch->source, source);
  g_signal_connect_data (policy, "notify::enable",
                         G_CALLBACK (policy_enable_notify_cb), watch,
                         (GClosureNotify) policy_watch_free, 0);

  watch = g_new0 (PolicyWatch, 1);
  g_weak_ref_init (&watch->tray, self);
  g_weak_ref_init (&watch->source, source);
  g_signal_connect_data (policy, "notify",
                         G_CALLBACK (policy_notify_cb), watch,
                         (GClosureNotify) policy_watch_free, 0);

  tray_on_source_enable_changed (self, policy, source);
}

/**
 * shell_message_tray_has_source:
 * @self: a #ShellMessageTray
 * @source: a source
 *
 * Returns: whether @source is registered with the tray (`contains ()`)
 */
gboolean
shell_message_tray_has_source (ShellMessageTray *self,
                               ShellSource      *source)
{
  g_return_val_if_fail (SHELL_IS_MESSAGE_TRAY (self), FALSE);
  g_return_val_if_fail (SHELL_IS_SOURCE (source), FALSE);

  return g_ptr_array_find (self->sources, source, NULL);
}

/**
 * shell_message_tray_get_sources:
 * @self: a #ShellMessageTray
 *
 * Returns: (transfer container) (element-type ShellSource): the sources
 *   registered with the tray, in the order they were added
 */
GList *
shell_message_tray_get_sources (ShellMessageTray *self)
{
  GList *sources = NULL;
  guint i;

  g_return_val_if_fail (SHELL_IS_MESSAGE_TRAY (self), NULL);

  for (i = self->sources->len; i > 0; i--)
    sources = g_list_prepend (sources, g_ptr_array_index (self->sources, i - 1));

  return sources;
}

/**
 * shell_message_tray_get_queue_count:
 * @self: a #ShellMessageTray
 *
 * Returns: #ShellMessageTray:queue-count
 */
guint
shell_message_tray_get_queue_count (ShellMessageTray *self)
{
  g_return_val_if_fail (SHELL_IS_MESSAGE_TRAY (self), 0);

  return self->notification_queue->len;
}

/**
 * shell_message_tray_get_system_source:
 * @self: a #ShellMessageTray
 *
 * Gets the source that should be used to send system notifications
 * (`getSystemSource ()`), creating it and registering it with the tray if
 * needed. It is created again after it was destroyed.
 *
 * Returns: (transfer none): the system source
 */
ShellSource *
shell_message_tray_get_system_source (ShellMessageTray *self)
{
  g_return_val_if_fail (SHELL_IS_MESSAGE_TRAY (self), NULL);

  if (self->system_source == NULL)
    {
      self->system_source = g_object_new (SHELL_TYPE_SOURCE,
                                          "title", _("System"),
                                          "icon-name", "cog-wheel-symbolic",
                                          NULL);
      g_signal_connect (self->system_source, "destroy",
                        G_CALLBACK (system_source_destroy_cb), self);
      shell_message_tray_add (self, self->system_source);
    }

  return self->system_source;
}

/* ------------------------------------------------------------------------ */
/* Timeouts                                                                  */
/* ------------------------------------------------------------------------ */

static void
reset_notification_left_timeout (ShellMessageTray *self)
{
  self->use_longer_notification_left_timeout = FALSE;
  if (self->notification_left_timeout_id != 0)
    {
      g_clear_handle_id (&self->notification_left_timeout_id, g_source_remove);
      self->notification_left_mouse_x = -1;
      self->notification_left_mouse_y = -1;
    }
}

static void
get_pointer (int *x,
             int *y)
{
  shell_global_get_pointer (shell_global_get (), x, y, NULL);
}

static void
on_notification_left_timeout (gpointer user_data)
{
  ShellMessageTray *self = user_data;
  int x, y;

  get_pointer (&x, &y);

  /* We extend the timeout once if the mouse moved no further than
   * MOUSE_LEFT_ACTOR_THRESHOLD to either side. */
  if (self->notification_left_mouse_x > -1 &&
      y < self->notification_left_mouse_y + MOUSE_LEFT_ACTOR_THRESHOLD &&
      y > self->notification_left_mouse_y - MOUSE_LEFT_ACTOR_THRESHOLD &&
      x < self->notification_left_mouse_x + MOUSE_LEFT_ACTOR_THRESHOLD &&
      x > self->notification_left_mouse_x - MOUSE_LEFT_ACTOR_THRESHOLD)
    {
      self->notification_left_mouse_x = -1;
      self->notification_left_timeout_id =
        g_timeout_add_once (LONGER_HIDE_TIMEOUT, on_notification_left_timeout, self);
      g_source_set_name_by_id (self->notification_left_timeout_id,
                               "[gnome-shell] this._onNotificationLeftTimeout");
    }
  else
    {
      self->notification_left_timeout_id = 0;
      self->use_longer_notification_left_timeout = FALSE;
      self->pointer_in_notification = FALSE;
      g_clear_handle_id (&self->notification_timeout_id, g_source_remove);
      tray_update_state (self);
    }
}

static void
banner_bin_hover_changed_cb (StWidget         *banner_bin,
                             GParamSpec       *pspec,
                             ShellMessageTray *self)
{
  gboolean hover = st_widget_get_hover (banner_bin);

  if (hover == self->notification_hovered)
    return;

  self->notification_hovered = hover;
  if (self->notification_hovered)
    {
      reset_notification_left_timeout (self);

      if (self->show_notification_mouse_x >= 0)
        {
          ClutterActor *actor_at_show_position;

          actor_at_show_position =
            self->stage != NULL ?
            clutter_stage_get_actor_at_pos (self->stage, CLUTTER_PICK_ALL,
                                            self->show_notification_mouse_x,
                                            self->show_notification_mouse_y) : NULL;
          self->show_notification_mouse_x = -1;
          self->show_notification_mouse_y = -1;

          /* Don't set pointer_in_notification to TRUE if the pointer was
           * initially in the area where the notification popped up. That way
           * we will not be expanding notifications that happen to pop up over
           * the pointer automatically. Instead, the user is able to expand
           * the notification by mousing away from it and then mousing back
           * in. Because this is an expected action, we set the boolean flag
           * that indicates that a longer timeout should be used before
           * popping down the notification. */
          if (actor_at_show_position != NULL &&
              clutter_actor_contains (CLUTTER_ACTOR (self->banner_bin), actor_at_show_position))
            {
              self->use_longer_notification_left_timeout = TRUE;
              return;
            }
        }

      self->pointer_in_notification = TRUE;
      tray_update_state (self);
    }
  else
    {
      int timeout;
      int x, y;

      /* We record the position of the mouse the moment it leaves the tray.
       * These coordinates are used in on_notification_left_timeout () to
       * determine if the mouse has moved far enough during the initial
       * timeout for us to consider that the user intended to leave the tray
       * and therefore hide the tray. If the mouse is still close to its
       * previous position, we extend the timeout once. */
      get_pointer (&x, &y);
      self->notification_left_mouse_x = x;
      self->notification_left_mouse_y = y;

      /* We wait just a little before hiding the message tray in case the user
       * quickly moves the mouse back into it. We wait for a longer period if
       * the notification popped up where the mouse pointer was already
       * positioned. That gives the user more time to mouse away from the
       * notification and mouse back in in order to expand it. */
      timeout = self->use_longer_notification_left_timeout ? LONGER_HIDE_TIMEOUT : HIDE_TIMEOUT;
      g_clear_handle_id (&self->notification_left_timeout_id, g_source_remove);
      self->notification_left_timeout_id =
        g_timeout_add_once (timeout, on_notification_left_timeout, self);
      g_source_set_name_by_id (self->notification_left_timeout_id,
                               "[gnome-shell] this._onNotificationLeftTimeout");
    }
}

static void
update_notification_timeout (ShellMessageTray *self,
                             guint             timeout);

static void
notification_timeout_cb (gpointer user_data)
{
  ShellMessageTray *self = user_data;
  int x, y;

  get_pointer (&x, &y);

  if (y < self->last_seen_mouse_y - 10 && !self->notification_hovered)
    {
      /* The mouse is moving towards the notification, so don't hide it yet.
       * (We just create a new timeout (and destroy the old one) each time
       * because the bookkeeping is simpler.) */
      self->notification_timeout_id = 0;
      update_notification_timeout (self, 1000);
    }
  else if (self->use_longer_notification_left_timeout &&
           self->notification_left_timeout_id == 0 &&
           (x != self->last_seen_mouse_x || y != self->last_seen_mouse_y))
    {
      /* Refresh the timeout if the notification originally popped up under
       * the pointer, and the pointer is hovering inside it. */
      self->notification_timeout_id = 0;
      update_notification_timeout (self, 1000);
    }
  else
    {
      self->notification_timeout_id = 0;
      tray_update_state (self);
    }

  self->last_seen_mouse_x = x;
  self->last_seen_mouse_y = y;
}

static void
update_notification_timeout (ShellMessageTray *self,
                             guint             timeout)
{
  g_clear_handle_id (&self->notification_timeout_id, g_source_remove);

  if (timeout > 0)
    {
      self->notification_timeout_id =
        g_timeout_add_once (timeout, notification_timeout_cb, self);
      g_source_set_name_by_id (self->notification_timeout_id,
                               "[gnome-shell] this._notificationTimeout");
    }
}

/* ------------------------------------------------------------------------ */
/* Banners                                                                   */
/* ------------------------------------------------------------------------ */

static void
ensure_banner_focused (ShellMessageTray *self)
{
  focus_grabber_grab_focus (&self->focus_grabber);
}

static void
expand_banner (ShellMessageTray *self,
               gboolean          auto_expanding)
{
  if (self->banner == NULL)
    return;

  /* Don't animate changes in notifications that are auto-expanding. */
  shell_message_expand (SHELL_MESSAGE (self->banner), !auto_expanding);

  /* Don't focus notifications that are auto-expanding. */
  if (!auto_expanding)
    ensure_banner_focused (self);
}

static void
showing_notification_complete_cb (gboolean finished,
                                  gpointer user_data)
{
  ShellMessageTray *self = user_data;

  if (!finished || self->notification == NULL)
    return;

  self->notification_state = SHELL_NOTIFICATION_STATE_SHOWN;

  /* _showNotificationCompleted () */
  if (shell_notification_get_urgency (self->notification) != SHELL_NOTIFICATION_URGENCY_CRITICAL)
    update_notification_timeout (self, NOTIFICATION_TIMEOUT);

  tray_update_state (self);
}

static void
tray_update_showing_notification (ShellMessageTray *self)
{
  ShellEaseParams params;
  ClutterActor *bin = CLUTTER_ACTOR (self->banner_bin);

  if (self->notification == NULL)
    return;

  shell_notification_set_acknowledged (self->notification, TRUE);
  shell_notification_play_sound (self->notification);

  /* We auto-expand notifications with CRITICAL urgency, or for which the
   * relevant setting is on in the control center. */
  if (shell_notification_get_urgency (self->notification) == SHELL_NOTIFICATION_URGENCY_CRITICAL ||
      shell_notification_policy_get_force_expanded (shell_source_get_policy (shell_notification_get_source (self->notification))))
    expand_banner (self, TRUE);

  /* We tween all notifications to full opacity. This ensures that both new
   * notifications and notifications that might have been in the process of
   * hiding get full opacity.
   *
   * We tween any notification showing in the banner mode to the appropriate
   * height (which is banner height or expanded height, depending on the
   * notification state). This ensures that both new notifications and
   * notifications in the banner mode that might have been in the process of
   * hiding are shown with the correct height.
   *
   * We use showing_notification_complete_cb () to extend the time the updated
   * notification is being shown. */

  self->notification_state = SHELL_NOTIFICATION_STATE_SHOWING;
  clutter_actor_remove_all_transitions (bin);

  init_ease_params (&params, SHELL_MESSAGE_TRAY_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (bin, &params, NULL, NULL, NULL,
                    "opacity", 255u,
                    NULL);

  clutter_actor_set_pivot_point (bin, 0.5, 0.5);
  clutter_actor_set_scale (bin, 0.9, 0.9);

  init_ease_params (&params, SHELL_MESSAGE_TRAY_ANIMATION_TIME, CLUTTER_EASE_OUT_BACK);
  shell_actor_ease (bin, &params,
                    showing_notification_complete_cb, g_object_ref (self), g_object_unref,
                    "y", 0.0,
                    "scale-x", 1.0,
                    "scale-y", 1.0,
                    NULL);
}

static void
idle_monitor_became_active_cb (MetaIdleMonitor *monitor,
                               guint            watch_id,
                               gpointer         user_data)
{
  ShellMessageTray *self = user_data;

  self->user_active_watch_id = 0;
  self->user_active_while_notification_shown = TRUE;
  update_notification_timeout (self, 2000);
  tray_update_state (self);
}

static void
tray_show_notification (ShellMessageTray *self)
{
  ClutterActor *bin = CLUTTER_ACTOR (self->banner_bin);
  g_autoptr (ShellNotification) next = NULL;
  int x, y;

  next = g_ptr_array_steal_index (self->notification_queue, 0);
  g_clear_object (&self->notification);
  self->notification = g_steal_pointer (&next);
  emit_queue_changed (self);

  /* The session of a test script is idle for ever: its banners must expire
   * like those of a user who is at the computer */
  self->user_active_while_notification_shown =
    self->idle_monitor == NULL ||
    shell_test_runner_is_enabled () ||
    meta_idle_monitor_get_idletime (self->idle_monitor) <= IDLE_TIME;
  if (!self->user_active_while_notification_shown)
    {
      /* If the user isn't active, set up a watch to let us know when the user
       * becomes active. */
      if (self->user_active_watch_id != 0)
        meta_idle_monitor_remove_watch (self->idle_monitor, self->user_active_watch_id);
      self->user_active_watch_id =
        meta_idle_monitor_add_user_active_watch (self->idle_monitor,
                                                 idle_monitor_became_active_cb,
                                                 self, NULL);
    }


  g_set_weak_pointer (&self->banner,
                      CLUTTER_ACTOR (shell_notification_message_new (self->notification)));
  st_widget_set_can_focus (ST_WIDGET (self->banner), FALSE);
  shell_message_set_expand_button_visible (SHELL_MESSAGE (self->banner), FALSE);
  st_widget_add_style_class_name (ST_WIDGET (self->banner), "notification-banner");

  clutter_actor_add_child (bin, self->banner);

  clutter_actor_set_opacity (bin, 0);
  clutter_actor_set_y (bin, -clutter_actor_get_height (self->banner));
  clutter_actor_show (CLUTTER_ACTOR (self));

  if (self->compositor != NULL)
    meta_compositor_disable_unredirect (self->compositor);
  tray_update_showing_notification (self);
  emit_hook_after ("notification.shown", self->notification);

  get_pointer (&x, &y);
  /* We save the position of the mouse at the time when we started showing the
   * notification in order to determine if the notification popped up under
   * it. We make that check if the user starts moving the mouse and
   * banner_bin_hover_changed_cb () gets called. We don't expand the
   * notification if it just happened to pop up under the mouse unless the
   * user explicitly mouses away from it and then mouses back in. */
  self->show_notification_mouse_x = x;
  self->show_notification_mouse_y = y;
  /* We save the coordinates of the mouse at the time when we started showing
   * the notification and then we update it in notification_timeout_cb (). We
   * don't pop down the notification if the mouse is moving towards it or
   * within it. */
  self->last_seen_mouse_x = x;
  self->last_seen_mouse_y = y;

  reset_notification_left_timeout (self);
}

static void
tray_hide_notification_completed (ShellMessageTray *self)
{
  g_autoptr (ShellNotification) notification = g_steal_pointer (&self->notification);

  if (!self->notification_removed && notification != NULL &&
      shell_notification_get_is_transient (notification))
    shell_notification_destroy (notification, SHELL_NOTIFICATION_DESTROYED_REASON_EXPIRED);

  self->pointer_in_notification = FALSE;
  self->notification_removed = FALSE;
  if (self->compositor != NULL)
    meta_compositor_enable_unredirect (self->compositor);

  if (self->banner != NULL)
    {
      ClutterActor *banner = self->banner;

      g_clear_weak_pointer (&self->banner);
      clutter_actor_destroy (banner);
    }

  clutter_actor_hide (CLUTTER_ACTOR (self));
}

static void
hide_notification_complete_cb (gboolean finished,
                               gpointer user_data)
{
  ShellMessageTray *self = user_data;

  /* onStopped: whether or not the animation ran to its end */
  self->notification_state = SHELL_NOTIFICATION_STATE_HIDDEN;
  tray_hide_notification_completed (self);
  tray_update_state (self);
}

static void
tray_hide_notification (ShellMessageTray *self,
                        gboolean          animate)
{
  ClutterActor *bin = CLUTTER_ACTOR (self->banner_bin);
  ShellEaseParams params;
  int duration = animate ? SHELL_MESSAGE_TRAY_ANIMATION_TIME : 0;

  focus_grabber_ungrab_focus (&self->focus_grabber);

  reset_notification_left_timeout (self);
  clutter_actor_remove_all_transitions (bin);

  self->notification_state = SHELL_NOTIFICATION_STATE_HIDING;

  init_ease_params (&params, duration, CLUTTER_EASE_OUT_BACK);
  shell_actor_ease (bin, &params, NULL, NULL, NULL,
                    "opacity", 0u,
                    NULL);
  shell_actor_ease (bin, &params,
                    hide_notification_complete_cb, g_object_ref (self), g_object_unref,
                    "y", (double) -clutter_actor_get_height (bin),
                    NULL);
}

/* All of the logic for what happens when occurs here; the various event
 * handlers merely update variables such as 'pointer_in_notification', and
 * tray_update_state () figures out what (if anything) needs to be done at the
 * present time. */
static void
tray_update_state (ShellMessageTray *self)
{
  ShellMain *shell_main = shell_main_get_default ();
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main);
  ShellSessionMode *session_mode = shell_main_get_session_mode (shell_main);
  ShellMonitor *primary_monitor = NULL;
  gboolean has_monitor;
  gboolean has_notifications = FALSE;
  gboolean changed = FALSE;
  guint i;

  if (self->disposed)
    return;

  if (layout_manager != NULL)
    primary_monitor = shell_layout_manager_get_primary_monitor (layout_manager);

  has_monitor = primary_monitor != NULL;
  set_actor_visible (CLUTTER_ACTOR (self),
                     !self->banner_blocked && has_monitor && self->banner != NULL);
  if (self->banner_blocked || !has_monitor)
    goto out;

  /* If our state changes caused tray_update_state () to be called, just exit
   * now to prevent reentrancy issues. */
  if (self->updating_state)
    goto out;

  self->updating_state = TRUE;

  /* Filter out acknowledged notifications. */
  for (i = 0; i < self->notification_queue->len;)
    {
      if (shell_notification_get_acknowledged (g_ptr_array_index (self->notification_queue, i)))
        {
          g_ptr_array_remove_index (self->notification_queue, i);
          changed = TRUE;
        }
      else
        {
          i++;
        }
    }

  if (changed)
    emit_queue_changed (self);

  if (session_mode != NULL)
    g_object_get (session_mode, "has-notifications", &has_notifications, NULL);

  if (self->notification_state == SHELL_NOTIFICATION_STATE_HIDDEN)
    {
      ShellNotification *next_notification = NULL;

      if (self->notification_queue->len > 0)
        next_notification = g_ptr_array_index (self->notification_queue, 0);

      if (has_notifications && next_notification != NULL)
        {
          gboolean limited = self->busy || shell_monitor_get_in_fullscreen (primary_monitor);
          gboolean show_next_notification =
            !limited ||
            shell_notification_get_for_feedback (next_notification) ||
            shell_notification_get_urgency (next_notification) == SHELL_NOTIFICATION_URGENCY_CRITICAL;

          if (show_next_notification)
            tray_show_notification (self);
        }
    }
  else if (self->notification_state == SHELL_NOTIFICATION_STATE_SHOWING ||
           self->notification_state == SHELL_NOTIFICATION_STATE_SHOWN)
    {
      gboolean expired;
      gboolean must_close;

      expired = (self->user_active_while_notification_shown &&
                 self->notification_state == SHELL_NOTIFICATION_STATE_SHOWN &&
                 self->notification_timeout_id == 0 &&
                 self->notification != NULL &&
                 shell_notification_get_urgency (self->notification) != SHELL_NOTIFICATION_URGENCY_CRITICAL &&
                 !self->pointer_in_notification) ||
                self->notification_expired;
      must_close = self->notification_removed || !has_notifications || expired;

      if (must_close)
        {
          gboolean animate = has_notifications && !self->notification_removed;

          tray_hide_notification (self, animate);
        }
      else if (self->notification_state == SHELL_NOTIFICATION_STATE_SHOWN &&
               self->pointer_in_notification &&
               self->banner != NULL)
        {
          if (!shell_message_get_expanded (SHELL_MESSAGE (self->banner)))
            expand_banner (self, FALSE);
          else
            ensure_banner_focused (self);
        }
    }

  self->updating_state = FALSE;

  /* Clean transient variables that are used to communicate actions to
   * tray_update_state () */
  self->notification_expired = FALSE;

out:
  g_clear_pointer (&primary_monitor, shell_monitor_free);
}

/**
 * shell_message_tray_expand_active_notification:
 * @self: a #ShellMessageTray
 *
 * Expands the banner that is shown and moves the focus to it. This is what
 * the `focus-active-notification` keybinding does; ShellMain installs the
 * keybinding.
 */
void
shell_message_tray_expand_active_notification (ShellMessageTray *self)
{
  g_return_if_fail (SHELL_IS_MESSAGE_TRAY (self));

  if (self->banner == NULL)
    return;

  expand_banner (self, FALSE);
}

/* ------------------------------------------------------------------------ */
/* Properties                                                                */
/* ------------------------------------------------------------------------ */

/**
 * shell_message_tray_get_banner_alignment:
 * @self: a #ShellMessageTray
 *
 * Returns: #ShellMessageTray:banner-alignment
 */
ClutterActorAlign
shell_message_tray_get_banner_alignment (ShellMessageTray *self)
{
  g_return_val_if_fail (SHELL_IS_MESSAGE_TRAY (self), CLUTTER_ACTOR_ALIGN_CENTER);

  return clutter_actor_get_x_align (CLUTTER_ACTOR (self->banner_bin));
}

/**
 * shell_message_tray_set_banner_alignment:
 * @self: a #ShellMessageTray
 * @alignment: where the banners are placed along the top of the monitor
 *
 * Sets #ShellMessageTray:banner-alignment.
 */
void
shell_message_tray_set_banner_alignment (ShellMessageTray *self,
                                         ClutterActorAlign alignment)
{
  g_return_if_fail (SHELL_IS_MESSAGE_TRAY (self));

  if (clutter_actor_get_x_align (CLUTTER_ACTOR (self->banner_bin)) == alignment)
    return;

  clutter_actor_set_x_align (CLUTTER_ACTOR (self->banner_bin), alignment);
  g_object_notify_by_pspec (G_OBJECT (self), tray_props[TRAY_PROP_BANNER_ALIGNMENT]);
}

/**
 * shell_message_tray_get_banner_blocked:
 * @self: a #ShellMessageTray
 *
 * Returns: #ShellMessageTray:banner-blocked
 */
gboolean
shell_message_tray_get_banner_blocked (ShellMessageTray *self)
{
  g_return_val_if_fail (SHELL_IS_MESSAGE_TRAY (self), FALSE);

  return self->banner_blocked;
}

/**
 * shell_message_tray_set_banner_blocked:
 * @self: a #ShellMessageTray
 * @blocked: whether banners are held back
 *
 * Sets #ShellMessageTray:banner-blocked.
 */
void
shell_message_tray_set_banner_blocked (ShellMessageTray *self,
                                       gboolean          blocked)
{
  g_return_if_fail (SHELL_IS_MESSAGE_TRAY (self));

  blocked = !!blocked;
  if (self->banner_blocked == blocked)
    return;

  self->banner_blocked = blocked;
  g_object_notify_by_pspec (G_OBJECT (self), tray_props[TRAY_PROP_BANNER_BLOCKED]);
  tray_update_state (self);
}

/**
 * shell_message_tray_get_dragging:
 * @self: a #ShellMessageTray
 *
 * Returns: #ShellMessageTray:dragging
 */
gboolean
shell_message_tray_get_dragging (ShellMessageTray *self)
{
  g_return_val_if_fail (SHELL_IS_MESSAGE_TRAY (self), FALSE);

  return self->dragging;
}

/**
 * shell_message_tray_set_dragging:
 * @self: a #ShellMessageTray
 * @dragging: whether the user is dragging a window, an item or something
 *   from another application
 *
 * Sets #ShellMessageTray:dragging. The tray does not take part in the pick
 * while that is going on.
 */
void
shell_message_tray_set_dragging (ShellMessageTray *self,
                                 gboolean          dragging)
{
  g_return_if_fail (SHELL_IS_MESSAGE_TRAY (self));

  dragging = !!dragging;
  if (self->dragging == dragging)
    return;

  self->dragging = dragging;
  shell_util_set_hidden_from_pick (CLUTTER_ACTOR (self), dragging);
  g_object_notify_by_pspec (G_OBJECT (self), tray_props[TRAY_PROP_DRAGGING]);
}

static void
shell_message_tray_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellMessageTray *self = SHELL_MESSAGE_TRAY (object);

  switch (prop_id)
    {
    case TRAY_PROP_BANNER_ALIGNMENT:
      g_value_set_enum (value, shell_message_tray_get_banner_alignment (self));
      break;

    case TRAY_PROP_BANNER_BLOCKED:
      g_value_set_boolean (value, self->banner_blocked);
      break;

    case TRAY_PROP_QUEUE_COUNT:
      g_value_set_uint (value, self->notification_queue->len);
      break;

    case TRAY_PROP_DRAGGING:
      g_value_set_boolean (value, self->dragging);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_message_tray_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellMessageTray *self = SHELL_MESSAGE_TRAY (object);

  switch (prop_id)
    {
    case TRAY_PROP_BANNER_ALIGNMENT:
      shell_message_tray_set_banner_alignment (self, g_value_get_enum (value));
      break;

    case TRAY_PROP_BANNER_BLOCKED:
      shell_message_tray_set_banner_blocked (self, g_value_get_boolean (value));
      break;

    case TRAY_PROP_DRAGGING:
      shell_message_tray_set_dragging (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------------ */
/* Construction                                                              */
/* ------------------------------------------------------------------------ */

static gboolean
close_binding_cb (GObject             *object,
                  const char          *action_name,
                  guint                key_val,
                  ClutterModifierType  modifiers,
                  gpointer             user_data)
{
  ShellMessageTray *self = SHELL_MESSAGE_TRAY (object);

  /* _expireNotification () */
  self->notification_expired = TRUE;
  tray_update_state (self);

  return CLUTTER_EVENT_STOP;
}

static void
session_updated_cb (ShellSessionMode *session_mode,
                    ShellMessageTray *self)
{
  tray_update_state (self);
}

static void
display_in_fullscreen_changed_cb (MetaDisplay      *display,
                                  ShellMessageTray *self)
{
  tray_update_state (self);
}

static void
presence_status_changed_cb (ShellSessionManagerPresence *presence,
                            guint                        status,
                            ShellMessageTray            *self)
{
  if (status == SHELL_PRESENCE_STATUS_BUSY)
    {
      /* remove notification and allow the summary to be closed now */
      g_clear_handle_id (&self->notification_timeout_id, g_source_remove);
      self->busy = TRUE;
    }
  else if (status != SHELL_PRESENCE_STATUS_IDLE)
    {
      /* We preserve the previous value of busy if the status turns to IDLE so
       * that we don't start showing notifications queued during the BUSY
       * state as the screensaver gets activated. */
      self->busy = FALSE;
    }

  tray_update_state (self);
}

static void
presence_ready_cb (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  GWeakRef *tray_ref = user_data;
  g_autoptr (ShellMessageTray) self = g_weak_ref_get (tray_ref);
  g_autoptr (ShellSessionManagerPresence) presence = NULL;

  presence = shell_gnome_session_presence_new_finish (res, NULL);

  g_weak_ref_clear (tray_ref);
  g_free (tray_ref);

  if (self == NULL || presence == NULL)
    return;

  self->presence = g_steal_pointer (&presence);
  g_signal_connect_object (self->presence, "status-changed",
                           G_CALLBACK (presence_status_changed_cb), self, 0);
  presence_status_changed_cb (self->presence,
                              shell_session_manager_presence_get_status (self->presence),
                              self);
}

static void
on_shutdown (ShellGlobal      *global,
             ShellMessageTray *self)
{
  /* Nothing may animate or show up while the shell is going away */
  g_clear_handle_id (&self->notification_timeout_id, g_source_remove);
  g_clear_handle_id (&self->notification_left_timeout_id, g_source_remove);

  g_cancellable_cancel (self->cancellable);

  if (self->user_active_watch_id != 0 && self->idle_monitor != NULL)
    meta_idle_monitor_remove_watch (self->idle_monitor, self->user_active_watch_id);
  self->user_active_watch_id = 0;
}

static void
shell_message_tray_constructed (GObject *object)
{
  ShellMessageTray *self = SHELL_MESSAGE_TRAY (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellGlobal *global = shell_global_get ();
  ShellMain *shell_main = shell_main_get_default ();
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main);
  ShellSessionMode *session_mode = shell_main_get_session_mode (shell_main);
  ShellMonitorConstraint *constraint;
  GWeakRef *tray_ref;

  G_OBJECT_CLASS (shell_message_tray_parent_class)->constructed (object);

  clutter_actor_set_layout_manager (actor, clutter_bin_layout_new ());
  clutter_actor_set_clip_to_allocation (actor, TRUE);
  clutter_actor_hide (actor);

  g_set_weak_pointer (&self->stage, shell_global_get_stage (global));
  g_set_weak_pointer (&self->display, shell_global_get_display (global));
  g_set_weak_pointer (&self->compositor, shell_global_get_compositor (global));
  g_set_weak_pointer (&self->idle_monitor,
                      meta_backend_get_core_idle_monitor (shell_global_get_backend (global)));

  g_signal_connect_object (global, "shutdown", G_CALLBACK (on_shutdown), self, 0);

  /* Presence */
  self->cancellable = g_cancellable_new ();
  tray_ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (tray_ref, self);
  shell_gnome_session_presence_new (self->cancellable, presence_ready_cb, tray_ref);

  constraint = shell_monitor_constraint_new ();
  shell_monitor_constraint_set_primary (constraint, TRUE);
  if (layout_manager != NULL)
    {
      g_object_bind_property (shell_layout_manager_get_panel_box (layout_manager), "visible",
                              constraint, "work-area",
                              G_BINDING_SYNC_CREATE);
    }
  clutter_actor_add_constraint (actor, CLUTTER_CONSTRAINT (constraint));

  self->banner_bin = g_object_new (ST_TYPE_WIDGET,
                                   "name", "notification-container",
                                   "reactive", TRUE,
                                   "track-hover", TRUE,
                                   "y-align", CLUTTER_ACTOR_ALIGN_START,
                                   "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                   "y-expand", TRUE,
                                   "x-expand", TRUE,
                                   NULL);
  clutter_actor_set_layout_manager (CLUTTER_ACTOR (self->banner_bin), clutter_bin_layout_new ());
  g_signal_connect_object (self->banner_bin, "notify::hover",
                           G_CALLBACK (banner_bin_hover_changed_cb), self, 0);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (self->banner_bin));

  self->focus_grabber.actor = CLUTTER_ACTOR (self->banner_bin);
  g_set_weak_pointer (&self->focus_grabber.stage, self->stage);

  if (layout_manager != NULL)
    {
      shell_layout_manager_add_chrome (layout_manager, actor, SHELL_CHROME_FLAGS_NONE);
      shell_layout_manager_track_chrome (layout_manager, CLUTTER_ACTOR (self->banner_bin));
    }

  if (self->display != NULL)
    {
      g_signal_connect_object (self->display, "in-fullscreen-changed",
                               G_CALLBACK (display_in_fullscreen_changed_cb), self, 0);
    }

  if (session_mode != NULL)
    {
      g_signal_connect_object (session_mode, "updated",
                               G_CALLBACK (session_updated_cb), self, 0);
    }

  tray_update_state (self);
}

static void
shell_message_tray_dispose (GObject *object)
{
  ShellMessageTray *self = SHELL_MESSAGE_TRAY (object);
  guint i;

  self->disposed = TRUE;

  if (self->cancellable != NULL)
    {
      g_cancellable_cancel (self->cancellable);
      g_clear_object (&self->cancellable);
    }

  g_clear_handle_id (&self->notification_timeout_id, g_source_remove);
  g_clear_handle_id (&self->notification_left_timeout_id, g_source_remove);

  if (self->user_active_watch_id != 0 && self->idle_monitor != NULL)
    meta_idle_monitor_remove_watch (self->idle_monitor, self->user_active_watch_id);
  self->user_active_watch_id = 0;

  focus_grabber_clear (&self->focus_grabber);

  for (i = 0; i < self->sources->len; i++)
    g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->sources, i), self);
  g_ptr_array_set_size (self->sources, 0);
  g_ptr_array_set_size (self->notification_queue, 0);

  if (self->system_source != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->system_source, self);
      g_clear_object (&self->system_source);
    }

  g_clear_object (&self->notification);
  g_clear_object (&self->presence);
  g_clear_weak_pointer (&self->banner);
  g_clear_weak_pointer (&self->stage);
  g_clear_weak_pointer (&self->display);
  g_clear_weak_pointer (&self->compositor);
  g_clear_weak_pointer (&self->idle_monitor);

  G_OBJECT_CLASS (shell_message_tray_parent_class)->dispose (object);
}

static void
shell_message_tray_finalize (GObject *object)
{
  ShellMessageTray *self = SHELL_MESSAGE_TRAY (object);

  g_clear_pointer (&self->notification_queue, g_ptr_array_unref);
  g_clear_pointer (&self->sources, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_message_tray_parent_class)->finalize (object);
}

static void
shell_message_tray_class_init (ShellMessageTrayClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterBindingPool *binding_pool;

  object_class->get_property = shell_message_tray_get_property;
  object_class->set_property = shell_message_tray_set_property;
  object_class->constructed = shell_message_tray_constructed;
  object_class->dispose = shell_message_tray_dispose;
  object_class->finalize = shell_message_tray_finalize;

  /**
   * ShellMessageTray:banner-alignment:
   *
   * Where the banners are placed along the top of the monitor. The panel
   * aligns them with the date menu; ShellMain forwards
   * #ShellPanel:banner-alignment.
   */
  tray_props[TRAY_PROP_BANNER_ALIGNMENT] =
    g_param_spec_enum ("banner-alignment", NULL, NULL,
                       CLUTTER_TYPE_ACTOR_ALIGN,
                       CLUTTER_ACTOR_ALIGN_CENTER,
                       G_PARAM_READWRITE |
                       G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellMessageTray:banner-blocked:
   *
   * Whether banners are held back, for example while a menu is open in the
   * same box of the top bar. ShellMain forwards #ShellPanel:banner-blocked.
   */
  tray_props[TRAY_PROP_BANNER_BLOCKED] =
    g_param_spec_boolean ("banner-blocked", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellMessageTray:queue-count:
   *
   * The number of notifications that wait for their banner.
   */
  tray_props[TRAY_PROP_QUEUE_COUNT] =
    g_param_spec_uint ("queue-count", NULL, NULL,
                       0, G_MAXUINT, 0,
                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellMessageTray:dragging:
   *
   * Whether a drag is going on, in the overview or from another application.
   * The tray is hidden from the pick meanwhile; ShellMain sets it from the drag
   * signals of the overview and of the Xdnd handler.
   */
  tray_props[TRAY_PROP_DRAGGING] =
    g_param_spec_boolean ("dragging", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, TRAY_N_PROPS, tray_props);

  /**
   * ShellMessageTray::queue-changed:
   * @self: the tray
   *
   * The queue of notifications that wait for their banner changed.
   */
  tray_signals[TRAY_QUEUE_CHANGED] =
    g_signal_new ("queue-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellMessageTray::source-added:
   * @self: the tray
   * @source: the source that was added
   */
  tray_signals[TRAY_SOURCE_ADDED] =
    g_signal_new ("source-added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_SOURCE);

  /**
   * ShellMessageTray::source-removed:
   * @self: the tray
   * @source: the source that was removed
   */
  tray_signals[TRAY_SOURCE_REMOVED] =
    g_signal_new ("source-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_SOURCE);

  binding_pool = clutter_binding_pool_get_for_class (klass);
  clutter_binding_pool_install_action (binding_pool,
                                       "close",
                                       CLUTTER_KEY_Escape,
                                       CLUTTER_RELEASE_MASK,
                                       G_CALLBACK (close_binding_cb),
                                       NULL,
                                       NULL);

  declare_hooks ();
}

static void
shell_message_tray_init (ShellMessageTray *self)
{
  self->notification_queue = g_ptr_array_new_with_free_func (g_object_unref);
  self->sources = g_ptr_array_new_with_free_func (g_object_unref);

  self->notification_state = SHELL_NOTIFICATION_STATE_HIDDEN;
  self->notification_left_mouse_x = -1;
  self->notification_left_mouse_y = -1;
  self->show_notification_mouse_x = -1;
  self->show_notification_mouse_y = -1;
}

/**
 * shell_message_tray_new:
 *
 * Creates the message tray (`new MessageTray ()`). It adds itself to the
 * chrome of the layout manager of #ShellMain, which ShellMain creates first.
 *
 * Returns: (transfer full): a new #ShellMessageTray
 */
ShellMessageTray *
shell_message_tray_new (void)
{
  return g_object_new (SHELL_TYPE_MESSAGE_TRAY, NULL);
}
