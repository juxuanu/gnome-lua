/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * window-attention-handler.c: Shows a notification when a window asks for
 * attention. Port of js/ui/windowAttentionHandler.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The message tray is the construct-only property
 *    #ShellWindowAttentionHandler:message-tray (`Main.messageTray`).
 *  - `Main.activateWindow ()` is done here (switching to the workspace of the
 *    window if needed); the rest of it, hiding the overview and closing the
 *    calendar, is up to the handler of
 *    #ShellWindowAttentionHandler::window-activated, which ShellMain connects
 *    (as it does for #ShellCtrlAltTabManager::window-activated).
 *  - A window without a title gives the body `“” is ready` (the JS
 *    formatted the string "null").
 *  - A window that no application is tracking is ignored (the JS threw).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>
#include <meta/display.h>
#include <meta/meta-workspace-manager.h>
#include <meta/window.h>
#include <meta/workspace.h>

#include "shell-app.h"
#include "shell-global.h"
#include "shell-window-tracker.h"
#include "ui/message-tray.h"
#include "ui/window-attention-handler.h"

/* ------------------------------------------------------------------------ */
/* ShellWindowAttentionSource                                                */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WINDOW_ATTENTION_SOURCE (shell_window_attention_source_get_type ())
G_DECLARE_FINAL_TYPE (ShellWindowAttentionSource, shell_window_attention_source,
                      SHELL, WINDOW_ATTENTION_SOURCE, ShellSource)

enum
{
  SOURCE_PROP_0,

  SOURCE_PROP_APP,
  SOURCE_PROP_WINDOW,

  SOURCE_N_PROPS
};

static GParamSpec *source_props[SOURCE_N_PROPS] = { NULL, };

enum
{
  SOURCE_WINDOW_ACTIVATED,

  SOURCE_LAST_SIGNAL
};

static guint source_signals[SOURCE_LAST_SIGNAL] = { 0, };

struct _ShellWindowAttentionSource
{
  ShellSource parent;

  ShellApp *app;
  MetaWindow *window;
  ShellNotification *notification;        /* weak */
};

G_DEFINE_FINAL_TYPE (ShellWindowAttentionSource, shell_window_attention_source,
                     SHELL_TYPE_SOURCE)

static void
get_title_and_banner (ShellApp   *app,
                      MetaWindow *window,
                      char      **title,
                      char      **banner)
{
  const char *window_title = meta_window_get_title (window);

  *title = g_strdup (shell_app_get_name (app));
  *banner = g_strdup_printf (_("“%s” is ready"), window_title != NULL ? window_title : "");
}

static void
source_sync (ShellWindowAttentionSource *self)
{
  gboolean demands_attention = FALSE;
  gboolean urgent = FALSE;

  g_object_get (self->window,
                "demands-attention", &demands_attention,
                "urgent", &urgent,
                NULL);

  if (demands_attention || urgent)
    return;

  shell_source_destroy (SHELL_SOURCE (self), SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);
}

static void
source_window_gone_cb (ShellWindowAttentionSource *self)
{
  shell_source_destroy (SHELL_SOURCE (self), SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);
}

static void
source_window_title_notify_cb (MetaWindow                 *window,
                               GParamSpec                 *pspec,
                               ShellWindowAttentionSource *self)
{
  g_autofree char *title = NULL;
  g_autofree char *body = NULL;

  if (self->notification == NULL)
    return;

  get_title_and_banner (self->app, window, &title, &body);
  shell_notification_set_title (self->notification, title);
  shell_notification_set_body (self->notification, body);
}

static void
source_on_shutdown (ShellGlobal                *global,
                    ShellWindowAttentionSource *self)
{
  /* The windows are unmanaged after the shell was torn down */
  if (self->window != NULL)
    g_signal_handlers_disconnect_by_data (self->window, self);
}

static void
source_open (ShellSource *source)
{
  ShellWindowAttentionSource *self = SHELL_WINDOW_ATTENTION_SOURCE (source);
  ShellGlobal *global = shell_global_get ();
  MetaWorkspaceManager *workspace_manager = shell_global_get_workspace_manager (global);
  MetaWorkspace *workspace = meta_window_get_workspace (self->window);
  guint32 time = shell_global_get_current_time (global);

  /* Main.activateWindow () */
  if (workspace != NULL &&
      workspace != meta_workspace_manager_get_active_workspace (workspace_manager))
    meta_workspace_activate_with_focus (workspace, self->window, time);
  else
    meta_window_activate (self->window, time);

  g_signal_emit (self, source_signals[SOURCE_WINDOW_ACTIVATED], 0, self->window, time);
}

static void
source_destroy (ShellSource                      *source,
                ShellNotificationDestroyedReason  reason)
{
  ShellWindowAttentionSource *self = SHELL_WINDOW_ATTENTION_SOURCE (source);
  ShellSourceClass *parent_class = SHELL_SOURCE_CLASS (shell_window_attention_source_parent_class);

  if (self->window != NULL)
    g_signal_handlers_disconnect_by_data (self->window, self);

  parent_class->destroy (source, reason);
}

static void
shell_window_attention_source_get_property (GObject    *object,
                                            guint       prop_id,
                                            GValue     *value,
                                            GParamSpec *pspec)
{
  ShellWindowAttentionSource *self = SHELL_WINDOW_ATTENTION_SOURCE (object);

  switch (prop_id)
    {
    case SOURCE_PROP_APP:
      g_value_set_object (value, self->app);
      break;

    case SOURCE_PROP_WINDOW:
      g_value_set_object (value, self->window);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_window_attention_source_set_property (GObject      *object,
                                            guint         prop_id,
                                            const GValue *value,
                                            GParamSpec   *pspec)
{
  ShellWindowAttentionSource *self = SHELL_WINDOW_ATTENTION_SOURCE (object);

  switch (prop_id)
    {
    case SOURCE_PROP_APP:
      g_set_object (&self->app, g_value_get_object (value));
      break;

    case SOURCE_PROP_WINDOW:
      g_set_object (&self->window, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_window_attention_source_constructed (GObject *object)
{
  ShellWindowAttentionSource *self = SHELL_WINDOW_ATTENTION_SOURCE (object);
  ShellMessageSource *message_source = SHELL_MESSAGE_SOURCE (self);

  G_OBJECT_CLASS (shell_window_attention_source_parent_class)->constructed (object);

  shell_message_source_set_title (message_source, shell_app_get_name (self->app));
  shell_message_source_set_icon (message_source, shell_app_get_icon (self->app));

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (source_on_shutdown), self, 0);

  g_signal_connect_swapped (self->window, "notify::demands-attention",
                            G_CALLBACK (source_sync), self);
  g_signal_connect_swapped (self->window, "notify::urgent",
                            G_CALLBACK (source_sync), self);
  g_signal_connect_swapped (self->window, "focus",
                            G_CALLBACK (source_window_gone_cb), self);
  g_signal_connect_swapped (self->window, "unmanaged",
                            G_CALLBACK (source_window_gone_cb), self);
  g_signal_connect (self->window, "notify::title",
                    G_CALLBACK (source_window_title_notify_cb), self);
}

static void
shell_window_attention_source_dispose (GObject *object)
{
  ShellWindowAttentionSource *self = SHELL_WINDOW_ATTENTION_SOURCE (object);

  if (self->window != NULL)
    g_signal_handlers_disconnect_by_data (self->window, self);

  g_clear_weak_pointer (&self->notification);
  g_clear_object (&self->window);
  g_clear_object (&self->app);

  G_OBJECT_CLASS (shell_window_attention_source_parent_class)->dispose (object);
}

static void
shell_window_attention_source_class_init (ShellWindowAttentionSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellSourceClass *source_class = SHELL_SOURCE_CLASS (klass);

  object_class->get_property = shell_window_attention_source_get_property;
  object_class->set_property = shell_window_attention_source_set_property;
  object_class->constructed = shell_window_attention_source_constructed;
  object_class->dispose = shell_window_attention_source_dispose;

  source_class->open = source_open;
  source_class->destroy = source_destroy;

  source_props[SOURCE_PROP_APP] =
    g_param_spec_object ("app", NULL, NULL,
                         SHELL_TYPE_APP,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  source_props[SOURCE_PROP_WINDOW] =
    g_param_spec_object ("window", NULL, NULL,
                         META_TYPE_WINDOW,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, SOURCE_N_PROPS, source_props);

  source_signals[SOURCE_WINDOW_ACTIVATED] =
    g_signal_new ("window-activated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  META_TYPE_WINDOW,
                  G_TYPE_UINT);
}

static void
shell_window_attention_source_init (ShellWindowAttentionSource *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellWindowAttentionHandler                                               */
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
  WINDOW_ACTIVATED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellWindowAttentionHandler
{
  GObject parent;

  ShellMessageTray *message_tray;           /* weak */
  MetaDisplay *display;                     /* weak */
};

G_DEFINE_FINAL_TYPE (ShellWindowAttentionHandler, shell_window_attention_handler, G_TYPE_OBJECT)

static void
source_window_activated_cb (ShellWindowAttentionSource  *source,
                            MetaWindow                  *window,
                            guint                        time,
                            ShellWindowAttentionHandler *self)
{
  g_signal_emit (self, signals[WINDOW_ACTIVATED], 0, window, time);
}

static void
notification_activated_cb (ShellNotification *notification,
                           ShellSource       *source)
{
  shell_source_open (source);
}

static void
on_window_demands_attention (MetaDisplay                 *display,
                             MetaWindow                  *window,
                             ShellWindowAttentionHandler *self)
{
  g_autoptr (ShellWindowAttentionSource) source = NULL;
  g_autoptr (ShellNotification) notification = NULL;
  g_autofree char *title = NULL;
  g_autofree char *body = NULL;
  g_autoptr (ShellNotificationPolicy) policy = NULL;
  ShellApp *app;

  /* We don't want to show the notification when the window is already
   * focused, because this is rather pointless.
   * Some apps (like GIMP) do things like setting the urgency hint on the
   * toolbar windows which would result into a notification even though GIMP
   * itself is focused.
   * We are just ignoring the hint on skip_taskbar windows for now.
   * (Which is the same behaviour as with metacity + panel) */
  if (window == NULL || meta_window_has_focus (window) ||
      meta_window_is_skip_taskbar (window))
    return;

  if (self->message_tray == NULL)
    return;

  app = shell_window_tracker_get_window_app (shell_window_tracker_get_default (), window);
  if (app == NULL)
    return;

  policy = shell_notification_policy_new_for_app (app);
  source = g_object_new (SHELL_TYPE_WINDOW_ATTENTION_SOURCE,
                         "policy", policy,
                         "app", app,
                         "window", window,
                         NULL);
  g_signal_connect_object (source, "window-activated",
                           G_CALLBACK (source_window_activated_cb), self, 0);
  shell_message_tray_add (self->message_tray, SHELL_SOURCE (source));

  get_title_and_banner (app, window, &title, &body);

  notification = shell_notification_new (SHELL_SOURCE (source), title, body);
  shell_notification_set_for_feedback (notification, TRUE);
  g_signal_connect_object (notification, "activated",
                           G_CALLBACK (notification_activated_cb), source, 0);

  g_set_weak_pointer (&source->notification, notification);
  shell_source_add_notification (SHELL_SOURCE (source), notification);
}

static void
shell_window_attention_handler_get_property (GObject    *object,
                                             guint       prop_id,
                                             GValue     *value,
                                             GParamSpec *pspec)
{
  ShellWindowAttentionHandler *self = SHELL_WINDOW_ATTENTION_HANDLER (object);

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
shell_window_attention_handler_set_property (GObject      *object,
                                             guint         prop_id,
                                             const GValue *value,
                                             GParamSpec   *pspec)
{
  ShellWindowAttentionHandler *self = SHELL_WINDOW_ATTENTION_HANDLER (object);

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
shell_window_attention_handler_constructed (GObject *object)
{
  ShellWindowAttentionHandler *self = SHELL_WINDOW_ATTENTION_HANDLER (object);

  G_OBJECT_CLASS (shell_window_attention_handler_parent_class)->constructed (object);

  g_set_weak_pointer (&self->display, shell_global_get_display (shell_global_get ()));
  g_signal_connect (self->display, "window-demands-attention",
                    G_CALLBACK (on_window_demands_attention), self);
  g_signal_connect (self->display, "window-marked-urgent",
                    G_CALLBACK (on_window_demands_attention), self);
}

static void
shell_window_attention_handler_dispose (GObject *object)
{
  ShellWindowAttentionHandler *self = SHELL_WINDOW_ATTENTION_HANDLER (object);

  if (self->display != NULL)
    g_signal_handlers_disconnect_by_data (self->display, self);

  g_clear_weak_pointer (&self->display);
  g_clear_weak_pointer (&self->message_tray);

  G_OBJECT_CLASS (shell_window_attention_handler_parent_class)->dispose (object);
}

static void
shell_window_attention_handler_class_init (ShellWindowAttentionHandlerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_window_attention_handler_get_property;
  object_class->set_property = shell_window_attention_handler_set_property;
  object_class->constructed = shell_window_attention_handler_constructed;
  object_class->dispose = shell_window_attention_handler_dispose;

  /**
   * ShellWindowAttentionHandler:message-tray:
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
   * ShellWindowAttentionHandler::window-activated:
   * @self: the handler
   * @window: the window that was activated
   * @timestamp: the time used to activate the window
   *
   * Emitted after the user activated the notification of a window and the
   * handler activated the window (switching to its workspace if needed), to do
   * the rest of `Main.activateWindow ()`: hide the overview and close the
   * calendar.
   */
  signals[WINDOW_ACTIVATED] =
    g_signal_new ("window-activated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  META_TYPE_WINDOW,
                  G_TYPE_UINT);
}

static void
shell_window_attention_handler_init (ShellWindowAttentionHandler *self)
{
}

/**
 * shell_window_attention_handler_new:
 * @message_tray: the message tray to add the notification sources to
 *
 * Starts to follow the windows that ask for attention
 * (`new WindowAttentionHandler ()`); ShellMain creates the only instance.
 *
 * Returns: (transfer full): a new #ShellWindowAttentionHandler
 */
ShellWindowAttentionHandler *
shell_window_attention_handler_new (ShellMessageTray *message_tray)
{
  g_return_val_if_fail (SHELL_IS_MESSAGE_TRAY (message_tray), NULL);

  return g_object_new (SHELL_TYPE_WINDOW_ATTENTION_HANDLER,
                       "message-tray", message_tray,
                       NULL);
}
