/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * unlock-dialog.c: The dialog of the lock screen: the clock, the
 * notifications, the authentication prompt and the buttons of the bottom.
 * Port of js/ui/unlockDialog.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The notifications, the clock, the stack and the layout of the dialog are
 *    private types of this file. The stack (`UnlockStack`) does not derive
 *    from #ShellStack, which is final: it is a #StWidget that lays its
 *    children out the same way.
 *  - The time limits manager (`Main.timeLimitsManager`) is given to the
 *    constructor.
 *  - Switching to the login screen failing is a warning instead of an
 *    exception.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#define GNOME_DESKTOP_USE_UNSTABLE_API

#include <act/act.h>
#include <atk/atk.h>
#include <gdm/gdm-client.h>
#include <gdm/gdm-user-switching.h>
#include <glib/gi18n-lib.h>
#include <clutter/clutter-pango.h>
#include <libgnome-desktop/gnome-wall-clock.h>
#include <math.h>

#include "gdm/auth-menu-button.h"
#include "gdm/auth-prompt.h"
#include "misc/date-utils.h"
#include "misc/text-utils.h"
#include "shell-action-modes.h"
#include "shell-blur-effect.h"
#include "shell-global.h"
#include "shell-stack.h"
#include "shell-util.h"
#include "ui/background.h"
#include "ui/ctrl-alt-tab.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/message-tray.h"
#include "ui/mpris.h"
#include "ui/swipe-tracker.h"
#include "ui/unlock-dialog.h"
#include "ui/window-manager.h"

/* The timeout before going back automatically to the lock screen (in seconds) */
#define IDLE_TIMEOUT (2 * 60)

/* The timeout before showing the unlock hint (in seconds) */
#define HINT_TIMEOUT 4

#define CROSSFADE_TIME 300
#define FADE_OUT_TRANSLATION 200
#define FADE_OUT_SCALE 0.3

#define BLUR_BRIGHTNESS 0.65
#define BLUR_RADIUS 90

#define AUTH_PROMPT_VERTICAL_CENTER_OFFSET 50

/* The name of the section of the menu is translated, so it cannot be a
 * constant */
static const char *
primary_unlock_method_section_name (void)
{
  return _("Unlock Options");
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static ShellEaseParams
ease_params (int                  duration,
             int                  delay,
             ClutterAnimationMode mode)
{
  ShellEaseParams params = {
    .duration = duration,
    .delay = delay,
    .mode = mode,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  return params;
}

static void
set_actor_visible (ClutterActor *actor,
                   gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

static ClutterActor *
get_ui_group (void)
{
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());

  if (layout_manager == NULL)
    return NULL;

  return CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager));
}

static gboolean
use_motion (void)
{
  return st_settings_get_reduced_motion (st_settings_get ()) != ST_REDUCED_MOTION_REDUCE;
}

static int
get_scale_factor (void)
{
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());

  return st_theme_context_get_scale_factor (st_theme_context_get_for_stage (stage));
}

/* `{sectionName: name}`, a floating reference */
static GVariant *
section_criteria (const char *section_name)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "sectionName", g_variant_new_string (section_name));
  return g_variant_builder_end (&builder);
}

/* `{sectionName, ...mechanism}`, a floating reference */
static GVariant *
mechanism_item (const char *section_name,
                GVariant   *mechanism)
{
  GVariantBuilder builder;
  GVariantIter iter;
  const char *key;
  GVariant *value;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "sectionName", g_variant_new_string (section_name));

  g_variant_iter_init (&iter, mechanism);
  while (g_variant_iter_next (&iter, "{&sv}", &key, &value))
    {
      g_autoptr (GVariant) owned = value;

      if (g_str_equal (key, "sectionName"))
        continue;

      g_variant_builder_add (&builder, "{sv}", key, owned);
    }

  return g_variant_builder_end (&builder);
}

/* `text.replace (/\n/g, ' ')` */
static char *
replace_newlines (const char *text)
{
  char *copy = g_strdup (text != NULL ? text : "");
  char *p;

  for (p = copy; *p != '\0'; p++)
    {
      if (*p == '\n')
        *p = ' ';
    }

  return copy;
}

/* ------------------------------------------------------------------------ */
/* ShellUnlockNotificationsBox                                               */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_UNLOCK_NOTIFICATIONS_BOX (shell_unlock_notifications_box_get_type ())
G_DECLARE_FINAL_TYPE (ShellUnlockNotificationsBox, shell_unlock_notifications_box,
                      SHELL, UNLOCK_NOTIFICATIONS_BOX, StBoxLayout)

/* What `_sources` keeps for each source */
typedef struct
{
  ShellUnlockNotificationsBox *box;       /* the box owns the info */
  ShellSource *source;                    /* weak */
  ShellNotificationPolicy *policy;        /* owned */
  gboolean visible;
  gboolean detailed;
  ClutterActor *source_box;
  StLabel *title_label;
  StLabel *count_label;
  gboolean has_critical_notification;
  gulong policy_changed_id;
} NotificationSourceInfo;

/* `NotificationsBox` */
struct _ShellUnlockNotificationsBox
{
  StBoxLayout parent;

  ClutterActor *notification_box;
  ClutterActor *scroll_view;

  GHashTable *players;       /* ShellMprisPlayer (owned) -> ShellMediaMessage (weak) */
  ShellMprisSource *media_source;
  GSettings *settings;
  GHashTable *sources;       /* ShellSource (weak) -> NotificationSourceInfo */
  ShellMessageTray *message_tray;     /* weak */
};

G_DEFINE_FINAL_TYPE (ShellUnlockNotificationsBox, shell_unlock_notifications_box, ST_TYPE_BOX_LAYOUT)

enum
{
  NOTIFICATIONS_WAKE_UP_SCREEN,
  N_NOTIFICATIONS_SIGNALS
};

static guint notifications_signals[N_NOTIFICATIONS_SIGNALS] = { 0 };

static void notification_source_info_free (NotificationSourceInfo *info);

/* `_updateVisibility ()` */
static void
notifications_update_visibility (ShellUnlockNotificationsBox *self)
{
  g_autoptr (GList) children = clutter_actor_get_children (self->notification_box);
  gboolean any_visible = FALSE;
  GList *l;

  for (l = children; l != NULL; l = l->next)
    {
      if (clutter_actor_is_visible (l->data))
        {
          any_visible = TRUE;
          break;
        }
    }

  set_actor_visible (self->notification_box, any_visible);
  set_actor_visible (CLUTTER_ACTOR (self), any_visible);
}

static gboolean
title_to_detailed_text_cb (GBinding     *binding,
                           const GValue *from_value,
                           GValue       *to_value,
                           gpointer      user_data)
{
  g_autofree char *text = replace_newlines (g_value_get_string (from_value));

  g_value_set_string (to_value, text);
  return TRUE;
}

/* `_makeNotificationSource ()` */
static void
make_notification_source (NotificationSourceInfo *info)
{
  ShellSource *source = info->source;
  ClutterActor *box = info->source_box;
  ClutterActor *icon_actor;
  ClutterActor *text_box;
  ClutterActor *title;
  ClutterActor *count_label;
  int count;
  g_autofree char *count_text = NULL;

  icon_actor = g_object_new (ST_TYPE_ICON,
                             "style-class", "unlock-dialog-notification-icon",
                             "fallback-icon-name", "application-x-executable",
                             NULL);
  g_object_bind_property (source, "icon", icon_actor, "gicon", G_BINDING_SYNC_CREATE);
  clutter_actor_add_child (box, icon_actor);

  text_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                           "x-expand", TRUE,
                           "y-expand", TRUE,
                           "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                           NULL);
  clutter_actor_add_child (box, text_box);

  title = g_object_new (ST_TYPE_LABEL,
                        "style-class", "unlock-dialog-notification-label",
                        "x-expand", TRUE,
                        "x-align", CLUTTER_ACTOR_ALIGN_START,
                        NULL);
  g_object_bind_property (source, "title", title, "text", G_BINDING_SYNC_CREATE);
  clutter_actor_add_child (text_box, title);

  count = shell_source_get_unseen_count (source);
  count_text = g_strdup_printf ("%d", count);
  count_label = g_object_new (ST_TYPE_LABEL,
                              "text", count_text,
                              "visible", count > 1,
                              "style-class", "unlock-dialog-notification-count-text",
                              NULL);
  clutter_actor_add_child (text_box, count_label);

  set_actor_visible (box, count != 0);

  info->title_label = ST_LABEL (title);
  info->count_label = ST_LABEL (count_label);
}

/* `_makeNotificationDetailedSource ()` */
static void
make_notification_detailed_source (NotificationSourceInfo *info)
{
  ShellSource *source = info->source;
  ClutterActor *box = info->source_box;
  ClutterActor *icon_actor;
  ClutterActor *text_box;
  ClutterActor *title;
  g_autoptr (GList) notifications = NULL;
  gboolean visible = FALSE;
  GList *l;

  icon_actor = g_object_new (ST_TYPE_ICON,
                             "style-class", "unlock-dialog-notification-icon",
                             "fallback-icon-name", "application-x-executable",
                             NULL);
  g_object_bind_property (source, "icon", icon_actor, "gicon", G_BINDING_SYNC_CREATE);
  clutter_actor_add_child (box, icon_actor);

  text_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                           "orientation", CLUTTER_ORIENTATION_VERTICAL,
                           NULL);
  clutter_actor_add_child (box, text_box);

  title = g_object_new (ST_TYPE_LABEL,
                        "style-class", "unlock-dialog-notification-label",
                        NULL);
  g_object_bind_property_full (source, "title", title, "text",
                               G_BINDING_SYNC_CREATE,
                               title_to_detailed_text_cb, NULL, NULL, NULL);
  clutter_actor_add_child (text_box, title);

  notifications = shell_source_get_notifications (source);
  for (l = notifications; l != NULL; l = l->next)
    {
      ShellNotification *notification = l->data;
      g_autofree char *body = NULL;
      g_autofree char *escaped_title = NULL;
      g_autofree char *markup = NULL;
      ClutterActor *label;

      if (shell_notification_get_acknowledged (notification))
        continue;

      if (shell_notification_get_body (notification) != NULL &&
          shell_notification_get_body (notification)[0] != '\0')
        {
          g_autofree char *body_text = replace_newlines (shell_notification_get_body (notification));

          body = shell_util_fix_markup (body_text,
                                        shell_notification_get_use_body_markup (notification));
        }

      escaped_title = shell_util_fix_markup (shell_notification_get_title (notification) != NULL
                                             ? shell_notification_get_title (notification) : "",
                                             FALSE);

      label = g_object_new (ST_TYPE_LABEL,
                            "style-class", "unlock-dialog-notification-count-text",
                            NULL);
      markup = g_strdup_printf ("<b>%s</b> %s", escaped_title, body != NULL ? body : "");
      clutter_text_set_markup (st_label_get_clutter_text (ST_LABEL (label)), markup);
      clutter_actor_add_child (text_box, label);

      visible = TRUE;
    }

  set_actor_visible (box, visible);

  info->title_label = ST_LABEL (title);
  info->count_label = NULL;
}

/* `_shouldShowDetails ()` */
static gboolean
should_show_details (ShellSource *source)
{
  return shell_notification_policy_get_details_in_lock_screen (shell_source_get_policy (source)) ||
         shell_source_get_narrowest_privacy_scope (source) == SHELL_NOTIFICATION_PRIVACY_SCOPE_SYSTEM;
}

/* `_updateSourceBoxStyle ()` */
static void
update_source_box_style (NotificationSourceInfo *info)
{
  g_autoptr (GList) notifications = shell_source_get_notifications (info->source);
  gboolean has_critical_notification = FALSE;
  GList *l;

  for (l = notifications; l != NULL; l = l->next)
    {
      if (shell_notification_get_urgency (l->data) == SHELL_NOTIFICATION_URGENCY_CRITICAL)
        {
          has_critical_notification = TRUE;
          break;
        }
    }

  if (has_critical_notification != info->has_critical_notification)
    {
      info->has_critical_notification = has_critical_notification;

      if (has_critical_notification)
        st_widget_add_style_class_name (ST_WIDGET (info->source_box), "critical");
      else
        st_widget_remove_style_class_name (ST_WIDGET (info->source_box), "critical");
    }
}

/* `_showSource ()` */
static void
show_source (NotificationSourceInfo *info)
{
  if (info->detailed)
    make_notification_detailed_source (info);
  else
    make_notification_source (info);

  set_actor_visible (info->source_box,
                     info->visible && shell_source_get_unseen_count (info->source) > 0);

  update_source_box_style (info);
}

/* `_wakeUpScreenForSource ()` */
static void
wake_up_screen_for_source (ShellUnlockNotificationsBox *self,
                           NotificationSourceInfo      *info)
{
  if (!g_settings_get_boolean (self->settings, "show-banners"))
    return;

  if (info != NULL && info->source_box != NULL && clutter_actor_is_visible (info->source_box))
    g_signal_emit (self, notifications_signals[NOTIFICATIONS_WAKE_UP_SCREEN], 0);
}

/* `_titleChanged ()` */
static void
source_title_changed_cb (GObject                *object,
                         GParamSpec             *pspec,
                         NotificationSourceInfo *info)
{
  if (info->title_label != NULL)
    st_label_set_text (info->title_label,
                       shell_message_source_get_title (SHELL_MESSAGE_SOURCE (info->source)));
}

/* `_countChanged ()` */
static void
source_count_changed_cb (GObject                *object,
                         GParamSpec             *pspec,
                         NotificationSourceInfo *info)
{
  ShellUnlockNotificationsBox *self = info->box;
  ShellSource *source = info->source;
  gboolean new_detailed = should_show_details (source);
  gboolean old_detailed = info->detailed;

  /* A change in the number of notifications may change whether we show
   * details. */
  info->detailed = new_detailed;

  if (info->detailed || old_detailed != new_detailed)
    {
      /* A new notification was pushed, or a previous notification was destroyed.
       * Give up, and build the list again. */
      clutter_actor_destroy_all_children (info->source_box);
      info->title_label = NULL;
      info->count_label = NULL;
      show_source (info);
    }
  else if (info->count_label != NULL)
    {
      int count = shell_source_get_unseen_count (source);
      g_autofree char *text = g_strdup_printf ("%d", count);

      st_label_set_text (info->count_label, text);
      set_actor_visible (CLUTTER_ACTOR (info->count_label), count > 1);
    }

  set_actor_visible (info->source_box,
                     info->visible && shell_source_get_unseen_count (source) > 0);

  notifications_update_visibility (self);
  wake_up_screen_for_source (self, info);
}

/* `_visibleChanged ()` */
static void
source_visible_changed (NotificationSourceInfo *info)
{
  ShellUnlockNotificationsBox *self = info->box;
  gboolean show_in_lock_screen = shell_notification_policy_get_show_in_lock_screen (info->policy);

  if (info->visible == show_in_lock_screen)
    return;

  info->visible = show_in_lock_screen;
  set_actor_visible (info->source_box,
                     info->visible && shell_source_get_unseen_count (info->source) > 0);

  notifications_update_visibility (self);
  wake_up_screen_for_source (self, info);
}

/* `_detailedChanged ()` */
static void
source_detailed_changed (NotificationSourceInfo *info)
{
  gboolean new_detailed = should_show_details (info->source);

  if (info->detailed == new_detailed)
    return;

  info->detailed = new_detailed;

  clutter_actor_destroy_all_children (info->source_box);
  info->title_label = NULL;
  info->count_label = NULL;
  show_source (info);
}

static void
policy_notify_cb (GObject                *object,
                  GParamSpec             *pspec,
                  NotificationSourceInfo *info)
{
  if (g_str_equal (pspec->name, "show-in-lock-screen"))
    source_visible_changed (info);
  else
    source_detailed_changed (info);
}

static void
source_destroy_cb (ShellSource            *source,
                   NotificationSourceInfo *info)
{
  ShellUnlockNotificationsBox *self = info->box;

  /* Removes the info from the table, which frees it */
  g_hash_table_remove (self->sources, source);
  notifications_update_visibility (self);
}

static void
notification_source_info_free (NotificationSourceInfo *info)
{
  if (info->source != NULL)
    {
      g_signal_handlers_disconnect_by_data (info->source, info);
      g_clear_weak_pointer (&info->source);
    }

  if (info->policy != NULL)
    {
      g_clear_signal_handler (&info->policy_changed_id, info->policy);
      g_clear_object (&info->policy);
    }

  if (info->source_box != NULL)
    {
      ClutterActor *source_box = info->source_box;

      g_clear_weak_pointer (&info->source_box);
      clutter_actor_destroy (source_box);
    }

  g_free (info);
}

/* What the animation of the height of a new source needs */
typedef struct
{
  ShellUnlockNotificationsBox *box;     /* strong */
  ClutterActor *widget;                 /* weak */
} SourceHeightEaseData;

static void
source_height_ease_data_free (SourceHeightEaseData *data)
{
  g_clear_object (&data->box);
  g_clear_weak_pointer (&data->widget);
  g_free (data);
}

static void
source_height_ease_done_cb (gboolean finished,
                            gpointer user_data)
{
  SourceHeightEaseData *data = user_data;

  if (!finished)
    return;

  if (data->box->scroll_view != NULL)
    g_object_set (data->box->scroll_view, "vscrollbar-policy", ST_POLICY_AUTOMATIC, NULL);

  if (data->widget != NULL)
    clutter_actor_set_height (data->widget, -1);
}

/* `_sourceAdded ()` */
static void
notifications_source_added (ShellUnlockNotificationsBox *self,
                            ShellSource                 *source,
                            gboolean                     initial)
{
  NotificationSourceInfo *info = g_new0 (NotificationSourceInfo, 1);
  ShellNotificationPolicy *policy = shell_source_get_policy (source);
  ClutterActor *source_box;

  info->box = self;
  g_set_weak_pointer (&info->source, source);
  info->policy = g_object_ref (policy);
  info->visible = shell_notification_policy_get_show_in_lock_screen (policy);
  info->detailed = should_show_details (source);

  source_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                             "style-class", "unlock-dialog-notification-source",
                             "x-expand", TRUE,
                             NULL);
  g_set_weak_pointer (&info->source_box, source_box);
  show_source (info);
  clutter_actor_insert_child_at_index (self->notification_box, info->source_box,
                                       g_hash_table_size (self->players));

  g_signal_connect (source, "notify::count", G_CALLBACK (source_count_changed_cb), info);
  g_signal_connect (source, "notify::title", G_CALLBACK (source_title_changed_cb), info);
  g_signal_connect (source, "destroy", G_CALLBACK (source_destroy_cb), info);
  info->policy_changed_id = g_signal_connect (info->policy, "notify",
                                              G_CALLBACK (policy_notify_cb), info);

  g_hash_table_insert (self->sources, source, info);

  if (!initial)
    {
      ShellEaseParams params = ease_params (250, -1, CLUTTER_EASE_OUT_QUAD);
      ClutterActor *widget = info->source_box;
      SourceHeightEaseData *data = g_new0 (SourceHeightEaseData, 1);
      float nat_height;

      data->box = g_object_ref (self);
      g_set_weak_pointer (&data->widget, widget);

      /* block scrollbars while animating, if they're not needed now */
      if (clutter_actor_get_height (self->scroll_view) >=
          clutter_actor_get_height (self->notification_box))
        g_object_set (self->scroll_view, "vscrollbar-policy", ST_POLICY_NEVER, NULL);

      clutter_actor_get_preferred_height (widget, -1, NULL, &nat_height);
      clutter_actor_set_height (widget, 0);

      shell_actor_ease (widget, &params,
                        source_height_ease_done_cb, data,
                        (GDestroyNotify) source_height_ease_data_free,
                        "height", (double) nat_height,
                        NULL);

      notifications_update_visibility (self);
      wake_up_screen_for_source (self, info);
    }
}

static void
message_tray_source_added_cb (ShellMessageTray            *tray,
                              ShellSource                 *source,
                              ShellUnlockNotificationsBox *self)
{
  notifications_source_added (self, source, FALSE);
}

/* `_addPlayer ()` */
static void
notifications_add_player (ShellUnlockNotificationsBox *self,
                          ShellMprisPlayer            *player)
{
  ShellMediaMessage *message = shell_media_message_new (player);

  g_hash_table_insert (self->players, g_object_ref (player), message);
  clutter_actor_insert_child_at_index (self->notification_box, CLUTTER_ACTOR (message), 0);
  notifications_update_visibility (self);
}

/* `_removePlayer ()` */
static void
notifications_remove_player (ShellUnlockNotificationsBox *self,
                             ShellMprisPlayer            *player)
{
  ShellMediaMessage *message = g_hash_table_lookup (self->players, player);

  if (message == NULL)
    return;

  g_hash_table_steal (self->players, player);
  clutter_actor_destroy (CLUTTER_ACTOR (message));
  g_object_unref (player);
  notifications_update_visibility (self);
}

static void
media_player_added_cb (ShellMprisSource            *source,
                       ShellMprisPlayer            *player,
                       ShellUnlockNotificationsBox *self)
{
  notifications_add_player (self, player);
}

static void
media_player_removed_cb (ShellMprisSource            *source,
                         ShellMprisPlayer            *player,
                         ShellUnlockNotificationsBox *self)
{
  notifications_remove_player (self, player);
}

static void
shell_unlock_notifications_box_constructed (GObject *object)
{
  ShellUnlockNotificationsBox *self = SHELL_UNLOCK_NOTIFICATIONS_BOX (object);
  g_autoptr (GList) players = NULL;
  g_autoptr (GList) sources = NULL;
  GList *l;

  G_OBJECT_CLASS (shell_unlock_notifications_box_parent_class)->constructed (object);

  self->notification_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                         "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                         "style-class", "unlock-dialog-notifications-container",
                                         NULL);

  self->scroll_view = g_object_new (ST_TYPE_SCROLL_VIEW,
                                    "child", self->notification_box,
                                    NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->scroll_view);

  self->media_source = shell_mpris_source_new ();
  g_signal_connect_object (self->media_source, "player-added",
                           G_CALLBACK (media_player_added_cb), self, 0);
  g_signal_connect_object (self->media_source, "player-removed",
                           G_CALLBACK (media_player_removed_cb), self, 0);
  players = shell_mpris_source_get_players (self->media_source);
  for (l = players; l != NULL; l = l->next)
    notifications_add_player (self, l->data);

  self->settings = g_settings_new ("org.gnome.desktop.notifications");

  self->message_tray = shell_main_get_message_tray (shell_main_get_default ());
  if (self->message_tray != NULL)
    {
      g_object_add_weak_pointer (G_OBJECT (self->message_tray), (gpointer *) &self->message_tray);

      sources = shell_message_tray_get_sources (self->message_tray);
      for (l = sources; l != NULL; l = l->next)
        notifications_source_added (self, l->data, TRUE);
    }
  notifications_update_visibility (self);

  if (self->message_tray != NULL)
    g_signal_connect_object (self->message_tray, "source-added",
                             G_CALLBACK (message_tray_source_added_cb), self, 0);
}

/* `_onDestroy ()` */
static void
shell_unlock_notifications_box_destroy (ClutterActor *actor)
{
  ShellUnlockNotificationsBox *self = SHELL_UNLOCK_NOTIFICATIONS_BOX (actor);
  g_autoptr (GList) players = NULL;
  GList *l;

  if (self->sources != NULL)
    g_hash_table_remove_all (self->sources);

  if (self->players != NULL)
    {
      players = g_hash_table_get_keys (self->players);
      g_list_foreach (players, (GFunc) g_object_ref, NULL);
      for (l = players; l != NULL; l = l->next)
        notifications_remove_player (self, l->data);
      g_list_free_full (g_steal_pointer (&players), g_object_unref);
    }

  CLUTTER_ACTOR_CLASS (shell_unlock_notifications_box_parent_class)->destroy (actor);
}

static void
shell_unlock_notifications_box_dispose (GObject *object)
{
  ShellUnlockNotificationsBox *self = SHELL_UNLOCK_NOTIFICATIONS_BOX (object);

  if (self->message_tray != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->message_tray, self);
      g_object_remove_weak_pointer (G_OBJECT (self->message_tray), (gpointer *) &self->message_tray);
      self->message_tray = NULL;
    }

  if (self->media_source != NULL)
    g_signal_handlers_disconnect_by_data (self->media_source, self);
  g_clear_object (&self->media_source);
  g_clear_object (&self->settings);

  if (self->sources != NULL)
    g_hash_table_remove_all (self->sources);
  if (self->players != NULL)
    g_hash_table_remove_all (self->players);

  self->notification_box = NULL;
  self->scroll_view = NULL;

  G_OBJECT_CLASS (shell_unlock_notifications_box_parent_class)->dispose (object);
}

static void
shell_unlock_notifications_box_finalize (GObject *object)
{
  ShellUnlockNotificationsBox *self = SHELL_UNLOCK_NOTIFICATIONS_BOX (object);

  g_clear_pointer (&self->sources, g_hash_table_unref);
  g_clear_pointer (&self->players, g_hash_table_unref);

  G_OBJECT_CLASS (shell_unlock_notifications_box_parent_class)->finalize (object);
}

static void
shell_unlock_notifications_box_class_init (ShellUnlockNotificationsBoxClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_unlock_notifications_box_constructed;
  object_class->dispose = shell_unlock_notifications_box_dispose;
  object_class->finalize = shell_unlock_notifications_box_finalize;

  actor_class->destroy = shell_unlock_notifications_box_destroy;

  /**
   * ShellUnlockNotificationsBox::wake-up-screen:
   * @self: the #ShellUnlockNotificationsBox
   *
   * Emitted when a notification should wake up the screen.
   */
  notifications_signals[NOTIFICATIONS_WAKE_UP_SCREEN] =
    g_signal_new ("wake-up-screen",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_unlock_notifications_box_init (ShellUnlockNotificationsBox *self)
{
  self->players = g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, NULL);
  self->sources = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                         (GDestroyNotify) notification_source_info_free);
}

static ClutterActor *
shell_unlock_notifications_box_new (void)
{
  return g_object_new (SHELL_TYPE_UNLOCK_NOTIFICATIONS_BOX,
                       "orientation", CLUTTER_ORIENTATION_VERTICAL,
                       "name", "unlockDialogNotifications",
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellUnlockClock                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_UNLOCK_CLOCK (shell_unlock_clock_get_type ())
G_DECLARE_FINAL_TYPE (ShellUnlockClock, shell_unlock_clock,
                      SHELL, UNLOCK_CLOCK, StBoxLayout)

/* `UnlockDialogClock` */
struct _ShellUnlockClock
{
  StBoxLayout parent;

  StLabel *time;
  StLabel *date;
  StLabel *hint;

  GnomeWallClock *wall_clock;
  ClutterSeat *seat;                    /* weak */
  MetaMonitorManager *monitor_manager;  /* weak */
  MetaIdleMonitor *idle_monitor;        /* weak */
  guint idle_watch_id;

  char *auth_hint;
};

G_DEFINE_FINAL_TYPE (ShellUnlockClock, shell_unlock_clock, ST_TYPE_BOX_LAYOUT)

/* `_updateClock ()` */
static void
unlock_clock_update_clock (ShellUnlockClock *self)
{
  g_autofree char *time_text = g_strstrip (g_strdup (gnome_wall_clock_get_clock (self->wall_clock)));
  g_autofree char *date_text = NULL;
  const char *date_format;

  st_label_set_text (self->time, time_text);

  /* Translators: This is a time format for a date in
     long format */
  date_format = shell_util_translate_time_string (N_("%A %B %-d"));
  date_text = shell_date_format_unix_msecs (g_get_real_time () / 1000, date_format);
  st_label_set_text (self->date, date_text);
}

/* `_updateHint ()` */
static void
unlock_clock_update_hint (ShellUnlockClock *self)
{
  const char *text;

  if (self->auth_hint != NULL && self->auth_hint[0] != '\0')
    text = self->auth_hint;
  else if (self->seat != NULL && clutter_seat_get_touch_mode (self->seat))
    text = _("Swipe up");
  else
    text = _("Click or press a key");

  st_label_set_text (self->hint, text);
}

/* `selectAuthHint ()` */
static void
unlock_clock_select_auth_hint (ShellUnlockClock *self,
                               const char       *hint)
{
  g_free (self->auth_hint);
  self->auth_hint = g_strdup (hint);
  unlock_clock_update_hint (self);
}

static void
unlock_clock_clock_notify_cb (GObject          *object,
                              GParamSpec       *pspec,
                              ShellUnlockClock *self)
{
  unlock_clock_update_clock (self);
}

static void
unlock_clock_touch_mode_cb (GObject          *object,
                            GParamSpec       *pspec,
                            ShellUnlockClock *self)
{
  unlock_clock_update_hint (self);
}

static void
unlock_clock_power_save_mode_changed_cb (MetaMonitorManager *manager,
                                         ShellUnlockClock   *self)
{
  clutter_actor_set_opacity (CLUTTER_ACTOR (self->hint), 0);
}

static void
unlock_clock_idle_cb (MetaIdleMonitor *monitor,
                      guint            watch_id,
                      gpointer         user_data)
{
  ShellUnlockClock *self = SHELL_UNLOCK_CLOCK (user_data);
  ShellEaseParams params = ease_params (CROSSFADE_TIME, -1, 0);

  shell_actor_ease (CLUTTER_ACTOR (self->hint), &params, NULL, NULL, NULL,
                    "opacity", 255u,
                    NULL);
}

static void
shell_unlock_clock_constructed (GObject *object)
{
  ShellUnlockClock *self = SHELL_UNLOCK_CLOCK (object);
  ShellGlobal *global = shell_global_get ();
  MetaBackend *backend = shell_global_get_backend (global);
  ClutterBackend *clutter_backend;

  G_OBJECT_CLASS (shell_unlock_clock_parent_class)->constructed (object);

  self->time = g_object_new (ST_TYPE_LABEL,
                             "style-class", "unlock-dialog-clock-time",
                             "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             NULL);
  self->date = g_object_new (ST_TYPE_LABEL,
                             "style-class", "unlock-dialog-clock-date",
                             "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             NULL);
  self->hint = g_object_new (ST_TYPE_LABEL,
                             "style-class", "unlock-dialog-clock-hint",
                             "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             "opacity", 0,
                             NULL);

  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (self->time));
  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (self->date));
  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (self->hint));

  self->wall_clock = gnome_wall_clock_new ();
  g_object_set (self->wall_clock, "time-only", TRUE, NULL);
  g_signal_connect (self->wall_clock, "notify::clock",
                    G_CALLBACK (unlock_clock_clock_notify_cb), self);

  clutter_backend = clutter_context_get_backend (clutter_actor_get_context (CLUTTER_ACTOR (self)));
  g_set_weak_pointer (&self->seat, clutter_backend_get_default_seat (clutter_backend));
  g_signal_connect (self->seat, "notify::touch-mode",
                    G_CALLBACK (unlock_clock_touch_mode_cb), self);

  g_set_weak_pointer (&self->monitor_manager, meta_backend_get_monitor_manager (backend));
  g_signal_connect (self->monitor_manager, "power-save-mode-changed",
                    G_CALLBACK (unlock_clock_power_save_mode_changed_cb), self);

  g_set_weak_pointer (&self->idle_monitor, meta_backend_get_core_idle_monitor (backend));
  self->idle_watch_id = meta_idle_monitor_add_idle_watch (self->idle_monitor,
                                                          HINT_TIMEOUT * 1000,
                                                          unlock_clock_idle_cb, self, NULL);

  unlock_clock_update_clock (self);
  unlock_clock_update_hint (self);
}

/* `_onDestroy ()` */
static void
shell_unlock_clock_destroy (ClutterActor *actor)
{
  ShellUnlockClock *self = SHELL_UNLOCK_CLOCK (actor);

  if (self->wall_clock != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->wall_clock, self);
      g_object_run_dispose (G_OBJECT (self->wall_clock));
    }

  if (self->idle_watch_id != 0 && self->idle_monitor != NULL)
    meta_idle_monitor_remove_watch (self->idle_monitor, self->idle_watch_id);
  self->idle_watch_id = 0;

  CLUTTER_ACTOR_CLASS (shell_unlock_clock_parent_class)->destroy (actor);
}

static void
shell_unlock_clock_dispose (GObject *object)
{
  ShellUnlockClock *self = SHELL_UNLOCK_CLOCK (object);

  if (self->seat != NULL)
    g_signal_handlers_disconnect_by_data (self->seat, self);
  g_clear_weak_pointer (&self->seat);

  if (self->monitor_manager != NULL)
    g_signal_handlers_disconnect_by_data (self->monitor_manager, self);
  g_clear_weak_pointer (&self->monitor_manager);

  if (self->idle_watch_id != 0 && self->idle_monitor != NULL)
    meta_idle_monitor_remove_watch (self->idle_monitor, self->idle_watch_id);
  self->idle_watch_id = 0;
  g_clear_weak_pointer (&self->idle_monitor);

  g_clear_object (&self->wall_clock);

  self->time = NULL;
  self->date = NULL;
  self->hint = NULL;

  G_OBJECT_CLASS (shell_unlock_clock_parent_class)->dispose (object);
}

static void
shell_unlock_clock_finalize (GObject *object)
{
  ShellUnlockClock *self = SHELL_UNLOCK_CLOCK (object);

  g_clear_pointer (&self->auth_hint, g_free);

  G_OBJECT_CLASS (shell_unlock_clock_parent_class)->finalize (object);
}

static void
shell_unlock_clock_class_init (ShellUnlockClockClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_unlock_clock_constructed;
  object_class->dispose = shell_unlock_clock_dispose;
  object_class->finalize = shell_unlock_clock_finalize;

  actor_class->destroy = shell_unlock_clock_destroy;
}

static void
shell_unlock_clock_init (ShellUnlockClock *self)
{
}

static ClutterActor *
shell_unlock_clock_new (void)
{
  return g_object_new (SHELL_TYPE_UNLOCK_CLOCK,
                       "style-class", "unlock-dialog-clock",
                       "orientation", CLUTTER_ORIENTATION_VERTICAL,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellUnlockStack                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_UNLOCK_STACK (shell_unlock_stack_get_type ())
G_DECLARE_FINAL_TYPE (ShellUnlockStack, shell_unlock_stack,
                      SHELL, UNLOCK_STACK, StWidget)

/* `UnlockStack`: draws the prompt and the clock on top of each other, and is
 * as high as the transition between them says */
struct _ShellUnlockStack
{
  StWidget parent;

  ClutterActor *prompt_box;   /* weak */
  ClutterActor *clock;        /* weak */
  double progress;
};

G_DEFINE_FINAL_TYPE (ShellUnlockStack, shell_unlock_stack, ST_TYPE_WIDGET)

static void
unlock_stack_allocate (ClutterActor          *actor,
                       const ClutterActorBox *box)
{
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  ClutterActorBox content_box;
  ClutterActor *child;

  clutter_actor_set_allocation (actor, box);

  st_theme_node_get_content_box (theme_node, box, &content_box);

  for (child = clutter_actor_get_first_child (actor);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      ClutterActorBox child_box = content_box;

      clutter_actor_allocate (child, &child_box);
    }
}

static void
unlock_stack_get_preferred_width (ClutterActor *actor,
                                  float         for_height,
                                  float        *min_width_p,
                                  float        *natural_width_p)
{
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  float min = 0, natural = 0;
  ClutterActor *child;

  st_theme_node_adjust_for_height (theme_node, &for_height);

  for (child = clutter_actor_get_first_child (actor);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      float child_min, child_natural;

      clutter_actor_get_preferred_width (child, for_height, &child_min, &child_natural);
      min = MAX (min, child_min);
      natural = MAX (natural, child_natural);
    }

  if (min_width_p != NULL)
    *min_width_p = min;
  if (natural_width_p != NULL)
    *natural_width_p = natural;

  st_theme_node_adjust_preferred_width (theme_node, min_width_p, natural_width_p);
}

static void
unlock_stack_get_preferred_height (ClutterActor *actor,
                                   float         for_width,
                                   float        *min_height_p,
                                   float        *natural_height_p)
{
  ShellUnlockStack *self = SHELL_UNLOCK_STACK (actor);
  float clock_min_height = 0, clock_nat_height = 0;
  float prompt_min_height = 0, prompt_nat_height = 0;

  if (self->clock != NULL)
    clutter_actor_get_preferred_height (self->clock, for_width,
                                        &clock_min_height, &clock_nat_height);
  if (self->prompt_box != NULL)
    clutter_actor_get_preferred_height (self->prompt_box, for_width,
                                        &prompt_min_height, &prompt_nat_height);

  if (min_height_p != NULL)
    *min_height_p = clock_min_height + (prompt_min_height - clock_min_height) * self->progress;
  if (natural_height_p != NULL)
    *natural_height_p = clock_nat_height + (prompt_nat_height - clock_nat_height) * self->progress;
}

static gboolean
unlock_stack_navigate_focus (StWidget        *widget,
                             ClutterActor    *from,
                             StDirectionType  direction)
{
  ClutterActor *top_actor;

  /* If the stack is itself focusable, then focus into or out of
   * it, as appropriate. */
  if (st_widget_get_can_focus (widget))
    {
      if (from != NULL && clutter_actor_contains (CLUTTER_ACTOR (widget), from))
        return FALSE;

      if (clutter_actor_is_mapped (CLUTTER_ACTOR (widget)))
        {
          clutter_actor_grab_key_focus (CLUTTER_ACTOR (widget));
          return TRUE;
        }

      return FALSE;
    }

  top_actor = clutter_actor_get_last_child (CLUTTER_ACTOR (widget));
  while (top_actor != NULL && !clutter_actor_is_visible (top_actor))
    top_actor = clutter_actor_get_previous_sibling (top_actor);

  if (ST_IS_WIDGET (top_actor))
    return st_widget_navigate_focus (ST_WIDGET (top_actor), from, direction, FALSE);

  return FALSE;
}

static void
shell_unlock_stack_dispose (GObject *object)
{
  ShellUnlockStack *self = SHELL_UNLOCK_STACK (object);

  g_clear_weak_pointer (&self->prompt_box);
  g_clear_weak_pointer (&self->clock);

  G_OBJECT_CLASS (shell_unlock_stack_parent_class)->dispose (object);
}

static void
shell_unlock_stack_class_init (ShellUnlockStackClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);

  object_class->dispose = shell_unlock_stack_dispose;

  actor_class->allocate = unlock_stack_allocate;
  actor_class->get_preferred_width = unlock_stack_get_preferred_width;
  actor_class->get_preferred_height = unlock_stack_get_preferred_height;

  widget_class->navigate_focus = unlock_stack_navigate_focus;
}

static void
shell_unlock_stack_init (ShellUnlockStack *self)
{
}

static ShellUnlockStack *
shell_unlock_stack_new (ClutterActor *prompt_box,
                        ClutterActor *clock)
{
  ShellUnlockStack *self = g_object_new (SHELL_TYPE_UNLOCK_STACK, NULL);

  g_set_weak_pointer (&self->prompt_box, prompt_box);
  g_set_weak_pointer (&self->clock, clock);

  clutter_actor_add_child (CLUTTER_ACTOR (self), prompt_box);
  clutter_actor_add_child (CLUTTER_ACTOR (self), clock);

  return self;
}

static void
shell_unlock_stack_set_progress (ShellUnlockStack *self,
                                 double            progress)
{
  if (self->progress == progress)
    return;

  self->progress = progress;
  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

/* ------------------------------------------------------------------------ */
/* ShellUnlockDialogLayout                                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_UNLOCK_DIALOG_LAYOUT (shell_unlock_dialog_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellUnlockDialogLayout, shell_unlock_dialog_layout,
                      SHELL, UNLOCK_DIALOG_LAYOUT, ClutterLayoutManager)

/* `UnlockDialogLayout` */
struct _ShellUnlockDialogLayout
{
  ClutterLayoutManager parent;

  ShellUnlockStack *stack;                /* weak */
  ClutterActor *notifications;            /* weak */
  ClutterActor *auth_indicator_button;    /* weak */
  ClutterActor *bottom_button_group;      /* weak */
};

G_DEFINE_FINAL_TYPE (ShellUnlockDialogLayout, shell_unlock_dialog_layout,
                     CLUTTER_TYPE_LAYOUT_MANAGER)

static void
unlock_dialog_layout_get_preferred_width (ClutterLayoutManager *manager,
                                          ClutterActor         *container,
                                          float                 for_height,
                                          float                *min_width_p,
                                          float                *natural_width_p)
{
  ShellUnlockDialogLayout *self = SHELL_UNLOCK_DIALOG_LAYOUT (manager);

  if (self->stack == NULL)
    {
      if (min_width_p != NULL)
        *min_width_p = 0;
      if (natural_width_p != NULL)
        *natural_width_p = 0;
      return;
    }

  clutter_actor_get_preferred_width (CLUTTER_ACTOR (self->stack), for_height,
                                     min_width_p, natural_width_p);
}

static void
unlock_dialog_layout_get_preferred_height (ClutterLayoutManager *manager,
                                           ClutterActor         *container,
                                           float                 for_width,
                                           float                *min_height_p,
                                           float                *natural_height_p)
{
  ShellUnlockDialogLayout *self = SHELL_UNLOCK_DIALOG_LAYOUT (manager);

  if (self->stack == NULL)
    {
      if (min_height_p != NULL)
        *min_height_p = 0;
      if (natural_height_p != NULL)
        *natural_height_p = 0;
      return;
    }

  clutter_actor_get_preferred_height (CLUTTER_ACTOR (self->stack), for_width,
                                      min_height_p, natural_height_p);
}

static void
unlock_dialog_layout_allocate (ClutterLayoutManager   *manager,
                               ClutterActor           *container,
                               const ClutterActorBox  *box)
{
  ShellUnlockDialogLayout *self = SHELL_UNLOCK_DIALOG_LAYOUT (manager);
  float width, height;
  float tenth_of_height, center_y;
  float stack_width, stack_height;
  float notifications_width, notifications_height;
  float column_width, column_x1;
  float max_notifications_height;
  float progress, offset_y, stack_y;
  ClutterActorBox actor_box;

  if (self->stack == NULL || self->notifications == NULL)
    return;

  clutter_actor_box_get_size (box, &width, &height);

  tenth_of_height = height / 10.0;
  center_y = height / 2.0;

  clutter_actor_get_preferred_size (CLUTTER_ACTOR (self->stack), NULL, NULL,
                                    &stack_width, &stack_height);
  clutter_actor_get_preferred_size (self->notifications, NULL, NULL,
                                    &notifications_width, &notifications_height);

  column_width = MAX (stack_width, notifications_width);

  column_x1 = floorf ((width - column_width) / 2.0);

  /* Notifications */
  max_notifications_height = MIN (notifications_height,
                                  height - tenth_of_height - stack_height);

  actor_box.x1 = column_x1;
  actor_box.y1 = height - max_notifications_height;
  actor_box.x2 = column_x1 + column_width;
  actor_box.y2 = actor_box.y1 + max_notifications_height;

  clutter_actor_allocate (self->notifications, &actor_box);

  /* Authentication Box
   * Offset applies only to the authPrompt, not the clock, so it fades
   * in/out together with the crossfade between the two. */
  progress = ((ShellUnlockStack *) self->stack)->progress;
  offset_y = AUTH_PROMPT_VERTICAL_CENTER_OFFSET * progress;
  stack_y = MIN (floorf (center_y - stack_height / 2.0) + offset_y,
                 height - stack_height - max_notifications_height);

  actor_box.x1 = column_x1;
  actor_box.y1 = stack_y;
  actor_box.x2 = column_x1 + column_width;
  actor_box.y2 = stack_y + stack_height;

  clutter_actor_allocate (CLUTTER_ACTOR (self->stack), &actor_box);

  /* Auth Indicator button (bottom start) */
  if (self->auth_indicator_button != NULL &&
      clutter_actor_is_visible (self->auth_indicator_button))
    {
      float nat_width, nat_height;

      clutter_actor_get_preferred_size (self->auth_indicator_button, NULL, NULL,
                                        &nat_width, &nat_height);

      if (clutter_actor_get_text_direction (self->auth_indicator_button) ==
          CLUTTER_TEXT_DIRECTION_RTL)
        actor_box.x1 = box->x2 - nat_width;
      else
        actor_box.x1 = box->x1;

      actor_box.y1 = box->y2 - nat_height;
      actor_box.x2 = actor_box.x1 + nat_width;
      actor_box.y2 = actor_box.y1 + nat_height;

      clutter_actor_allocate (self->auth_indicator_button, &actor_box);
    }

  /* bottom button group, (has login options and switch user buttons) (bottom end) */
  if (self->bottom_button_group != NULL &&
      clutter_actor_is_visible (self->bottom_button_group))
    {
      float nat_width, nat_height;

      clutter_actor_get_preferred_size (self->bottom_button_group, NULL, NULL,
                                        &nat_width, &nat_height);

      if (clutter_actor_get_text_direction (self->bottom_button_group) ==
          CLUTTER_TEXT_DIRECTION_RTL)
        actor_box.x1 = box->x1;
      else
        actor_box.x1 = box->x2 - nat_width;

      actor_box.y1 = box->y2 - nat_height;
      actor_box.x2 = actor_box.x1 + nat_width;
      actor_box.y2 = actor_box.y1 + nat_height;

      clutter_actor_allocate (self->bottom_button_group, &actor_box);
    }
}

static void
shell_unlock_dialog_layout_dispose (GObject *object)
{
  ShellUnlockDialogLayout *self = SHELL_UNLOCK_DIALOG_LAYOUT (object);

  g_clear_weak_pointer (&self->stack);
  g_clear_weak_pointer (&self->notifications);
  g_clear_weak_pointer (&self->auth_indicator_button);
  g_clear_weak_pointer (&self->bottom_button_group);

  G_OBJECT_CLASS (shell_unlock_dialog_layout_parent_class)->dispose (object);
}

static void
shell_unlock_dialog_layout_class_init (ShellUnlockDialogLayoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);

  object_class->dispose = shell_unlock_dialog_layout_dispose;

  layout_class->get_preferred_width = unlock_dialog_layout_get_preferred_width;
  layout_class->get_preferred_height = unlock_dialog_layout_get_preferred_height;
  layout_class->allocate = unlock_dialog_layout_allocate;
}

static void
shell_unlock_dialog_layout_init (ShellUnlockDialogLayout *self)
{
}

static ClutterLayoutManager *
shell_unlock_dialog_layout_new (ShellUnlockStack *stack,
                                ClutterActor     *notifications,
                                ClutterActor     *auth_indicator_button,
                                ClutterActor     *bottom_button_group)
{
  ShellUnlockDialogLayout *self = g_object_new (SHELL_TYPE_UNLOCK_DIALOG_LAYOUT, NULL);

  g_set_weak_pointer (&self->stack, stack);
  g_set_weak_pointer (&self->notifications, notifications);
  g_set_weak_pointer (&self->auth_indicator_button, auth_indicator_button);
  g_set_weak_pointer (&self->bottom_button_group, bottom_button_group);

  return CLUTTER_LAYOUT_MANAGER (self);
}

/* ------------------------------------------------------------------------ */
/* ShellUnlockDialog                                                         */
/* ------------------------------------------------------------------------ */

/**
 * ShellUnlockDialog:
 *
 * The dialog of the lock screen (`UnlockDialog`).
 */
struct _ShellUnlockDialog
{
  StWidget parent;

  ShellTimeLimitsManager *time_limits_manager;

  GdmClient *gdm_client;

  StAdjustment *adjustment;
  ShellSwipeTracker *swipe_tracker;
  ClutterAction *key_controller;
  ClutterActor *active_page;

  ClutterActor *background_group;
  GPtrArray *bg_managers;               /* ShellBackgroundManager */

  ActUserManager *user_manager;
  char *user_name;
  ActUser *user;

  ClutterActor *prompt_box;
  ClutterActor *clock;
  ShellUnlockStack *stack;

  gboolean allow_cancel;

  ClutterActor *notifications_box;
  ClutterActor *bottom_button_group;
  StButton *other_user_button;
  ShellAuthMenuButton *auth_menu_button;
  ShellAuthMenuButtonIndicator *auth_indicator_button;

  GSettings *screen_saver_settings;
  GSettings *lockdown_settings;

  MetaIdleMonitor *idle_monitor;        /* weak */
  guint idle_watch_id;

  ShellAuthPrompt *auth_prompt;
  GVariant *selected_auth_mechanism;

  ClutterGrab *grab;
  gboolean is_modal;
};

G_DEFINE_FINAL_TYPE (ShellUnlockDialog, shell_unlock_dialog, ST_TYPE_WIDGET)

enum
{
  PROP_0,
  PROP_TIME_LIMITS_MANAGER,
  PROP_ALLOW_CANCEL,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  FAILED,
  WAKE_UP_SCREEN,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

static void unlock_dialog_show_clock (ShellUnlockDialog *self);
static void unlock_dialog_show_prompt (ShellUnlockDialog *self);
static void unlock_dialog_update_user_switch_visibility (ShellUnlockDialog *self);

/* ---- the transition between the clock and the prompt ---- */

/* `_setTransitionProgress ()` */
static void
set_transition_progress (ShellUnlockDialog *self,
                         double             progress)
{
  gboolean motion = use_motion ();
  int scale_factor = get_scale_factor ();
  double scale = FADE_OUT_SCALE + (1 - FADE_OUT_SCALE) * progress;
  double clock_scale = FADE_OUT_SCALE + (1 - FADE_OUT_SCALE) * (1 - progress);

  shell_unlock_stack_set_progress (self->stack, progress);

  set_actor_visible (self->prompt_box, progress > 0);
  set_actor_visible (self->clock, progress < 1);

  clutter_actor_set_reactive (CLUTTER_ACTOR (self->other_user_button), progress > 0);
  st_widget_set_can_focus (ST_WIDGET (self->other_user_button), progress > 0);

  clutter_actor_set_opacity (CLUTTER_ACTOR (self->auth_indicator_button),
                             CLAMP ((int) (255 * progress), 0, 255));
  clutter_actor_set_scale (CLUTTER_ACTOR (self->auth_indicator_button), scale, scale);
  unlock_dialog_update_user_switch_visibility (self);

  clutter_actor_set_opacity (self->prompt_box, CLAMP ((int) (255 * progress), 0, 255));
  if (motion)
    {
      clutter_actor_set_scale (self->prompt_box, scale, scale);
      clutter_actor_set_translation (self->prompt_box, 0,
                                     FADE_OUT_TRANSLATION * (1 - progress) * scale_factor, 0);
    }

  clutter_actor_set_opacity (self->clock, CLAMP ((int) (255 * (1 - progress)), 0, 255));
  if (motion)
    {
      clutter_actor_set_scale (self->clock, clock_scale, clock_scale);
      clutter_actor_set_translation (self->clock, 0,
                                     -FADE_OUT_TRANSLATION * progress * scale_factor, 0);
    }

  clutter_actor_set_opacity (self->bottom_button_group, CLAMP ((int) (255 * progress), 0, 255));
  if (motion)
    clutter_actor_set_scale (self->bottom_button_group, scale, scale);
}

static void
adjustment_value_notify_cb (GObject           *object,
                            GParamSpec        *pspec,
                            ShellUnlockDialog *self)
{
  set_transition_progress (self, st_adjustment_get_value (self->adjustment));
}

/* `this._adjustment.ease (value, {...})`; the callback only runs if the
 * animation finished */
static void
ease_adjustment (ShellUnlockDialog   *self,
                 double               target,
                 int                  duration,
                 ClutterAnimationMode mode,
                 ShellEaseCallback    callback,
                 gpointer             user_data,
                 GDestroyNotify       destroy)
{
  ShellEaseParams params = ease_params (duration, -1, mode);
  g_auto (GValue) value = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;

  g_value_init (&value, G_TYPE_DOUBLE);
  g_value_set_double (&value, target);

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (self->adjustment), "value",
                                      &value, &params, callback, user_data, destroy, &error))
    g_debug ("%s: %s", G_STRFUNC, error->message);
}

/* ---- the backgrounds ---- */

/* `_updateBackgroundEffects ()` */
static void
update_background_effects (ShellUnlockDialog *self)
{
  int scale_factor = get_scale_factor ();
  ClutterActor *widget;

  for (widget = clutter_actor_get_first_child (self->background_group);
       widget != NULL;
       widget = clutter_actor_get_next_sibling (widget))
    {
      ClutterEffect *effect = clutter_actor_get_effect (widget, "blur");

      if (effect != NULL)
        {
          shell_blur_effect_set_brightness (SHELL_BLUR_EFFECT (effect), BLUR_BRIGHTNESS);
          shell_blur_effect_set_radius (SHELL_BLUR_EFFECT (effect), BLUR_RADIUS * scale_factor);
        }
    }
}

/* `_createBackground ()` */
static void
create_background (ShellUnlockDialog   *self,
                   ShellLayoutManager  *layout_manager,
                   int                  monitor_index)
{
  ShellMonitor *monitor = shell_layout_manager_get_monitor (layout_manager, monitor_index);
  ClutterActor *widget;
  ShellBackgroundManager *bg_manager;

  if (monitor == NULL)
    return;

  widget = g_object_new (ST_TYPE_WIDGET,
                         "style-class", "screen-shield-background",
                         "x", (double) monitor->x,
                         "y", (double) monitor->y,
                         "width", (double) monitor->width,
                         "height", (double) monitor->height,
                         NULL);
  clutter_actor_add_effect_with_name (widget, "blur", CLUTTER_EFFECT (shell_blur_effect_new ()));

  bg_manager = g_object_new (SHELL_TYPE_BACKGROUND_MANAGER,
                             "container", widget,
                             "monitor-index", monitor_index,
                             "control-position", FALSE,
                             NULL);
  g_ptr_array_add (self->bg_managers, bg_manager);

  clutter_actor_add_child (self->background_group, widget);

  shell_monitor_free (monitor);
}

/* `_updateBackgrounds ()` */
static void
update_backgrounds (ShellUnlockDialog *self)
{
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  guint i;
  int n_monitors;

  for (i = 0; i < self->bg_managers->len; i++)
    shell_background_manager_destroy (g_ptr_array_index (self->bg_managers, i));
  g_ptr_array_set_size (self->bg_managers, 0);

  clutter_actor_destroy_all_children (self->background_group);

  if (layout_manager == NULL)
    return;

  n_monitors = shell_layout_manager_get_n_monitors (layout_manager);
  for (i = 0; i < (guint) n_monitors; i++)
    create_background (self, layout_manager, i);
  update_background_effects (self);
}

static void
scale_factor_notify_cb (GObject           *object,
                        GParamSpec        *pspec,
                        ShellUnlockDialog *self)
{
  update_background_effects (self);
}

static void
monitors_changed_cb (ShellLayoutManager *layout_manager,
                     ShellUnlockDialog  *self)
{
  update_backgrounds (self);
}

/* ---- the auth prompt ---- */

/* `_updateAuthBlocked ()` */
static void
update_auth_blocked (ShellUnlockDialog *self)
{
  if (self->auth_prompt != NULL && self->time_limits_manager != NULL)
    shell_auth_prompt_set_auth_blocked (self->auth_prompt,
                                        shell_time_limits_manager_get_should_lock_session (self->time_limits_manager));
}

/* `_fail ()` */
static void
unlock_dialog_fail (ShellUnlockDialog *self)
{
  unlock_dialog_show_clock (self);
  g_signal_emit (self, signals[FAILED], 0);
}

static void
auth_prompt_failed_cb (ShellAuthPrompt   *prompt,
                       ShellUnlockDialog *self)
{
  unlock_dialog_fail (self);
}

/* `_onReset ()` */
static void
auth_prompt_reset_cb (ShellAuthPrompt   *prompt,
                      guint              reset_type,
                      ShellUnlockDialog *self)
{
  const char *user_name;

  if (reset_type != SHELL_AUTH_PROMPT_RESET_DONT_PROVIDE_USERNAME)
    {
      shell_auth_prompt_set_user (self->auth_prompt, self->user);
      user_name = self->user_name;
    }
  else
    {
      user_name = NULL;
    }

  shell_auth_prompt_begin (self->auth_prompt, user_name, NULL);
}

/* `_onLoading ()` */
static void
auth_prompt_loading_cb (ShellAuthPrompt   *prompt,
                        gboolean           is_loading,
                        ShellUnlockDialog *self)
{
  clutter_actor_set_reactive (CLUTTER_ACTOR (self->auth_menu_button), !is_loading);
}

/* `_onMechanismsChanged ()` */
static void
auth_prompt_mechanisms_changed_cb (ShellAuthPrompt   *prompt,
                                   GVariant          *mechanisms,
                                   GVariant          *selected_mechanism,
                                   ShellUnlockDialog *self)
{
  g_autoptr (GVariant) criteria = g_variant_ref_sink (section_criteria (primary_unlock_method_section_name ()));
  gsize n_mechanisms = g_variant_n_children (mechanisms);
  gsize i;

  shell_auth_menu_button_clear_items (self->auth_menu_button, criteria);
  shell_auth_menu_button_clear_items (SHELL_AUTH_MENU_BUTTON (self->auth_indicator_button), NULL);

  if (n_mechanisms == 0)
    return;

  for (i = 0; i < n_mechanisms; i++)
    {
      g_autoptr (GVariant) mechanism = g_variant_get_child_value (mechanisms, i);
      gboolean selectable = FALSE;

      g_variant_lookup (mechanism, "selectable", "b", &selectable);
      if (selectable)
        shell_auth_menu_button_add_item (self->auth_menu_button,
                                         mechanism_item (primary_unlock_method_section_name (),
                                                         mechanism));
      else
        shell_auth_menu_button_add_item (SHELL_AUTH_MENU_BUTTON (self->auth_indicator_button),
                                         mechanism);
    }

  if (g_variant_n_children (selected_mechanism) > 0)
    shell_auth_menu_button_set_active_item (self->auth_menu_button, selected_mechanism);

  shell_auth_menu_button_indicator_update_description_label (self->auth_indicator_button);
}

/* `_ensureAuthPrompt ()` */
static void
ensure_auth_prompt (ShellUnlockDialog *self)
{
  ShellAuthPromptStatus status;

  if (self->auth_prompt == NULL)
    {
      self->auth_prompt = shell_auth_prompt_new (self->gdm_client,
                                                 SHELL_AUTH_PROMPT_MODE_UNLOCK_ONLY);
      g_signal_connect (self->auth_prompt, "failed",
                        G_CALLBACK (auth_prompt_failed_cb), self);
      g_signal_connect (self->auth_prompt, "cancelled",
                        G_CALLBACK (auth_prompt_failed_cb), self);
      g_signal_connect (self->auth_prompt, "reset",
                        G_CALLBACK (auth_prompt_reset_cb), self);
      g_signal_connect (self->auth_prompt, "loading",
                        G_CALLBACK (auth_prompt_loading_cb), self);
      g_signal_connect (self->auth_prompt, "mechanisms-changed",
                        G_CALLBACK (auth_prompt_mechanisms_changed_cb), self);
      clutter_actor_add_child (self->prompt_box, CLUTTER_ACTOR (self->auth_prompt));
    }

  status = shell_auth_prompt_get_verification_status (self->auth_prompt);
  switch (status)
    {
    case SHELL_AUTH_PROMPT_STATUS_NOT_VERIFYING:
    case SHELL_AUTH_PROMPT_STATUS_VERIFICATION_CANCELLED:
    case SHELL_AUTH_PROMPT_STATUS_VERIFICATION_FAILED:
      shell_auth_prompt_reset (self->auth_prompt, FALSE, FALSE);
      shell_auth_prompt_update_sensitivity (self->auth_prompt,
                                            status == SHELL_AUTH_PROMPT_STATUS_NOT_VERIFYING);
      break;

    case SHELL_AUTH_PROMPT_STATUS_VERIFYING:
    case SHELL_AUTH_PROMPT_STATUS_VERIFICATION_SUCCEEDED:
    case SHELL_AUTH_PROMPT_STATUS_VERIFICATION_IN_PROGRESS:
    default:
      break;
    }

  update_auth_blocked (self);
}

/* `_maybeDestroyAuthPrompt ()` */
static void
maybe_destroy_auth_prompt (ShellUnlockDialog *self)
{
  ClutterActor *focus = clutter_stage_get_key_focus (shell_global_get_stage (shell_global_get ()));

  if (focus == NULL ||
      (self->auth_prompt != NULL && clutter_actor_contains (CLUTTER_ACTOR (self->auth_prompt), focus)) ||
      (self->other_user_button != NULL && focus == CLUTTER_ACTOR (self->other_user_button)))
    clutter_actor_grab_key_focus (CLUTTER_ACTOR (self));

  if (self->auth_prompt != NULL)
    {
      clutter_actor_destroy (CLUTTER_ACTOR (self->auth_prompt));
      self->auth_prompt = NULL;
      shell_auth_menu_button_clear_items (SHELL_AUTH_MENU_BUTTON (self->auth_indicator_button), NULL);
    }
}

static void
show_clock_done_cb (gboolean finished,
                    gpointer user_data)
{
  if (finished)
    maybe_destroy_auth_prompt (SHELL_UNLOCK_DIALOG (user_data));
}

/* `_showClock ()` */
static void
unlock_dialog_show_clock (ShellUnlockDialog *self)
{
  if (self->active_page == self->clock)
    return;

  self->active_page = self->clock;

  ease_adjustment (self, 0, CROSSFADE_TIME, CLUTTER_EASE_OUT_QUAD,
                   show_clock_done_cb, g_object_ref (self), g_object_unref);
}

/* `_showPrompt ()` */
static void
unlock_dialog_show_prompt (ShellUnlockDialog *self)
{
  ensure_auth_prompt (self);

  if (self->active_page == self->prompt_box)
    return;

  self->active_page = self->prompt_box;

  ease_adjustment (self, 1, CROSSFADE_TIME, CLUTTER_EASE_OUT_QUAD, NULL, NULL, NULL);
}

/* `_selectAuthMechanism ()` */
static void
select_auth_mechanism (ShellUnlockDialog *self,
                       GVariant          *auth_mechanism)
{
  g_autoptr (GVariant) old_mechanism = self->selected_auth_mechanism;
  g_autofree char *hint = NULL;

  self->selected_auth_mechanism = NULL;

  if (!shell_auth_prompt_select_mechanism (self->auth_prompt, auth_mechanism))
    {
      if (old_mechanism != NULL)
        shell_auth_menu_button_set_active_item (self->auth_menu_button, old_mechanism);
      self->selected_auth_mechanism = g_steal_pointer (&old_mechanism);
      return;
    }

  self->selected_auth_mechanism = g_variant_ref (auth_mechanism);

  g_variant_lookup (auth_mechanism, "hint", "s", &hint);
  unlock_clock_select_auth_hint (SHELL_UNLOCK_CLOCK (self->clock), hint);
}

static void
auth_menu_active_item_changed_cb (ShellAuthMenuButton *button,
                                  const char          *section_name,
                                  ShellUnlockDialog   *self)
{
  g_autoptr (GVariant) auth_mechanism = shell_auth_menu_button_get_active_item (button, NULL);

  if (auth_mechanism == NULL)
    return;

  select_auth_mechanism (self, auth_mechanism);
  shell_auth_menu_button_close_menu (button);
}

/* ---- input ---- */

static gboolean
key_press_cb (ClutterKeyController *controller,
              ShellUnlockDialog    *self)
{
  uint32_t keyval;
  gunichar unichar;

  if (self->active_page == self->prompt_box ||
      (self->prompt_box != NULL && clutter_actor_is_visible (self->prompt_box)))
    return CLUTTER_EVENT_PROPAGATE;

  clutter_key_controller_get_key (controller, &keyval, NULL, &unichar);
  if (keyval == CLUTTER_KEY_Shift_L ||
      keyval == CLUTTER_KEY_Shift_R ||
      keyval == CLUTTER_KEY_Shift_Lock ||
      keyval == CLUTTER_KEY_Caps_Lock)
    return CLUTTER_EVENT_PROPAGATE;

  unlock_dialog_show_prompt (self);

  if (g_unichar_isprint (unichar))
    shell_auth_prompt_start_preemptive_input (self->auth_prompt, unichar);

  return CLUTTER_EVENT_PROPAGATE;
}

static void
discrete_scroll_cb (ClutterScrollController *controller,
                    ClutterSprite           *sprite,
                    ClutterScrollSource      source,
                    double                   dx,
                    double                   dy,
                    ShellUnlockDialog       *self)
{
  if (dy < 0)
    unlock_dialog_show_clock (self);
  else if (dy > 0)
    unlock_dialog_show_prompt (self);
}

static void
click_recognize_cb (ClutterGesture    *gesture,
                    ShellUnlockDialog *self)
{
  unlock_dialog_show_prompt (self);
}

/* `_escape ()` */
static void
idle_escape_cb (MetaIdleMonitor *monitor,
                guint            watch_id,
                gpointer         user_data)
{
  ShellUnlockDialog *self = SHELL_UNLOCK_DIALOG (user_data);

  if (self->auth_prompt != NULL && self->allow_cancel)
    shell_auth_prompt_cancel (self->auth_prompt);
}

/* `_swipeBegin ()` */
static void
swipe_begin_cb (ShellSwipeTracker *tracker,
                guint              monitor,
                ShellUnlockDialog *self)
{
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  static const double snap_points[] = { 0, 1 };
  double progress;

  if (layout_manager == NULL ||
      (int) monitor != shell_layout_manager_get_primary_index (layout_manager))
    return;

  st_adjustment_remove_transition (self->adjustment, "value");

  ensure_auth_prompt (self);

  progress = st_adjustment_get_value (self->adjustment);
  shell_swipe_tracker_confirm_swipe (tracker,
                                     clutter_actor_get_height (CLUTTER_ACTOR (self->stack)),
                                     snap_points, G_N_ELEMENTS (snap_points),
                                     progress, round (progress));
}

/* `_swipeUpdate ()` */
static void
swipe_update_cb (ShellSwipeTracker *tracker,
                 double             progress,
                 ShellUnlockDialog *self)
{
  st_adjustment_set_value (self->adjustment, progress);
}

static void
swipe_end_done_cb (gboolean finished,
                   gpointer user_data)
{
  ShellUnlockDialog *self = SHELL_UNLOCK_DIALOG (user_data);

  if (finished && self->active_page == self->clock)
    maybe_destroy_auth_prompt (self);
}

/* `_swipeEnd ()` */
static void
swipe_end_cb (ShellSwipeTracker *tracker,
              guint64            duration,
              double             end_progress,
              ShellUnlockDialog *self)
{
  self->active_page = end_progress != 0 ? self->prompt_box : self->clock;

  ease_adjustment (self, end_progress, (int) duration, CLUTTER_EASE_OUT_CUBIC,
                   swipe_end_done_cb, g_object_ref (self), g_object_unref);
}

/* ---- the switch user button ---- */

static void
auth_prompt_destroyed_switch_user_cb (ClutterActor *actor,
                                      gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  if (!gdm_goto_login_session_sync (NULL, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

/* `_otherUserClicked ()` */
static void
other_user_clicked_cb (StButton          *button,
                       int                clicked_button,
                       ShellUnlockDialog *self)
{
  if (self->auth_prompt == NULL)
    return;

  g_signal_connect (self->auth_prompt, "destroy",
                    G_CALLBACK (auth_prompt_destroyed_switch_user_cb), NULL);
  shell_auth_prompt_cancel (self->auth_prompt);
}

/* `_updateUserSwitchVisibility ()` */
static void
unlock_dialog_update_user_switch_visibility (ShellUnlockDialog *self)
{
  gboolean has_multiple_users = FALSE;

  if (self->other_user_button == NULL || self->prompt_box == NULL)
    return;

  g_object_get (self->user_manager, "has-multiple-users", &has_multiple_users, NULL);

  set_actor_visible (CLUTTER_ACTOR (self->other_user_button),
                     act_user_manager_can_switch (self->user_manager) &&
                     has_multiple_users &&
                     g_settings_get_boolean (self->screen_saver_settings, "user-switch-enabled") &&
                     !g_settings_get_boolean (self->lockdown_settings, "disable-user-switching") &&
                     clutter_actor_is_visible (self->prompt_box));
}

static void
user_switch_visibility_changed_cb (gpointer instance,
                                   gpointer pspec_or_key,
                                   gpointer user_data)
{
  unlock_dialog_update_user_switch_visibility (SHELL_UNLOCK_DIALOG (user_data));
}

static void
should_lock_session_notify_cb (GObject           *object,
                               GParamSpec        *pspec,
                               ShellUnlockDialog *self)
{
  update_auth_blocked (self);
}

static void
notifications_wake_up_screen_cb (ShellUnlockNotificationsBox *box,
                                 ShellUnlockDialog           *self)
{
  g_signal_emit (self, signals[WAKE_UP_SCREEN], 0);
}

/* ---- the actor ---- */

static void
shell_unlock_dialog_constructed (GObject *object)
{
  ShellUnlockDialog *self = SHELL_UNLOCK_DIALOG (object);
  ShellMain *main_obj = shell_main_get_default ();
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (main_obj);
  ShellWindowManager *window_manager = shell_main_get_window_manager (main_obj);
  ShellGlobal *global = shell_global_get ();
  MetaBackend *backend = shell_global_get_backend (global);
  const char *extensions[3];
  ClutterAction *discrete_scroll;
  ClutterAction *click_gesture;
  StThemeContext *theme_context;
  ClutterActor *main_box;
  ShellMonitorConstraint *constraint;

  G_OBJECT_CLASS (shell_unlock_dialog_parent_class)->constructed (object);

  clutter_actor_set_accessible_role (CLUTTER_ACTOR (self), ATK_ROLE_WINDOW);

  self->bg_managers = g_ptr_array_new_with_free_func (g_object_unref);

  self->gdm_client = gdm_client_new ();
  extensions[0] = gdm_user_verifier_choice_list_interface_info ()->name;
  extensions[1] = gdm_user_verifier_custom_json_interface_info ()->name;
  extensions[2] = NULL;
  gdm_client_set_enabled_extensions (self->gdm_client, extensions);

  self->adjustment = st_adjustment_new (CLUTTER_ACTOR (self), 0, 0, 2, 0, 1, 1);
  g_signal_connect (self->adjustment, "notify::value",
                    G_CALLBACK (adjustment_value_notify_cb), self);

  self->swipe_tracker = shell_swipe_tracker_new (CLUTTER_ACTOR (self),
                                                 CLUTTER_ORIENTATION_VERTICAL,
                                                 SHELL_ACTION_MODE_UNLOCK_SCREEN);
  g_signal_connect (self->swipe_tracker, "begin", G_CALLBACK (swipe_begin_cb), self);
  g_signal_connect (self->swipe_tracker, "update", G_CALLBACK (swipe_update_cb), self);
  g_signal_connect (self->swipe_tracker, "end", G_CALLBACK (swipe_end_cb), self);

  discrete_scroll = clutter_scroll_controller_new (CLUTTER_SCROLL_CONTROLLER_FLAG_DISCRETE |
                                                   CLUTTER_SCROLL_CONTROLLER_FLAG_SCROLL_VERTICAL);
  g_signal_connect (discrete_scroll, "scroll", G_CALLBACK (discrete_scroll_cb), self);
  clutter_actor_add_action (CLUTTER_ACTOR (self), discrete_scroll);

  self->key_controller = clutter_key_controller_new (NULL);
  g_object_ref_sink (self->key_controller);
  g_signal_connect (self->key_controller, "key-press", G_CALLBACK (key_press_cb), self);
  clutter_actor_add_action (CLUTTER_ACTOR (self), self->key_controller);

  self->active_page = NULL;

  click_gesture = clutter_click_gesture_new ();
  g_signal_connect (click_gesture, "recognize", G_CALLBACK (click_recognize_cb), self);
  clutter_actor_add_action (CLUTTER_ACTOR (self), click_gesture);

  /* Background */
  self->background_group = clutter_actor_new ();
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->background_group);

  theme_context = st_theme_context_get_for_stage (shell_global_get_stage (global));
  g_signal_connect_object (theme_context, "notify::scale-factor",
                           G_CALLBACK (scale_factor_notify_cb), self, 0);

  update_backgrounds (self);
  if (layout_manager != NULL)
    g_signal_connect_object (layout_manager, "monitors-changed",
                             G_CALLBACK (monitors_changed_cb), self, 0);

  self->user_manager = g_object_ref (act_user_manager_get_default ());
  self->user_name = g_strdup (g_get_user_name ());
  self->user = g_object_ref (act_user_manager_get_user (self->user_manager, self->user_name));

  /* Authentication & Clock stack */
  self->prompt_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                   "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                   NULL);
  clutter_actor_set_pivot_point (self->prompt_box, 0.5f, 0.5f);
  clutter_actor_hide (self->prompt_box);

  self->clock = shell_unlock_clock_new ();
  clutter_actor_set_pivot_point (self->clock, 0.5f, 0.5f);

  self->stack = shell_unlock_stack_new (self->prompt_box, self->clock);
  unlock_dialog_show_clock (self);

  self->allow_cancel = FALSE;

  if (window_manager != NULL)
    {
      ShellCtrlAltTabManager *ctrl_alt_tab = shell_window_manager_get_ctrl_alt_tab_manager (window_manager);

      if (ctrl_alt_tab != NULL)
        shell_ctrl_alt_tab_manager_add_group (ctrl_alt_tab, CLUTTER_ACTOR (self),
                                              _("Unlock Window"),
                                              "dialog-password-symbolic");
    }

  /* Notifications */
  self->notifications_box = shell_unlock_notifications_box_new ();
  g_signal_connect (self->notifications_box, "wake-up-screen",
                    G_CALLBACK (notifications_wake_up_screen_cb), self);

  self->bottom_button_group = g_object_new (ST_TYPE_BOX_LAYOUT,
                                            "style-class", "login-dialog-bottom-button-group",
                                            NULL);
  clutter_actor_set_pivot_point (self->bottom_button_group, 0.5f, 0.5f);

  /* Switch User button */
  self->other_user_button = g_object_new (ST_TYPE_BUTTON,
                                          "style-class", "login-dialog-button switch-user-button",
                                          "accessible-name", _("Log in as another user"),
                                          "button-mask", ST_BUTTON_PRIMARY | ST_BUTTON_SECONDARY,
                                          "reactive", FALSE,
                                          "x-align", CLUTTER_ACTOR_ALIGN_END,
                                          "y-align", CLUTTER_ACTOR_ALIGN_END,
                                          "label", _("Switch User…"),
                                          NULL);
  g_signal_connect (self->other_user_button, "clicked",
                    G_CALLBACK (other_user_clicked_cb), self);
  clutter_actor_add_child (self->bottom_button_group, CLUTTER_ACTOR (self->other_user_button));

  /* Login Options button */
  self->auth_menu_button = shell_auth_menu_button_new (NULL, FALSE);
  g_object_set (self->auth_menu_button,
                "accessible-name", _("Login Options"),
                "visible", FALSE,
                "y-align", CLUTTER_ACTOR_ALIGN_END,
                NULL);
  g_signal_connect (self->auth_menu_button, "active-item-changed",
                    G_CALLBACK (auth_menu_active_item_changed_cb), self);
  clutter_actor_add_child (self->bottom_button_group, CLUTTER_ACTOR (self->auth_menu_button));

  /* Auth Indicators */
  self->auth_indicator_button = shell_auth_menu_button_indicator_new (TRUE);
  g_object_set (self->auth_indicator_button,
                "accessible-name", _("Background Authentication Methods"),
                "visible", FALSE,
                NULL);
  clutter_actor_set_pivot_point (CLUTTER_ACTOR (self->auth_indicator_button), 0.5f, 0.5f);

  self->screen_saver_settings = g_settings_new ("org.gnome.desktop.screensaver");
  g_signal_connect_object (self->screen_saver_settings, "changed::user-switch-enabled",
                           G_CALLBACK (user_switch_visibility_changed_cb), self, 0);

  self->lockdown_settings = g_settings_new ("org.gnome.desktop.lockdown");
  g_signal_connect_object (self->lockdown_settings, "changed::disable-user-switching",
                           G_CALLBACK (user_switch_visibility_changed_cb), self, 0);

  g_signal_connect_object (self->user, "notify::is-loaded",
                           G_CALLBACK (user_switch_visibility_changed_cb), self, 0);
  g_signal_connect_object (self->user_manager, "notify::has-multiple-users",
                           G_CALLBACK (user_switch_visibility_changed_cb), self, 0);

  unlock_dialog_update_user_switch_visibility (self);

  /* When parental controls session limits are enabled, the screen will be
   * locked upon reaching the time limit. In those cases, tweak the lock screen,
   * so that the children cannot unlock without parental supervision. */
  if (self->time_limits_manager != NULL)
    g_signal_connect_object (self->time_limits_manager, "notify::should-lock-session",
                             G_CALLBACK (should_lock_session_notify_cb), self, 0);
  update_auth_blocked (self);

  /* Main Box */
  main_box = g_object_new (ST_TYPE_WIDGET, NULL);
  constraint = shell_monitor_constraint_new ();
  shell_monitor_constraint_set_primary (constraint, TRUE);
  clutter_actor_add_constraint (main_box, CLUTTER_CONSTRAINT (constraint));
  clutter_actor_add_child (main_box, CLUTTER_ACTOR (self->stack));
  clutter_actor_add_child (main_box, self->notifications_box);
  clutter_actor_add_child (main_box, CLUTTER_ACTOR (self->auth_indicator_button));
  clutter_actor_add_child (main_box, self->bottom_button_group);
  clutter_actor_set_layout_manager (main_box,
                                    shell_unlock_dialog_layout_new (self->stack,
                                                                    self->notifications_box,
                                                                    CLUTTER_ACTOR (self->auth_indicator_button),
                                                                    self->bottom_button_group));
  clutter_actor_add_child (CLUTTER_ACTOR (self), main_box);

  g_set_weak_pointer (&self->idle_monitor, meta_backend_get_core_idle_monitor (backend));
  self->idle_watch_id = meta_idle_monitor_add_idle_watch (self->idle_monitor,
                                                          IDLE_TIMEOUT * 1000,
                                                          idle_escape_cb, self, NULL);
}

/* `_onDestroy ()` */
static void
shell_unlock_dialog_destroy (ClutterActor *actor)
{
  ShellUnlockDialog *self = SHELL_UNLOCK_DIALOG (actor);

  shell_unlock_dialog_pop_modal (self);

  if (self->idle_watch_id != 0 && self->idle_monitor != NULL)
    meta_idle_monitor_remove_watch (self->idle_monitor, self->idle_watch_id);
  self->idle_watch_id = 0;

  g_clear_object (&self->gdm_client);

  if (self->swipe_tracker != NULL)
    {
      shell_swipe_tracker_destroy (self->swipe_tracker);
      g_signal_handlers_disconnect_by_data (self->swipe_tracker, self);
    }
  g_clear_object (&self->swipe_tracker);

  CLUTTER_ACTOR_CLASS (shell_unlock_dialog_parent_class)->destroy (actor);
}

static void
shell_unlock_dialog_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  ShellUnlockDialog *self = SHELL_UNLOCK_DIALOG (object);

  switch (prop_id)
    {
    case PROP_TIME_LIMITS_MANAGER:
      g_set_object (&self->time_limits_manager, g_value_get_object (value));
      break;

    case PROP_ALLOW_CANCEL:
      shell_unlock_dialog_set_allow_cancel (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_unlock_dialog_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellUnlockDialog *self = SHELL_UNLOCK_DIALOG (object);

  switch (prop_id)
    {
    case PROP_TIME_LIMITS_MANAGER:
      g_value_set_object (value, self->time_limits_manager);
      break;

    case PROP_ALLOW_CANCEL:
      g_value_set_boolean (value, self->allow_cancel);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_unlock_dialog_dispose (GObject *object)
{
  ShellUnlockDialog *self = SHELL_UNLOCK_DIALOG (object);

  if (self->idle_watch_id != 0 && self->idle_monitor != NULL)
    meta_idle_monitor_remove_watch (self->idle_monitor, self->idle_watch_id);
  self->idle_watch_id = 0;
  g_clear_weak_pointer (&self->idle_monitor);

  if (self->time_limits_manager != NULL)
    g_signal_handlers_disconnect_by_data (self->time_limits_manager, self);
  g_clear_object (&self->time_limits_manager);

  if (self->adjustment != NULL)
    g_signal_handlers_disconnect_by_data (self->adjustment, self);
  g_clear_object (&self->adjustment);

  if (self->swipe_tracker != NULL)
    g_signal_handlers_disconnect_by_data (self->swipe_tracker, self);
  g_clear_object (&self->swipe_tracker);

  if (self->key_controller != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->key_controller, self);
      g_clear_object (&self->key_controller);
    }

  if (self->bg_managers != NULL)
    g_ptr_array_set_size (self->bg_managers, 0);

  g_clear_object (&self->gdm_client);
  g_clear_object (&self->grab);
  g_clear_object (&self->user);
  g_clear_object (&self->screen_saver_settings);
  g_clear_object (&self->lockdown_settings);
  g_clear_pointer (&self->selected_auth_mechanism, g_variant_unref);

  if (self->user_manager != NULL)
    g_signal_handlers_disconnect_by_data (self->user_manager, self);
  g_clear_object (&self->user_manager);

  self->background_group = NULL;
  self->prompt_box = NULL;
  self->clock = NULL;
  self->stack = NULL;
  self->notifications_box = NULL;
  self->bottom_button_group = NULL;
  self->other_user_button = NULL;
  self->auth_menu_button = NULL;
  self->auth_indicator_button = NULL;
  self->auth_prompt = NULL;
  self->active_page = NULL;

  G_OBJECT_CLASS (shell_unlock_dialog_parent_class)->dispose (object);
}

static void
shell_unlock_dialog_finalize (GObject *object)
{
  ShellUnlockDialog *self = SHELL_UNLOCK_DIALOG (object);

  g_clear_pointer (&self->bg_managers, g_ptr_array_unref);
  g_clear_pointer (&self->user_name, g_free);

  G_OBJECT_CLASS (shell_unlock_dialog_parent_class)->finalize (object);
}

static void
shell_unlock_dialog_class_init (ShellUnlockDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_unlock_dialog_constructed;
  object_class->set_property = shell_unlock_dialog_set_property;
  object_class->get_property = shell_unlock_dialog_get_property;
  object_class->dispose = shell_unlock_dialog_dispose;
  object_class->finalize = shell_unlock_dialog_finalize;

  actor_class->destroy = shell_unlock_dialog_destroy;

  /**
   * ShellUnlockDialog:time-limits-manager:
   *
   * The #ShellTimeLimitsManager (`Main.timeLimitsManager`) whose
   * :should-lock-session blocks the authentication.
   */
  props[PROP_TIME_LIMITS_MANAGER] =
    g_param_spec_object ("time-limits-manager", NULL, NULL,
                         SHELL_TYPE_TIME_LIMITS_MANAGER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellUnlockDialog:allow-cancel:
   *
   * Whether the dialog cancels the authentication when the session is idle
   * (`allowCancel`).
   */
  props[PROP_ALLOW_CANCEL] =
    g_param_spec_boolean ("allow-cancel", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellUnlockDialog::failed:
   * @self: the #ShellUnlockDialog
   *
   * Emitted when the authentication failed or was cancelled and the dialog
   * went back to the clock.
   */
  signals[FAILED] =
    g_signal_new ("failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellUnlockDialog::wake-up-screen:
   * @self: the #ShellUnlockDialog
   *
   * Emitted when a notification should wake up the screen.
   */
  signals[WAKE_UP_SCREEN] =
    g_signal_new ("wake-up-screen",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_unlock_dialog_init (ShellUnlockDialog *self)
{
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

/**
 * shell_unlock_dialog_new:
 * @parent_actor: the actor the dialog is added to
 * @time_limits_manager: (nullable): the manager of the time limits
 *
 * Creates the dialog of the lock screen (`new UnlockDialog (parentActor)`).
 *
 * Returns: (transfer none): the new #ShellUnlockDialog, which belongs to
 *   @parent_actor
 */
ShellUnlockDialog *
shell_unlock_dialog_new (ClutterActor           *parent_actor,
                         ShellTimeLimitsManager *time_limits_manager)
{
  ShellUnlockDialog *self;

  g_return_val_if_fail (CLUTTER_IS_ACTOR (parent_actor), NULL);

  self = g_object_new (SHELL_TYPE_UNLOCK_DIALOG,
                       "style-class", "unlock-dialog",
                       "visible", FALSE,
                       "reactive", TRUE,
                       "time-limits-manager", time_limits_manager,
                       NULL);
  clutter_actor_add_child (parent_actor, CLUTTER_ACTOR (self));

  return self;
}

/**
 * shell_unlock_dialog_get_allow_cancel:
 * @self: a #ShellUnlockDialog
 *
 * Returns: the value of :allow-cancel
 */
gboolean
shell_unlock_dialog_get_allow_cancel (ShellUnlockDialog *self)
{
  g_return_val_if_fail (SHELL_IS_UNLOCK_DIALOG (self), FALSE);

  return self->allow_cancel;
}

/**
 * shell_unlock_dialog_set_allow_cancel:
 * @self: a #ShellUnlockDialog
 * @allow_cancel: whether to cancel the authentication when the session is idle
 *
 * Sets :allow-cancel.
 */
void
shell_unlock_dialog_set_allow_cancel (ShellUnlockDialog *self,
                                      gboolean           allow_cancel)
{
  g_return_if_fail (SHELL_IS_UNLOCK_DIALOG (self));

  if (self->allow_cancel == allow_cancel)
    return;

  self->allow_cancel = allow_cancel;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ALLOW_CANCEL]);
}

/**
 * shell_unlock_dialog_cancel:
 * @self: a #ShellUnlockDialog
 *
 * Cancels the authentication, if there is one (`cancel ()`).
 */
void
shell_unlock_dialog_cancel (ShellUnlockDialog *self)
{
  g_return_if_fail (SHELL_IS_UNLOCK_DIALOG (self));

  if (self->auth_prompt != NULL)
    shell_auth_prompt_cancel (self->auth_prompt);
}

/**
 * shell_unlock_dialog_finish:
 * @self: a #ShellUnlockDialog
 * @func: called when the dialog is done with the messages of the verifier
 * @user_data: (closure func): data for @func
 * @destroy: (nullable): called on @user_data after @func
 *
 * Waits for the messages of the verifier to be shown, if there is an
 * authentication, and calls @func (`finish (onComplete)`).
 */
void
shell_unlock_dialog_finish (ShellUnlockDialog          *self,
                            ShellAuthPromptFinishFunc   func,
                            gpointer                    user_data,
                            GDestroyNotify              destroy)
{
  g_return_if_fail (SHELL_IS_UNLOCK_DIALOG (self));
  g_return_if_fail (func != NULL);

  if (self->auth_prompt == NULL)
    {
      func (user_data);
      if (destroy != NULL)
        destroy (user_data);
      return;
    }

  shell_auth_prompt_finish (self->auth_prompt, func, user_data, destroy);
}

/**
 * shell_unlock_dialog_open:
 * @self: a #ShellUnlockDialog
 *
 * Shows the dialog and grabs the input (`open ()`).
 *
 * Returns: %TRUE
 */
gboolean
shell_unlock_dialog_open (ShellUnlockDialog *self)
{
  ShellMain *main_obj = shell_main_get_default ();
  ClutterActor *ui_group = get_ui_group ();

  g_return_val_if_fail (SHELL_IS_UNLOCK_DIALOG (self), FALSE);

  clutter_actor_show (CLUTTER_ACTOR (self));

  if (self->is_modal)
    return TRUE;

  g_clear_object (&self->grab);
  self->grab = shell_main_push_modal (main_obj, ui_group, SHELL_ACTION_MODE_UNLOCK_SCREEN);
  if (self->grab != NULL)
    g_object_ref (self->grab);
  self->is_modal = TRUE;

  return TRUE;
}

/**
 * shell_unlock_dialog_activate:
 * @self: a #ShellUnlockDialog
 *
 * Shows the prompt instead of the clock (`activate ()`).
 */
void
shell_unlock_dialog_activate (ShellUnlockDialog *self)
{
  g_return_if_fail (SHELL_IS_UNLOCK_DIALOG (self));

  unlock_dialog_show_prompt (self);
}

/**
 * shell_unlock_dialog_pop_modal:
 * @self: a #ShellUnlockDialog
 *
 * Drops the grab of the input (`popModal ()`).
 */
void
shell_unlock_dialog_pop_modal (ShellUnlockDialog *self)
{
  g_return_if_fail (SHELL_IS_UNLOCK_DIALOG (self));

  if (self->is_modal)
    {
      g_autoptr (GError) error = NULL;

      if (self->grab != NULL)
        shell_main_pop_modal (shell_main_get_default (), self->grab, &error);
      g_clear_object (&self->grab);
      self->is_modal = FALSE;
    }
}
