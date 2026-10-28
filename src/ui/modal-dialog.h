/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * modal-dialog.h: A dialog that grabs the input of the whole shell while it
 * is open, dims the monitor it is on behind a lightbox and fades in and out.
 * Port of js/ui/modalDialog.js.
 *
 * #ShellModalDialog is derivable: the end session, run, access, polkit,
 * keyring, network agent, mount, extension, location, display change, welcome,
 * audio device, ... dialogs still are JS classes registered with
 * GObject.registerClass() that extend it. The overridable methods of
 * ModalDialog (`open()` and `close()`) are virtual functions here, which JS
 * subclasses override with `vfunc_open ()` and `vfunc_close ()` and chain up
 * to with `super.vfunc_open ()` and `super.vfunc_close ()`.
 */

#pragma once

#include "st.h"

#include "shell-action-modes.h"
#include "ui/dialog.h"

G_BEGIN_DECLS

/**
 * ShellModalDialogState:
 * @SHELL_MODAL_DIALOG_STATE_OPENED: the dialog is fully visible and modal
 * @SHELL_MODAL_DIALOG_STATE_CLOSED: the dialog is hidden
 * @SHELL_MODAL_DIALOG_STATE_OPENING: the dialog is fading in
 * @SHELL_MODAL_DIALOG_STATE_CLOSING: the dialog is fading out
 * @SHELL_MODAL_DIALOG_STATE_FADED_OUT: the dialog was dismissed with
 *   shell_modal_dialog_fade_out_dialog (): it is gone but the lightbox stays
 *
 * The state of a #ShellModalDialog. Same values as the former
 * `ModalDialog.State` of modalDialog.js.
 */
typedef enum
{
  SHELL_MODAL_DIALOG_STATE_OPENED = 0,
  SHELL_MODAL_DIALOG_STATE_CLOSED = 1,
  SHELL_MODAL_DIALOG_STATE_OPENING = 2,
  SHELL_MODAL_DIALOG_STATE_CLOSING = 3,
  SHELL_MODAL_DIALOG_STATE_FADED_OUT = 4,
} ShellModalDialogState;

#define SHELL_TYPE_MODAL_DIALOG (shell_modal_dialog_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellModalDialog, shell_modal_dialog,
                          SHELL, MODAL_DIALOG, StWidget)

/**
 * ShellModalDialogClass:
 * @parent_class: the parent class
 * @open: opens the dialog (`ModalDialog.open ()`). Returns whether the dialog
 *   is open or opening afterwards
 * @close: closes the dialog (`ModalDialog.close ()`)
 */
struct _ShellModalDialogClass
{
  StWidgetClass parent_class;

  gboolean (* open)  (ShellModalDialog *self);
  void     (* close) (ShellModalDialog *self);

  /*< private >*/
  gpointer padding[8];
};

void          shell_modal_dialog_set_default_container (ClutterActor *container);

ShellModalDialogState shell_modal_dialog_get_state      (ShellModalDialog *self);

ShellDialog  *shell_modal_dialog_get_dialog_layout      (ShellModalDialog *self);
StBoxLayout  *shell_modal_dialog_get_content_layout     (ShellModalDialog *self);
StWidget     *shell_modal_dialog_get_button_layout      (ShellModalDialog *self);
StWidget     *shell_modal_dialog_get_background_stack   (ShellModalDialog *self);

StButton     *shell_modal_dialog_add_button             (ShellModalDialog          *self,
                                                         const char                *label,
                                                         uint32_t                   key,
                                                         gboolean                   is_default,
                                                         ShellDialogButtonCallback  action,
                                                         gpointer                   user_data,
                                                         GDestroyNotify             destroy);
void          shell_modal_dialog_clear_buttons          (ShellModalDialog *self);

void          shell_modal_dialog_set_initial_key_focus  (ShellModalDialog *self,
                                                         ClutterActor     *actor);

gboolean      shell_modal_dialog_open                   (ShellModalDialog *self);
void          shell_modal_dialog_close                  (ShellModalDialog *self);
void          shell_modal_dialog_pop_modal              (ShellModalDialog *self);
gboolean      shell_modal_dialog_push_modal             (ShellModalDialog *self);
void          shell_modal_dialog_fade_out_dialog        (ShellModalDialog *self);

G_END_DECLS
