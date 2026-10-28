/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * auth-services-sssd-switchable.c: The switchable authentication of SSSD
 * (password, smartcard, passkey and web login in one PAM service). Port of
 * js/gdm/authServicesSSSDSwitchable.js.
 *
 * SSSD describes the mechanisms of the user in a JSON request of the custom
 * JSON interface of GDM, and gets the answers in the same format. The
 * mechanisms keep the JSON object SSSD sent (the keys of the JS objects:
 * prompt, certificates, pinPrompt, initInstruction, ...).
 *
 * Differences from the JS module: a request without mechanisms ends in a
 * ::mechanisms-changed with no mechanism, so the legacy services take over (the
 * JS threw a TypeError before telling anybody); the pin attempts and the
 * timeout of the web login are read as numbers of any kind; the answers are
 * sent with the proxies' asynchronous calls.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n-lib.h>

#include "gdm/auth-services-sssd-switchable.h"
#include "gdm/fido2-token-manager.h"
#include "gdm/settings.h"
#include "gdm/smartcard-manager.h"

typedef enum
{
  MECHANISMS_STATUS_WAITING,
  MECHANISMS_STATUS_NOT_FOUND,
  MECHANISMS_STATUS_FOUND,
} MechanismsStatus;

typedef enum
{
  PROMPT_STATUS_NONE,
  PROMPT_STATUS_PASSWORD_PROMPT,
  PROMPT_STATUS_PASSWORD_WAITING,
  PROMPT_STATUS_CERT_LIST_PROMPT,
  PROMPT_STATUS_PIN_PROMPT,
  PROMPT_STATUS_PIN_WAITING,
  PROMPT_STATUS_INSERT_KEY_PROMPT,
  PROMPT_STATUS_TOUCH_PROMPT,
  PROMPT_STATUS_WEB_LOGIN_INTRO_PROMPT,
  PROMPT_STATUS_WEB_LOGIN_DIALOG_PROMPT,
  PROMPT_STATUS_WEB_LOGIN_DIALOG_WAITING,
} PromptStatus;

struct _ShellGdmAuthServicesSSSDSwitchable
{
  ShellGdmAuthServices parent;

  ShellSmartcardManager *smartcard_manager;
  ShellFido2TokenManager *fido2_token_manager;

  MechanismsStatus mechanisms_status;
  PromptStatus prompt_status;

  JsonObject *mechanisms;               /* nullable */
  JsonArray *priority_list;             /* nullable */
  ShellGdmAuthMechanism *saved_mechanism;   /* nullable */
  JsonObject *selected_smartcard;       /* nullable, a certificate of the selected mechanism */

  gboolean resetting_password;
  guint web_login_timeout_id;
};

G_DEFINE_FINAL_TYPE (ShellGdmAuthServicesSSSDSwitchable, shell_gdm_auth_services_sssd_switchable,
                     SHELL_TYPE_GDM_AUTH_SERVICES)

static const char * const supported_roles[] = {
  SHELL_GDM_ROLE_PASSWORD,
  SHELL_GDM_ROLE_SMARTCARD,
  SHELL_GDM_ROLE_PASSKEY,
  SHELL_GDM_ROLE_WEB_LOGIN,
  NULL
};

static const char * const no_roles[] = { NULL };

static const char *
sssd_role_to_service (const char *role)
{
  if (g_strcmp0 (role, SHELL_GDM_ROLE_PASSWORD) == 0 ||
      g_strcmp0 (role, SHELL_GDM_ROLE_SMARTCARD) == 0 ||
      g_strcmp0 (role, SHELL_GDM_ROLE_PASSKEY) == 0 ||
      g_strcmp0 (role, SHELL_GDM_ROLE_WEB_LOGIN) == 0)
    return SHELL_GDM_SWITCHABLE_AUTH_SERVICE_NAME;

  return NULL;
}

gboolean
shell_gdm_auth_services_sssd_switchable_is_enabled (GSettings *settings)
{
  return g_settings_get_boolean (settings, SHELL_GDM_SWITCHABLE_AUTHENTICATION_KEY);
}

gboolean
shell_gdm_auth_services_sssd_switchable_has_service (const char *service_name)
{
  return g_strcmp0 (service_name, SHELL_GDM_SWITCHABLE_AUTH_SERVICE_NAME) == 0;
}

/* ---- JSON helpers: a missing or mistyped member is just not there ---- */

static JsonNode *
json_member (JsonObject *object,
             const char *name)
{
  if (object == NULL)
    return NULL;

  return json_object_get_member (object, name);
}

static const char *
json_get_string (JsonObject *object,
                 const char *name)
{
  JsonNode *node = json_member (object, name);

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;

  return json_node_get_string (node);
}

static gboolean
json_get_boolean (JsonObject *object,
                  const char *name)
{
  JsonNode *node = json_member (object, name);

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_BOOLEAN)
    return FALSE;

  return json_node_get_boolean (node);
}

static double
json_get_number (JsonObject *object,
                 const char *name)
{
  JsonNode *node = json_member (object, name);

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
    return 0;

  if (json_node_get_value_type (node) == G_TYPE_INT64 ||
      json_node_get_value_type (node) == G_TYPE_DOUBLE)
    return json_node_get_double (node);

  return 0;
}

static JsonArray *
json_get_array (JsonObject *object,
                const char *name)
{
  JsonNode *node = json_member (object, name);

  if (node == NULL || !JSON_NODE_HOLDS_ARRAY (node))
    return NULL;

  return json_node_get_array (node);
}

static const char *
mechanism_string (const ShellGdmAuthMechanism *mechanism,
                  const char                  *name)
{
  return mechanism != NULL ? json_get_string (mechanism->json, name) : NULL;
}

static gboolean
is_selected_service (ShellGdmAuthServices *services,
                     const char           *service_name)
{
  return services->selected_mechanism != NULL &&
         g_strcmp0 (service_name, services->selected_mechanism->service_name) == 0;
}

static gboolean
selected_has_role (ShellGdmAuthServices *services,
                   const char           *role)
{
  return services->selected_mechanism != NULL &&
         g_strcmp0 (services->selected_mechanism->role, role) == 0;
}

static gboolean
has_enabled_mechanism_with_role (ShellGdmAuthServices *services,
                                 const char           *role)
{
  guint i;

  for (i = 0; i < services->enabled_mechanisms->len; i++)
    {
      ShellGdmAuthMechanism *mechanism = g_ptr_array_index (services->enabled_mechanisms, i);

      if (g_strcmp0 (mechanism->role, role) == 0)
        return TRUE;
    }

  return FALSE;
}

/* ---- responses ---- */

/* The JSON SSSD gets for @answer: {authSelection: {status: "Ok", <id>: <response>}} */
static char *
format_response (ShellGdmAuthServicesSSSDSwitchable *self,
                 const char                         *answer)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);
  const ShellGdmAuthMechanism *mechanism = services->selected_mechanism;
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  const char *role = mechanism->role;

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "authSelection");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "status");
  json_builder_add_string_value (builder, "Ok");
  json_builder_set_member_name (builder, mechanism->id != NULL ? mechanism->id : "undefined");
  json_builder_begin_object (builder);

  if (g_strcmp0 (role, SHELL_GDM_ROLE_PASSWORD) == 0)
    {
      json_builder_set_member_name (builder, "password");
      json_builder_add_string_value (builder, answer != NULL ? answer : "");
    }
  else if (g_strcmp0 (role, SHELL_GDM_ROLE_SMARTCARD) == 0)
    {
      static const char * const names[] = { "tokenName", "moduleName", "keyId", "label" };
      guint i;

      json_builder_set_member_name (builder, "pin");
      json_builder_add_string_value (builder, answer != NULL ? answer : "");

      for (i = 0; i < G_N_ELEMENTS (names); i++)
        {
          JsonNode *node = json_member (self->selected_smartcard, names[i]);

          if (node == NULL)
            continue;

          json_builder_set_member_name (builder, names[i]);
          json_builder_add_value (builder, json_node_copy (node));
        }
    }
  else if (g_strcmp0 (role, SHELL_GDM_ROLE_PASSKEY) == 0)
    {
      static const char * const names[] = { "kerberos", "cryptoChallenge" };
      guint i;

      json_builder_set_member_name (builder, "pin");
      json_builder_add_string_value (builder, answer != NULL ? answer : "");

      for (i = 0; i < G_N_ELEMENTS (names); i++)
        {
          JsonNode *node = json_member (mechanism->json, names[i]);

          if (node == NULL)
            continue;

          json_builder_set_member_name (builder, names[i]);
          json_builder_add_value (builder, json_node_copy (node));
        }
    }
  else if (g_strcmp0 (role, SHELL_GDM_ROLE_WEB_LOGIN) == 0)
    {
      /* An empty response */
    }
  else
    {
      g_warning ("%s: unknown role '%s'", G_STRFUNC, role);
    }

  json_builder_end_object (builder);
  json_builder_end_object (builder);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);

  return json_generator_to_data (generator, NULL);
}

static void
send_response (ShellGdmAuthServicesSSSDSwitchable *self,
               const char                         *answer)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);
  g_autofree char *json = NULL;

  if (services->selected_mechanism == NULL)
    return;

  json = format_response (self, answer);
  shell_gdm_auth_services_send_custom_json (services,
                                            services->selected_mechanism->service_name,
                                            json);
}

static void
clear_web_login_timeout (ShellGdmAuthServicesSSSDSwitchable *self)
{
  g_clear_handle_id (&self->web_login_timeout_id, g_source_remove);
}

/* ---- starting the logins ---- */

static void
ask_secret (ShellGdmAuthServices *services,
            const char           *service_name,
            const char           *question)
{
  shell_gdm_auth_services_emit_ask_question (services, service_name,
                                             question != NULL ? question : "", TRUE);
}

static void
start_password_login (ShellGdmAuthServicesSSSDSwitchable *self)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);
  g_autofree char *service_name = g_strdup (services->selected_mechanism->service_name);
  g_autofree char *prompt = g_strdup (mechanism_string (services->selected_mechanism, "prompt"));

  self->prompt_status = PROMPT_STATUS_PASSWORD_PROMPT;
  ask_secret (services, service_name, prompt);
}

/* Currently sssd can't split cert data in a more granular way
 * so it's parsed manually here */
static GVariant *
parse_cert_instruction (const char *cert_instruction)
{
  GVariantBuilder builder;
  g_auto (GStrv) lines = NULL;
  g_auto (GStrv) fields = NULL;
  const char *description = NULL;
  const char *subject = NULL;
  g_autofree char *common_name = NULL;
  g_autofree char *organization = NULL;
  guint i;

  if (cert_instruction != NULL)
    {
      lines = g_strsplit (cert_instruction, "\n", -1);
      description = lines[0];
      subject = lines[0] != NULL ? lines[1] : NULL;
    }

  if (subject != NULL)
    {
      fields = g_strsplit (subject, ",", -1);
      for (i = 0; fields[i] != NULL; i++)
        {
          g_strstrip (fields[i]);

          if (common_name == NULL && g_str_has_prefix (fields[i], "CN="))
            common_name = g_strdup (fields[i] + 3);
          if (organization == NULL && g_str_has_prefix (fields[i], "O="))
            organization = g_strdup (fields[i] + 2);
        }
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (common_name != NULL)
    g_variant_builder_add (&builder, "{sv}", "title", g_variant_new_string (common_name));
  if (description != NULL)
    g_variant_builder_add (&builder, "{sv}", "subtitle", g_variant_new_string (description));
  if (organization != NULL)
    {
      g_variant_builder_add (&builder, "{sv}", "iconName",
                             g_variant_new_string ("vcard-symbolic"));
      g_variant_builder_add (&builder, "{sv}", "iconTitle",
                             g_variant_new_string (_("Organization")));
      g_variant_builder_add (&builder, "{sv}", "iconSubtitle",
                             g_variant_new_string (organization));
    }

  return g_variant_builder_end (&builder);
}

static void
start_smartcard_login (ShellGdmAuthServicesSSSDSwitchable *self);

static void
smartcard_choice_handler (ShellGdmAuthServices *services,
                          const char           *service_name,
                          const char           *key,
                          gpointer              user_data)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);
  JsonArray *certificates;
  guint i;

  if (!is_selected_service (services, service_name))
    return;

  if (!selected_has_role (services, SHELL_GDM_ROLE_SMARTCARD))
    return;

  certificates = json_get_array (services->selected_mechanism->json, "certificates");
  for (i = 0; certificates != NULL && i < json_array_get_length (certificates); i++)
    {
      JsonObject *cert = json_array_get_object_element (certificates, i);

      if (cert != NULL && g_strcmp0 (json_get_string (cert, "keyId"), key) == 0)
        {
          g_autofree char *service = g_strdup (service_name);
          g_autofree char *pin_prompt = g_strdup (json_get_string (cert, "pinPrompt"));

          g_clear_pointer (&self->selected_smartcard, json_object_unref);
          self->selected_smartcard = json_object_ref (cert);
          self->prompt_status = PROMPT_STATUS_PIN_PROMPT;
          ask_secret (services, service, pin_prompt);
          return;
        }
    }
}

static void
start_smartcard_login (ShellGdmAuthServicesSSSDSwitchable *self)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);
  g_autofree char *service_name = g_strdup (services->selected_mechanism->service_name);
  JsonArray *certificates = json_get_array (services->selected_mechanism->json, "certificates");
  guint n_certificates = certificates != NULL ? json_array_get_length (certificates) : 0;
  g_autoptr (ShellGdmAuthHandler) handler = NULL;
  GVariantBuilder builder;
  GVariant *choice_list;
  const char *prompt_message;
  guint i;

  if (n_certificates == 1)
    {
      JsonObject *cert = json_array_get_object_element (certificates, 0);
      g_autofree char *pin_prompt = g_strdup (json_get_string (cert, "pinPrompt"));

      g_clear_pointer (&self->selected_smartcard, json_object_unref);
      if (cert != NULL)
        self->selected_smartcard = json_object_ref (cert);

      self->prompt_status = PROMPT_STATUS_PIN_PROMPT;
      ask_secret (services, service_name, pin_prompt);
      return;
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sa{sv})"));
  for (i = 0; i < n_certificates; i++)
    {
      JsonObject *cert = json_array_get_object_element (certificates, i);
      const char *key_id = json_get_string (cert, "keyId");

      if (key_id == NULL)
        continue;

      g_variant_builder_add (&builder, "(s@a{sv})", key_id,
                             parse_cert_instruction (json_get_string (cert, "certInstruction")));
    }
  choice_list = g_variant_ref_sink (g_variant_builder_end (&builder));

  prompt_message = n_certificates == 0 ? _("Insert Smartcard") : _("Select Identity");

  self->prompt_status = PROMPT_STATUS_CERT_LIST_PROMPT;

  handler = shell_gdm_auth_services_new_handler (services, service_name,
                                                 smartcard_choice_handler, NULL, NULL);
  shell_gdm_auth_services_emit_show_choice_list (services, service_name, prompt_message,
                                                 choice_list, handler);
  g_variant_unref (choice_list);
}

static void
start_fido2_token_login (ShellGdmAuthServicesSSSDSwitchable *self)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);
  g_autofree char *service_name = g_strdup (services->selected_mechanism->service_name);
  g_autofree char *init_instruction = g_strdup (mechanism_string (services->selected_mechanism,
                                                                  "initInstruction"));
  g_autofree char *pin_prompt = g_strdup (mechanism_string (services->selected_mechanism,
                                                            "pinPrompt"));
  gboolean key_connected = json_get_boolean (services->selected_mechanism->json, "keyConnected");
  double pin_attempts = json_get_number (services->selected_mechanism->json, "pinAttempts");

  if (!key_connected)
    {
      self->prompt_status = PROMPT_STATUS_INSERT_KEY_PROMPT;
      shell_gdm_auth_services_emit_show_choice_list (services, service_name,
                                                     init_instruction, NULL, NULL);
      return;
    }

  self->prompt_status = PROMPT_STATUS_PIN_PROMPT;
  ask_secret (services, service_name, pin_prompt);

  if (pin_attempts <= 3 && pin_attempts > 0)
    {
      g_autofree char *message =
        g_strdup_printf (_("You have %d attempts left. If the passkey gets locked, you may not able to access your account."),
                         (int) pin_attempts);

      shell_gdm_auth_services_emit_queue_message (services, service_name, message,
                                                  SHELL_GDM_MESSAGE_TYPE_INFO, FALSE);
    }
}

typedef struct
{
  char *service_name;
  char *link_prompt;
  char *uri;
  char *code;
} WebLoginData;

static void
web_login_data_free (gpointer data)
{
  WebLoginData *web_login = data;

  g_free (web_login->service_name);
  g_free (web_login->link_prompt);
  g_free (web_login->uri);
  g_free (web_login->code);
  g_free (web_login);
}

static void
web_login_done_handler (ShellGdmAuthServices *services,
                        const char           *service_name,
                        const char           *argument,
                        gpointer              user_data)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);

  if (!selected_has_role (services, SHELL_GDM_ROLE_WEB_LOGIN))
    return;

  self->prompt_status = PROMPT_STATUS_WEB_LOGIN_DIALOG_WAITING;

  send_response (self, NULL);

  clear_web_login_timeout (self);
}

static void
show_web_login (ShellGdmAuthServicesSSSDSwitchable *self,
                const WebLoginData                 *web_login)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);
  g_autoptr (ShellGdmAuthHandler) done_handler = NULL;

  self->prompt_status = PROMPT_STATUS_WEB_LOGIN_DIALOG_PROMPT;

  done_handler = shell_gdm_auth_services_new_handler (services, web_login->service_name,
                                                      web_login_done_handler, NULL, NULL);
  shell_gdm_auth_services_emit_web_login (services,
                                          web_login->service_name,
                                          web_login->link_prompt,
                                          web_login->uri,
                                          web_login->code,
                                          _("Done"),
                                          done_handler);
}

static void
web_login_intro_handler (ShellGdmAuthServices *services,
                         const char           *service_name,
                         const char           *argument,
                         gpointer              user_data)
{
  show_web_login (SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services), user_data);
}

static void
start_web_login (ShellGdmAuthServicesSSSDSwitchable *self)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);
  const ShellGdmAuthMechanism *mechanism = services->selected_mechanism;
  const char *init_prompt = mechanism_string (mechanism, "initPrompt");
  const char *link_prompt = mechanism_string (mechanism, "linkPrompt");
  const char *uri = mechanism_string (mechanism, "uri");
  WebLoginData *web_login;

  if (link_prompt == NULL || link_prompt[0] == '\0' || uri == NULL || uri[0] == '\0')
    return;

  if (mechanism->needs_refresh)
    {
      shell_gdm_auth_services_emit_reset (services, TRUE, FALSE);
      return;
    }

  web_login = g_new0 (WebLoginData, 1);
  web_login->service_name = g_strdup (mechanism->service_name);
  web_login->link_prompt = g_strdup (link_prompt);
  web_login->uri = g_strdup (uri);
  web_login->code = g_strdup (mechanism_string (mechanism, "code"));

  if (init_prompt != NULL && init_prompt[0] != '\0')
    {
      g_autoptr (ShellGdmAuthHandler) handler = NULL;
      g_autofree char *label = g_strdup (init_prompt);
      g_autofree char *service_name = g_strdup (mechanism->service_name);

      self->prompt_status = PROMPT_STATUS_WEB_LOGIN_INTRO_PROMPT;

      handler = shell_gdm_auth_services_new_handler (services, service_name,
                                                     web_login_intro_handler,
                                                     web_login, web_login_data_free);
      shell_gdm_auth_services_emit_show_button (services, service_name, label, handler);
    }
  else
    {
      show_web_login (self, web_login);
      web_login_data_free (web_login);
    }
}

/* ---- virtual functions ---- */

static gboolean
sssd_handle_begin_verification_error (ShellGdmAuthServices *services,
                                      const GError         *error)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);

  if (g_strcmp0 (shell_gdm_init_error_get_service_name (error),
                 SHELL_GDM_SWITCHABLE_AUTH_SERVICE_NAME) != 0)
    return FALSE;

  self->mechanisms_status = MECHANISMS_STATUS_NOT_FOUND;
  shell_gdm_auth_services_emit_mechanisms_changed (services);

  return TRUE;
}

static void
sssd_handle_answer_query (ShellGdmAuthServices *services,
                          const char           *service_name,
                          const char           *answer)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);
  g_autofree char *service = NULL;
  g_autofree char *touch_instruction = NULL;

  if (!is_selected_service (services, service_name))
    return;

  if (selected_has_role (services, SHELL_GDM_ROLE_PASSWORD) && self->resetting_password)
    {
      shell_gdm_auth_services_send_answer (services, service_name, answer);
      return;
    }

  service = g_strdup (service_name);

  if (selected_has_role (services, SHELL_GDM_ROLE_PASSWORD))
    {
      send_response (self, answer);
      self->prompt_status = PROMPT_STATUS_PASSWORD_WAITING;
    }
  else if (selected_has_role (services, SHELL_GDM_ROLE_SMARTCARD))
    {
      send_response (self, answer);
      self->prompt_status = PROMPT_STATUS_PIN_WAITING;
    }
  else if (selected_has_role (services, SHELL_GDM_ROLE_PASSKEY))
    {
      touch_instruction = g_strdup (mechanism_string (services->selected_mechanism,
                                                      "touchInstruction"));
      send_response (self, answer);

      self->prompt_status = PROMPT_STATUS_TOUCH_PROMPT;
      shell_gdm_auth_services_emit_show_choice_list (services, service,
                                                     touch_instruction, NULL, NULL);
    }
}

static void
sssd_handle_select_mechanism (ShellGdmAuthServices *services)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);

  self->prompt_status = PROMPT_STATUS_NONE;

  if (services->selected_mechanism == NULL)
    return;

  if (selected_has_role (services, SHELL_GDM_ROLE_PASSWORD))
    start_password_login (self);
  else if (selected_has_role (services, SHELL_GDM_ROLE_SMARTCARD))
    start_smartcard_login (self);
  else if (selected_has_role (services, SHELL_GDM_ROLE_PASSKEY))
    start_fido2_token_login (self);
  else if (selected_has_role (services, SHELL_GDM_ROLE_WEB_LOGIN))
    start_web_login (self);
}

static const char * const *
sssd_get_supported_roles (ShellGdmAuthServices *services)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);

  /* When we couldn't get mechanisms (NOT_FOUND), we don't support any
   * role so they cascade to lower-priority authServices. */
  if (self->mechanisms_status == MECHANISMS_STATUS_NOT_FOUND)
    return no_roles;

  return supported_roles;
}

static void
sssd_handle_reset (ShellGdmAuthServices *services)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);

  g_clear_pointer (&self->saved_mechanism, shell_gdm_auth_mechanism_unref);
  self->mechanisms_status = MECHANISMS_STATUS_WAITING;
}

static void
sssd_handle_cancel (ShellGdmAuthServices *services)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);

  if (services->selected_mechanism != NULL)
    {
      g_clear_pointer (&self->saved_mechanism, shell_gdm_auth_mechanism_unref);
      self->saved_mechanism = shell_gdm_auth_mechanism_ref (services->selected_mechanism);
      self->mechanisms_status = MECHANISMS_STATUS_WAITING;
    }
}

static gboolean
sssd_cancel_requested (ShellGdmAuthServices *services)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);

  if (services->selected_mechanism == NULL)
    return FALSE;

  if (selected_has_role (services, SHELL_GDM_ROLE_PASSWORD))
    {
      /* Waiting for authentication response: soft reset */
      if (self->prompt_status == PROMPT_STATUS_PASSWORD_WAITING)
        {
          shell_gdm_auth_services_emit_reset (services, TRUE, FALSE);
          return TRUE;
        }

      /* At password prompt: hard reset */
      return FALSE;
    }
  else if (selected_has_role (services, SHELL_GDM_ROLE_SMARTCARD))
    {
      JsonArray *certificates;

      /* Waiting for PIN verification response: soft reset */
      if (self->prompt_status == PROMPT_STATUS_PIN_WAITING)
        {
          shell_gdm_auth_services_emit_reset (services, TRUE, FALSE);
          return TRUE;
        }

      /* At PIN entry after selecting from multiple certificates:
       * step back to the certificate list */
      certificates = json_get_array (services->selected_mechanism->json, "certificates");
      if (self->prompt_status == PROMPT_STATUS_PIN_PROMPT &&
          certificates != NULL && json_array_get_length (certificates) > 1)
        {
          g_clear_pointer (&self->selected_smartcard, json_object_unref);
          start_smartcard_login (self);
          return TRUE;
        }

      /* At certificate list or single-cert PIN: hard reset */
      return FALSE;
    }
  else if (selected_has_role (services, SHELL_GDM_ROLE_PASSKEY))
    {
      /* At touch confirmation after PIN submission: soft reset
       * back to PIN entry (PIN response was already sent to SSSD) */
      if (self->prompt_status == PROMPT_STATUS_TOUCH_PROMPT)
        {
          shell_gdm_auth_services_emit_reset (services, TRUE, FALSE);
          return TRUE;
        }

      /* At PIN entry or key insertion prompt: hard reset */
      return FALSE;
    }
  else if (selected_has_role (services, SHELL_GDM_ROLE_WEB_LOGIN))
    {
      /* At login dialog after "Done" clicked: soft reset to retry */
      if (self->prompt_status == PROMPT_STATUS_WEB_LOGIN_DIALOG_WAITING)
        {
          shell_gdm_auth_services_emit_reset (services, TRUE, FALSE);
          return TRUE;
        }

      /* At login dialog with a login button available:
       * step back to the login button */
      if (self->prompt_status == PROMPT_STATUS_WEB_LOGIN_DIALOG_PROMPT &&
          mechanism_string (services->selected_mechanism, "initPrompt") != NULL &&
          mechanism_string (services->selected_mechanism, "initPrompt")[0] != '\0')
        {
          start_web_login (self);
          return TRUE;
        }

      /* At login button or direct dialog (no initPrompt): hard reset */
      return FALSE;
    }

  return FALSE;
}

static void
sssd_handle_clear (ShellGdmAuthServices *services)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);

  g_clear_pointer (&self->mechanisms, json_object_unref);
  g_clear_pointer (&self->priority_list, json_array_unref);
  g_ptr_array_set_size (services->enabled_mechanisms, 0);
  g_clear_pointer (&services->selected_mechanism, shell_gdm_auth_mechanism_unref);
  g_clear_pointer (&self->selected_smartcard, json_object_unref);

  self->resetting_password = FALSE;
  self->prompt_status = PROMPT_STATUS_NONE;

  clear_web_login_timeout (self);
}

static void
sssd_handle_on_custom_json_request (ShellGdmAuthServices *services,
                                    const char           *service_name,
                                    const char           *protocol,
                                    guint                 version,
                                    const char           *json)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GError) error = NULL;
  JsonNode *root;
  JsonObject *auth_selection = NULL;
  JsonObject *mechanisms = NULL;
  JsonArray *priority = NULL;

  if (g_strcmp0 (service_name, SHELL_GDM_SWITCHABLE_AUTH_SERVICE_NAME) != 0)
    return;

  if (self->mechanisms_status != MECHANISMS_STATUS_WAITING)
    g_message ("Received unexpected JSON message, something might be wrong");

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, json, -1, &error))
    {
      g_message ("Could not parse the JSON request of the switchable authentication: %s",
                 error->message);
      return;
    }

  root = json_parser_get_root (parser);
  if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
    {
      JsonNode *node = json_member (json_node_get_object (root), "authSelection");

      if (node != NULL && JSON_NODE_HOLDS_OBJECT (node))
        auth_selection = json_node_get_object (node);
    }

  if (auth_selection != NULL)
    {
      JsonNode *node = json_member (auth_selection, "mechanisms");

      if (node != NULL && JSON_NODE_HOLDS_OBJECT (node))
        mechanisms = json_node_get_object (node);

      priority = json_get_array (auth_selection, "priority");
    }

  g_clear_pointer (&self->mechanisms, json_object_unref);
  g_clear_pointer (&self->priority_list, json_array_unref);
  self->mechanisms = mechanisms != NULL ? json_object_ref (mechanisms) : NULL;
  self->priority_list = priority != NULL ? json_array_ref (priority) : NULL;

  self->mechanisms_status = self->mechanisms != NULL
    ? MECHANISMS_STATUS_FOUND
    : MECHANISMS_STATUS_NOT_FOUND;

  shell_gdm_auth_services_update_enabled_mechanisms (services);
}

typedef struct
{
  ShellGdmAuthServicesSSSDSwitchable *self;
  ShellGdmAuthMechanism *mechanism;
} WebLoginTimeout;

static void
web_login_timeout_free (gpointer data)
{
  WebLoginTimeout *timeout = data;

  shell_gdm_auth_mechanism_unref (timeout->mechanism);
  g_free (timeout);
}

static gboolean
on_web_login_timeout (gpointer user_data)
{
  WebLoginTimeout *timeout = user_data;
  ShellGdmAuthServicesSSSDSwitchable *self = timeout->self;
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);

  self->web_login_timeout_id = 0;

  if (!selected_has_role (services, SHELL_GDM_ROLE_WEB_LOGIN))
    timeout->mechanism->needs_refresh = TRUE;
  else
    shell_gdm_auth_services_emit_reset (services, TRUE, FALSE);

  return G_SOURCE_REMOVE;
}

static void
track_web_login_timeout (ShellGdmAuthServicesSSSDSwitchable *self)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);
  ShellGdmAuthMechanism *web_login_mechanism = NULL;
  WebLoginTimeout *timeout;
  double seconds;
  guint i;

  clear_web_login_timeout (self);

  for (i = 0; i < services->enabled_mechanisms->len; i++)
    {
      ShellGdmAuthMechanism *mechanism = g_ptr_array_index (services->enabled_mechanisms, i);

      if (g_strcmp0 (mechanism->role, SHELL_GDM_ROLE_WEB_LOGIN) == 0)
        {
          web_login_mechanism = mechanism;
          break;
        }
    }

  if (web_login_mechanism == NULL)
    return;

  seconds = json_get_number (web_login_mechanism->json, "timeout");
  if (seconds < 1)
    return;

  timeout = g_new0 (WebLoginTimeout, 1);
  timeout->self = self;
  timeout->mechanism = shell_gdm_auth_mechanism_ref (web_login_mechanism);

  self->web_login_timeout_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                                           (guint) seconds,
                                                           on_web_login_timeout,
                                                           timeout,
                                                           web_login_timeout_free);
  g_source_set_name_by_id (self->web_login_timeout_id, "[gnome-shell] web login timeout");
}

static void
sssd_handle_update_enabled_mechanisms (ShellGdmAuthServices *services)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);
  ShellGdmAuthMechanism *selected = NULL;
  GList *ids, *l;
  guint i;

  ids = self->mechanisms != NULL ? json_object_get_members (self->mechanisms) : NULL;
  for (l = ids; l != NULL; l = l->next)
    {
      const char *id = l->data;
      JsonNode *node = json_object_get_member (self->mechanisms, id);
      JsonObject *object;
      ShellGdmAuthMechanism *mechanism;
      const char *role;

      if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
        continue;

      object = json_node_get_object (node);
      role = json_get_string (object, "role");

      /* filter out mechanisms with roles that are not enabled */
      if (role == NULL ||
          !shell_gdm_auth_services_mechanism_enabled_for_role (services, role))
        continue;

      mechanism = shell_gdm_auth_mechanism_new (SHELL_GDM_SWITCHABLE_AUTH_SERVICE_NAME, role);
      mechanism->id = g_strdup (id);
      mechanism->name = g_strdup (json_get_string (object, "name"));
      mechanism->json = json_object_ref (object);
      g_ptr_array_add (services->enabled_mechanisms, mechanism);
    }
  g_list_free (ids);

  track_web_login_timeout (self);

  if (self->saved_mechanism != NULL)
    {
      for (i = 0; i < services->enabled_mechanisms->len; i++)
        {
          ShellGdmAuthMechanism *mechanism = g_ptr_array_index (services->enabled_mechanisms, i);

          if (g_strcmp0 (mechanism->role, self->saved_mechanism->role) == 0)
            {
              selected = mechanism;
              break;
            }
        }
    }

  if (selected == NULL && self->priority_list != NULL &&
      json_array_get_length (self->priority_list) > 0)
    {
      const char *first = json_array_get_string_element (self->priority_list, 0);

      for (i = 0; first != NULL && i < services->enabled_mechanisms->len; i++)
        {
          ShellGdmAuthMechanism *mechanism = g_ptr_array_index (services->enabled_mechanisms, i);

          if (g_strcmp0 (mechanism->id, first) == 0)
            {
              selected = mechanism;
              break;
            }
        }
    }

  if (selected == NULL && services->enabled_mechanisms->len > 0)
    selected = g_ptr_array_index (services->enabled_mechanisms, 0);

  if (selected != NULL)
    {
      /* selecting can run the login of the mechanism, which can reset the services */
      shell_gdm_auth_mechanism_ref (selected);
      shell_gdm_auth_services_select_mechanism (services, selected);
      shell_gdm_auth_mechanism_unref (selected);
    }
  else
    {
      shell_gdm_auth_services_select_mechanism (services, NULL);
    }

  g_clear_pointer (&self->saved_mechanism, shell_gdm_auth_mechanism_unref);
}

static gboolean
event_expected (ShellGdmAuthServicesSSSDSwitchable *self)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);

  /* If legacy PAM messages are received before receiving JSON PAM
   * messages informing about mechanisms, then pam_unix is being
   * used and the user is not supported by pam_sss using JSON.
   * Fallback to legacy authentication services. */
  if (self->mechanisms_status == MECHANISMS_STATUS_WAITING)
    {
      self->mechanisms_status = MECHANISMS_STATUS_NOT_FOUND;
      shell_gdm_auth_services_emit_mechanisms_changed (services);
      return FALSE;
    }

  return TRUE;
}

static void
sssd_handle_on_info (ShellGdmAuthServices *services,
                     const char           *service_name,
                     const char           *info)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);

  if (g_strcmp0 (service_name, SHELL_GDM_SWITCHABLE_AUTH_SERVICE_NAME) != 0 ||
      !event_expected (self))
    return;

  /* sssd can't inform about expired password from JSON so it's needed
   * to check the info message and handle the reset using the old flow */
  if (is_selected_service (services, service_name) &&
      selected_has_role (services, SHELL_GDM_ROLE_PASSWORD) &&
      info != NULL && strstr (info, "Password expired. Change your password now") != NULL)
    self->resetting_password = TRUE;

  if (is_selected_service (services, service_name))
    {
      shell_gdm_auth_services_emit_queue_message (services, service_name, info,
                                                  SHELL_GDM_MESSAGE_TYPE_INFO, FALSE);
    }
}

static void
sssd_handle_on_problem (ShellGdmAuthServices *services,
                        const char           *service_name,
                        const char           *problem)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);

  if (g_strcmp0 (service_name, SHELL_GDM_SWITCHABLE_AUTH_SERVICE_NAME) != 0 ||
      !event_expected (self))
    return;

  if (is_selected_service (services, service_name))
    {
      shell_gdm_auth_services_emit_queue_priority_message (services, service_name, problem,
                                                           SHELL_GDM_MESSAGE_TYPE_ERROR, FALSE);
    }
}

static void
sssd_handle_on_info_query (ShellGdmAuthServices *services,
                           const char           *service_name,
                           const char           *question)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);

  if (g_strcmp0 (service_name, SHELL_GDM_SWITCHABLE_AUTH_SERVICE_NAME) != 0)
    return;

  event_expected (self);
}

static void
sssd_handle_on_secret_info_query (ShellGdmAuthServices *services,
                                  const char           *service_name,
                                  const char           *secret_question)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);

  if (g_strcmp0 (service_name, SHELL_GDM_SWITCHABLE_AUTH_SERVICE_NAME) != 0 ||
      !event_expected (self))
    return;

  if (is_selected_service (services, service_name) &&
      selected_has_role (services, SHELL_GDM_ROLE_PASSWORD) &&
      self->resetting_password)
    {
      ask_secret (services, service_name, secret_question);
    }
}

static void
sssd_handle_on_conversation_stopped (ShellGdmAuthServices *services,
                                     const char           *service_name)
{
  if (!is_selected_service (services, service_name))
    return;

  if (g_hash_table_contains (services->unavailable_services, service_name))
    return;

  services->fail_counter++;
  shell_gdm_auth_services_verification_failed (services, service_name, TRUE);
}

static void
sssd_handle_on_service_unavailable (ShellGdmAuthServices *services,
                                    const char           *service_name,
                                    const char           *error_message)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);

  if (g_strcmp0 (service_name, SHELL_GDM_SWITCHABLE_AUTH_SERVICE_NAME) != 0)
    return;

  self->mechanisms_status = MECHANISMS_STATUS_NOT_FOUND;
  shell_gdm_auth_services_emit_mechanisms_changed (services);
}

static gboolean
sssd_handle_can_start_service (ShellGdmAuthServices *services,
                               const char           *service_name)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (services);

  return g_strcmp0 (service_name, SHELL_GDM_SWITCHABLE_AUTH_SERVICE_NAME) == 0 &&
         self->mechanisms_status == MECHANISMS_STATUS_WAITING;
}

/* ---- the managers ---- */

static void
on_smartcard_changed (ShellSmartcardManager                *manager,
                      GDBusProxy                           *token,
                      ShellGdmAuthServicesSSSDSwitchable   *self)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);

  if (services->selected_mechanism == NULL ||
      !has_enabled_mechanism_with_role (services, SHELL_GDM_ROLE_SMARTCARD))
    return;

  shell_gdm_auth_services_emit_reset (services, TRUE, TRUE);
}

static void
on_fido2_token_changed (ShellFido2TokenManager              *manager,
                        GObject                             *device,
                        ShellGdmAuthServicesSSSDSwitchable  *self)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);

  if (services->selected_mechanism == NULL ||
      !has_enabled_mechanism_with_role (services, SHELL_GDM_ROLE_PASSKEY))
    return;

  shell_gdm_auth_services_emit_reset (services, TRUE, TRUE);
}

/* ---- GObject ---- */

static void
shell_gdm_auth_services_sssd_switchable_constructed (GObject *object)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (object);

  G_OBJECT_CLASS (shell_gdm_auth_services_sssd_switchable_parent_class)->constructed (object);

  self->smartcard_manager = shell_smartcard_manager_get_default ();
  g_signal_connect_object (self->smartcard_manager, "smartcard-inserted",
                           G_CALLBACK (on_smartcard_changed), self, 0);
  g_signal_connect_object (self->smartcard_manager, "smartcard-removed",
                           G_CALLBACK (on_smartcard_changed), self, 0);

  self->fido2_token_manager = shell_fido2_token_manager_get_default ();
  g_signal_connect_object (self->fido2_token_manager, "fido2-token-inserted",
                           G_CALLBACK (on_fido2_token_changed), self, 0);
  g_signal_connect_object (self->fido2_token_manager, "fido2-token-removed",
                           G_CALLBACK (on_fido2_token_changed), self, 0);
}

static void
shell_gdm_auth_services_sssd_switchable_dispose (GObject *object)
{
  ShellGdmAuthServicesSSSDSwitchable *self = SHELL_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (object);

  clear_web_login_timeout (self);
  g_clear_pointer (&self->mechanisms, json_object_unref);
  g_clear_pointer (&self->priority_list, json_array_unref);
  g_clear_pointer (&self->selected_smartcard, json_object_unref);
  g_clear_pointer (&self->saved_mechanism, shell_gdm_auth_mechanism_unref);

  G_OBJECT_CLASS (shell_gdm_auth_services_sssd_switchable_parent_class)->dispose (object);
}

static void
shell_gdm_auth_services_sssd_switchable_class_init (ShellGdmAuthServicesSSSDSwitchableClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellGdmAuthServicesClass *services_class = SHELL_GDM_AUTH_SERVICES_CLASS (klass);

  object_class->constructed = shell_gdm_auth_services_sssd_switchable_constructed;
  object_class->dispose = shell_gdm_auth_services_sssd_switchable_dispose;

  services_class->supported_roles = supported_roles;
  services_class->role_to_service = sssd_role_to_service;

  services_class->get_supported_roles = sssd_get_supported_roles;
  services_class->handle_begin_verification_error = sssd_handle_begin_verification_error;
  services_class->handle_select_mechanism = sssd_handle_select_mechanism;
  services_class->handle_reset = sssd_handle_reset;
  services_class->handle_cancel = sssd_handle_cancel;
  services_class->handle_clear = sssd_handle_clear;
  services_class->handle_update_enabled_mechanisms = sssd_handle_update_enabled_mechanisms;
  services_class->handle_on_info = sssd_handle_on_info;
  services_class->handle_on_problem = sssd_handle_on_problem;
  services_class->handle_on_info_query = sssd_handle_on_info_query;
  services_class->handle_on_secret_info_query = sssd_handle_on_secret_info_query;
  services_class->handle_on_conversation_stopped = sssd_handle_on_conversation_stopped;
  services_class->handle_on_service_unavailable = sssd_handle_on_service_unavailable;
  services_class->handle_on_custom_json_request = sssd_handle_on_custom_json_request;
  services_class->handle_can_start_service = sssd_handle_can_start_service;
  services_class->cancel_requested = sssd_cancel_requested;
  services_class->handle_answer_query = sssd_handle_answer_query;
}

static void
shell_gdm_auth_services_sssd_switchable_init (ShellGdmAuthServicesSSSDSwitchable *self)
{
  self->mechanisms_status = MECHANISMS_STATUS_WAITING;
}
