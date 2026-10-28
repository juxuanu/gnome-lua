/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * auth-services-legacy.c: The password, smartcard and fingerprint PAM
 * services of GDM. Port of js/gdm/authServicesLegacy.js.
 *
 * Differences from the JS module: the pending removal of the source that
 * fails the fingerprint verification is forgotten when it is removed (the JS
 * removed it a second time); the failure of the fingerprint is ignored if the
 * services were cleared meanwhile.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include "gdm/auth-services-legacy.h"
#include "gdm/fingerprint-manager.h"
#include "gdm/settings.h"
#include "gdm/smartcard-manager.h"

#define FINGERPRINT_ERROR_TIMEOUT_WAIT 15
#define FINGERPRINT_READY_TIMEOUT_MS 500

typedef struct
{
  char *service;
  ShellCredentialManager *manager;
  gulong handler_id;
} CredentialEntry;

struct _ShellGdmAuthServicesLegacy
{
  ShellGdmAuthServices parent;

  GPtrArray *credential_managers;   /* CredentialEntry, in the order they were added */
  ShellSmartcardManager *smartcard_manager;
  ShellFingerprintManager *fingerprint_manager;   /* nullable */

  gboolean smartcard_in_progress;
  guint fingerprint_failed_id;
  guint fingerprint_ready_timeout_id;
};

G_DEFINE_FINAL_TYPE (ShellGdmAuthServicesLegacy, shell_gdm_auth_services_legacy,
                     SHELL_TYPE_GDM_AUTH_SERVICES)

static const char * const supported_roles[] = {
  SHELL_GDM_ROLE_PASSWORD,
  SHELL_GDM_ROLE_SMARTCARD,
  SHELL_GDM_ROLE_FINGERPRINT,
  NULL
};

static const char *
legacy_role_to_service (const char *role)
{
  if (g_strcmp0 (role, SHELL_GDM_ROLE_PASSWORD) == 0)
    return SHELL_GDM_PASSWORD_SERVICE_NAME;
  if (g_strcmp0 (role, SHELL_GDM_ROLE_SMARTCARD) == 0)
    return SHELL_GDM_SMARTCARD_SERVICE_NAME;
  if (g_strcmp0 (role, SHELL_GDM_ROLE_FINGERPRINT) == 0)
    return SHELL_GDM_FINGERPRINT_SERVICE_NAME;

  return NULL;
}

gboolean
shell_gdm_auth_services_legacy_is_enabled (GSettings *settings)
{
  return g_settings_get_boolean (settings, SHELL_GDM_PASSWORD_AUTHENTICATION_KEY) ||
         g_settings_get_boolean (settings, SHELL_GDM_FINGERPRINT_AUTHENTICATION_KEY) ||
         g_settings_get_boolean (settings, SHELL_GDM_SMARTCARD_AUTHENTICATION_KEY);
}

gboolean
shell_gdm_auth_services_legacy_has_service (const char *service_name)
{
  return g_strcmp0 (service_name, SHELL_GDM_PASSWORD_SERVICE_NAME) == 0 ||
         g_strcmp0 (service_name, SHELL_GDM_SMARTCARD_SERVICE_NAME) == 0 ||
         g_strcmp0 (service_name, SHELL_GDM_FINGERPRINT_SERVICE_NAME) == 0;
}

/* ---- helpers ---- */

static const char *
selected_service_name (ShellGdmAuthServices *services)
{
  return services->selected_mechanism != NULL
    ? services->selected_mechanism->service_name
    : NULL;
}

static gboolean
is_selected_service (ShellGdmAuthServices *services,
                     const char           *service_name)
{
  return services->selected_mechanism != NULL &&
         g_strcmp0 (service_name, services->selected_mechanism->service_name) == 0;
}

static gboolean
has_enabled_mechanism_for_service (ShellGdmAuthServices *services,
                                   const char           *service_name)
{
  guint i;

  for (i = 0; i < services->enabled_mechanisms->len; i++)
    {
      ShellGdmAuthMechanism *mechanism = g_ptr_array_index (services->enabled_mechanisms, i);

      if (g_strcmp0 (mechanism->service_name, service_name) == 0)
        return TRUE;
    }

  return FALSE;
}

static ShellGdmAuthMechanism *
find_enabled_mechanism_for_role (ShellGdmAuthServices *services,
                                 const char           *role)
{
  guint i;

  for (i = 0; i < services->enabled_mechanisms->len; i++)
    {
      ShellGdmAuthMechanism *mechanism = g_ptr_array_index (services->enabled_mechanisms, i);

      if (g_strcmp0 (mechanism->role, role) == 0)
        return mechanism;
    }

  return NULL;
}

static CredentialEntry *
find_credential_entry (ShellGdmAuthServicesLegacy *self,
                       const char                 *service_name)
{
  guint i;

  if (service_name == NULL)
    return NULL;

  for (i = 0; i < self->credential_managers->len; i++)
    {
      CredentialEntry *entry = g_ptr_array_index (self->credential_managers, i);

      if (g_str_equal (entry->service, service_name))
        return entry;
    }

  return NULL;
}

static gboolean
has_any_credential_manager_token (ShellGdmAuthServicesLegacy *self)
{
  guint i;

  for (i = 0; i < self->credential_managers->len; i++)
    {
      CredentialEntry *entry = g_ptr_array_index (self->credential_managers, i);

      if (shell_credential_manager_get_token (entry->manager) != NULL)
        return TRUE;
    }

  return FALSE;
}

static void
clear_fingerprint_signal_handlers (ShellGdmAuthServicesLegacy *self)
{
  g_clear_handle_id (&self->fingerprint_failed_id, g_source_remove);
  g_clear_handle_id (&self->fingerprint_ready_timeout_id, g_source_remove);
}

static void
set_fingerprint_ready (ShellGdmAuthServicesLegacy *self,
                       gboolean                    ready)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);
  ShellGdmAuthMechanism *mechanism;
  ShellGdmReady value = ready ? SHELL_GDM_READY_YES : SHELL_GDM_READY_NO;

  mechanism = find_enabled_mechanism_for_role (services, SHELL_GDM_ROLE_FINGERPRINT);
  if (mechanism == NULL || mechanism->ready == value)
    return;

  mechanism->ready = value;

  shell_gdm_auth_services_emit_mechanisms_changed (services);
}

/* ---- callbacks of the managers ---- */

static void
on_credential_manager_authenticated (ShellCredentialManager     *credential_manager,
                                     const char                 *token,
                                     ShellGdmAuthServicesLegacy *self)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);

  g_clear_pointer (&services->selected_mechanism, shell_gdm_auth_mechanism_unref);
  services->selected_mechanism =
    shell_gdm_auth_mechanism_new (shell_credential_manager_get_service (credential_manager),
                                  SHELL_GDM_ROLE_PASSWORD);
  shell_gdm_auth_mechanism_apply_role_properties (services->selected_mechanism);

  shell_gdm_auth_services_emit_reset (services, TRUE, FALSE);
}

static void
on_smartcard_changed (ShellSmartcardManager     *manager,
                      GDBusProxy                *token,
                      ShellGdmAuthServicesLegacy *self)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);

  if (services->selected_mechanism == NULL ||
      g_strcmp0 (services->selected_mechanism->role, SHELL_GDM_ROLE_SMARTCARD) != 0 ||
      (self->smartcard_in_progress && shell_smartcard_manager_has_inserted_tokens (manager)))
    return;

  shell_gdm_auth_services_emit_reset (services, TRUE, FALSE);
}

static void
on_fingerprint_changed (ShellFingerprintManager    *manager,
                        ShellGdmAuthServicesLegacy *self)
{
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);

  if (!shell_gdm_auth_services_mechanism_enabled_for_role (services, SHELL_GDM_ROLE_FINGERPRINT))
    return;

  shell_gdm_auth_services_update_enabled_mechanisms (services);
  shell_gdm_auth_services_emit_reset (services, TRUE, TRUE);
}

/* ---- virtual functions ---- */

static void
legacy_handle_answer_query (ShellGdmAuthServices *services,
                            const char           *service_name,
                            const char           *answer)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);

  if (!is_selected_service (services, service_name))
    return;

  if (g_strcmp0 (services->selected_mechanism->role, SHELL_GDM_ROLE_SMARTCARD) == 0)
    self->smartcard_in_progress = TRUE;

  shell_gdm_auth_services_send_answer (services, service_name, answer);
}

static void
legacy_handle_begin_verification (ShellGdmAuthServices *services)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);

  if (self->fingerprint_manager != NULL)
    shell_fingerprint_manager_check_reader_type (self->fingerprint_manager,
                                                 services->cancellable);

  shell_gdm_auth_services_emit_mechanisms_changed (services);
}

static void
legacy_handle_select_mechanism (ShellGdmAuthServices *services)
{
  if (services->selected_mechanism != NULL)
    shell_gdm_auth_services_emit_reset (services, TRUE, FALSE);
}

static gboolean
legacy_handle_needs_username (ShellGdmAuthServices *services)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);
  ShellGdmAuthMechanism *first;

  /* Username won't be needed when there's only one mechanism and is
   * Smartcard, or if the selected mechanism is a credential manager */
  first = services->enabled_mechanisms->len == 1
    ? g_ptr_array_index (services->enabled_mechanisms, 0)
    : NULL;
  if (first != NULL && g_strcmp0 (first->role, SHELL_GDM_ROLE_SMARTCARD) == 0)
    return FALSE;

  if (find_credential_entry (self, selected_service_name (services)) != NULL)
    return FALSE;

  return TRUE;
}

static void
legacy_handle_reset (ShellGdmAuthServices *services)
{
  g_clear_pointer (&services->selected_mechanism, shell_gdm_auth_mechanism_unref);
}

static void
legacy_handle_clear (ShellGdmAuthServices *services)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);

  self->smartcard_in_progress = FALSE;
  clear_fingerprint_signal_handlers (self);
}

static void
on_fingerprint_ready_timeout (gpointer user_data)
{
  ShellGdmAuthServicesLegacy *self = user_data;

  self->fingerprint_ready_timeout_id = 0;
  set_fingerprint_ready (self, TRUE);
}

static void
legacy_handle_on_conversation_started (ShellGdmAuthServices *services,
                                       const char           *service_name)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);

  if (g_strcmp0 (service_name, SHELL_GDM_FINGERPRINT_SERVICE_NAME) != 0 ||
      self->fingerprint_ready_timeout_id != 0)
    return;

  self->fingerprint_ready_timeout_id =
    g_timeout_add_once (FINGERPRINT_READY_TIMEOUT_MS, on_fingerprint_ready_timeout, self);
  g_source_set_name_by_id (self->fingerprint_ready_timeout_id,
                           "[gnome-shell] fingerprint ready timeout");
}

static void
legacy_handle_update_enabled_roles (ShellGdmAuthServices *services)
{
  g_clear_pointer (&services->selected_mechanism, shell_gdm_auth_mechanism_unref);
  shell_gdm_auth_services_update_enabled_mechanisms (services);
}

static void
legacy_handle_update_enabled_mechanisms (ShellGdmAuthServices *services)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);
  const struct
  {
    const char *service_name;
    const char *role;
    const char *name;
    const char *setting;
  } mechanisms[] = {
    {
      SHELL_GDM_PASSWORD_SERVICE_NAME, SHELL_GDM_ROLE_PASSWORD,
      _("Password"), SHELL_GDM_PASSWORD_AUTHENTICATION_KEY,
    },
    {
      SHELL_GDM_SMARTCARD_SERVICE_NAME, SHELL_GDM_ROLE_SMARTCARD,
      _("Smartcard"), SHELL_GDM_SMARTCARD_AUTHENTICATION_KEY,
    },
    {
      SHELL_GDM_FINGERPRINT_SERVICE_NAME, SHELL_GDM_ROLE_FINGERPRINT,
      _("Fingerprint"), SHELL_GDM_FINGERPRINT_AUTHENTICATION_KEY,
    },
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (mechanisms); i++)
    {
      ShellGdmAuthMechanism *mechanism;

      if (!shell_gdm_auth_services_mechanism_enabled_for_role (services, mechanisms[i].role) ||
          !g_settings_get_boolean (services->settings, mechanisms[i].setting))
        continue;

      mechanism = shell_gdm_auth_mechanism_new (mechanisms[i].service_name,
                                                mechanisms[i].role);
      mechanism->name = g_strdup (mechanisms[i].name);
      g_ptr_array_add (services->enabled_mechanisms, mechanism);
    }

  if (self->fingerprint_manager == NULL ||
      !shell_fingerprint_manager_get_reader_found (self->fingerprint_manager))
    {
      for (i = services->enabled_mechanisms->len; i > 0; i--)
        {
          ShellGdmAuthMechanism *mechanism = g_ptr_array_index (services->enabled_mechanisms, i - 1);

          if (g_strcmp0 (mechanism->role, SHELL_GDM_ROLE_FINGERPRINT) == 0)
            g_ptr_array_remove_index (services->enabled_mechanisms, i - 1);
        }
    }
  else
    {
      /* Mark fingerprint as not ready until service confirms
       * it's working for this user */
      set_fingerprint_ready (self, FALSE);
    }
}

static void
legacy_handle_on_info (ShellGdmAuthServices *services,
                       const char           *service_name,
                       const char           *info)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);

  if (is_selected_service (services, service_name))
    {
      shell_gdm_auth_services_emit_queue_message (services, service_name, info,
                                                  SHELL_GDM_MESSAGE_TYPE_INFO, FALSE);
    }
  else if (g_strcmp0 (service_name, SHELL_GDM_FINGERPRINT_SERVICE_NAME) == 0 &&
           has_enabled_mechanism_for_service (services, service_name))
    {
      const char *message;

      /* We don't show fingerprint messages directly since it's
       * not the main auth service. Instead we use the messages
       * as a cue to display our own message. */
      if (self->fingerprint_manager != NULL &&
          shell_fingerprint_manager_get_reader_type (self->fingerprint_manager) ==
          SHELL_FINGERPRINT_READER_TYPE_SWIPE)
        {
          /* Translators: this message is shown below the password entry field
           * to indicate the user can swipe their finger on the fingerprint reader */
          message = _("(or swipe finger across reader)");
        }
      else
        {
          /* Translators: this message is shown below the password entry field
           * to indicate the user can place their finger on the fingerprint reader instead */
          message = _("(or place finger on reader)");
        }

      shell_gdm_auth_services_emit_queue_message (services, service_name, message,
                                                  SHELL_GDM_MESSAGE_TYPE_HINT, FALSE);
    }
}

static gboolean
on_fingerprint_failed (gpointer user_data)
{
  ShellGdmAuthServicesLegacy *self = user_data;
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);

  self->fingerprint_failed_id = 0;

  if (services->cancellable != NULL && !g_cancellable_is_cancelled (services->cancellable))
    {
      g_autoptr (ShellGdmAuthServicesLegacy) keep = g_object_ref (self);

      shell_gdm_auth_services_verification_failed (services,
                                                   SHELL_GDM_FINGERPRINT_SERVICE_NAME,
                                                   FALSE);
    }

  return G_SOURCE_REMOVE;
}

static void
legacy_handle_on_problem (ShellGdmAuthServices *services,
                          const char           *service_name,
                          const char           *problem)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);
  gboolean is_fingerprint =
    g_strcmp0 (service_name, SHELL_GDM_FINGERPRINT_SERVICE_NAME) == 0 &&
    has_enabled_mechanism_for_service (services, service_name);

  if (is_selected_service (services, service_name) || is_fingerprint)
    {
      shell_gdm_auth_services_emit_queue_priority_message (services, service_name, problem,
                                                           SHELL_GDM_MESSAGE_TYPE_ERROR,
                                                           g_strcmp0 (service_name,
                                                                      SHELL_GDM_FINGERPRINT_SERVICE_NAME) == 0);
    }

  if (!is_fingerprint)
    return;

  /* pam_fprintd allows the user to retry multiple (maybe even infinite!
   * times before failing the authentication conversation.
   * We don't want this behavior to bypass the max-tries setting the user has set,
   * so we count the problem messages to know how many times the user has failed.
   * Once we hit the max number of failures we allow, it's time to failure the
   * conversation from our side. We can't do that right away, however, because
   * we may drop pending messages coming from pam_fprintd. In order to make sure
   * the user sees everything, we queue the failure up to get handled in the
   * near future, after we've finished up the current round of messages. */
  services->fail_counter++;

  if (shell_gdm_auth_services_can_retry (services))
    return;

  g_clear_handle_id (&self->fingerprint_failed_id, g_source_remove);

  self->fingerprint_failed_id = g_timeout_add (FINGERPRINT_ERROR_TIMEOUT_WAIT,
                                               on_fingerprint_failed, self);
  g_source_set_name_by_id (self->fingerprint_failed_id,
                           "[gnome-shell] fingerprint failed");
}

static void
legacy_handle_on_info_query (ShellGdmAuthServices *services,
                             const char           *service_name,
                             const char           *question)
{
  if (!is_selected_service (services, service_name))
    return;

  shell_gdm_auth_services_emit_ask_question (services, service_name, question, FALSE);
}

static gboolean
try_credential_manager_auto_fill (ShellGdmAuthServicesLegacy *self,
                                  const char                 *service_name)
{
  CredentialEntry *entry = find_credential_entry (self, service_name);
  const char *token;

  if (entry == NULL)
    return FALSE;

  token = shell_credential_manager_get_token (entry->manager);
  if (token == NULL)
    return FALSE;

  shell_gdm_auth_services_send_answer (SHELL_GDM_AUTH_SERVICES (self), service_name, token);
  return TRUE;
}

static void
legacy_handle_on_secret_info_query (ShellGdmAuthServices *services,
                                    const char           *service_name,
                                    const char           *secret_question)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);

  /* Try to auto-fill with credential manager token first */
  if (try_credential_manager_auto_fill (self, service_name))
    return;

  if (!is_selected_service (services, service_name))
    return;

  shell_gdm_auth_services_emit_ask_question (services, service_name, secret_question, TRUE);
}

static void
legacy_handle_on_conversation_stopped (ShellGdmAuthServices *services,
                                       const char           *service_name)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);
  CredentialEntry *entry;

  if (!is_selected_service (services, service_name) &&
      g_strcmp0 (service_name, SHELL_GDM_FINGERPRINT_SERVICE_NAME) != 0)
    return;

  /* If the login failed with the preauthenticated oVirt credentials
   * then discard the credentials and revert to default authentication
   * mechanism. */
  entry = find_credential_entry (self, service_name);
  if (entry != NULL)
    {
      g_autoptr (ShellCredentialManager) manager = g_object_ref (entry->manager);

      shell_credential_manager_set_token (manager, NULL);
      g_clear_pointer (&services->selected_mechanism, shell_gdm_auth_mechanism_unref);
      shell_gdm_auth_services_verification_failed (services, service_name, FALSE);
      return;
    }

  if (g_strcmp0 (service_name, SHELL_GDM_FINGERPRINT_SERVICE_NAME) == 0)
    {
      clear_fingerprint_signal_handlers (self);
      if (g_hash_table_contains (services->unavailable_services, service_name))
        set_fingerprint_ready (self, FALSE);
    }

  if (g_hash_table_contains (services->unavailable_services, service_name))
    return;

  /* if the password service fails, then cancel everything.
   * But if, e.g., fingerprint fails, still give
   * password authentication a chance to succeed */
  if (is_selected_service (services, service_name))
    services->fail_counter++;

  shell_gdm_auth_services_verification_failed (services, service_name, TRUE);
}

static void
legacy_handle_on_service_unavailable (ShellGdmAuthServices *services,
                                      const char           *service_name,
                                      const char           *error_message)
{
  if (g_strcmp0 (service_name, SHELL_GDM_FINGERPRINT_SERVICE_NAME) != 0 ||
      !has_enabled_mechanism_for_service (services, service_name) ||
      error_message == NULL || error_message[0] == '\0')
    return;

  shell_gdm_auth_services_emit_queue_message (services, service_name, error_message,
                                              SHELL_GDM_MESSAGE_TYPE_ERROR, TRUE);
}

static void
legacy_handle_verification_failed (ShellGdmAuthServices *services,
                                   const char           *service_name)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);

  if (g_strcmp0 (service_name, SHELL_GDM_FINGERPRINT_SERVICE_NAME) == 0 &&
      has_enabled_mechanism_for_service (services, service_name))
    g_clear_handle_id (&self->fingerprint_failed_id, g_source_remove);
}

static void
legacy_handle_on_verification_complete (ShellGdmAuthServices *services,
                                        const char           *service_name)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);
  CredentialEntry *entry = find_credential_entry (self, service_name);

  if (entry != NULL)
    {
      g_autoptr (ShellCredentialManager) manager = g_object_ref (entry->manager);

      shell_credential_manager_set_token (manager, NULL);
    }
}

static void
legacy_choice_handler (ShellGdmAuthServices *services,
                       const char           *service_name,
                       const char           *argument,
                       gpointer              user_data)
{
  if (!is_selected_service (services, service_name))
    return;

  shell_gdm_auth_services_send_choice (services, service_name, argument);
}

static void
legacy_handle_on_choice_list_query (ShellGdmAuthServices *services,
                                    const char           *service_name,
                                    const char           *prompt_message,
                                    GVariant             *list)
{
  g_autoptr (ShellGdmAuthHandler) handler = NULL;
  GVariantBuilder builder;
  GVariantIter iter;
  const char *key;
  const char *value;
  GVariant *choice_list;

  if (!is_selected_service (services, service_name))
    return;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sa{sv})"));
  g_variant_iter_init (&iter, list);
  while (g_variant_iter_next (&iter, "{&s&s}", &key, &value))
    {
      GVariantBuilder content;

      g_variant_builder_init (&content, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&content, "{sv}", "title", g_variant_new_string (value));
      g_variant_builder_add (&builder, "(sa{sv})", key, &content);
    }
  choice_list = g_variant_ref_sink (g_variant_builder_end (&builder));

  handler = shell_gdm_auth_services_new_handler (services, service_name,
                                                 legacy_choice_handler, NULL, NULL);

  shell_gdm_auth_services_emit_show_choice_list (services, service_name, prompt_message,
                                                 choice_list, handler);
  g_variant_unref (choice_list);
}

static GPtrArray *
legacy_handle_get_credential_manager_services (ShellGdmAuthServices *services)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  guint i;

  for (i = 0; i < self->credential_managers->len; i++)
    {
      CredentialEntry *entry = g_ptr_array_index (self->credential_managers, i);

      g_ptr_array_add (names, g_strdup (entry->service));
    }

  return names;
}

static gboolean
legacy_handle_can_start_service (ShellGdmAuthServices *services,
                                 const char           *service_name)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);

  if (has_any_credential_manager_token (self))
    {
      CredentialEntry *entry = find_credential_entry (self, service_name);

      /* The services that are not credential managers are started too */
      return entry == NULL ||
             shell_credential_manager_get_token (entry->manager) != NULL;
    }

  return is_selected_service (services, service_name) ||
         (g_strcmp0 (service_name, SHELL_GDM_FINGERPRINT_SERVICE_NAME) == 0 &&
          has_enabled_mechanism_for_service (services, service_name) &&
          services->user_name != NULL && services->user_name[0] != '\0');
}

static void
credential_entry_free (gpointer data)
{
  CredentialEntry *entry = data;

  g_signal_handler_disconnect (entry->manager, entry->handler_id);
  g_object_unref (entry->manager);
  g_free (entry->service);
  g_free (entry);
}

static void
legacy_add_credential_manager (ShellGdmAuthServices   *services,
                               const char             *service_name,
                               ShellCredentialManager *credential_manager)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);
  CredentialEntry *entry;

  if (find_credential_entry (self, service_name) != NULL)
    return;

  entry = g_new0 (CredentialEntry, 1);
  entry->service = g_strdup (service_name);
  entry->manager = g_object_ref (credential_manager);
  g_ptr_array_add (self->credential_managers, entry);

  if (shell_credential_manager_get_token (credential_manager) != NULL)
    on_credential_manager_authenticated (credential_manager,
                                         shell_credential_manager_get_token (credential_manager),
                                         self);

  entry->handler_id =
    g_signal_connect (credential_manager, "user-authenticated",
                      G_CALLBACK (on_credential_manager_authenticated), self);
}

static void
legacy_remove_credential_manager (ShellGdmAuthServices *services,
                                  const char           *service_name)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (services);
  CredentialEntry *entry = find_credential_entry (self, service_name);

  if (entry == NULL)
    return;

  g_ptr_array_remove (self->credential_managers, entry);

  if (is_selected_service (services, service_name))
    shell_gdm_auth_services_emit_reset (services, FALSE, FALSE);
}

/* ---- GObject ---- */

static void
shell_gdm_auth_services_legacy_constructed (GObject *object)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (object);
  ShellGdmAuthServices *services = SHELL_GDM_AUTH_SERVICES (self);

  G_OBJECT_CLASS (shell_gdm_auth_services_legacy_parent_class)->constructed (object);

  self->smartcard_manager = shell_smartcard_manager_get_default ();
  g_signal_connect_object (self->smartcard_manager, "smartcard-inserted",
                           G_CALLBACK (on_smartcard_changed), self, 0);
  g_signal_connect_object (self->smartcard_manager, "smartcard-removed",
                           G_CALLBACK (on_smartcard_changed), self, 0);

  if (services->reauth_only)
    {
      self->fingerprint_manager = shell_fingerprint_manager_get_default ();
      g_signal_connect_object (self->fingerprint_manager, "reader-type-changed",
                               G_CALLBACK (on_fingerprint_changed), self, 0);
    }

  shell_gdm_auth_services_update_enabled_mechanisms (services);

  legacy_add_credential_manager (services, SHELL_OVIRT_SERVICE_NAME,
                                 SHELL_CREDENTIAL_MANAGER (shell_ovirt_credential_manager_get_default ()));
  legacy_add_credential_manager (services, SHELL_VMWARE_SERVICE_NAME,
                                 SHELL_CREDENTIAL_MANAGER (shell_vmware_credential_manager_get_default ()));
}

static void
shell_gdm_auth_services_legacy_dispose (GObject *object)
{
  ShellGdmAuthServicesLegacy *self = SHELL_GDM_AUTH_SERVICES_LEGACY (object);

  clear_fingerprint_signal_handlers (self);
  g_clear_pointer (&self->credential_managers, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_gdm_auth_services_legacy_parent_class)->dispose (object);
}

static void
shell_gdm_auth_services_legacy_class_init (ShellGdmAuthServicesLegacyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellGdmAuthServicesClass *services_class = SHELL_GDM_AUTH_SERVICES_CLASS (klass);

  object_class->constructed = shell_gdm_auth_services_legacy_constructed;
  object_class->dispose = shell_gdm_auth_services_legacy_dispose;

  services_class->supported_roles = supported_roles;
  services_class->role_to_service = legacy_role_to_service;

  services_class->handle_begin_verification = legacy_handle_begin_verification;
  services_class->handle_select_mechanism = legacy_handle_select_mechanism;
  services_class->handle_needs_username = legacy_handle_needs_username;
  services_class->handle_reset = legacy_handle_reset;
  services_class->handle_clear = legacy_handle_clear;
  services_class->handle_update_enabled_roles = legacy_handle_update_enabled_roles;
  services_class->handle_update_enabled_mechanisms = legacy_handle_update_enabled_mechanisms;
  services_class->handle_on_info = legacy_handle_on_info;
  services_class->handle_on_problem = legacy_handle_on_problem;
  services_class->handle_on_info_query = legacy_handle_on_info_query;
  services_class->handle_on_secret_info_query = legacy_handle_on_secret_info_query;
  services_class->handle_on_conversation_started = legacy_handle_on_conversation_started;
  services_class->handle_on_conversation_stopped = legacy_handle_on_conversation_stopped;
  services_class->handle_on_service_unavailable = legacy_handle_on_service_unavailable;
  services_class->handle_on_verification_complete = legacy_handle_on_verification_complete;
  services_class->handle_on_choice_list_query = legacy_handle_on_choice_list_query;
  services_class->handle_verification_failed = legacy_handle_verification_failed;
  services_class->handle_get_credential_manager_services = legacy_handle_get_credential_manager_services;
  services_class->handle_can_start_service = legacy_handle_can_start_service;
  services_class->add_credential_manager = legacy_add_credential_manager;
  services_class->remove_credential_manager = legacy_remove_credential_manager;
  services_class->handle_answer_query = legacy_handle_answer_query;
}

static void
shell_gdm_auth_services_legacy_init (ShellGdmAuthServicesLegacy *self)
{
  self->credential_managers = g_ptr_array_new_with_free_func (credential_entry_free);
}
