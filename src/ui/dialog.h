/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * dialog.h: The widgets making up a modal dialog: the dialog frame with its
 * content area and buttons, the title/description message content and the
 * list sections used by the session and mount dialogs. Port of
 * js/ui/dialog.js.
 *
 * ShellDialog and ShellMessageDialogContent are derivable so that extensions
 * can still subclass them through GObject.registerClass().
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

/**
 * ShellDialogButtonCallback:
 * @user_data: (closure): the user data given along with the callback
 *
 * Called when a dialog button is clicked, or when the key bound to it is
 * released while the button is reactive.
 */
typedef void (* ShellDialogButtonCallback) (gpointer user_data);

/* ------------------------------------------------------------------------ */
/* ShellDialog                                                              */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_DIALOG (shell_dialog_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellDialog, shell_dialog, SHELL, DIALOG, StWidget)

struct _ShellDialogClass
{
  StWidgetClass parent_class;

  /*< private >*/
  gpointer padding[6];
};

ShellDialog  *shell_dialog_new                    (ClutterActor *parent_actor,
                                                   const char   *dialog_style_class);

StBoxLayout  *shell_dialog_get_frame              (ShellDialog  *self);
StBoxLayout  *shell_dialog_get_content_layout     (ShellDialog  *self);
StWidget     *shell_dialog_get_button_layout      (ShellDialog  *self);

StButton     *shell_dialog_add_button             (ShellDialog               *self,
                                                   const char                *label,
                                                   uint32_t                   key,
                                                   gboolean                   is_default,
                                                   ShellDialogButtonCallback  action,
                                                   gpointer                   user_data,
                                                   GDestroyNotify             destroy);
void          shell_dialog_clear_buttons          (ShellDialog  *self);
void          shell_dialog_make_inactive          (ShellDialog  *self);

ClutterActor *shell_dialog_get_initial_key_focus  (ShellDialog  *self);
void          shell_dialog_set_initial_key_focus  (ShellDialog  *self,
                                                   ClutterActor *actor);

/* ------------------------------------------------------------------------ */
/* ShellMessageDialogContent                                                */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MESSAGE_DIALOG_CONTENT (shell_message_dialog_content_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellMessageDialogContent, shell_message_dialog_content,
                          SHELL, MESSAGE_DIALOG_CONTENT, StBoxLayout)

struct _ShellMessageDialogContentClass
{
  StBoxLayoutClass parent_class;

  /*< private >*/
  gpointer padding[6];
};

const char *shell_message_dialog_content_get_title        (ShellMessageDialogContent *self);
void        shell_message_dialog_content_set_title        (ShellMessageDialogContent *self,
                                                           const char                *title);
const char *shell_message_dialog_content_get_description  (ShellMessageDialogContent *self);
void        shell_message_dialog_content_set_description  (ShellMessageDialogContent *self,
                                                           const char                *description);

/* ------------------------------------------------------------------------ */
/* ShellListSection                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LIST_SECTION (shell_list_section_get_type ())
G_DECLARE_FINAL_TYPE (ShellListSection, shell_list_section,
                      SHELL, LIST_SECTION, StBoxLayout)

const char       *shell_list_section_get_title (ShellListSection *self);
void              shell_list_section_set_title (ShellListSection *self,
                                                const char       *title);
StBoxLayout      *shell_list_section_get_list  (ShellListSection *self);

/* ------------------------------------------------------------------------ */
/* ShellListSectionItem                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LIST_SECTION_ITEM (shell_list_section_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellListSectionItem, shell_list_section_item,
                      SHELL, LIST_SECTION_ITEM, StBoxLayout)

ClutterActor *shell_list_section_item_get_icon_actor    (ShellListSectionItem *self);
void          shell_list_section_item_set_icon_actor    (ShellListSectionItem *self,
                                                         ClutterActor         *icon_actor);
const char   *shell_list_section_item_get_title         (ShellListSectionItem *self);
void          shell_list_section_item_set_title         (ShellListSectionItem *self,
                                                         const char           *title);
const char   *shell_list_section_item_get_description   (ShellListSectionItem *self);
void          shell_list_section_item_set_description   (ShellListSectionItem *self,
                                                         const char           *description);

G_END_DECLS
