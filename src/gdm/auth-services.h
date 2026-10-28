/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * auth-services.h: The base class of the groups of PAM services that verify
 * a user (the legacy password/smartcard/fingerprint services, the switchable
 * authentication of SSSD). Port of js/gdm/authServices.js.
 *
 * This header is private to the gdm module of libshell, it is not in the GIR:
 * #ShellGdmUserVerifier is the only user of the services.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gdm/gdm-client.h>
#include <json-glib/json-glib.h>

#include "gdm/credential-manager.h"
#include "gdm/user-verifier.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* Errors                                                                    */
/* ------------------------------------------------------------------------ */

/*
 * The error of the authentication initialization phase (InitError of
 * js/gdm/userVerifier.js): requesting the user verifier proxies or starting a
 * service failed. It wraps the underlying error, and says which service
 * failed.
 */
#define SHELL_GDM_INIT_ERROR (shell_gdm_init_error_quark ())

typedef enum
{
  SHELL_GDM_INIT_ERROR_FAILED,
} ShellGdmInitError;

GQuark       shell_gdm_init_error_quark (void);
GError      *shell_gdm_init_error_new   (GError     *cause,
                                         const char *service_name,
                                         const char *format,
                                         ...) G_GNUC_PRINTF (3, 4);
const char  *shell_gdm_init_error_get_service_name (const GError *error);
const GError *shell_gdm_init_error_get_cause       (const GError *error);

/* ------------------------------------------------------------------------ */
/* Mechanisms                                                                */
/* ------------------------------------------------------------------------ */

typedef enum
{
  SHELL_GDM_READY_UNSET = -1,
  SHELL_GDM_READY_NO = 0,
  SHELL_GDM_READY_YES = 1,
} ShellGdmReady;

/*
 * A way of authenticating: the plain objects of the JS (`{serviceName, role,
 * id, name, ...}`). The fields of the role (RoleProperties) are applied by
 * shell_gdm_auth_mechanism_apply_role_properties ().
 */
typedef struct
{
  int ref_count;

  char *service_name;
  char *role;
  char *id;                 /* nullable */
  char *name;               /* nullable */

  /* RoleProperties */
  gboolean selectable;
  gboolean preemptive_input;
  char *hint;               /* nullable */
  char *icon_name;          /* nullable */
  char *description;        /* nullable */

  ShellGdmReady ready;
  gboolean needs_refresh;

  JsonObject *json;         /* nullable, the fields SSSD sent */
} ShellGdmAuthMechanism;

ShellGdmAuthMechanism *shell_gdm_auth_mechanism_new   (const char            *service_name,
                                                       const char            *role);
ShellGdmAuthMechanism *shell_gdm_auth_mechanism_copy  (const ShellGdmAuthMechanism *mechanism);
ShellGdmAuthMechanism *shell_gdm_auth_mechanism_ref   (ShellGdmAuthMechanism *mechanism);
void                   shell_gdm_auth_mechanism_unref (ShellGdmAuthMechanism *mechanism);
gboolean               shell_gdm_auth_mechanism_equals (const ShellGdmAuthMechanism *a,
                                                        const ShellGdmAuthMechanism *b);
void                   shell_gdm_auth_mechanism_apply_role_properties (ShellGdmAuthMechanism *mechanism);
GVariant              *shell_gdm_auth_mechanism_to_variant (const ShellGdmAuthMechanism *mechanism);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ShellGdmAuthMechanism, shell_gdm_auth_mechanism_unref)

/* ------------------------------------------------------------------------ */
/* ShellGdmAuthServices                                                      */
/* ------------------------------------------------------------------------ */

/* The user verifier proxies the services work with; the services take their own references */
typedef struct
{
  GdmUserVerifier *user_verifier;
  GdmUserVerifierChoiceList *choice_list;       /* nullable */
  GdmUserVerifierCustomJSON *custom_json;       /* nullable */
} ShellGdmUserVerifierProxies;

/* Declared by hand, G_DECLARE_DERIVABLE_TYPE () does not let the subclasses see the instance */
#define SHELL_TYPE_GDM_AUTH_SERVICES (shell_gdm_auth_services_get_type ())
#define SHELL_GDM_AUTH_SERVICES(o) (G_TYPE_CHECK_INSTANCE_CAST ((o), SHELL_TYPE_GDM_AUTH_SERVICES, ShellGdmAuthServices))
#define SHELL_GDM_AUTH_SERVICES_CLASS(k) (G_TYPE_CHECK_CLASS_CAST ((k), SHELL_TYPE_GDM_AUTH_SERVICES, ShellGdmAuthServicesClass))
#define SHELL_IS_GDM_AUTH_SERVICES(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), SHELL_TYPE_GDM_AUTH_SERVICES))
#define SHELL_GDM_AUTH_SERVICES_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), SHELL_TYPE_GDM_AUTH_SERVICES, ShellGdmAuthServicesClass))

typedef struct _ShellGdmAuthServices ShellGdmAuthServices;
typedef struct _ShellGdmAuthServicesClass ShellGdmAuthServicesClass;

GType shell_gdm_auth_services_get_type (void) G_GNUC_CONST;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ShellGdmAuthServices, g_object_unref)

struct _ShellGdmAuthServices
{
  GObject parent;

  /* Protected: the subclasses read and write these */
  GSettings *settings;
  int allowed_failures;
  gboolean reauth_only;

  int fail_counter;
  GHashTable *active_services;         /* a set of service names */
  GHashTable *unavailable_services;    /* a set of service names */

  GCancellable *cancellable;           /* nullable */
  char *user_name;                     /* nullable */

  GdmUserVerifier *user_verifier;                  /* nullable */
  GdmUserVerifierChoiceList *user_verifier_choice_list;   /* nullable */
  GdmUserVerifierCustomJSON *user_verifier_custom_json;   /* nullable */

  GPtrArray *enabled_roles;            /* const char *, not owned */
  ShellGdmAuthMechanism *selected_mechanism;   /* nullable */
  GPtrArray *enabled_mechanisms;       /* ShellGdmAuthMechanism */

  GList *pending_waits;                /* WaitData of auth-services.c */
  gboolean destroyed;
};

struct _ShellGdmAuthServicesClass
{
  GObjectClass parent_class;

  /* SupportedRoles: a NULL-terminated array of roles */
  const char * const *supported_roles;

  /* RoleToService */
  const char *(* role_to_service) (const char *role);

  const char * const *(* get_supported_roles)      (ShellGdmAuthServices *self);

  void     (* handle_begin_verification)           (ShellGdmAuthServices *self);
  /* Returns whether the error is dealt with, the begin verification then succeeds */
  gboolean (* handle_begin_verification_error)     (ShellGdmAuthServices *self,
                                                    const GError         *error);
  void     (* handle_select_mechanism)             (ShellGdmAuthServices *self);
  gboolean (* handle_needs_username)               (ShellGdmAuthServices *self);
  void     (* handle_reset)                        (ShellGdmAuthServices *self);
  void     (* handle_cancel)                       (ShellGdmAuthServices *self);
  void     (* handle_clear)                        (ShellGdmAuthServices *self);
  void     (* handle_update_enabled_roles)         (ShellGdmAuthServices *self);
  void     (* handle_update_enabled_mechanisms)    (ShellGdmAuthServices *self);

  void     (* handle_on_info)                      (ShellGdmAuthServices *self,
                                                    const char           *service_name,
                                                    const char           *info);
  void     (* handle_on_problem)                   (ShellGdmAuthServices *self,
                                                    const char           *service_name,
                                                    const char           *problem);
  void     (* handle_on_info_query)                (ShellGdmAuthServices *self,
                                                    const char           *service_name,
                                                    const char           *question);
  void     (* handle_on_secret_info_query)         (ShellGdmAuthServices *self,
                                                    const char           *service_name,
                                                    const char           *secret_question);
  void     (* handle_on_conversation_started)      (ShellGdmAuthServices *self,
                                                    const char           *service_name);
  void     (* handle_on_conversation_stopped)      (ShellGdmAuthServices *self,
                                                    const char           *service_name);
  void     (* handle_on_service_unavailable)       (ShellGdmAuthServices *self,
                                                    const char           *service_name,
                                                    const char           *error_message);
  void     (* handle_on_verification_complete)     (ShellGdmAuthServices *self,
                                                    const char           *service_name);
  void     (* handle_on_choice_list_query)         (ShellGdmAuthServices *self,
                                                    const char           *service_name,
                                                    const char           *prompt_message,
                                                    GVariant             *list);
  void     (* handle_on_custom_json_request)       (ShellGdmAuthServices *self,
                                                    const char           *service_name,
                                                    const char           *protocol,
                                                    guint                 version,
                                                    const char           *json);

  void     (* handle_verification_failed)          (ShellGdmAuthServices *self,
                                                    const char           *service_name);
  /* Returns an array of newly allocated service names */
  GPtrArray *(* handle_get_credential_manager_services) (ShellGdmAuthServices *self);
  gboolean (* handle_can_start_service)            (ShellGdmAuthServices *self,
                                                    const char           *service_name);

  gboolean (* cancel_requested)                    (ShellGdmAuthServices *self);
  void     (* add_credential_manager)              (ShellGdmAuthServices   *self,
                                                    const char             *service_name,
                                                    ShellCredentialManager *credential_manager);
  void     (* remove_credential_manager)           (ShellGdmAuthServices *self,
                                                    const char           *service_name);

  /* The action behind answerHandler of ask-question, see shell_gdm_auth_services_new_handler () */
  void     (* handle_answer_query)                 (ShellGdmAuthServices *self,
                                                    const char           *service_name,
                                                    const char           *answer);
};

/* Called by the user verifier */
const char * const *shell_gdm_auth_services_get_supported_roles (ShellGdmAuthServices *self);
ShellGdmAuthMechanism *shell_gdm_auth_services_get_selected_mechanism (ShellGdmAuthServices *self);
GPtrArray *shell_gdm_auth_services_get_enabled_mechanisms (ShellGdmAuthServices *self);

void     shell_gdm_auth_services_begin_verification_async  (ShellGdmAuthServices                 *self,
                                                            const char                           *user_name,
                                                            const ShellGdmUserVerifierProxies    *proxies,
                                                            GAsyncReadyCallback                   callback,
                                                            gpointer                              user_data);
gboolean shell_gdm_auth_services_begin_verification_finish (ShellGdmAuthServices *self,
                                                            GAsyncResult         *res,
                                                            GError              **error);

gboolean shell_gdm_auth_services_select_mechanism  (ShellGdmAuthServices         *self,
                                                    const ShellGdmAuthMechanism  *mechanism);
gboolean shell_gdm_auth_services_needs_username    (ShellGdmAuthServices *self);
void     shell_gdm_auth_services_reset             (ShellGdmAuthServices *self);
void     shell_gdm_auth_services_cancel            (ShellGdmAuthServices *self);
gboolean shell_gdm_auth_services_cancel_requested  (ShellGdmAuthServices *self);
void     shell_gdm_auth_services_destroy           (ShellGdmAuthServices *self);
void     shell_gdm_auth_services_clear             (ShellGdmAuthServices *self);
void     shell_gdm_auth_services_update_enabled_roles (ShellGdmAuthServices *self,
                                                       const char * const   *disable_roles);
void     shell_gdm_auth_services_add_credential_manager    (ShellGdmAuthServices   *self,
                                                            const char             *service_name,
                                                            ShellCredentialManager *credential_manager);
void     shell_gdm_auth_services_remove_credential_manager (ShellGdmAuthServices *self,
                                                            const char           *service_name);

/* For the subclasses */
gboolean shell_gdm_auth_services_mechanism_enabled_for_role (ShellGdmAuthServices *self,
                                                             const char           *role);
gboolean shell_gdm_auth_services_can_retry          (ShellGdmAuthServices *self);
void     shell_gdm_auth_services_verification_failed (ShellGdmAuthServices *self,
                                                      const char           *service_name,
                                                      gboolean              should_retry);
void     shell_gdm_auth_services_update_enabled_mechanisms (ShellGdmAuthServices *self);
void     shell_gdm_auth_services_send_answer        (ShellGdmAuthServices *self,
                                                     const char           *service_name,
                                                     const char           *answer);
void     shell_gdm_auth_services_send_choice        (ShellGdmAuthServices *self,
                                                     const char           *service_name,
                                                     const char           *key);
void     shell_gdm_auth_services_send_custom_json   (ShellGdmAuthServices *self,
                                                     const char           *service_name,
                                                     const char           *json);

typedef void (*ShellGdmAuthServicesHandlerFunc) (ShellGdmAuthServices *self,
                                                 const char           *service_name,
                                                 const char           *argument,
                                                 gpointer              user_data);

ShellGdmAuthHandler *shell_gdm_auth_services_new_handler (ShellGdmAuthServices            *self,
                                                          const char                      *service_name,
                                                          ShellGdmAuthServicesHandlerFunc  func,
                                                          gpointer                         user_data,
                                                          GDestroyNotify                   destroy);
ShellGdmAuthHandler *shell_gdm_auth_services_new_answer_handler (ShellGdmAuthServices *self,
                                                                 const char           *service_name);

void     shell_gdm_auth_services_emit_queue_message          (ShellGdmAuthServices *self,
                                                              const char           *service_name,
                                                              const char           *message,
                                                              ShellGdmMessageType   type,
                                                              gboolean              wiggle);
void     shell_gdm_auth_services_emit_queue_priority_message (ShellGdmAuthServices *self,
                                                              const char           *service_name,
                                                              const char           *message,
                                                              ShellGdmMessageType   type,
                                                              gboolean              wiggle);
void     shell_gdm_auth_services_emit_ask_question          (ShellGdmAuthServices *self,
                                                             const char           *service_name,
                                                             const char           *question,
                                                             gboolean              secret);
void     shell_gdm_auth_services_emit_show_choice_list      (ShellGdmAuthServices *self,
                                                             const char           *service_name,
                                                             const char           *prompt_message,
                                                             GVariant             *choice_list,
                                                             ShellGdmAuthHandler  *handler);
void     shell_gdm_auth_services_emit_show_button           (ShellGdmAuthServices *self,
                                                             const char           *service_name,
                                                             const char           *label,
                                                             ShellGdmAuthHandler  *handler);
void     shell_gdm_auth_services_emit_web_login             (ShellGdmAuthServices *self,
                                                             const char           *service_name,
                                                             const char           *message,
                                                             const char           *url,
                                                             const char           *code,
                                                             const char           *done_label,
                                                             ShellGdmAuthHandler  *done_handler);
void     shell_gdm_auth_services_emit_reset                 (ShellGdmAuthServices *self,
                                                             gboolean              soft_reset,
                                                             gboolean              reuse_entry_text);
void     shell_gdm_auth_services_emit_mechanisms_changed    (ShellGdmAuthServices *self);

G_END_DECLS
