/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * auth-services.c: The base class of the groups of PAM services that verify
 * a user. Port of js/gdm/authServices.js.
 *
 * A #ShellGdmAuthServices listens to the user verifier proxies of GDM, starts
 * the PAM services of its roles and turns what they say into the internal
 * signals that #ShellGdmUserVerifier forwards to the prompt. The JS
 * `_handle*` methods of the subclasses are the virtual functions of the class.
 *
 * Differences from the JS module:
 *
 *  - the signals carry their arguments instead of one object, and the
 *    callbacks of the prompts (answerHandler, choiceHandler, callback and the
 *    actions of the buttons) are #ShellGdmAuthHandler objects;
 *  - the answers and the choices are sent with the proxies' asynchronous
 *    calls, their failure is logged as a message unless it was cancelled;
 *  - the roles of the mechanisms are applied when the mechanisms are
 *    copied, as the JS did with the spread operator.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n-lib.h>

#include "gdm/auth-services.h"

/* ------------------------------------------------------------------------ */
/* InitError                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct
{
  char *service_name;
  GError *cause;
} ShellGdmInitErrorPrivate;

static void
shell_gdm_init_error_private_init (ShellGdmInitErrorPrivate *priv)
{
  priv->service_name = NULL;
  priv->cause = NULL;
}

static void
shell_gdm_init_error_private_copy (const ShellGdmInitErrorPrivate *src,
                                   ShellGdmInitErrorPrivate       *dest)
{
  dest->service_name = g_strdup (src->service_name);
  dest->cause = src->cause != NULL ? g_error_copy (src->cause) : NULL;
}

static void
shell_gdm_init_error_private_clear (ShellGdmInitErrorPrivate *priv)
{
  g_clear_pointer (&priv->service_name, g_free);
  g_clear_error (&priv->cause);
}

G_DEFINE_EXTENDED_ERROR (ShellGdmInitError, shell_gdm_init_error)

/**
 * shell_gdm_init_error_new:
 * @cause: (transfer full) (nullable): the error that made the initialization fail
 * @service_name: (nullable): the PAM service that failed
 * @format: the message
 *
 * Creates the error of the authentication initialization phase.
 *
 * Returns: (transfer full): a new #GError
 */
GError *
shell_gdm_init_error_new (GError     *cause,
                          const char *service_name,
                          const char *format,
                          ...)
{
  ShellGdmInitErrorPrivate *priv;
  GError *error;
  va_list args;

  va_start (args, format);
  error = g_error_new_valist (SHELL_GDM_INIT_ERROR, SHELL_GDM_INIT_ERROR_FAILED,
                              format, args);
  va_end (args);

  priv = shell_gdm_init_error_get_private (error);
  priv->service_name = g_strdup (service_name);
  priv->cause = cause;

  return error;
}

const char *
shell_gdm_init_error_get_service_name (const GError *error)
{
  ShellGdmInitErrorPrivate *priv;

  if (error == NULL || error->domain != SHELL_GDM_INIT_ERROR)
    return NULL;

  priv = shell_gdm_init_error_get_private (error);
  return priv->service_name;
}

const GError *
shell_gdm_init_error_get_cause (const GError *error)
{
  ShellGdmInitErrorPrivate *priv;

  if (error == NULL || error->domain != SHELL_GDM_INIT_ERROR)
    return NULL;

  priv = shell_gdm_init_error_get_private (error);
  return priv->cause;
}

/* ------------------------------------------------------------------------ */
/* ShellGdmAuthMechanism                                                     */
/* ------------------------------------------------------------------------ */

ShellGdmAuthMechanism *
shell_gdm_auth_mechanism_new (const char *service_name,
                              const char *role)
{
  ShellGdmAuthMechanism *mechanism = g_new0 (ShellGdmAuthMechanism, 1);

  mechanism->ref_count = 1;
  mechanism->service_name = g_strdup (service_name);
  mechanism->role = g_strdup (role);
  mechanism->ready = SHELL_GDM_READY_UNSET;

  return mechanism;
}

ShellGdmAuthMechanism *
shell_gdm_auth_mechanism_copy (const ShellGdmAuthMechanism *mechanism)
{
  ShellGdmAuthMechanism *copy;

  copy = shell_gdm_auth_mechanism_new (mechanism->service_name, mechanism->role);
  copy->id = g_strdup (mechanism->id);
  copy->name = g_strdup (mechanism->name);
  copy->selectable = mechanism->selectable;
  copy->preemptive_input = mechanism->preemptive_input;
  copy->hint = g_strdup (mechanism->hint);
  copy->icon_name = g_strdup (mechanism->icon_name);
  copy->description = g_strdup (mechanism->description);
  copy->ready = mechanism->ready;
  copy->needs_refresh = mechanism->needs_refresh;
  copy->json = mechanism->json != NULL ? json_object_ref (mechanism->json) : NULL;

  return copy;
}

ShellGdmAuthMechanism *
shell_gdm_auth_mechanism_ref (ShellGdmAuthMechanism *mechanism)
{
  mechanism->ref_count++;
  return mechanism;
}

void
shell_gdm_auth_mechanism_unref (ShellGdmAuthMechanism *mechanism)
{
  if (--mechanism->ref_count > 0)
    return;

  g_free (mechanism->service_name);
  g_free (mechanism->role);
  g_free (mechanism->id);
  g_free (mechanism->name);
  g_free (mechanism->hint);
  g_free (mechanism->icon_name);
  g_free (mechanism->description);
  g_clear_pointer (&mechanism->json, json_object_unref);
  g_free (mechanism);
}

/* Two mechanisms are the same if they have the same service and role; two
 * missing mechanisms are the same too. */
gboolean
shell_gdm_auth_mechanism_equals (const ShellGdmAuthMechanism *a,
                                 const ShellGdmAuthMechanism *b)
{
  if (a == NULL || b == NULL)
    return a == b;

  return g_strcmp0 (a->service_name, b->service_name) == 0 &&
         g_strcmp0 (a->role, b->role) == 0;
}

/* RoleProperties of js/gdm/authServices.js */
void
shell_gdm_auth_mechanism_apply_role_properties (ShellGdmAuthMechanism *mechanism)
{
  const char *role = mechanism->role;

  if (g_strcmp0 (role, SHELL_GDM_ROLE_PASSWORD) == 0)
    {
      mechanism->selectable = TRUE;
      mechanism->preemptive_input = TRUE;
    }
  else if (g_strcmp0 (role, SHELL_GDM_ROLE_SMARTCARD) == 0)
    {
      mechanism->selectable = TRUE;
      g_free (mechanism->hint);
      mechanism->hint = g_strdup (_("Insert smartcard"));
    }
  else if (g_strcmp0 (role, SHELL_GDM_ROLE_PASSKEY) == 0)
    {
      mechanism->selectable = TRUE;
      g_free (mechanism->hint);
      mechanism->hint = g_strdup (_("Insert security key"));
    }
  else if (g_strcmp0 (role, SHELL_GDM_ROLE_WEB_LOGIN) == 0)
    {
      mechanism->selectable = TRUE;
    }
  else if (g_strcmp0 (role, SHELL_GDM_ROLE_FINGERPRINT) == 0)
    {
      g_free (mechanism->icon_name);
      mechanism->icon_name = g_strdup ("fingerprint-auth-symbolic");
      g_free (mechanism->description);
      mechanism->description = g_strdup (_("Unlock with fingerprint"));
    }
}

/*
 * The mechanism as the prompt knows it: an `a{sv}` with the keys of the plain
 * JS object (serviceName, role, id, name, selectable, preemptiveInput, hint,
 * iconName, description; the ones that are not set are left out).
 */
GVariant *
shell_gdm_auth_mechanism_to_variant (const ShellGdmAuthMechanism *mechanism)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);

  g_variant_builder_add (&builder, "{sv}", "serviceName",
                         g_variant_new_string (mechanism->service_name));
  g_variant_builder_add (&builder, "{sv}", "role",
                         g_variant_new_string (mechanism->role));
  if (mechanism->id != NULL)
    g_variant_builder_add (&builder, "{sv}", "id", g_variant_new_string (mechanism->id));
  if (mechanism->name != NULL)
    g_variant_builder_add (&builder, "{sv}", "name", g_variant_new_string (mechanism->name));
  if (mechanism->selectable)
    g_variant_builder_add (&builder, "{sv}", "selectable", g_variant_new_boolean (TRUE));
  if (mechanism->preemptive_input)
    g_variant_builder_add (&builder, "{sv}", "preemptiveInput", g_variant_new_boolean (TRUE));
  if (mechanism->hint != NULL)
    g_variant_builder_add (&builder, "{sv}", "hint", g_variant_new_string (mechanism->hint));
  if (mechanism->icon_name != NULL)
    g_variant_builder_add (&builder, "{sv}", "iconName", g_variant_new_string (mechanism->icon_name));
  if (mechanism->description != NULL)
    g_variant_builder_add (&builder, "{sv}", "description", g_variant_new_string (mechanism->description));

  return g_variant_builder_end (&builder);
}

/* ------------------------------------------------------------------------ */
/* ShellGdmAuthServices                                                      */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_SETTINGS,
  PROP_ALLOWED_FAILURES,
  PROP_REAUTH_ONLY,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  DESTROY,
  QUEUE_MESSAGE,
  QUEUE_PRIORITY_MESSAGE,
  WAIT_PENDING_MESSAGES,
  FILTER_MESSAGES,
  VERIFICATION_FAILED,
  VERIFICATION_COMPLETE,
  ASK_QUESTION,
  RESET,
  SHOW_CHOICE_LIST,
  SHOW_BUTTON,
  MECHANISMS_CHANGED,
  WEB_LOGIN,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE (ShellGdmAuthServices, shell_gdm_auth_services, G_TYPE_OBJECT)

#define KLASS(self) SHELL_GDM_AUTH_SERVICES_GET_CLASS (self)

#define WAIT_PENDING_MESSAGES_TIMEOUT_SECONDS 10

/* ---- user verifier proxies ---- */

static void
disconnect_user_verifier_signals (ShellGdmAuthServices *self)
{
  if (self->user_verifier != NULL)
    {
      GDBusConnection *connection;

      connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (self->user_verifier));
      if (connection != NULL)
        g_signal_handlers_disconnect_by_data (connection, self);

      g_signal_handlers_disconnect_by_data (self->user_verifier, self);
    }

  if (self->user_verifier_choice_list != NULL)
    g_signal_handlers_disconnect_by_data (self->user_verifier_choice_list, self);

  if (self->user_verifier_custom_json != NULL)
    g_signal_handlers_disconnect_by_data (self->user_verifier_custom_json, self);
}

static void
clear_user_verifier (ShellGdmAuthServices *self)
{
  disconnect_user_verifier_signals (self);

  g_clear_object (&self->user_verifier);
  g_clear_object (&self->user_verifier_choice_list);
  g_clear_object (&self->user_verifier_custom_json);
}

static void
on_connection_closed (GDBusConnection      *connection,
                      gboolean              remote_peer_vanished,
                      GError               *error,
                      ShellGdmAuthServices *self)
{
  clear_user_verifier (self);
}

static void
on_info (GdmUserVerifier      *verifier,
         const char           *service_name,
         const char           *info,
         ShellGdmAuthServices *self)
{
  if (KLASS (self)->handle_on_info != NULL)
    KLASS (self)->handle_on_info (self, service_name, info);
}

static void
on_problem (GdmUserVerifier      *verifier,
            const char           *service_name,
            const char           *problem,
            ShellGdmAuthServices *self)
{
  if (KLASS (self)->handle_on_problem != NULL)
    KLASS (self)->handle_on_problem (self, service_name, problem);
}

static void
on_info_query (GdmUserVerifier      *verifier,
               const char           *service_name,
               const char           *question,
               ShellGdmAuthServices *self)
{
  if (KLASS (self)->handle_on_info_query != NULL)
    KLASS (self)->handle_on_info_query (self, service_name, question);
}

static void
on_secret_info_query (GdmUserVerifier      *verifier,
                      const char           *service_name,
                      const char           *secret_question,
                      ShellGdmAuthServices *self)
{
  if (KLASS (self)->handle_on_secret_info_query != NULL)
    KLASS (self)->handle_on_secret_info_query (self, service_name, secret_question);
}

static void
on_conversation_started (GdmUserVerifier      *verifier,
                         const char           *service_name,
                         ShellGdmAuthServices *self)
{
  g_hash_table_add (self->active_services, g_strdup (service_name));

  if (KLASS (self)->handle_on_conversation_started != NULL)
    KLASS (self)->handle_on_conversation_started (self, service_name);
}

static void
on_conversation_stopped (GdmUserVerifier      *verifier,
                         const char           *service_name,
                         ShellGdmAuthServices *self)
{
  g_hash_table_remove (self->active_services, service_name);

  g_signal_emit (self, signals[FILTER_MESSAGES], 0,
                 service_name, SHELL_GDM_MESSAGE_TYPE_ERROR);

  if (KLASS (self)->handle_on_conversation_stopped != NULL)
    KLASS (self)->handle_on_conversation_stopped (self, service_name);
}

static void
on_service_unavailable (GdmUserVerifier      *verifier,
                        const char           *service_name,
                        const char           *error_message,
                        ShellGdmAuthServices *self)
{
  g_hash_table_add (self->unavailable_services, g_strdup (service_name));

  if (self->selected_mechanism != NULL &&
      g_strcmp0 (self->selected_mechanism->service_name, service_name) == 0 &&
      error_message != NULL && error_message[0] != '\0')
    {
      shell_gdm_auth_services_emit_queue_message (self, service_name, error_message,
                                                  SHELL_GDM_MESSAGE_TYPE_ERROR, FALSE);
    }

  if (KLASS (self)->handle_on_service_unavailable != NULL)
    KLASS (self)->handle_on_service_unavailable (self, service_name, error_message);
}

static void
on_reset (GdmUserVerifier      *verifier,
          ShellGdmAuthServices *self)
{
  shell_gdm_auth_services_emit_reset (self, FALSE, FALSE);
}

static void
on_verification_complete (GdmUserVerifier      *verifier,
                          const char           *service_name,
                          ShellGdmAuthServices *self)
{
  if (KLASS (self)->handle_on_verification_complete != NULL)
    KLASS (self)->handle_on_verification_complete (self, service_name);

  g_signal_emit (self, signals[VERIFICATION_COMPLETE], 0);
}

static void
on_choice_query (GdmUserVerifierChoiceList *choice_list,
                 const char                *service_name,
                 const char                *prompt_message,
                 GVariant                  *list,
                 ShellGdmAuthServices      *self)
{
  if (KLASS (self)->handle_on_choice_list_query != NULL)
    KLASS (self)->handle_on_choice_list_query (self, service_name, prompt_message, list);
}

static void
on_custom_json_request (GdmUserVerifierCustomJSON *custom_json,
                        const char                *service_name,
                        const char                *protocol,
                        guint                      version,
                        const char                *request,
                        ShellGdmAuthServices      *self)
{
  if (KLASS (self)->handle_on_custom_json_request != NULL)
    KLASS (self)->handle_on_custom_json_request (self, service_name, protocol,
                                                 version, request);
}

static void
update_user_verifier (ShellGdmAuthServices            *self,
                      const ShellGdmUserVerifierProxies *proxies)
{
  GDBusConnection *connection;

  disconnect_user_verifier_signals (self);

  g_set_object (&self->user_verifier, proxies->user_verifier);
  g_set_object (&self->user_verifier_choice_list, proxies->choice_list);
  g_set_object (&self->user_verifier_custom_json, proxies->custom_json);

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (self->user_verifier));
  if (connection != NULL)
    g_signal_connect (connection, "closed", G_CALLBACK (on_connection_closed), self);

  g_signal_connect (self->user_verifier, "info", G_CALLBACK (on_info), self);
  g_signal_connect (self->user_verifier, "problem", G_CALLBACK (on_problem), self);
  g_signal_connect (self->user_verifier, "info-query", G_CALLBACK (on_info_query), self);
  g_signal_connect (self->user_verifier, "secret-info-query",
                    G_CALLBACK (on_secret_info_query), self);
  g_signal_connect (self->user_verifier, "conversation-started",
                    G_CALLBACK (on_conversation_started), self);
  g_signal_connect (self->user_verifier, "conversation-stopped",
                    G_CALLBACK (on_conversation_stopped), self);
  g_signal_connect (self->user_verifier, "service-unavailable",
                    G_CALLBACK (on_service_unavailable), self);
  g_signal_connect (self->user_verifier, "reset", G_CALLBACK (on_reset), self);
  g_signal_connect (self->user_verifier, "verification-complete",
                    G_CALLBACK (on_verification_complete), self);

  if (self->user_verifier_choice_list != NULL)
    g_signal_connect (self->user_verifier_choice_list, "choice-query",
                      G_CALLBACK (on_choice_query), self);

  if (self->user_verifier_custom_json != NULL)
    g_signal_connect (self->user_verifier_custom_json, "request",
                      G_CALLBACK (on_custom_json_request), self);
}

/* ---- sending ---- */

static void
call_done (GObject      *source,
           GAsyncResult *res,
           gpointer      user_data)
{
  const char *what = user_data;
  g_autoptr (GError) error = NULL;
  gboolean ok = FALSE;

  if (g_str_equal (what, "answer"))
    ok = gdm_user_verifier_call_answer_query_finish (GDM_USER_VERIFIER (source), res, &error);
  else if (g_str_equal (what, "choice"))
    ok = gdm_user_verifier_choice_list_call_select_choice_finish (GDM_USER_VERIFIER_CHOICE_LIST (source),
                                                                  res, &error);
  else
    ok = gdm_user_verifier_custom_json_call_reply_finish (GDM_USER_VERIFIER_CUSTOM_JSON (source),
                                                          res, &error);

  if (!ok && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("Failed to send the %s to the user verifier: %s", what, error->message);
}

void
shell_gdm_auth_services_send_answer (ShellGdmAuthServices *self,
                                     const char           *service_name,
                                     const char           *answer)
{
  if (self->user_verifier == NULL)
    return;

  gdm_user_verifier_call_answer_query (self->user_verifier, service_name,
                                       answer != NULL ? answer : "",
                                       self->cancellable, call_done, (gpointer) "answer");
}

void
shell_gdm_auth_services_send_choice (ShellGdmAuthServices *self,
                                     const char           *service_name,
                                     const char           *key)
{
  if (self->user_verifier_choice_list == NULL)
    return;

  gdm_user_verifier_choice_list_call_select_choice (self->user_verifier_choice_list,
                                                    service_name,
                                                    key != NULL ? key : "",
                                                    self->cancellable, call_done, (gpointer) "choice");
}

void
shell_gdm_auth_services_send_custom_json (ShellGdmAuthServices *self,
                                          const char           *service_name,
                                          const char           *json)
{
  if (self->user_verifier_custom_json == NULL)
    return;

  gdm_user_verifier_custom_json_call_reply (self->user_verifier_custom_json,
                                            service_name, json,
                                            self->cancellable, call_done, (gpointer) "reply");
}

/* ---- handlers ---- */

typedef struct
{
  ShellGdmAuthServices *self;
  char *service_name;
  ShellGdmAuthServicesHandlerFunc func;
  gpointer user_data;
  GDestroyNotify destroy;
} HandlerData;

static void
handler_data_free (gpointer data)
{
  HandlerData *handler = data;

  if (handler->destroy != NULL)
    handler->destroy (handler->user_data);

  g_object_unref (handler->self);
  g_free (handler->service_name);
  g_free (handler);
}

static void
handler_call (const char *argument,
              gpointer    data)
{
  HandlerData *handler = data;

  handler->func (handler->self, handler->service_name, argument, handler->user_data);
}

/*
 * Creates the action behind a prompt: @func is called with the service
 * @service_name and the argument of shell_gdm_auth_handler_invoke ().
 */
ShellGdmAuthHandler *
shell_gdm_auth_services_new_handler (ShellGdmAuthServices            *self,
                                     const char                      *service_name,
                                     ShellGdmAuthServicesHandlerFunc  func,
                                     gpointer                         user_data,
                                     GDestroyNotify                   destroy)
{
  HandlerData *handler = g_new0 (HandlerData, 1);

  handler->self = g_object_ref (self);
  handler->service_name = g_strdup (service_name);
  handler->func = func;
  handler->user_data = user_data;
  handler->destroy = destroy;

  return shell_gdm_auth_handler_new (handler_call, handler, handler_data_free);
}

static void
answer_handler_func (ShellGdmAuthServices *self,
                     const char           *service_name,
                     const char           *argument,
                     gpointer              user_data)
{
  if (KLASS (self)->handle_answer_query != NULL)
    KLASS (self)->handle_answer_query (self, service_name, argument);
}

ShellGdmAuthHandler *
shell_gdm_auth_services_new_answer_handler (ShellGdmAuthServices *self,
                                            const char           *service_name)
{
  return shell_gdm_auth_services_new_handler (self, service_name,
                                              answer_handler_func, NULL, NULL);
}

/* ---- emitting ---- */

void
shell_gdm_auth_services_emit_queue_message (ShellGdmAuthServices *self,
                                            const char           *service_name,
                                            const char           *message,
                                            ShellGdmMessageType   type,
                                            gboolean              wiggle)
{
  g_signal_emit (self, signals[QUEUE_MESSAGE], 0, service_name, message, type, wiggle);
}

void
shell_gdm_auth_services_emit_queue_priority_message (ShellGdmAuthServices *self,
                                                     const char           *service_name,
                                                     const char           *message,
                                                     ShellGdmMessageType   type,
                                                     gboolean              wiggle)
{
  g_signal_emit (self, signals[QUEUE_PRIORITY_MESSAGE], 0, service_name, message, type, wiggle);
}

void
shell_gdm_auth_services_emit_ask_question (ShellGdmAuthServices *self,
                                           const char           *service_name,
                                           const char           *question,
                                           gboolean              secret)
{
  g_autoptr (ShellGdmAuthHandler) handler =
    shell_gdm_auth_services_new_answer_handler (self, service_name);

  g_signal_emit (self, signals[ASK_QUESTION], 0, service_name, question, secret, handler);
}

void
shell_gdm_auth_services_emit_show_choice_list (ShellGdmAuthServices *self,
                                               const char           *service_name,
                                               const char           *prompt_message,
                                               GVariant             *choice_list,
                                               ShellGdmAuthHandler  *handler)
{
  g_signal_emit (self, signals[SHOW_CHOICE_LIST], 0,
                 service_name, prompt_message, choice_list, handler);
}

void
shell_gdm_auth_services_emit_show_button (ShellGdmAuthServices *self,
                                          const char           *service_name,
                                          const char           *label,
                                          ShellGdmAuthHandler  *handler)
{
  g_signal_emit (self, signals[SHOW_BUTTON], 0, service_name, label, handler);
}

void
shell_gdm_auth_services_emit_web_login (ShellGdmAuthServices *self,
                                        const char           *service_name,
                                        const char           *message,
                                        const char           *url,
                                        const char           *code,
                                        const char           *done_label,
                                        ShellGdmAuthHandler  *done_handler)
{
  g_signal_emit (self, signals[WEB_LOGIN], 0,
                 service_name, message, url, code, done_label, done_handler);
}

void
shell_gdm_auth_services_emit_reset (ShellGdmAuthServices *self,
                                    gboolean              soft_reset,
                                    gboolean              reuse_entry_text)
{
  g_signal_emit (self, signals[RESET], 0, soft_reset, reuse_entry_text);
}

void
shell_gdm_auth_services_emit_mechanisms_changed (ShellGdmAuthServices *self)
{
  g_signal_emit (self, signals[MECHANISMS_CHANGED], 0);
}

/* ---- accessors ---- */

const char * const *
shell_gdm_auth_services_get_supported_roles (ShellGdmAuthServices *self)
{
  g_return_val_if_fail (SHELL_IS_GDM_AUTH_SERVICES (self), NULL);

  if (KLASS (self)->get_supported_roles != NULL)
    return KLASS (self)->get_supported_roles (self);

  return KLASS (self)->supported_roles;
}

ShellGdmAuthMechanism *
shell_gdm_auth_services_get_selected_mechanism (ShellGdmAuthServices *self)
{
  g_return_val_if_fail (SHELL_IS_GDM_AUTH_SERVICES (self), NULL);

  return self->selected_mechanism;
}

/* The enabled mechanisms that are ready (all but the ones that are explicitly not) */
GPtrArray *
shell_gdm_auth_services_get_enabled_mechanisms (ShellGdmAuthServices *self)
{
  GPtrArray *mechanisms;
  guint i;

  g_return_val_if_fail (SHELL_IS_GDM_AUTH_SERVICES (self), NULL);

  mechanisms = g_ptr_array_new_with_free_func ((GDestroyNotify) shell_gdm_auth_mechanism_unref);

  for (i = 0; i < self->enabled_mechanisms->len; i++)
    {
      ShellGdmAuthMechanism *mechanism = g_ptr_array_index (self->enabled_mechanisms, i);

      if (mechanism->ready != SHELL_GDM_READY_NO)
        g_ptr_array_add (mechanisms, shell_gdm_auth_mechanism_ref (mechanism));
    }

  return mechanisms;
}

static gboolean
roles_contain (GPtrArray  *roles,
               const char *role)
{
  guint i;

  for (i = 0; i < roles->len; i++)
    {
      if (g_strcmp0 (g_ptr_array_index (roles, i), role) == 0)
        return TRUE;
    }

  return FALSE;
}

gboolean
shell_gdm_auth_services_mechanism_enabled_for_role (ShellGdmAuthServices *self,
                                                    const char           *role)
{
  return roles_contain (self->enabled_roles, role);
}

/* ---- starting the services ---- */

typedef struct
{
  GPtrArray *services;   /* char */
  guint index;
  gboolean for_user;     /* the call in flight is begin_verification_for_user () */
} StartData;

static void
start_data_free (StartData *data)
{
  g_clear_pointer (&data->services, g_ptr_array_unref);
  g_free (data);
}

static gboolean
can_start_service (ShellGdmAuthServices *self,
                   const char           *service_name)
{
  return !g_hash_table_contains (self->active_services, service_name) &&
         !g_hash_table_contains (self->unavailable_services, service_name) &&
         KLASS (self)->handle_can_start_service != NULL &&
         KLASS (self)->handle_can_start_service (self, service_name);
}

static GPtrArray *
get_enabled_services (ShellGdmAuthServices *self)
{
  GPtrArray *services = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) credential_services = NULL;
  guint i, j;

  for (i = 0; i < self->enabled_roles->len; i++)
    {
      const char *service = KLASS (self)->role_to_service != NULL
        ? KLASS (self)->role_to_service (g_ptr_array_index (self->enabled_roles, i))
        : NULL;

      if (service == NULL)
        continue;

      g_ptr_array_add (services, g_strdup (service));
    }

  if (KLASS (self)->handle_get_credential_manager_services != NULL)
    credential_services = KLASS (self)->handle_get_credential_manager_services (self);

  if (credential_services != NULL)
    {
      for (i = 0; i < credential_services->len; i++)
        g_ptr_array_add (services, g_strdup (g_ptr_array_index (credential_services, i)));
    }

  /* Remove duplicates */
  for (i = 0; i < services->len; i++)
    {
      for (j = services->len - 1; j > i; j--)
        {
          if (g_strcmp0 (g_ptr_array_index (services, i), g_ptr_array_index (services, j)) == 0)
            g_ptr_array_remove_index (services, j);
        }
    }

  return services;
}

static void
complete_begin_verification (GTask  *task,
                             GError *error)
{
  ShellGdmAuthServices *self = g_task_get_source_object (task);

  if (error != NULL)
    {
      const GError *cause = shell_gdm_init_error_get_cause (error);

      /* Cancelled: stop silently */
      if (cause != NULL && g_error_matches (cause, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_error_free (error);
          g_task_return_boolean (task, TRUE);
          return;
        }

      self->fail_counter++;

      if (KLASS (self)->handle_begin_verification_error != NULL &&
          KLASS (self)->handle_begin_verification_error (self, error))
        {
          g_error_free (error);
          g_task_return_boolean (task, TRUE);
          return;
        }

      g_task_return_error (task, error);
      return;
    }

  if (KLASS (self)->handle_begin_verification != NULL)
    KLASS (self)->handle_begin_verification (self);

  g_task_return_boolean (task, TRUE);
}

static void start_next_service (GTask *task);

static void
service_started (GObject      *source,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  ShellGdmAuthServices *self = g_task_get_source_object (task);
  StartData *data = g_task_get_task_data (task);
  const char *service_name = g_ptr_array_index (data->services, data->index - 1);
  g_autoptr (GError) cause = NULL;
  gboolean ok;

  if (data->for_user)
    ok = gdm_user_verifier_call_begin_verification_for_user_finish (GDM_USER_VERIFIER (source),
                                                                    res, &cause);
  else
    ok = gdm_user_verifier_call_begin_verification_finish (GDM_USER_VERIFIER (source),
                                                           res, &cause);

  if (!ok)
    {
      GError *error;

      g_hash_table_remove (self->active_services, service_name);

      if (g_dbus_error_is_remote_error (cause))
        {
          g_autofree char *remote_error = g_dbus_error_get_remote_error (cause);

          if (g_strcmp0 (remote_error,
                         "org.gnome.DisplayManager.SessionWorker.Error.ServiceUnavailable") == 0)
            g_hash_table_add (self->unavailable_services, g_strdup (service_name));
        }

      if (data->for_user)
        error = shell_gdm_init_error_new (g_steal_pointer (&cause), service_name,
                                          "Failed to start %s verification for user",
                                          service_name);
      else
        error = shell_gdm_init_error_new (g_steal_pointer (&cause), service_name,
                                          "Failed to start %s verification",
                                          service_name);

      complete_begin_verification (task, error);
      return;
    }

  start_next_service (task);
}

static void
start_next_service (GTask *task)
{
  ShellGdmAuthServices *self = g_task_get_source_object (task);
  StartData *data = g_task_get_task_data (task);

  while (data->index < data->services->len)
    {
      const char *service_name = g_ptr_array_index (data->services, data->index);

      data->index++;

      if (!can_start_service (self, service_name))
        continue;

      g_hash_table_add (self->active_services, g_strdup (service_name));

      if (self->user_verifier == NULL)
        {
          g_hash_table_remove (self->active_services, service_name);
          complete_begin_verification (task,
                                       shell_gdm_init_error_new (g_error_new_literal (G_IO_ERROR,
                                                                                      G_IO_ERROR_CANCELLED,
                                                                                      "Operation was cancelled"),
                                                                 service_name,
                                                                 "Failed to start %s verification",
                                                                 service_name));
          return;
        }

      data->for_user = self->user_name != NULL;

      if (data->for_user)
        {
          gdm_user_verifier_call_begin_verification_for_user (self->user_verifier,
                                                              service_name,
                                                              self->user_name,
                                                              self->cancellable,
                                                              service_started,
                                                              g_object_ref (task));
        }
      else
        {
          gdm_user_verifier_call_begin_verification (self->user_verifier,
                                                     service_name,
                                                     self->cancellable,
                                                     service_started,
                                                     g_object_ref (task));
        }

      return;
    }

  complete_begin_verification (task, NULL);
}

/*
 * Starts the services of the enabled roles for @user_name (or the user that
 * the services ask for if it is %NULL), with the proxies of the user verifier.
 * A failure comes back as a #SHELL_GDM_INIT_ERROR; cancelling is not a failure.
 */
void
shell_gdm_auth_services_begin_verification_async (ShellGdmAuthServices              *self,
                                                  const char                        *user_name,
                                                  const ShellGdmUserVerifierProxies *proxies,
                                                  GAsyncReadyCallback                callback,
                                                  gpointer                           user_data)
{
  g_autoptr (GTask) task = NULL;
  StartData *data;

  g_return_if_fail (SHELL_IS_GDM_AUTH_SERVICES (self));

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  g_free (self->user_name);
  self->user_name = g_strdup (user_name);

  update_user_verifier (self, proxies);

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, shell_gdm_auth_services_begin_verification_async);

  data = g_new0 (StartData, 1);
  data->services = get_enabled_services (self);
  g_task_set_task_data (task, data, (GDestroyNotify) start_data_free);

  start_next_service (task);
}

gboolean
shell_gdm_auth_services_begin_verification_finish (ShellGdmAuthServices *self,
                                                   GAsyncResult         *res,
                                                   GError              **error)
{
  g_return_val_if_fail (g_task_is_valid (res, self), FALSE);

  return g_task_propagate_boolean (G_TASK (res), error);
}

/* ---- the public operations ---- */

static void cancel_pending_waits (ShellGdmAuthServices *self);

gboolean
shell_gdm_auth_services_select_mechanism (ShellGdmAuthServices        *self,
                                          const ShellGdmAuthMechanism *mechanism)
{
  guint i;

  g_return_val_if_fail (SHELL_IS_GDM_AUTH_SERVICES (self), FALSE);

  if (shell_gdm_auth_mechanism_equals (self->selected_mechanism, mechanism))
    return FALSE;

  g_clear_pointer (&self->selected_mechanism, shell_gdm_auth_mechanism_unref);

  for (i = 0; mechanism != NULL && i < self->enabled_mechanisms->len; i++)
    {
      ShellGdmAuthMechanism *candidate = g_ptr_array_index (self->enabled_mechanisms, i);

      if (shell_gdm_auth_mechanism_equals (candidate, mechanism))
        {
          self->selected_mechanism = shell_gdm_auth_mechanism_ref (candidate);
          break;
        }
    }

  if (KLASS (self)->handle_select_mechanism != NULL)
    KLASS (self)->handle_select_mechanism (self);

  return self->selected_mechanism != NULL;
}

gboolean
shell_gdm_auth_services_needs_username (ShellGdmAuthServices *self)
{
  g_return_val_if_fail (SHELL_IS_GDM_AUTH_SERVICES (self), TRUE);

  if (KLASS (self)->handle_needs_username != NULL)
    return KLASS (self)->handle_needs_username (self);

  return TRUE;
}

void
shell_gdm_auth_services_reset (ShellGdmAuthServices *self)
{
  g_return_if_fail (SHELL_IS_GDM_AUTH_SERVICES (self));

  self->fail_counter = 0;

  if (KLASS (self)->handle_reset != NULL)
    KLASS (self)->handle_reset (self);
}

void
shell_gdm_auth_services_cancel (ShellGdmAuthServices *self)
{
  g_return_if_fail (SHELL_IS_GDM_AUTH_SERVICES (self));

  if (KLASS (self)->handle_cancel != NULL)
    KLASS (self)->handle_cancel (self);
}

gboolean
shell_gdm_auth_services_cancel_requested (ShellGdmAuthServices *self)
{
  g_return_val_if_fail (SHELL_IS_GDM_AUTH_SERVICES (self), FALSE);

  if (KLASS (self)->cancel_requested != NULL)
    return KLASS (self)->cancel_requested (self);

  return FALSE;
}

void
shell_gdm_auth_services_clear (ShellGdmAuthServices *self)
{
  g_return_if_fail (SHELL_IS_GDM_AUTH_SERVICES (self));

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_hash_table_remove_all (self->unavailable_services);
  g_hash_table_remove_all (self->active_services);

  g_clear_pointer (&self->user_name, g_free);

  clear_user_verifier (self);

  if (KLASS (self)->handle_clear != NULL)
    KLASS (self)->handle_clear (self);
}

void
shell_gdm_auth_services_destroy (ShellGdmAuthServices *self)
{
  g_return_if_fail (SHELL_IS_GDM_AUTH_SERVICES (self));

  shell_gdm_auth_services_reset (self);
  shell_gdm_auth_services_clear (self);
  self->destroyed = TRUE;
  cancel_pending_waits (self);
  g_signal_emit (self, signals[DESTROY], 0);
}

void
shell_gdm_auth_services_update_enabled_roles (ShellGdmAuthServices *self,
                                              const char * const   *disable_roles)
{
  const char * const *supported_roles;
  g_autoptr (GPtrArray) updated_roles = g_ptr_array_new ();
  gboolean same;
  guint i;

  g_return_if_fail (SHELL_IS_GDM_AUTH_SERVICES (self));

  supported_roles = shell_gdm_auth_services_get_supported_roles (self);
  for (i = 0; supported_roles != NULL && supported_roles[i] != NULL; i++)
    {
      if (disable_roles == NULL || !g_strv_contains (disable_roles, supported_roles[i]))
        g_ptr_array_add (updated_roles, (gpointer) supported_roles[i]);
    }

  same = updated_roles->len == self->enabled_roles->len;
  for (i = 0; same && i < updated_roles->len; i++)
    same = roles_contain (self->enabled_roles, g_ptr_array_index (updated_roles, i));

  if (same)
    return;

  g_ptr_array_set_size (self->enabled_roles, 0);
  for (i = 0; i < updated_roles->len; i++)
    g_ptr_array_add (self->enabled_roles, g_ptr_array_index (updated_roles, i));

  if (KLASS (self)->handle_update_enabled_roles != NULL)
    KLASS (self)->handle_update_enabled_roles (self);
}

void
shell_gdm_auth_services_add_credential_manager (ShellGdmAuthServices   *self,
                                                const char             *service_name,
                                                ShellCredentialManager *credential_manager)
{
  g_return_if_fail (SHELL_IS_GDM_AUTH_SERVICES (self));

  if (KLASS (self)->add_credential_manager != NULL)
    KLASS (self)->add_credential_manager (self, service_name, credential_manager);
}

void
shell_gdm_auth_services_remove_credential_manager (ShellGdmAuthServices *self,
                                                   const char           *service_name)
{
  g_return_if_fail (SHELL_IS_GDM_AUTH_SERVICES (self));

  if (KLASS (self)->remove_credential_manager != NULL)
    KLASS (self)->remove_credential_manager (self, service_name);
}

/* ---- mechanisms ---- */

/* Asks the subclass for the mechanisms, applies their roles, and tells the verifier */
void
shell_gdm_auth_services_update_enabled_mechanisms (ShellGdmAuthServices *self)
{
  g_autoptr (GPtrArray) mapped = g_ptr_array_new_with_free_func ((GDestroyNotify) shell_gdm_auth_mechanism_unref);
  guint i;

  g_clear_pointer (&self->selected_mechanism, shell_gdm_auth_mechanism_unref);
  g_ptr_array_set_size (self->enabled_mechanisms, 0);

  if (KLASS (self)->handle_update_enabled_mechanisms != NULL)
    KLASS (self)->handle_update_enabled_mechanisms (self);

  /* The mechanisms are copied with the properties of their roles; the one
   * the subclass selected is still the original. */
  for (i = 0; i < self->enabled_mechanisms->len; i++)
    {
      ShellGdmAuthMechanism *copy;

      copy = shell_gdm_auth_mechanism_copy (g_ptr_array_index (self->enabled_mechanisms, i));
      shell_gdm_auth_mechanism_apply_role_properties (copy);
      g_ptr_array_add (mapped, copy);
    }

  g_ptr_array_set_size (self->enabled_mechanisms, 0);
  for (i = 0; i < mapped->len; i++)
    g_ptr_array_add (self->enabled_mechanisms, shell_gdm_auth_mechanism_ref (g_ptr_array_index (mapped, i)));

  shell_gdm_auth_services_emit_mechanisms_changed (self);
}

/* ---- failures ---- */

gboolean
shell_gdm_auth_services_can_retry (ShellGdmAuthServices *self)
{
  return self->user_name != NULL && self->user_name[0] != '\0' &&
         (self->reauth_only || self->fail_counter < self->allowed_failures);
}

typedef struct
{
  ShellGdmAuthServices *self;
  GTask *task;
  guint timeout_id;
  GCancellable *cancellable;    /* the cancellable of the services when it started waiting */
  gboolean soft_reset;
  gboolean returned;
} WaitData;

static void
wait_data_free (WaitData *wait)
{
  g_clear_handle_id (&wait->timeout_id, g_source_remove);
  g_clear_object (&wait->task);
  g_clear_object (&wait->cancellable);
  g_free (wait);
}

static gboolean
on_wait_timeout (gpointer user_data)
{
  WaitData *wait = user_data;

  wait->timeout_id = 0;
  if (wait->cancellable != NULL)
    g_cancellable_cancel (wait->cancellable);

  return G_SOURCE_REMOVE;
}

static void
wait_done (GObject      *source,
           GAsyncResult *res,
           gpointer      user_data)
{
  WaitData *wait = user_data;
  ShellGdmAuthServices *self = SHELL_GDM_AUTH_SERVICES (source);
  g_autoptr (ShellGdmAuthServices) keep = g_object_ref (self);
  g_autoptr (GError) error = NULL;
  gboolean soft_reset = wait->soft_reset;

  self->pending_waits = g_list_remove (self->pending_waits, wait);

  if (!g_task_propagate_boolean (G_TASK (res), &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Waiting for the pending messages failed: %s", error->message);
      wait_data_free (wait);
      return;
    }

  wait_data_free (wait);

  shell_gdm_auth_services_emit_reset (self, soft_reset, FALSE);
}

void
shell_gdm_auth_services_verification_failed (ShellGdmAuthServices *self,
                                             const char           *service_name,
                                             gboolean              should_retry)
{
  g_autoptr (ShellGdmAuthServices) keep = g_object_ref (self);
  WaitData *wait;
  gboolean done_trying;

  if (KLASS (self)->handle_verification_failed != NULL)
    KLASS (self)->handle_verification_failed (self, service_name);

  done_trying = !should_retry || !shell_gdm_auth_services_can_retry (self);

  g_signal_emit (self, signals[VERIFICATION_FAILED], 0, service_name, !done_trying);

  if (self->destroyed)
    return;

  wait = g_new0 (WaitData, 1);
  wait->self = self;
  wait->soft_reset = !done_trying;
  g_set_object (&wait->cancellable, self->cancellable);
  wait->timeout_id = g_timeout_add_seconds (WAIT_PENDING_MESSAGES_TIMEOUT_SECONDS,
                                            on_wait_timeout, wait);
  g_source_set_name_by_id (wait->timeout_id, "[gnome-shell] wait for the pending messages");

  wait->task = g_task_new (self, NULL, wait_done, wait);
  g_task_set_source_tag (wait->task, shell_gdm_auth_services_verification_failed);
  self->pending_waits = g_list_prepend (self->pending_waits, wait);

  /* The verifier returns the task when the pending messages have been shown */
  g_signal_emit (self, signals[WAIT_PENDING_MESSAGES], 0, wait->task);
}

/* The tasks hold a reference on the services: return them when they are done with */
static void
cancel_pending_waits (ShellGdmAuthServices *self)
{
  g_autoptr (GList) waits = g_list_copy (self->pending_waits);
  GList *l;

  for (l = waits; l != NULL; l = l->next)
    {
      WaitData *wait = l->data;
      g_autoptr (GTask) task = NULL;

      /* The callback of the task frees the wait data, and a second return
       * would be a critical for the verifier */
      if (wait->returned || g_task_get_completed (wait->task))
        continue;

      wait->returned = TRUE;
      g_clear_handle_id (&wait->timeout_id, g_source_remove);

      task = g_object_ref (wait->task);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                               "Operation was cancelled");
    }
}

/* ---- GObject ---- */

static void
shell_gdm_auth_services_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  ShellGdmAuthServices *self = SHELL_GDM_AUTH_SERVICES (object);

  switch (prop_id)
    {
    case PROP_SETTINGS:
      g_set_object (&self->settings, g_value_get_object (value));
      break;

    case PROP_ALLOWED_FAILURES:
      self->allowed_failures = g_value_get_int (value);
      break;

    case PROP_REAUTH_ONLY:
      self->reauth_only = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_gdm_auth_services_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  ShellGdmAuthServices *self = SHELL_GDM_AUTH_SERVICES (object);

  switch (prop_id)
    {
    case PROP_SETTINGS:
      g_value_set_object (value, self->settings);
      break;

    case PROP_ALLOWED_FAILURES:
      g_value_set_int (value, self->allowed_failures);
      break;

    case PROP_REAUTH_ONLY:
      g_value_set_boolean (value, self->reauth_only);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_gdm_auth_services_constructed (GObject *object)
{
  ShellGdmAuthServices *self = SHELL_GDM_AUTH_SERVICES (object);
  const char * const *roles;
  guint i;

  G_OBJECT_CLASS (shell_gdm_auth_services_parent_class)->constructed (object);

  roles = shell_gdm_auth_services_get_supported_roles (self);
  for (i = 0; roles != NULL && roles[i] != NULL; i++)
    g_ptr_array_add (self->enabled_roles, (gpointer) roles[i]);
}

static void
shell_gdm_auth_services_dispose (GObject *object)
{
  ShellGdmAuthServices *self = SHELL_GDM_AUTH_SERVICES (object);

  self->destroyed = TRUE;

  cancel_pending_waits (self);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  clear_user_verifier (self);

  g_clear_pointer (&self->selected_mechanism, shell_gdm_auth_mechanism_unref);
  g_clear_pointer (&self->enabled_mechanisms, g_ptr_array_unref);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (shell_gdm_auth_services_parent_class)->dispose (object);
}

static void
shell_gdm_auth_services_finalize (GObject *object)
{
  ShellGdmAuthServices *self = SHELL_GDM_AUTH_SERVICES (object);

  g_clear_pointer (&self->enabled_roles, g_ptr_array_unref);
  g_clear_pointer (&self->active_services, g_hash_table_unref);
  g_clear_pointer (&self->unavailable_services, g_hash_table_unref);
  g_clear_pointer (&self->user_name, g_free);

  G_OBJECT_CLASS (shell_gdm_auth_services_parent_class)->finalize (object);
}

static void
shell_gdm_auth_services_class_init (ShellGdmAuthServicesClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = shell_gdm_auth_services_set_property;
  object_class->get_property = shell_gdm_auth_services_get_property;
  object_class->constructed = shell_gdm_auth_services_constructed;
  object_class->dispose = shell_gdm_auth_services_dispose;
  object_class->finalize = shell_gdm_auth_services_finalize;

  props[PROP_SETTINGS] =
    g_param_spec_object ("settings", NULL, NULL,
                         G_TYPE_SETTINGS,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  props[PROP_ALLOWED_FAILURES] =
    g_param_spec_int ("allowed-failures", NULL, NULL,
                      0, G_MAXINT, 3,
                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  props[PROP_REAUTH_ONLY] =
    g_param_spec_boolean ("reauth-only", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  signals[DESTROY] =
    g_signal_new ("destroy", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /* (service-name, message (nullable), message-type, wiggle) */
  signals[QUEUE_MESSAGE] =
    g_signal_new ("queue-message", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN);

  signals[QUEUE_PRIORITY_MESSAGE] =
    g_signal_new ("queue-priority-message", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN);

  /* (GTask *): the verifier returns the task once the pending messages have been shown */
  signals[WAIT_PENDING_MESSAGES] =
    g_signal_new ("wait-pending-messages", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);

  signals[FILTER_MESSAGES] =
    g_signal_new ("filter-messages", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_STRING, G_TYPE_INT);

  signals[VERIFICATION_FAILED] =
    g_signal_new ("verification-failed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_STRING, G_TYPE_BOOLEAN);

  signals[VERIFICATION_COMPLETE] =
    g_signal_new ("verification-complete", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /* (service-name, question, secret, answer-handler) */
  signals[ASK_QUESTION] =
    g_signal_new ("ask-question", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, SHELL_TYPE_GDM_AUTH_HANDLER);

  /* (soft-reset, reuse-entry-text) */
  signals[RESET] =
    g_signal_new ("reset", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);

  /* (service-name, prompt-message, choice-list (nullable), choice-handler (nullable)) */
  signals[SHOW_CHOICE_LIST] =
    g_signal_new ("show-choice-list", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_VARIANT, SHELL_TYPE_GDM_AUTH_HANDLER);

  /* (service-name, label, handler) */
  signals[SHOW_BUTTON] =
    g_signal_new ("show-button", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  G_TYPE_STRING, G_TYPE_STRING, SHELL_TYPE_GDM_AUTH_HANDLER);

  signals[MECHANISMS_CHANGED] =
    g_signal_new ("mechanisms-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /* (service-name, message, url, code (nullable), done-label, done-handler) */
  signals[WEB_LOGIN] =
    g_signal_new ("web-login", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 6,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                  G_TYPE_STRING, SHELL_TYPE_GDM_AUTH_HANDLER);
}

static void
shell_gdm_auth_services_init (ShellGdmAuthServices *self)
{
  self->allowed_failures = 3;
  self->active_services = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->unavailable_services = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->enabled_roles = g_ptr_array_new ();
  self->enabled_mechanisms = g_ptr_array_new_with_free_func ((GDestroyNotify) shell_gdm_auth_mechanism_unref);
}
