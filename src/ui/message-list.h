/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * message-list.h: The messages of the message tray banners, of the date menu
 * and of the lock screen. Port of js/ui/messageList.js.
 *
 * #ShellMessage is the base class (derivable): a header with the title and
 * the icon of the source, an icon, a title, a body that expands, an optional
 * action area and media controls. It is a #StButton; the JS subclass
 * `MediaMessage` (js/ui/mpris.js) overrides `vfunc_clicked ()` and the
 * virtual function `can_close`. #ShellNotificationMessage is the message of
 * a #ShellNotification, final.
 *
 * #ShellMessageView is the scrollable list of the date menu. Its notification
 * sources come from the #ShellMessageTray in #ShellMessageView:message-tray;
 * the media messages are added by the caller with
 * shell_message_view_add_media_message () because the MPRIS players live in
 * JS (js/ui/mpris.js, which also defines the `MediaMessage` class).
 *
 * The other classes of the JS module (the notification group, the layout
 * managers, the fade effect, the URL highlighter and the time label) are
 * private to message-list.c.
 *
 * `MessageList.Source` is #ShellMessageSource, in message-tray.h.
 */

#pragma once

#include "ui/message-tray.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellMessage                                                              */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MESSAGE (shell_message_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellMessage, shell_message, SHELL, MESSAGE, StButton)

/**
 * ShellMessageClass:
 * @parent_class: the parent class
 * @can_close: whether the user can close the message (`canClose ()`); the
 *   default returns %FALSE
 * @close: class handler of #ShellMessage::close
 *
 * The base class of the messages.
 */
struct _ShellMessageClass
{
  StButtonClass parent_class;

  gboolean (* can_close) (ShellMessage *self);
  void     (* close)     (ShellMessage *self);

  /*< private >*/
  gpointer padding[8];
};

ShellMessageSource *shell_message_get_source        (ShellMessage       *self);
void                shell_message_set_source        (ShellMessage       *self,
                                                     ShellMessageSource *source);
const char         *shell_message_get_title         (ShellMessage *self);
void                shell_message_set_title         (ShellMessage *self,
                                                     const char   *title);
const char         *shell_message_get_body          (ShellMessage *self);
void                shell_message_set_body          (ShellMessage *self,
                                                     const char   *body);
gboolean            shell_message_get_use_body_markup (ShellMessage *self);
void                shell_message_set_use_body_markup (ShellMessage *self,
                                                       gboolean      use_body_markup);
GIcon              *shell_message_get_icon          (ShellMessage *self);
void                shell_message_set_icon          (ShellMessage *self,
                                                     GIcon        *icon);
GDateTime          *shell_message_get_datetime      (ShellMessage *self);
void                shell_message_set_datetime      (ShellMessage *self,
                                                     GDateTime    *datetime);
gboolean            shell_message_get_expanded      (ShellMessage *self);

void                shell_message_set_action_area   (ShellMessage *self,
                                                     ClutterActor *actor);
StButton           *shell_message_add_media_control (ShellMessage *self,
                                                    const char   *icon_name);
void                shell_message_set_expand_button_visible (ShellMessage *self,
                                                             gboolean      visible);
void                shell_message_expand            (ShellMessage *self,
                                                     gboolean      animate);
void                shell_message_unexpand          (ShellMessage *self,
                                                     gboolean      animate);
gboolean            shell_message_can_close         (ShellMessage *self);
void                shell_message_close             (ShellMessage *self);

/* ------------------------------------------------------------------------ */
/* ShellNotificationMessage                                                  */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_NOTIFICATION_MESSAGE (shell_notification_message_get_type ())
G_DECLARE_FINAL_TYPE (ShellNotificationMessage, shell_notification_message,
                      SHELL, NOTIFICATION_MESSAGE, ShellMessage)

ShellNotificationMessage *shell_notification_message_new (ShellNotification *notification);

ShellNotification *shell_notification_message_get_notification (ShellNotificationMessage *self);

/* ------------------------------------------------------------------------ */
/* ShellMessageView                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MESSAGE_VIEW (shell_message_view_get_type ())
G_DECLARE_FINAL_TYPE (ShellMessageView, shell_message_view,
                      SHELL, MESSAGE_VIEW, StViewport)

ShellMessageView *shell_message_view_new (ShellMessageTray *message_tray);

ShellMessageTray *shell_message_view_get_message_tray  (ShellMessageView *self);
gboolean          shell_message_view_get_empty         (ShellMessageView *self);
gboolean          shell_message_view_get_can_clear     (ShellMessageView *self);
ClutterActor     *shell_message_view_get_expanded_group (ShellMessageView *self);

void              shell_message_view_add_media_message    (ShellMessageView *self,
                                                           ShellMessage     *message);
void              shell_message_view_remove_media_message (ShellMessageView *self,
                                                          ShellMessage     *message);
void              shell_message_view_clear                (ShellMessageView *self);
void              shell_message_view_collapse             (ShellMessageView *self);

G_END_DECLS
