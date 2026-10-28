/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * user-verifier.h: Talks to GDM to verify the user of the login and unlock
 * screens. Port of js/gdm/userVerifier.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gdm/gdm-client.h>

#include "gdm/batch.h"
#include "gdm/credential-manager.h"

G_BEGIN_DECLS

/**
 * ShellGdmMessageType:
 * @SHELL_GDM_MESSAGE_TYPE_NONE: no message, clears the current one
 * @SHELL_GDM_MESSAGE_TYPE_HINT: a hint
 * @SHELL_GDM_MESSAGE_TYPE_INFO: information
 * @SHELL_GDM_MESSAGE_TYPE_ERROR: an error
 *
 * Messages are kept in order by priority (`MessageType` of the JS module).
 */
typedef enum
{
  SHELL_GDM_MESSAGE_TYPE_NONE = 0,
  SHELL_GDM_MESSAGE_TYPE_HINT = 1,
  SHELL_GDM_MESSAGE_TYPE_INFO = 2,
  SHELL_GDM_MESSAGE_TYPE_ERROR = 3,
} ShellGdmMessageType;

/* The roles of the authentication mechanisms (`Role` of js/gdm/authServices.js) */
#define SHELL_GDM_ROLE_PASSWORD "password"
#define SHELL_GDM_ROLE_SMARTCARD "smartcard"
#define SHELL_GDM_ROLE_FINGERPRINT "fingerprint"
#define SHELL_GDM_ROLE_PASSKEY "passkey"
#define SHELL_GDM_ROLE_WEB_LOGIN "eidp"

/* The PAM services of the legacy and the switchable authentication */
#define SHELL_GDM_PASSWORD_SERVICE_NAME "gdm-password"
#define SHELL_GDM_SMARTCARD_SERVICE_NAME "gdm-smartcard"
#define SHELL_GDM_FINGERPRINT_SERVICE_NAME "gdm-fingerprint"
#define SHELL_GDM_SWITCHABLE_AUTH_SERVICE_NAME "gdm-switchable-auth"


#define SHELL_TYPE_GDM_AUTH_HANDLER (shell_gdm_auth_handler_get_type ())
G_DECLARE_FINAL_TYPE (ShellGdmAuthHandler, shell_gdm_auth_handler,
                      SHELL, GDM_AUTH_HANDLER, GObject)

/**
 * ShellGdmAuthHandlerFunc:
 * @argument: (nullable): the answer, the key of the choice, or %NULL
 * @user_data: (closure): the data given to shell_gdm_auth_handler_new ()
 *
 * The action behind a prompt of the user verifier.
 */
typedef void (*ShellGdmAuthHandlerFunc) (const char *argument,
                                         gpointer    user_data);

ShellGdmAuthHandler *shell_gdm_auth_handler_new (ShellGdmAuthHandlerFunc func,
                                                 gpointer                user_data,
                                                 GDestroyNotify          destroy);

void                 shell_gdm_auth_handler_invoke (ShellGdmAuthHandler *handler,
                                                    const char          *argument);


#define SHELL_TYPE_GDM_USER_VERIFIER (shell_gdm_user_verifier_get_type ())
G_DECLARE_FINAL_TYPE (ShellGdmUserVerifier, shell_gdm_user_verifier,
                      SHELL, GDM_USER_VERIFIER, GObject)

ShellGdmUserVerifier *shell_gdm_user_verifier_new (GdmClient *client,
                                                   gboolean   reauthentication_only);

gboolean shell_gdm_user_verifier_get_has_pending_messages  (ShellGdmUserVerifier *verifier);
int      shell_gdm_user_verifier_get_allowed_failures      (ShellGdmUserVerifier *verifier);
gboolean shell_gdm_user_verifier_get_preemptive_input      (ShellGdmUserVerifier *verifier);

void     shell_gdm_user_verifier_begin            (ShellGdmUserVerifier *verifier,
                                                   const char           *user_name,
                                                   ShellBatchHold       *hold);

gboolean shell_gdm_user_verifier_select_mechanism (ShellGdmUserVerifier *verifier,
                                                   const char           *service_name,
                                                   const char           *role);
gboolean shell_gdm_user_verifier_needs_username   (ShellGdmUserVerifier *verifier);
void     shell_gdm_user_verifier_set_driver_service (ShellGdmUserVerifier *verifier,
                                                     const char           *service_name);
void     shell_gdm_user_verifier_reset            (ShellGdmUserVerifier *verifier);
gboolean shell_gdm_user_verifier_cancel_requested (ShellGdmUserVerifier *verifier);
void     shell_gdm_user_verifier_cancel           (ShellGdmUserVerifier *verifier);
void     shell_gdm_user_verifier_clear            (ShellGdmUserVerifier *verifier);
void     shell_gdm_user_verifier_destroy          (ShellGdmUserVerifier *verifier);

void     shell_gdm_user_verifier_add_credential_manager    (ShellGdmUserVerifier   *verifier,
                                                            const char             *service_name,
                                                            ShellCredentialManager *credential_manager);
void     shell_gdm_user_verifier_remove_credential_manager (ShellGdmUserVerifier   *verifier,
                                                            const char             *service_name);

void     shell_gdm_user_verifier_message_shown    (ShellGdmUserVerifier *verifier);

void     shell_gdm_user_verifier_handle_pending_messages_async  (ShellGdmUserVerifier *verifier,
                                                                 GAsyncReadyCallback   callback,
                                                                 gpointer              user_data);
gboolean shell_gdm_user_verifier_handle_pending_messages_finish (ShellGdmUserVerifier *verifier,
                                                                 GAsyncResult         *res,
                                                                 GError              **error);

G_END_DECLS
