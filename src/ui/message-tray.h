/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * message-tray.h: Notifications and the tray that shows their banners. Port of
 * js/ui/messageTray.js (and of the Source class of js/ui/messageList.js).
 *
 * The types, with the name they had in the JS module:
 *
 *  - #ShellMessageSource: `MessageList.Source`, a title and an icon. Derivable;
 *    mpris.c creates plain ones for its players.
 *  - #ShellNotificationPolicy (`NotificationPolicy`, derivable, abstract),
 *    #ShellNotificationGenericPolicy and #ShellNotificationApplicationPolicy:
 *    what the user configured for the notifications of an application.
 *  - #ShellNotificationAction (`Action`) and #ShellNotificationSound (`Sound`).
 *  - #ShellNotification (`Notification`, derivable): `activate ()` and
 *    `destroy ()` are virtual functions (`vfunc_activate ()` and
 *    `vfunc_destroy (reason)` in a JS subclass; chain up with
 *    `super.vfunc_destroy (reason)`).
 *  - #ShellSource (`MessageTray.Source`, derivable): `open ()` and
 *    `destroy ()` are virtual functions too. The JS subclasses of the old class
 *    that overrode the `title` or `icon` getters set the properties instead.
 *  - #ShellMessageTray (`MessageTray`): the banner queue, final.
 *
 * `getSystemSource ()` is shell_message_tray_get_system_source (); the
 * `Main.messageTray` singleton is still created by ShellMain.
 *
 * The enumerations replace `MessageTray.State`, `.NotificationDestroyedReason`,
 * `.Urgency` and `.PrivacyScope`.
 */

#pragma once

#include <clutter/clutter.h>
#include <gio/gio.h>

#include "shell-app.h"
#include "st.h"

G_BEGIN_DECLS

/**
 * SHELL_MESSAGE_TRAY_ANIMATION_TIME:
 *
 * Duration in milliseconds of the animations of the banners
 * (`MessageTray.ANIMATION_TIME`).
 */
#define SHELL_MESSAGE_TRAY_ANIMATION_TIME 200

/**
 * ShellNotificationState:
 * @SHELL_NOTIFICATION_STATE_HIDDEN: not shown
 * @SHELL_NOTIFICATION_STATE_SHOWING: animating in
 * @SHELL_NOTIFICATION_STATE_SHOWN: shown
 * @SHELL_NOTIFICATION_STATE_HIDING: animating out
 *
 * The state of a banner, or of anything else that slides in and out
 * (`MessageTray.State`; screen-shield.c uses it for the lock screen).
 */
typedef enum
{
  SHELL_NOTIFICATION_STATE_HIDDEN  = 0,
  SHELL_NOTIFICATION_STATE_SHOWING = 1,
  SHELL_NOTIFICATION_STATE_SHOWN   = 2,
  SHELL_NOTIFICATION_STATE_HIDING  = 3,
} ShellNotificationState;

/**
 * ShellNotificationDestroyedReason:
 * @SHELL_NOTIFICATION_DESTROYED_REASON_EXPIRED: the notification was dismissed
 *   and the user did not interact with it
 * @SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED: the notification was
 *   destroyed as a result of a user action
 * @SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED: the associated source
 *   requested the destruction
 * @SHELL_NOTIFICATION_DESTROYED_REASON_REPLACED: a newer version of the
 *   notification replaced it
 *
 * Why a notification was destroyed; useful for the notifications received
 * through the notification daemon (`MessageTray.NotificationDestroyedReason`).
 */
typedef enum
{
  SHELL_NOTIFICATION_DESTROYED_REASON_EXPIRED       = 1,
  SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED     = 2,
  SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED = 3,
  SHELL_NOTIFICATION_DESTROYED_REASON_REPLACED      = 4,
} ShellNotificationDestroyedReason;

/**
 * ShellNotificationUrgency:
 * @SHELL_NOTIFICATION_URGENCY_LOW: shown in the list only, never as a banner
 * @SHELL_NOTIFICATION_URGENCY_NORMAL: the default
 * @SHELL_NOTIFICATION_URGENCY_HIGH: queued before the normal ones
 * @SHELL_NOTIFICATION_URGENCY_CRITICAL: bypasses do-not-disturb, is expanded
 *   and is never hidden by a timeout
 *
 * The message tray has its own urgency enumeration. LOW, NORMAL and CRITICAL
 * map to the corresponding values of the notifications received through the
 * notification daemon (`MessageTray.Urgency`).
 */
typedef enum
{
  SHELL_NOTIFICATION_URGENCY_LOW      = 0,
  SHELL_NOTIFICATION_URGENCY_NORMAL   = 1,
  SHELL_NOTIFICATION_URGENCY_HIGH     = 2,
  SHELL_NOTIFICATION_URGENCY_CRITICAL = 3,
} ShellNotificationUrgency;

/**
 * ShellNotificationPrivacyScope:
 * @SHELL_NOTIFICATION_PRIVACY_SCOPE_USER: the notification contains
 *   information private to the originating user account, for example the
 *   details of an e-mail
 * @SHELL_NOTIFICATION_PRIVACY_SCOPE_SYSTEM: the notification contains
 *   information private to the physical system, for example the battery
 *   status, and hence the same for every user
 *
 * The privacy of the details of a notification. This affects whether the
 * content of a notification is shown on the lock screen
 * (`MessageTray.PrivacyScope`).
 */
typedef enum
{
  SHELL_NOTIFICATION_PRIVACY_SCOPE_USER   = 0,
  SHELL_NOTIFICATION_PRIVACY_SCOPE_SYSTEM = 1,
} ShellNotificationPrivacyScope;

/* ------------------------------------------------------------------------ */
/* Types                                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MESSAGE_SOURCE (shell_message_source_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellMessageSource, shell_message_source,
                          SHELL, MESSAGE_SOURCE, GObject)

#define SHELL_TYPE_NOTIFICATION_POLICY (shell_notification_policy_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellNotificationPolicy, shell_notification_policy,
                          SHELL, NOTIFICATION_POLICY, GObject)

#define SHELL_TYPE_NOTIFICATION_ACTION (shell_notification_action_get_type ())
G_DECLARE_FINAL_TYPE (ShellNotificationAction, shell_notification_action,
                      SHELL, NOTIFICATION_ACTION, GObject)

#define SHELL_TYPE_NOTIFICATION_SOUND (shell_notification_sound_get_type ())
G_DECLARE_FINAL_TYPE (ShellNotificationSound, shell_notification_sound,
                      SHELL, NOTIFICATION_SOUND, GObject)

#define SHELL_TYPE_NOTIFICATION (shell_notification_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellNotification, shell_notification,
                          SHELL, NOTIFICATION, GObject)

#define SHELL_TYPE_SOURCE (shell_source_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellSource, shell_source,
                          SHELL, SOURCE, ShellMessageSource)

#define SHELL_TYPE_MESSAGE_TRAY (shell_message_tray_get_type ())
G_DECLARE_FINAL_TYPE (ShellMessageTray, shell_message_tray,
                      SHELL, MESSAGE_TRAY, StWidget)

/* ------------------------------------------------------------------------ */
/* ShellMessageSource                                                        */
/* ------------------------------------------------------------------------ */

/**
 * ShellMessageSourceClass:
 * @parent_class: the parent class
 *
 * A source of messages in the message list: a #ShellMessageSource:title and
 * a #ShellMessageSource:icon (`MessageList.Source`).
 */
struct _ShellMessageSourceClass
{
  GObjectClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

ShellMessageSource *shell_message_source_new           (void);

const char *shell_message_source_get_title             (ShellMessageSource *self);
void        shell_message_source_set_title             (ShellMessageSource *self,
                                                        const char         *title);
GIcon      *shell_message_source_get_icon              (ShellMessageSource *self);
void        shell_message_source_set_icon              (ShellMessageSource *self,
                                                        GIcon              *icon);
const char *shell_message_source_get_icon_name         (ShellMessageSource *self);
void        shell_message_source_set_icon_name         (ShellMessageSource *self,
                                                        const char         *icon_name);

/* ------------------------------------------------------------------------ */
/* ShellNotificationPolicy                                                   */
/* ------------------------------------------------------------------------ */

/**
 * ShellNotificationPolicyClass:
 * @parent_class: the parent class
 * @store: registers the policy where the settings application finds it
 *   (`store ()`); the default does nothing
 * @get_enable: whether the notifications are enabled (`enable`)
 * @get_enable_sound: whether the sound of the notifications is played
 *   (`enableSound`)
 * @get_show_banners: whether banners are shown (`showBanners`)
 * @get_force_expanded: whether banners are always expanded (`forceExpanded`)
 * @get_show_in_lock_screen: whether the notifications are shown on the lock
 *   screen (`showInLockScreen`)
 * @get_details_in_lock_screen: whether the details of the notifications are
 *   shown on the lock screen (`detailsInLockScreen`)
 *
 * An object that holds all bits of configurable policy related to a
 * notification source, such as whether to play sound or honour the critical
 * bit. A notification without a policy object inherits the default one.
 */
struct _ShellNotificationPolicyClass
{
  GObjectClass parent_class;

  void     (* store)                     (ShellNotificationPolicy *self);
  gboolean (* get_enable)                (ShellNotificationPolicy *self);
  gboolean (* get_enable_sound)          (ShellNotificationPolicy *self);
  gboolean (* get_show_banners)          (ShellNotificationPolicy *self);
  gboolean (* get_force_expanded)        (ShellNotificationPolicy *self);
  gboolean (* get_show_in_lock_screen)   (ShellNotificationPolicy *self);
  gboolean (* get_details_in_lock_screen) (ShellNotificationPolicy *self);

  /*< private >*/
  gpointer padding[8];
};

#define SHELL_TYPE_NOTIFICATION_GENERIC_POLICY (shell_notification_generic_policy_get_type ())
G_DECLARE_FINAL_TYPE (ShellNotificationGenericPolicy, shell_notification_generic_policy,
                      SHELL, NOTIFICATION_GENERIC_POLICY, ShellNotificationPolicy)

#define SHELL_TYPE_NOTIFICATION_APPLICATION_POLICY (shell_notification_application_policy_get_type ())
G_DECLARE_FINAL_TYPE (ShellNotificationApplicationPolicy, shell_notification_application_policy,
                      SHELL, NOTIFICATION_APPLICATION_POLICY, ShellNotificationPolicy)

ShellNotificationPolicy *shell_notification_policy_new_for_app (ShellApp *app);
ShellNotificationGenericPolicy *shell_notification_generic_policy_new (void);
ShellNotificationApplicationPolicy *shell_notification_application_policy_new (const char *id);

const char *shell_notification_policy_get_id                  (ShellNotificationPolicy *self);
void        shell_notification_policy_store                   (ShellNotificationPolicy *self);
void        shell_notification_policy_destroy                 (ShellNotificationPolicy *self);
gboolean    shell_notification_policy_get_enable              (ShellNotificationPolicy *self);
gboolean    shell_notification_policy_get_enable_sound        (ShellNotificationPolicy *self);
gboolean    shell_notification_policy_get_show_banners        (ShellNotificationPolicy *self);
gboolean    shell_notification_policy_get_force_expanded      (ShellNotificationPolicy *self);
gboolean    shell_notification_policy_get_show_in_lock_screen (ShellNotificationPolicy *self);
gboolean    shell_notification_policy_get_details_in_lock_screen (ShellNotificationPolicy *self);

/* ------------------------------------------------------------------------ */
/* ShellNotificationAction, ShellNotificationSound                            */
/* ------------------------------------------------------------------------ */

/**
 * ShellNotificationActionFunc:
 * @notification: the notification the action belongs to
 * @user_data: (closure): the data given to shell_notification_add_action ()
 *
 * Called when the user activates an action of a notification. Unless the
 * notification is resident it is destroyed afterwards.
 */
typedef void (* ShellNotificationActionFunc) (ShellNotification *notification,
                                              gpointer           user_data);

const char *shell_notification_action_get_label (ShellNotificationAction *self);
void        shell_notification_action_activate  (ShellNotificationAction *self);

ShellNotificationSound *shell_notification_sound_new  (GFile      *file,
                                                       const char *name);
void                    shell_notification_sound_play (ShellNotificationSound *self);

/* ------------------------------------------------------------------------ */
/* ShellNotification                                                         */
/* ------------------------------------------------------------------------ */

/**
 * ShellNotificationClass:
 * @parent_class: the parent class
 * @activate: the user activated the notification (`activate ()`). The default
 *   emits #ShellNotification::activated and destroys the notification unless
 *   it is resident
 * @destroy: destroys the notification (`destroy (reason)`). The default emits
 *   #ShellNotification::destroy and disposes the object
 */
struct _ShellNotificationClass
{
  GObjectClass parent_class;

  void (* activate) (ShellNotification *self);
  void (* destroy)  (ShellNotification *self,
                     ShellNotificationDestroyedReason reason);

  /*< private >*/
  gpointer padding[8];
};

ShellNotification *shell_notification_new (ShellSource *source,
                                           const char  *title,
                                           const char  *body);

ShellSource *shell_notification_get_source (ShellNotification *self);
guint        shell_notification_get_id     (ShellNotification *self);

const char  *shell_notification_get_title            (ShellNotification *self);
void         shell_notification_set_title            (ShellNotification *self,
                                                      const char        *title);
const char  *shell_notification_get_body             (ShellNotification *self);
void         shell_notification_set_body             (ShellNotification *self,
                                                      const char        *body);
gboolean     shell_notification_get_use_body_markup  (ShellNotification *self);
void         shell_notification_set_use_body_markup  (ShellNotification *self,
                                                      gboolean           use_body_markup);
GIcon       *shell_notification_get_gicon            (ShellNotification *self);
void         shell_notification_set_gicon            (ShellNotification *self,
                                                      GIcon             *gicon);
const char  *shell_notification_get_icon_name        (ShellNotification *self);
void         shell_notification_set_icon_name        (ShellNotification *self,
                                                      const char        *icon_name);
ShellNotificationSound *shell_notification_get_sound (ShellNotification *self);
void         shell_notification_set_sound            (ShellNotification      *self,
                                                      ShellNotificationSound *sound);
GDateTime   *shell_notification_get_datetime         (ShellNotification *self);
void         shell_notification_set_datetime         (ShellNotification *self,
                                                      GDateTime         *datetime);
ShellNotificationPrivacyScope shell_notification_get_privacy_scope (ShellNotification *self);
void         shell_notification_set_privacy_scope    (ShellNotification            *self,
                                                      ShellNotificationPrivacyScope privacy_scope);
ShellNotificationUrgency shell_notification_get_urgency (ShellNotification *self);
void         shell_notification_set_urgency          (ShellNotification        *self,
                                                      ShellNotificationUrgency  urgency);
gboolean     shell_notification_get_acknowledged     (ShellNotification *self);
void         shell_notification_set_acknowledged     (ShellNotification *self,
                                                      gboolean           acknowledged);
gboolean     shell_notification_get_resident         (ShellNotification *self);
void         shell_notification_set_resident         (ShellNotification *self,
                                                      gboolean           resident);
gboolean     shell_notification_get_for_feedback     (ShellNotification *self);
void         shell_notification_set_for_feedback     (ShellNotification *self,
                                                      gboolean           for_feedback);
gboolean     shell_notification_get_is_transient     (ShellNotification *self);
void         shell_notification_set_is_transient     (ShellNotification *self,
                                                      gboolean           is_transient);

GList       *shell_notification_get_actions   (ShellNotification *self);
ShellNotificationAction *shell_notification_add_action (ShellNotification           *self,
                                                        const char                  *label,
                                                        ShellNotificationActionFunc  callback,
                                                        gpointer                     user_data,
                                                        GDestroyNotify               destroy);
void         shell_notification_remove_action (ShellNotification       *self,
                                               ShellNotificationAction *action);
void         shell_notification_clear_actions (ShellNotification *self);
void         shell_notification_play_sound    (ShellNotification *self);
void         shell_notification_activate      (ShellNotification *self);
void         shell_notification_destroy       (ShellNotification                *self,
                                               ShellNotificationDestroyedReason  reason);

/* ------------------------------------------------------------------------ */
/* ShellSource                                                               */
/* ------------------------------------------------------------------------ */

/**
 * ShellSourceClass:
 * @parent_class: the parent class
 * @open: opens the application of the source when the user activates one of
 *   its notifications (`open ()`); the default does nothing
 * @destroy: destroys the notifications of the source, emits
 *   #ShellSource::destroy and disposes the source (`destroy (reason)`)
 *
 * A source of notifications: the application, or the part of the shell, that
 * sent them (`MessageTray.Source`). A source destroys itself when its last
 * notification is destroyed.
 */
struct _ShellSourceClass
{
  ShellMessageSourceClass parent_class;

  void (* open)    (ShellSource *self);
  void (* destroy) (ShellSource *self,
                    ShellNotificationDestroyedReason reason);

  /*< private >*/
  gpointer padding[8];
};

ShellNotificationPolicy *shell_source_get_policy  (ShellSource             *self);
void                     shell_source_set_policy  (ShellSource             *self,
                                                   ShellNotificationPolicy *policy);
const char *shell_source_get_owner                (ShellSource *self);
void        shell_source_set_owner                (ShellSource *self,
                                                   const char  *owner);
int         shell_source_get_count                (ShellSource *self);
int         shell_source_get_unseen_count         (ShellSource *self);
gboolean    shell_source_get_count_visible        (ShellSource *self);
ShellNotificationPrivacyScope shell_source_get_narrowest_privacy_scope (ShellSource *self);
GList      *shell_source_get_notifications        (ShellSource *self);
void        shell_source_count_updated            (ShellSource *self);
void        shell_source_add_notification         (ShellSource       *self,
                                                   ShellNotification *notification);
void        shell_source_open                     (ShellSource *self);
void        shell_source_destroy                  (ShellSource                      *self,
                                                   ShellNotificationDestroyedReason  reason);
void        shell_source_destroy_non_resident_notifications (ShellSource *self);

/* ------------------------------------------------------------------------ */
/* ShellMessageTray                                                          */
/* ------------------------------------------------------------------------ */

ShellMessageTray *shell_message_tray_new                 (void);

void              shell_message_tray_add                 (ShellMessageTray *self,
                                                          ShellSource      *source);
gboolean          shell_message_tray_has_source          (ShellMessageTray *self,
                                                          ShellSource      *source);
GList            *shell_message_tray_get_sources         (ShellMessageTray *self);
guint             shell_message_tray_get_queue_count     (ShellMessageTray *self);
ShellSource      *shell_message_tray_get_system_source   (ShellMessageTray *self);

ClutterActorAlign shell_message_tray_get_banner_alignment (ShellMessageTray *self);
void              shell_message_tray_set_banner_alignment (ShellMessageTray *self,
                                                           ClutterActorAlign alignment);
gboolean          shell_message_tray_get_banner_blocked   (ShellMessageTray *self);
void              shell_message_tray_set_banner_blocked   (ShellMessageTray *self,
                                                           gboolean          blocked);
gboolean          shell_message_tray_get_dragging         (ShellMessageTray *self);
void              shell_message_tray_set_dragging         (ShellMessageTray *self,
                                                           gboolean          dragging);
void              shell_message_tray_expand_active_notification (ShellMessageTray *self);

G_END_DECLS
