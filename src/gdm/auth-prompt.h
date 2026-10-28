/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * auth-prompt.h: The widget of the login screen and of the lock screen which
 * asks the user to authenticate, on top of #ShellGdmUserVerifier. Port of
 * js/gdm/authPrompt.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <act/act.h>
#include <gdm/gdm-client.h>

#include "gdm/batch.h"
#include "gdm/user-verifier.h"
#include "st.h"

G_BEGIN_DECLS

#define SHELL_AUTH_PROMPT_DEFAULT_BUTTON_WELL_ICON_SIZE 16
#define SHELL_AUTH_PROMPT_DEFAULT_BUTTON_WELL_ANIMATION_TIME 300
#define SHELL_AUTH_PROMPT_MESSAGE_FADE_OUT_ANIMATION_TIME 500

/**
 * ShellAuthPromptMode:
 * @SHELL_AUTH_PROMPT_MODE_UNLOCK_ONLY: the prompt of the lock screen: the
 *   user is the one of the session
 * @SHELL_AUTH_PROMPT_MODE_UNLOCK_OR_LOG_IN: the prompt of the login screen
 *
 * What the prompt is for (`AuthPromptMode`).
 */
typedef enum
{
  SHELL_AUTH_PROMPT_MODE_UNLOCK_ONLY = 0,
  SHELL_AUTH_PROMPT_MODE_UNLOCK_OR_LOG_IN = 1,
} ShellAuthPromptMode;

/**
 * ShellAuthPromptStatus:
 * @SHELL_AUTH_PROMPT_STATUS_NOT_VERIFYING: nothing is going on
 * @SHELL_AUTH_PROMPT_STATUS_VERIFYING: the verifier started and asks questions
 * @SHELL_AUTH_PROMPT_STATUS_VERIFICATION_FAILED: the user failed to authenticate
 * @SHELL_AUTH_PROMPT_STATUS_VERIFICATION_SUCCEEDED: the user authenticated
 * @SHELL_AUTH_PROMPT_STATUS_VERIFICATION_CANCELLED: the user went back
 * @SHELL_AUTH_PROMPT_STATUS_VERIFICATION_IN_PROGRESS: the user answered and
 *   the verifier is checking
 *
 * The :verification-status of the prompt (`AuthPromptStatus`).
 */
typedef enum
{
  SHELL_AUTH_PROMPT_STATUS_NOT_VERIFYING = 0,
  SHELL_AUTH_PROMPT_STATUS_VERIFYING = 1,
  SHELL_AUTH_PROMPT_STATUS_VERIFICATION_FAILED = 2,
  SHELL_AUTH_PROMPT_STATUS_VERIFICATION_SUCCEEDED = 3,
  SHELL_AUTH_PROMPT_STATUS_VERIFICATION_CANCELLED = 4,
  SHELL_AUTH_PROMPT_STATUS_VERIFICATION_IN_PROGRESS = 5,
} ShellAuthPromptStatus;

/**
 * ShellAuthPromptResetType:
 * @SHELL_AUTH_PROMPT_RESET_PROVIDE_USERNAME: the owner of the prompt has to
 *   find the user name
 * @SHELL_AUTH_PROMPT_RESET_DONT_PROVIDE_USERNAME: the verifier does not need
 *   a user name
 * @SHELL_AUTH_PROMPT_RESET_REUSE_USERNAME: begin again with the same user
 *
 * What the owner of the prompt has to do after #ShellAuthPrompt::reset
 * (`ResetType`).
 */
typedef enum
{
  SHELL_AUTH_PROMPT_RESET_PROVIDE_USERNAME = 0,
  SHELL_AUTH_PROMPT_RESET_DONT_PROVIDE_USERNAME = 1,
  SHELL_AUTH_PROMPT_RESET_REUSE_USERNAME = 2,
} ShellAuthPromptResetType;

/**
 * ShellAuthPromptFinishFunc:
 * @user_data: (closure): the data given to shell_auth_prompt_finish ()
 *
 * Called when the prompt is done with the messages of the verifier.
 */
typedef void (* ShellAuthPromptFinishFunc) (gpointer user_data);

#define SHELL_TYPE_AUTH_PROMPT (shell_auth_prompt_get_type ())
G_DECLARE_FINAL_TYPE (ShellAuthPrompt, shell_auth_prompt,
                      SHELL, AUTH_PROMPT, StBoxLayout)

ShellAuthPrompt *shell_auth_prompt_new (GdmClient          *client,
                                        ShellAuthPromptMode mode);

ShellAuthPromptStatus shell_auth_prompt_get_verification_status (ShellAuthPrompt *self);
void                  shell_auth_prompt_set_verification_status (ShellAuthPrompt       *self,
                                                                 ShellAuthPromptStatus  status);
guint                 shell_auth_prompt_get_prompt_step         (ShellAuthPrompt *self);
StButton             *shell_auth_prompt_get_cancel_button       (ShellAuthPrompt *self);

ShellBatchHold *shell_auth_prompt_show_timed_login_indicator (ShellAuthPrompt *self,
                                                              double           time);
void            shell_auth_prompt_hide_timed_login_indicator (ShellAuthPrompt *self);

void     shell_auth_prompt_update_entry      (ShellAuthPrompt *self,
                                              gboolean         secret);
void     shell_auth_prompt_clear             (ShellAuthPrompt *self,
                                              gboolean         reuse_entry_text);
void     shell_auth_prompt_set_question      (ShellAuthPrompt *self,
                                              const char      *question);
char    *shell_auth_prompt_get_answer        (ShellAuthPrompt *self);
void     shell_auth_prompt_set_message       (ShellAuthPrompt     *self,
                                              const char          *message,
                                              ShellGdmMessageType  type);
void     shell_auth_prompt_update_sensitivity (ShellAuthPrompt *self,
                                               gboolean         sensitive);
void     shell_auth_prompt_set_user          (ShellAuthPrompt *self,
                                              ActUser         *user);
gboolean shell_auth_prompt_select_mechanism  (ShellAuthPrompt *self,
                                              GVariant        *mechanism);
void     shell_auth_prompt_reset             (ShellAuthPrompt *self,
                                              gboolean         reuse_entry_text,
                                              gboolean         soft_reset);
void     shell_auth_prompt_start_preemptive_input (ShellAuthPrompt *self,
                                                   gunichar         unichar);
void     shell_auth_prompt_set_auth_blocked  (ShellAuthPrompt *self,
                                              gboolean         should_block);
void     shell_auth_prompt_begin             (ShellAuthPrompt *self,
                                              const char      *user_name,
                                              ShellBatchHold  *hold);
void     shell_auth_prompt_finish            (ShellAuthPrompt           *self,
                                              ShellAuthPromptFinishFunc  func,
                                              gpointer                   user_data,
                                              GDestroyNotify             destroy);
void     shell_auth_prompt_cancel            (ShellAuthPrompt *self);

G_END_DECLS
