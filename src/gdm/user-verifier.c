/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * user-verifier.c: Talks to GDM to verify the user of the login and unlock
 * screens. Port of js/gdm/userVerifier.js.
 *
 * #ShellGdmUserVerifier asks GDM for a user verifier (or a reauthentication
 * channel for a user who has a session), lets the groups of PAM services
 * (#ShellGdmAuthServices) start their services on it, and gives the prompt
 * what they say: questions, messages (queued, so the user has the time to
 * read them), lists of choices, buttons and the mechanisms the user can pick.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - the signals carry their arguments instead of one object. The prompts
 *    that expect an answer carry a #ShellGdmAuthHandler (the callbacks
 *    answerHandler, choiceHandler and callback of the JS objects), the
 *    mechanisms are `a{sv}` variants whose keys are those of the JS objects
 *    (serviceName, role, id, name, selectable, preemptiveInput, hint,
 *    iconName, description) and the lists of choices are `a(sa{sv})` of keys
 *    and `{title, subtitle, iconName, iconTitle, iconSubtitle}`;
 *  - ::show-message does not carry the resolver of a promise: when the
 *    message is wiggled the queue waits until the prompt calls
 *    shell_gdm_user_verifier_message_shown (), otherwise it goes on at once;
 *  - handlePendingMessages () is the asynchronous
 *    shell_gdm_user_verifier_handle_pending_messages_async ();
 *  - the failures to initialize the authentication are logged as messages
 *    (they are missing peers more often than programming errors); a
 *    cancelled initialization stops the services that did not begin yet;
 *  - the schema org.gnome.login-screen is not required: without it no
 *    authentication service is created;
 *  - nothing is emitted after ShellGlobal::shutdown.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <math.h>
#include <stdlib.h>

#include <glib/gi18n-lib.h>

#include "gdm/auth-services.h"
#include "gdm/auth-services-legacy.h"
#include "gdm/auth-services-sssd-switchable.h"
#include "gdm/settings.h"
#include "gdm/user-verifier.h"
#include "shell-enum-types.h"
#include "shell-global.h"

/* Give user 48ms to read each character of a PAM message
 * or 2 seconds, whichever is longer */
#define USER_READ_TIME 48
#define USER_READ_TIME_MIN 2000

/* ------------------------------------------------------------------------ */
/* ShellGdmAuthHandler                                                       */
/* ------------------------------------------------------------------------ */

struct _ShellGdmAuthHandler
{
  GObject parent;

  ShellGdmAuthHandlerFunc func;
  gpointer user_data;
  GDestroyNotify destroy;
};

G_DEFINE_FINAL_TYPE (ShellGdmAuthHandler, shell_gdm_auth_handler, G_TYPE_OBJECT)

static void
shell_gdm_auth_handler_finalize (GObject *object)
{
  ShellGdmAuthHandler *handler = SHELL_GDM_AUTH_HANDLER (object);

  if (handler->destroy != NULL)
    handler->destroy (handler->user_data);
  handler->destroy = NULL;

  G_OBJECT_CLASS (shell_gdm_auth_handler_parent_class)->finalize (object);
}

static void
shell_gdm_auth_handler_class_init (ShellGdmAuthHandlerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = shell_gdm_auth_handler_finalize;
}

static void
shell_gdm_auth_handler_init (ShellGdmAuthHandler *handler)
{
}

/**
 * shell_gdm_auth_handler_new: (skip)
 * @func: the action
 * @user_data: data for @func
 * @destroy: called with @user_data when the handler is finalized
 *
 * Creates the action behind a prompt of the user verifier.
 *
 * Returns: (transfer full): a new #ShellGdmAuthHandler
 */
ShellGdmAuthHandler *
shell_gdm_auth_handler_new (ShellGdmAuthHandlerFunc func,
                            gpointer                user_data,
                            GDestroyNotify          destroy)
{
  ShellGdmAuthHandler *handler = g_object_new (SHELL_TYPE_GDM_AUTH_HANDLER, NULL);

  handler->func = func;
  handler->user_data = user_data;
  handler->destroy = destroy;

  return handler;
}

/**
 * shell_gdm_auth_handler_invoke:
 * @handler: a #ShellGdmAuthHandler
 * @argument: (nullable): the answer of a question, the key of the chosen
 *   item of a list, or %NULL for a button
 *
 * Does what the prompt of the user verifier waited for.
 */
void
shell_gdm_auth_handler_invoke (ShellGdmAuthHandler *handler,
                               const char          *argument)
{
  g_autoptr (ShellGdmAuthHandler) keep = NULL;

  g_return_if_fail (SHELL_IS_GDM_AUTH_HANDLER (handler));

  keep = g_object_ref (handler);
  if (handler->func != NULL)
    handler->func (argument, handler->user_data);
}

/* ------------------------------------------------------------------------ */
/* Queued messages                                                           */
/* ------------------------------------------------------------------------ */

typedef struct
{
  int ref_count;
  char *service_name;     /* nullable */
  char *text;             /* nullable: clears the prompt */
  ShellGdmMessageType type;
  gboolean wiggle;
  guint interval;
} QueuedMessage;

static QueuedMessage *
queued_message_ref (QueuedMessage *message)
{
  message->ref_count++;
  return message;
}

static void
queued_message_unref (QueuedMessage *message)
{
  if (--message->ref_count > 0)
    return;

  g_free (message->service_name);
  g_free (message->text);
  g_free (message);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (QueuedMessage, queued_message_unref)

/* The multiplier of the time the messages stay on screen, from the
 * environment (GDM_MESSAGE_TIME_MULTIPLIER, for the tests) */
static double
get_message_time_multiplier (void)
{
  static double multiplier = 0;

  if (multiplier == 0)
    {
      const char *value = g_getenv ("GDM_MESSAGE_TIME_MULTIPLIER");
      char *end = NULL;
      double parsed = 0;

      multiplier = 1;
      if (value != NULL)
        {
          parsed = g_ascii_strtod (value, &end);
          if (end != value && isfinite (parsed) && parsed > 0)
            multiplier = parsed;
        }
    }

  return multiplier;
}

/* ------------------------------------------------------------------------ */
/* ShellGdmUserVerifier                                                      */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_CLIENT,
  PROP_REAUTHENTICATION_ONLY,
  PROP_HAS_PENDING_MESSAGES,
  PROP_ALLOWED_FAILURES,
  PROP_PREEMPTIVE_INPUT,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  ASK_QUESTION,
  SHOW_MESSAGE,
  SHOW_CHOICE_LIST,
  SHOW_BUTTON,
  MECHANISMS_CHANGED,
  WEB_LOGIN,
  VERIFICATION_FAILED,
  VERIFICATION_COMPLETE,
  RESET,
  NO_MORE_MESSAGES,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

/* The kinds of authentication services, by priority: earlier entries take
 * precedence for shared roles. Each of them claims the roles it supports;
 * unsupported roles cascade to the next one. */
typedef struct
{
  GType      (* get_type)    (void);
  gboolean   (* is_enabled)  (GSettings  *settings);
  gboolean   (* has_service) (const char *service_name);
} AuthServicesKind;

static const AuthServicesKind auth_services_kinds[] = {
  {
    shell_gdm_auth_services_sssd_switchable_get_type,
    shell_gdm_auth_services_sssd_switchable_is_enabled,
    shell_gdm_auth_services_sssd_switchable_has_service,
  },
  {
    shell_gdm_auth_services_legacy_get_type,
    shell_gdm_auth_services_legacy_is_enabled,
    shell_gdm_auth_services_legacy_has_service,
  },
};

struct _ShellGdmUserVerifier
{
  GObject parent;

  GdmClient *client;
  gboolean reauth_only;

  GCancellable *cancellable;      /* nullable */
  GPtrArray *auth_services;       /* ShellGdmAuthServices */
  char *driver_service;           /* nullable */

  GdmUserVerifier *user_verifier; /* nullable */

  int n_begins;
  gboolean mechanisms_changed_pending;
  gboolean redistributing_roles;

  GPtrArray *message_queue;       /* QueuedMessage */
  guint message_queue_timeout_id;
  QueuedMessage *awaited_message; /* nullable, the wiggled message the prompt did not finish with */
  guint showing_serial;
  guint last_serial;
  GList *pending_message_tasks;   /* GTask, with the cancellable at the time as data */

  GSettings *settings;            /* nullable */
  gulong settings_changed_id;
  int enabled_kinds;              /* a mask of auth_services_kinds, -1 before the first update */

  gboolean shutting_down;
  gboolean destroyed;
};

G_DEFINE_FINAL_TYPE (ShellGdmUserVerifier, shell_gdm_user_verifier, G_TYPE_OBJECT)

static void update_auth_services (ShellGdmUserVerifier *self);

/**
 * shell_gdm_user_verifier_new:
 * @client: the #GdmClient to ask for the user verifier
 * @reauthentication_only: whether to verify only users who already have a
 *   session (the unlock dialog), instead of falling back to the login session
 *
 * Creates a user verifier.
 *
 * Returns: (transfer full): a new #ShellGdmUserVerifier
 */
ShellGdmUserVerifier *
shell_gdm_user_verifier_new (GdmClient *client,
                             gboolean   reauthentication_only)
{
  return g_object_new (SHELL_TYPE_GDM_USER_VERIFIER,
                       "client", client,
                       "reauthentication-only", reauthentication_only,
                       NULL);
}

/**
 * shell_gdm_user_verifier_get_has_pending_messages:
 * @verifier: a #ShellGdmUserVerifier
 *
 * Returns: whether messages are waiting to be shown or are being shown
 */
gboolean
shell_gdm_user_verifier_get_has_pending_messages (ShellGdmUserVerifier *verifier)
{
  g_return_val_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier), FALSE);

  return verifier->message_queue != NULL && verifier->message_queue->len > 0;
}

/**
 * shell_gdm_user_verifier_get_allowed_failures:
 * @verifier: a #ShellGdmUserVerifier
 *
 * Returns: the number of failed attempts the login screen allows
 *   (the `allowed-failures` key of org.gnome.login-screen)
 */
int
shell_gdm_user_verifier_get_allowed_failures (ShellGdmUserVerifier *verifier)
{
  g_return_val_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier), 3);

  if (verifier->settings == NULL)
    return 3;

  return g_settings_get_int (verifier->settings, SHELL_GDM_ALLOWED_FAILURES_KEY);
}

static ShellGdmAuthMechanism *
get_selected_mechanism (ShellGdmUserVerifier *self)
{
  guint i;

  for (i = 0; i < self->auth_services->len; i++)
    {
      ShellGdmAuthMechanism *mechanism =
        shell_gdm_auth_services_get_selected_mechanism (g_ptr_array_index (self->auth_services, i));

      if (mechanism != NULL)
        return mechanism;
    }

  return NULL;
}

/**
 * shell_gdm_user_verifier_get_preemptive_input:
 * @verifier: a #ShellGdmUserVerifier
 *
 * Returns: whether the user can type before the selected mechanism asks
 *   (`selectedMechanism?.preemptiveInput` of the JS class)
 */
gboolean
shell_gdm_user_verifier_get_preemptive_input (ShellGdmUserVerifier *verifier)
{
  ShellGdmAuthMechanism *mechanism;

  g_return_val_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier), FALSE);

  mechanism = get_selected_mechanism (verifier);

  return mechanism != NULL && mechanism->preemptive_input;
}

/* ---- the user verifier proxy ---- */

static void
clear_user_verifier (ShellGdmUserVerifier *self)
{
  if (self->user_verifier != NULL)
    {
      GDBusConnection *connection;

      connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (self->user_verifier));
      if (connection != NULL)
        g_signal_handlers_disconnect_by_data (connection, self);
    }

  g_clear_object (&self->user_verifier);
}

static void
on_connection_closed (GDBusConnection      *connection,
                      gboolean              remote_peer_vanished,
                      GError               *error,
                      ShellGdmUserVerifier *self)
{
  clear_user_verifier (self);
}

static void
set_user_verifier (ShellGdmUserVerifier *self,
                   GdmUserVerifier      *user_verifier)
{
  GDBusConnection *connection;

  clear_user_verifier (self);

  self->user_verifier = g_object_ref (user_verifier);

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (self->user_verifier));
  if (connection != NULL)
    g_signal_connect (connection, "closed", G_CALLBACK (on_connection_closed), self);
}

static void
cancel_user_verifier (ShellGdmUserVerifier *self)
{
  g_autoptr (GError) error = NULL;

  if (self->user_verifier == NULL)
    return;

  if (!gdm_user_verifier_call_cancel_sync (self->user_verifier, NULL, &error))
    g_debug ("Could not cancel the user verification: %s", error->message);
}

/* ---- the queue of messages ---- */

static guint
get_interval_for_message (const char *message)
{
  /* We probably could be smarter here */
  if (message == NULL)
    return 0;

  return (guint) (MAX (g_utf8_strlen (message, -1) * USER_READ_TIME, USER_READ_TIME_MIN) *
                  get_message_time_multiplier ());
}

static void
complete_pending_message_tasks (ShellGdmUserVerifier *self)
{
  GList *tasks = self->pending_message_tasks;

  self->pending_message_tasks = NULL;

  while (tasks != NULL)
    {
      GTask *task = tasks->data;
      GCancellable *cancellable = g_object_steal_data (G_OBJECT (task), "message-cancellable");

      tasks = g_list_delete_link (tasks, tasks);

      if (cancellable != NULL && g_cancellable_is_cancelled (cancellable))
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "Operation was cancelled");
      else
        g_task_return_boolean (task, TRUE);

      g_clear_object (&cancellable);
      g_object_unref (task);
    }
}

static void
finish_message_queue (ShellGdmUserVerifier *self)
{
  if (self->message_queue->len == 0)
    return;

  g_ptr_array_set_size (self->message_queue, 0);

  if (!self->shutting_down)
    g_signal_emit (self, signals[NO_MORE_MESSAGES], 0);

  complete_pending_message_tasks (self);
}

static void queue_message_timeout (ShellGdmUserVerifier *self);

static void
on_message_timeout (gpointer user_data)
{
  ShellGdmUserVerifier *self = user_data;

  self->message_queue_timeout_id = 0;

  if (self->message_queue->len > 1)
    {
      g_ptr_array_remove_index (self->message_queue, 0);
      queue_message_timeout (self);
    }
  else
    {
      finish_message_queue (self);
    }
}

static void
start_message_timeout (ShellGdmUserVerifier *self,
                       QueuedMessage        *message)
{
  self->message_queue_timeout_id =
    g_timeout_add_once (message->interval, on_message_timeout, self);
  g_source_set_name_by_id (self->message_queue_timeout_id,
                           "[gnome-shell] this._queueMessageTimeout");
}

static void
queue_message_timeout (ShellGdmUserVerifier *self)
{
  g_autoptr (QueuedMessage) message = NULL;
  guint serial;

  if (self->message_queue_timeout_id != 0 || self->awaited_message != NULL ||
      self->shutting_down)
    return;

  if (self->message_queue->len == 0)
    return;

  message = queued_message_ref (g_ptr_array_index (self->message_queue, 0));
  serial = ++self->last_serial;
  self->showing_serial = serial;

  /* Nothing else is shown until the prompt is done with this message */
  self->awaited_message = queued_message_ref (message);

  g_signal_emit (self, signals[SHOW_MESSAGE], 0,
                 message->text, message->type, message->wiggle);

  /* The queue was cleared by a handler */
  if (self->showing_serial != serial)
    return;

  /* The prompt tells when the wiggle is over */
  if (message->wiggle)
    return;

  g_clear_pointer (&self->awaited_message, queued_message_unref);
  start_message_timeout (self, message);
}

/**
 * shell_gdm_user_verifier_message_shown:
 * @verifier: a #ShellGdmUserVerifier
 *
 * Tells the verifier that the prompt is done showing a message that was
 * emitted with its `should-wiggle` argument set (when the wiggle ended). The
 * message then stays on screen for the time the user needs to read it.
 */
void
shell_gdm_user_verifier_message_shown (ShellGdmUserVerifier *verifier)
{
  g_autoptr (QueuedMessage) message = NULL;

  g_return_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier));

  if (verifier->awaited_message == NULL || !verifier->awaited_message->wiggle)
    return;

  message = g_steal_pointer (&verifier->awaited_message);
  if (verifier->shutting_down)
    return;

  start_message_timeout (verifier, message);
}

static void
queue_message (ShellGdmUserVerifier *self,
               const char           *service_name,
               const char           *text,
               ShellGdmMessageType   type,
               gboolean              wiggle)
{
  QueuedMessage *message = g_new0 (QueuedMessage, 1);

  message->ref_count = 1;
  message->service_name = g_strdup (service_name);
  message->text = g_strdup (text);
  message->type = type;
  message->wiggle = wiggle;
  message->interval = get_interval_for_message (text);

  g_ptr_array_add (self->message_queue, message);
  queue_message_timeout (self);
}

static void
clear_message_queue (ShellGdmUserVerifier *self)
{
  finish_message_queue (self);

  g_clear_handle_id (&self->message_queue_timeout_id, g_source_remove);

  /* If the prompt was still wiggling, the message is dropped */
  g_clear_pointer (&self->awaited_message, queued_message_unref);
  self->showing_serial = 0;

  if (!self->shutting_down)
    g_signal_emit (self, signals[SHOW_MESSAGE], 0, NULL, SHELL_GDM_MESSAGE_TYPE_NONE, FALSE);
}

static void
queue_priority_message (ShellGdmUserVerifier *self,
                        const char           *service_name,
                        const char           *text,
                        ShellGdmMessageType   type,
                        gboolean              wiggle)
{
  GPtrArray *new_queue = g_ptr_array_new_with_free_func ((GDestroyNotify) queued_message_unref);
  QueuedMessage *current = self->message_queue->len > 0
    ? g_ptr_array_index (self->message_queue, 0)
    : NULL;
  gboolean includes_current = FALSE;
  guint i;

  for (i = 0; i < self->message_queue->len; i++)
    {
      QueuedMessage *message = g_ptr_array_index (self->message_queue, i);
      gboolean keep;

      if (g_strcmp0 (message->service_name, service_name) != 0 ||
          (int) message->type >= (int) type)
        keep = g_strcmp0 (message->text, text) != 0;
      else
        keep = FALSE;

      if (!keep)
        continue;

      if (message == current)
        includes_current = TRUE;

      g_ptr_array_add (new_queue, queued_message_ref (message));
    }

  if (!includes_current)
    clear_message_queue (self);

  g_ptr_array_unref (self->message_queue);
  self->message_queue = new_queue;

  queue_message (self, service_name, text, type, wiggle);
}

static void
filter_service_messages (ShellGdmUserVerifier *self,
                         const char           *service_name,
                         ShellGdmMessageType   type)
{
  guint i;

  /* This function allows to remove queued messages for the @service_name
   * whose type has lower priority than @type, replacing them
   * with a null message that will lead to clearing the prompt once done. */
  for (i = 0; i < self->message_queue->len; i++)
    {
      QueuedMessage *message = g_ptr_array_index (self->message_queue, i);

      if (g_strcmp0 (message->service_name, service_name) == 0)
        {
          queue_priority_message (self, service_name, NULL, type, FALSE);
          return;
        }
    }
}

/**
 * shell_gdm_user_verifier_handle_pending_messages_async:
 * @verifier: a #ShellGdmUserVerifier
 * @callback: (scope async) (closure user_data): called when the messages have been shown
 * @user_data: data for @callback
 *
 * Waits until the queued messages have been shown. The operation fails with
 * %G_IO_ERROR_CANCELLED if the verification was cancelled meanwhile.
 */
void
shell_gdm_user_verifier_handle_pending_messages_async (ShellGdmUserVerifier *verifier,
                                                       GAsyncReadyCallback   callback,
                                                       gpointer              user_data)
{
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier));

  task = g_task_new (verifier, NULL, callback, user_data);
  g_task_set_source_tag (task, shell_gdm_user_verifier_handle_pending_messages_async);

  if (verifier->message_queue->len == 0 || verifier->destroyed)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  if (verifier->cancellable != NULL)
    g_object_set_data_full (G_OBJECT (task), "message-cancellable",
                            g_object_ref (verifier->cancellable), g_object_unref);

  verifier->pending_message_tasks = g_list_append (verifier->pending_message_tasks,
                                                   g_steal_pointer (&task));
}

/**
 * shell_gdm_user_verifier_handle_pending_messages_finish:
 * @verifier: a #ShellGdmUserVerifier
 * @res: the #GAsyncResult
 * @error: return location for a #GError
 *
 * Returns: %TRUE if all the messages have been shown
 */
gboolean
shell_gdm_user_verifier_handle_pending_messages_finish (ShellGdmUserVerifier *verifier,
                                                        GAsyncResult         *res,
                                                        GError              **error)
{
  g_return_val_if_fail (g_task_is_valid (res, verifier), FALSE);

  return g_task_propagate_boolean (G_TASK (res), error);
}

/* ---- errors of the initialization ---- */

static void
queue_message_for_service (ShellGdmUserVerifier *self,
                           const char           *service_name,
                           const char           *message,
                           ShellGdmMessageType   type)
{
  queue_message (self, service_name, message, type, FALSE);
}

static void
verifier_verification_failed (ShellGdmUserVerifier *self,
                              const char           *service_name,
                              gboolean              can_retry)
{
  filter_service_messages (self, service_name, SHELL_GDM_MESSAGE_TYPE_ERROR);

  if (!self->shutting_down)
    g_signal_emit (self, signals[VERIFICATION_FAILED], 0, service_name, can_retry);
}

static void
report_init_error (ShellGdmUserVerifier *self,
                   const GError         *init_error)
{
  const char *service_name = shell_gdm_init_error_get_service_name (init_error);
  const GError *cause = shell_gdm_init_error_get_cause (init_error);

  g_message ("%s: %s", init_error->message,
             cause != NULL ? cause->message : "unknown error");

  queue_message_for_service (self, service_name, _("Authentication error"),
                             SHELL_GDM_MESSAGE_TYPE_ERROR);
  verifier_verification_failed (self, service_name, FALSE);
}

/* ---- the authentication services ---- */

static void
redistribute_roles (ShellGdmUserVerifier *self)
{
  guint i;

  if (self->auth_services->len < 2)
    return;

  self->redistributing_roles = TRUE;

  /* Each authServices disables the roles supported by the one
   * before it, cascading down the priority chain */
  for (i = 1; i < self->auth_services->len; i++)
    {
      ShellGdmAuthServices *previous = g_ptr_array_index (self->auth_services, i - 1);
      ShellGdmAuthServices *current = g_ptr_array_index (self->auth_services, i);

      shell_gdm_auth_services_update_enabled_roles (current,
                                                    shell_gdm_auth_services_get_supported_roles (previous));
    }

  self->redistributing_roles = FALSE;
}

static void
emit_mechanisms_changed (ShellGdmUserVerifier *self)
{
  GVariantBuilder mechanisms_builder;
  g_autoptr (GHashTable) seen_roles = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GPtrArray) visible = g_ptr_array_new_with_free_func ((GDestroyNotify) shell_gdm_auth_mechanism_unref);
  GVariant *mechanisms;
  GVariant *selected;
  ShellGdmAuthMechanism *selected_mechanism;
  guint i, j;

  redistribute_roles (self);

  /* Collect mechanisms from all authServices in priority order,
   * keeping only the first mechanism per role */
  for (i = 0; i < self->auth_services->len; i++)
    {
      ShellGdmAuthServices *services = g_ptr_array_index (self->auth_services, i);
      g_autoptr (GPtrArray) service_mechanisms =
        shell_gdm_auth_services_get_enabled_mechanisms (services);
      g_autoptr (GPtrArray) roles = g_ptr_array_new ();

      for (j = 0; j < service_mechanisms->len; j++)
        {
          ShellGdmAuthMechanism *mechanism = g_ptr_array_index (service_mechanisms, j);

          if (!g_hash_table_contains (seen_roles, mechanism->role))
            g_ptr_array_add (visible, shell_gdm_auth_mechanism_ref (mechanism));

          g_ptr_array_add (roles, mechanism->role);
        }

      for (j = 0; j < roles->len; j++)
        g_hash_table_add (seen_roles, g_ptr_array_index (roles, j));
    }

  g_variant_builder_init (&mechanisms_builder, G_VARIANT_TYPE ("aa{sv}"));
  for (i = 0; i < visible->len; i++)
    g_variant_builder_add_value (&mechanisms_builder,
                                 shell_gdm_auth_mechanism_to_variant (g_ptr_array_index (visible, i)));
  mechanisms = g_variant_builder_end (&mechanisms_builder);

  selected_mechanism = get_selected_mechanism (self);
  for (i = 0; selected_mechanism == NULL && i < visible->len; i++)
    {
      ShellGdmAuthMechanism *mechanism = g_ptr_array_index (visible, i);

      if (mechanism->selectable)
        selected_mechanism = mechanism;
    }

  if (selected_mechanism != NULL)
    {
      selected = shell_gdm_auth_mechanism_to_variant (selected_mechanism);
    }
  else
    {
      GVariantBuilder empty;

      g_variant_builder_init (&empty, G_VARIANT_TYPE_VARDICT);
      selected = g_variant_builder_end (&empty);
    }

  g_variant_ref_sink (mechanisms);
  g_variant_ref_sink (selected);
  g_signal_emit (self, signals[MECHANISMS_CHANGED], 0, mechanisms, selected);
  g_variant_unref (mechanisms);
  g_variant_unref (selected);
}

static void
on_mechanisms_changed (ShellGdmAuthServices *services,
                       ShellGdmUserVerifier *self)
{
  if (self->redistributing_roles || self->shutting_down)
    return;

  /* The mechanisms are announced when the services began */
  if (self->n_begins > 0)
    {
      self->mechanisms_changed_pending = TRUE;
      return;
    }

  emit_mechanisms_changed (self);
}

static void
on_wait_pending_messages_done (GObject      *source,
                               GAsyncResult *res,
                               gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  g_autoptr (GError) error = NULL;

  /* The services may have returned it already */
  if (g_task_get_completed (task))
    return;

  if (shell_gdm_user_verifier_handle_pending_messages_finish (SHELL_GDM_USER_VERIFIER (source),
                                                              res, &error))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, g_steal_pointer (&error));
}

static void
on_wait_pending_messages (ShellGdmAuthServices *services,
                          gpointer              task,
                          ShellGdmUserVerifier *self)
{
  shell_gdm_user_verifier_handle_pending_messages_async (self,
                                                         on_wait_pending_messages_done,
                                                         g_object_ref (task));
}

static void
on_queue_message (ShellGdmAuthServices *services,
                  const char           *service_name,
                  const char           *message,
                  int                   type,
                  gboolean              wiggle,
                  ShellGdmUserVerifier *self)
{
  queue_message (self, service_name, message, type, wiggle);
}

static void
on_queue_priority_message (ShellGdmAuthServices *services,
                           const char           *service_name,
                           const char           *message,
                           int                   type,
                           gboolean              wiggle,
                           ShellGdmUserVerifier *self)
{
  queue_priority_message (self, service_name, message, type, wiggle);
}

static void
on_filter_messages (ShellGdmAuthServices *services,
                    const char           *service_name,
                    int                   type,
                    ShellGdmUserVerifier *self)
{
  filter_service_messages (self, service_name, type);
}

static void
on_verification_failed (ShellGdmAuthServices *services,
                        const char           *service_name,
                        gboolean              can_retry,
                        ShellGdmUserVerifier *self)
{
  verifier_verification_failed (self, service_name, can_retry);
}

static void
on_verification_complete (ShellGdmAuthServices *services,
                          ShellGdmUserVerifier *self)
{
  if (!self->shutting_down)
    g_signal_emit (self, signals[VERIFICATION_COMPLETE], 0);
}

static void
on_ask_question (ShellGdmAuthServices *services,
                 const char           *service_name,
                 const char           *question,
                 gboolean              secret,
                 ShellGdmAuthHandler  *handler,
                 ShellGdmUserVerifier *self)
{
  if (!self->shutting_down)
    g_signal_emit (self, signals[ASK_QUESTION], 0, service_name, question, secret, handler);
}

static void
on_reset (ShellGdmAuthServices *services,
          gboolean              soft_reset,
          gboolean              reuse_entry_text,
          ShellGdmUserVerifier *self)
{
  if (!self->shutting_down)
    g_signal_emit (self, signals[RESET], 0, soft_reset, reuse_entry_text);
}

static void
on_show_choice_list (ShellGdmAuthServices *services,
                     const char           *service_name,
                     const char           *prompt_message,
                     GVariant             *choice_list,
                     ShellGdmAuthHandler  *handler,
                     ShellGdmUserVerifier *self)
{
  if (!self->shutting_down)
    g_signal_emit (self, signals[SHOW_CHOICE_LIST], 0,
                   service_name, prompt_message, choice_list, handler);
}

static void
on_show_button (ShellGdmAuthServices *services,
                const char           *service_name,
                const char           *label,
                ShellGdmAuthHandler  *handler,
                ShellGdmUserVerifier *self)
{
  if (!self->shutting_down)
    g_signal_emit (self, signals[SHOW_BUTTON], 0, service_name, label, handler);
}

static void
on_web_login (ShellGdmAuthServices *services,
              const char           *service_name,
              const char           *message,
              const char           *url,
              const char           *code,
              const char           *done_label,
              ShellGdmAuthHandler  *done_handler,
              ShellGdmUserVerifier *self)
{
  if (!self->shutting_down)
    g_signal_emit (self, signals[WEB_LOGIN], 0,
                   service_name, message, url, code, done_label, done_handler);
}

static void
connect_auth_services (ShellGdmUserVerifier *self)
{
  guint i;

  for (i = 0; i < self->auth_services->len; i++)
    {
      ShellGdmAuthServices *services = g_ptr_array_index (self->auth_services, i);

      g_signal_connect_object (services, "ask-question", G_CALLBACK (on_ask_question), self, 0);
      g_signal_connect_object (services, "queue-message", G_CALLBACK (on_queue_message), self, 0);
      g_signal_connect_object (services, "queue-priority-message",
                               G_CALLBACK (on_queue_priority_message), self, 0);
      g_signal_connect_object (services, "wait-pending-messages",
                               G_CALLBACK (on_wait_pending_messages), self, 0);
      g_signal_connect_object (services, "filter-messages", G_CALLBACK (on_filter_messages), self, 0);
      g_signal_connect_object (services, "verification-failed",
                               G_CALLBACK (on_verification_failed), self, 0);
      g_signal_connect_object (services, "verification-complete",
                               G_CALLBACK (on_verification_complete), self, 0);
      g_signal_connect_object (services, "reset", G_CALLBACK (on_reset), self, 0);
      g_signal_connect_object (services, "show-choice-list",
                               G_CALLBACK (on_show_choice_list), self, 0);
      g_signal_connect_object (services, "show-button", G_CALLBACK (on_show_button), self, 0);
      g_signal_connect_object (services, "mechanisms-changed",
                               G_CALLBACK (on_mechanisms_changed), self, 0);
      g_signal_connect_object (services, "web-login", G_CALLBACK (on_web_login), self, 0);
    }
}

static void
clear_auth_services (ShellGdmUserVerifier *self)
{
  g_autoptr (GPtrArray) old_services = self->auth_services;
  guint i;

  self->auth_services = g_ptr_array_new_with_free_func (g_object_unref);

  for (i = 0; i < old_services->len; i++)
    {
      ShellGdmAuthServices *services = g_ptr_array_index (old_services, i);

      shell_gdm_auth_services_destroy (services);
      g_signal_handlers_disconnect_by_data (services, self);
    }
}

static void
create_auth_services (ShellGdmUserVerifier *self)
{
  guint i;

  clear_auth_services (self);

  for (i = 0; i < G_N_ELEMENTS (auth_services_kinds); i++)
    {
      ShellGdmAuthServices *services;

      if ((self->enabled_kinds & (1 << i)) == 0)
        continue;

      services = g_object_new (auth_services_kinds[i].get_type (),
                               "settings", self->settings,
                               "allowed-failures",
                               shell_gdm_user_verifier_get_allowed_failures (self),
                               "reauth-only", self->reauth_only,
                               NULL);
      g_ptr_array_add (self->auth_services, services);
    }

  redistribute_roles (self);
  connect_auth_services (self);
}

static void
update_auth_services (ShellGdmUserVerifier *self)
{
  int enabled_kinds = 0;
  guint i;

  if (self->settings != NULL)
    {
      for (i = 0; i < G_N_ELEMENTS (auth_services_kinds); i++)
        {
          if (!auth_services_kinds[i].is_enabled (self->settings))
            continue;

          if (self->driver_service != NULL &&
              !auth_services_kinds[i].has_service (self->driver_service))
            continue;

          enabled_kinds |= 1 << i;
        }
    }

  if (self->enabled_kinds == enabled_kinds)
    return;

  self->enabled_kinds = enabled_kinds;
  create_auth_services (self);
}

static void
on_settings_changed (GSettings            *settings,
                     const char           *key,
                     ShellGdmUserVerifier *self)
{
  update_auth_services (self);
}

/* ---- begin () ---- */

typedef struct
{
  ShellGdmUserVerifier *self;
  char *user_name;                  /* nullable */
  ShellBatchHold *hold;             /* nullable */
  GCancellable *cancellable;
  ShellGdmUserVerifierProxies proxies;
  guint service_index;
} BeginData;

static void
begin_data_free (BeginData *data)
{
  g_clear_object (&data->proxies.user_verifier);
  g_clear_object (&data->proxies.choice_list);
  g_clear_object (&data->proxies.custom_json);
  g_clear_object (&data->cancellable);
  g_clear_object (&data->hold);
  g_clear_object (&data->self);
  g_free (data->user_name);
  g_free (data);
}

static void
finish_begin (BeginData *data)
{
  ShellGdmUserVerifier *self = data->self;

  self->n_begins--;

  if (data->hold != NULL)
    shell_batch_hold_release (data->hold);

  if (self->n_begins == 0 && self->mechanisms_changed_pending)
    {
      self->mechanisms_changed_pending = FALSE;

      if (!self->shutting_down && !self->destroyed)
        emit_mechanisms_changed (self);
    }

  begin_data_free (data);
}

static gboolean
begin_should_stop (BeginData *data)
{
  return data->self->shutting_down || data->self->destroyed ||
         g_cancellable_is_cancelled (data->cancellable);
}

static void begin_next_service (BeginData *data);

static void
service_begin_done (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  BeginData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (!shell_gdm_auth_services_begin_verification_finish (SHELL_GDM_AUTH_SERVICES (source),
                                                          res, &error))
    {
      if (!begin_should_stop (data))
        {
          if (error->domain == SHELL_GDM_INIT_ERROR)
            report_init_error (data->self, error);
          else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_message ("Failed to begin the verification: %s", error->message);
        }

      finish_begin (data);
      return;
    }

  begin_next_service (data);
}

static void
begin_next_service (BeginData *data)
{
  ShellGdmUserVerifier *self = data->self;
  ShellGdmAuthServices *services;

  if (begin_should_stop (data) || data->service_index >= self->auth_services->len)
    {
      finish_begin (data);
      return;
    }

  services = g_ptr_array_index (self->auth_services, data->service_index++);

  shell_gdm_auth_services_begin_verification_async (services, data->user_name,
                                                    &data->proxies,
                                                    service_begin_done, data);
}

static void
begin_with_proxies (BeginData *data)
{
  ShellGdmUserVerifier *self = data->self;
  GdmUserVerifierChoiceList *choice_list;
  GdmUserVerifierCustomJSON *custom_json;

  choice_list = gdm_client_get_user_verifier_choice_list (self->client);
  if (choice_list != NULL)
    data->proxies.choice_list = g_object_ref (choice_list);

  custom_json = gdm_client_get_user_verifier_custom_json (self->client);
  if (custom_json != NULL)
    data->proxies.custom_json = g_object_ref (custom_json);

  set_user_verifier (self, data->proxies.user_verifier);

  begin_next_service (data);
}

static void
user_verifier_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  BeginData *data = user_data;
  g_autoptr (GError) error = NULL;

  data->proxies.user_verifier =
    gdm_client_get_user_verifier_finish (GDM_CLIENT (source), res, &error);

  if (data->proxies.user_verifier == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          !begin_should_stop (data))
        {
          g_autoptr (GError) init_error =
            shell_gdm_init_error_new (g_steal_pointer (&error), NULL,
                                      "Failed to obtain user verifier");

          report_init_error (data->self, init_error);
        }

      finish_begin (data);
      return;
    }

  if (begin_should_stop (data))
    {
      finish_begin (data);
      return;
    }

  begin_with_proxies (data);
}

static void
reauthentication_channel_ready (GObject      *source,
                                GAsyncResult *res,
                                gpointer      user_data)
{
  BeginData *data = user_data;
  g_autoptr (GError) error = NULL;

  data->proxies.user_verifier =
    gdm_client_open_reauthentication_channel_finish (GDM_CLIENT (source), res, &error);

  if (data->proxies.user_verifier == NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
          begin_should_stop (data))
        {
          finish_begin (data);
          return;
        }

      if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED) &&
          !data->self->reauth_only)
        {
          /* Gdm emits org.freedesktop.DBus.Error.AccessDenied when there
           * is no session to reauthenticate. Fall back to performing
           * verification from this login session, still for the user: the
           * services begin the verification for data->user_name */
          gdm_client_get_user_verifier (data->self->client, data->cancellable,
                                        user_verifier_ready, data);
          return;
        }

      {
        g_autoptr (GError) init_error =
          shell_gdm_init_error_new (g_steal_pointer (&error), NULL,
                                    "Failed to open reauthentication channel");

        report_init_error (data->self, init_error);
      }

      finish_begin (data);
      return;
    }

  if (begin_should_stop (data))
    {
      finish_begin (data);
      return;
    }

  begin_with_proxies (data);
}

static void
begin_get_proxies (BeginData *data)
{
  ShellGdmUserVerifier *self = data->self;

  if (data->user_name != NULL)
    {
      gdm_client_open_reauthentication_channel (self->client, data->user_name,
                                                data->cancellable,
                                                reauthentication_channel_ready, data);
    }
  else
    {
      gdm_client_get_user_verifier (self->client, data->cancellable,
                                    user_verifier_ready, data);
    }
}

/**
 * shell_gdm_user_verifier_begin:
 * @verifier: a #ShellGdmUserVerifier
 * @user_name: (nullable): the user to verify, or %NULL if the services ask for it
 * @hold: (nullable): a #ShellBatchHold to release when the services began
 *
 * Starts the verification, cancelling the one that was in progress.
 */
void
shell_gdm_user_verifier_begin (ShellGdmUserVerifier *verifier,
                               const char           *user_name,
                               ShellBatchHold       *hold)
{
  BeginData *data;

  g_return_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier));
  g_return_if_fail (hold == NULL || SHELL_IS_BATCH_HOLD (hold));

  if (verifier->cancellable != NULL)
    g_cancellable_cancel (verifier->cancellable);
  g_clear_object (&verifier->cancellable);
  verifier->cancellable = g_cancellable_new ();

  data = g_new0 (BeginData, 1);
  data->self = g_object_ref (verifier);
  data->user_name = g_strdup (user_name);
  data->hold = hold != NULL ? g_object_ref (hold) : NULL;
  data->cancellable = g_object_ref (verifier->cancellable);

  verifier->n_begins++;

  if (verifier->shutting_down || verifier->destroyed)
    {
      finish_begin (data);
      return;
    }

  begin_get_proxies (data);
}

/* ---- the operations ---- */

/**
 * shell_gdm_user_verifier_select_mechanism:
 * @verifier: a #ShellGdmUserVerifier
 * @service_name: (nullable): the service of the mechanism, as given in the
 *   `serviceName` of the mechanism
 * @role: (nullable): the role of the mechanism
 *
 * Selects a mechanism. Every group of services updates its selected mechanism.
 *
 * Returns: whether one of the groups of services selected the mechanism
 */
gboolean
shell_gdm_user_verifier_select_mechanism (ShellGdmUserVerifier *verifier,
                                          const char           *service_name,
                                          const char           *role)
{
  g_autoptr (ShellGdmAuthMechanism) mechanism = NULL;
  g_autoptr (GPtrArray) services = NULL;
  gboolean selected = FALSE;
  guint i;

  g_return_val_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier), FALSE);

  if (service_name != NULL || role != NULL)
    mechanism = shell_gdm_auth_mechanism_new (service_name, role);

  /* The selection can change the services */
  services = g_ptr_array_new_with_free_func (g_object_unref);
  for (i = 0; i < verifier->auth_services->len; i++)
    g_ptr_array_add (services, g_object_ref (g_ptr_array_index (verifier->auth_services, i)));

  for (i = 0; i < services->len; i++)
    {
      if (shell_gdm_auth_services_select_mechanism (g_ptr_array_index (services, i), mechanism))
        selected = TRUE;
    }

  return selected;
}

/**
 * shell_gdm_user_verifier_needs_username:
 * @verifier: a #ShellGdmUserVerifier
 *
 * Returns: whether the user has to give the user name before the
 *   verification begins
 */
gboolean
shell_gdm_user_verifier_needs_username (ShellGdmUserVerifier *verifier)
{
  guint i;

  g_return_val_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier), TRUE);

  for (i = 0; i < verifier->auth_services->len; i++)
    {
      if (!shell_gdm_auth_services_needs_username (g_ptr_array_index (verifier->auth_services, i)))
        return FALSE;
    }

  return TRUE;
}

/**
 * shell_gdm_user_verifier_set_driver_service:
 * @verifier: a #ShellGdmUserVerifier
 * @service_name: (nullable): the only service to use, or %NULL for all
 *
 * Restricts the verification to the groups of services that have a service.
 */
void
shell_gdm_user_verifier_set_driver_service (ShellGdmUserVerifier *verifier,
                                            const char           *service_name)
{
  g_return_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier));

  if (g_strcmp0 (verifier->driver_service, service_name) == 0)
    return;

  g_free (verifier->driver_service);
  verifier->driver_service = g_strdup (service_name);
  shell_gdm_user_verifier_reset (verifier);
  update_auth_services (verifier);
}

/**
 * shell_gdm_user_verifier_reset:
 * @verifier: a #ShellGdmUserVerifier
 *
 * Resets the services, cancels the verification and clears the messages.
 */
void
shell_gdm_user_verifier_reset (ShellGdmUserVerifier *verifier)
{
  guint i;

  g_return_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier));

  for (i = 0; i < verifier->auth_services->len; i++)
    shell_gdm_auth_services_reset (g_ptr_array_index (verifier->auth_services, i));

  cancel_user_verifier (verifier);

  shell_gdm_user_verifier_clear (verifier);
}

/**
 * shell_gdm_user_verifier_cancel_requested:
 * @verifier: a #ShellGdmUserVerifier
 *
 * Asks the selected mechanism to go back a step instead of cancelling the
 * whole verification.
 *
 * Returns: whether the mechanism did (the prompt then goes back one step)
 */
gboolean
shell_gdm_user_verifier_cancel_requested (ShellGdmUserVerifier *verifier)
{
  guint i;

  g_return_val_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier), FALSE);

  for (i = 0; i < verifier->auth_services->len; i++)
    {
      ShellGdmAuthServices *services = g_ptr_array_index (verifier->auth_services, i);

      if (shell_gdm_auth_services_get_selected_mechanism (services) != NULL)
        return shell_gdm_auth_services_cancel_requested (services);
    }

  return FALSE;
}

/**
 * shell_gdm_user_verifier_cancel:
 * @verifier: a #ShellGdmUserVerifier
 *
 * Cancels the verification but remembers the selected mechanism.
 */
void
shell_gdm_user_verifier_cancel (ShellGdmUserVerifier *verifier)
{
  guint i;

  g_return_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier));

  for (i = 0; i < verifier->auth_services->len; i++)
    shell_gdm_auth_services_cancel (g_ptr_array_index (verifier->auth_services, i));

  cancel_user_verifier (verifier);

  shell_gdm_user_verifier_clear (verifier);
}

/**
 * shell_gdm_user_verifier_clear:
 * @verifier: a #ShellGdmUserVerifier
 *
 * Clears the services, the messages and the user verifier proxy.
 */
void
shell_gdm_user_verifier_clear (ShellGdmUserVerifier *verifier)
{
  g_autoptr (GPtrArray) services = NULL;
  guint i;

  g_return_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier));

  services = g_ptr_array_new_with_free_func (g_object_unref);
  for (i = 0; i < verifier->auth_services->len; i++)
    g_ptr_array_add (services, g_object_ref (g_ptr_array_index (verifier->auth_services, i)));

  for (i = 0; i < services->len; i++)
    shell_gdm_auth_services_clear (g_ptr_array_index (services, i));

  redistribute_roles (verifier);

  clear_message_queue (verifier);

  if (verifier->cancellable != NULL)
    g_cancellable_cancel (verifier->cancellable);
  g_clear_object (&verifier->cancellable);

  clear_user_verifier (verifier);
}

/**
 * shell_gdm_user_verifier_destroy:
 * @verifier: a #ShellGdmUserVerifier
 *
 * Destroys the services and cancels the verification. The verifier does
 * nothing afterwards.
 */
void
shell_gdm_user_verifier_destroy (ShellGdmUserVerifier *verifier)
{
  g_autoptr (GPtrArray) old_services = NULL;
  guint i;

  g_return_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier));

  if (verifier->destroyed)
    return;

  old_services = verifier->auth_services;
  verifier->auth_services = g_ptr_array_new_with_free_func (g_object_unref);
  for (i = 0; i < old_services->len; i++)
    {
      ShellGdmAuthServices *services = g_ptr_array_index (old_services, i);

      shell_gdm_auth_services_destroy (services);
      g_signal_handlers_disconnect_by_data (services, verifier);
    }

  shell_gdm_user_verifier_cancel (verifier);

  verifier->destroyed = TRUE;
  if (verifier->settings != NULL)
    g_clear_signal_handler (&verifier->settings_changed_id, verifier->settings);
  g_clear_object (&verifier->settings);

  /* Whoever waits for the messages is not going to get them */
  complete_pending_message_tasks (verifier);
}

/**
 * shell_gdm_user_verifier_add_credential_manager:
 * @verifier: a #ShellGdmUserVerifier
 * @service_name: the PAM service that takes the token of @credential_manager
 * @credential_manager: a #ShellCredentialManager
 *
 * Lets the services use the credentials of @credential_manager.
 */
void
shell_gdm_user_verifier_add_credential_manager (ShellGdmUserVerifier   *verifier,
                                                const char             *service_name,
                                                ShellCredentialManager *credential_manager)
{
  guint i;

  g_return_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier));
  g_return_if_fail (SHELL_IS_CREDENTIAL_MANAGER (credential_manager));

  for (i = 0; i < verifier->auth_services->len; i++)
    shell_gdm_auth_services_add_credential_manager (g_ptr_array_index (verifier->auth_services, i),
                                                    service_name, credential_manager);
}

/**
 * shell_gdm_user_verifier_remove_credential_manager:
 * @verifier: a #ShellGdmUserVerifier
 * @service_name: the PAM service of the credential manager
 *
 * Stops using a credential manager.
 */
void
shell_gdm_user_verifier_remove_credential_manager (ShellGdmUserVerifier *verifier,
                                                   const char           *service_name)
{
  guint i;

  g_return_if_fail (SHELL_IS_GDM_USER_VERIFIER (verifier));

  for (i = 0; i < verifier->auth_services->len; i++)
    shell_gdm_auth_services_remove_credential_manager (g_ptr_array_index (verifier->auth_services, i),
                                                       service_name);
}

/* ---- GObject ---- */

static void
shell_gdm_user_verifier_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  ShellGdmUserVerifier *self = SHELL_GDM_USER_VERIFIER (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_set_object (&self->client, g_value_get_object (value));
      break;

    case PROP_REAUTHENTICATION_ONLY:
      self->reauth_only = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_gdm_user_verifier_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  ShellGdmUserVerifier *self = SHELL_GDM_USER_VERIFIER (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_value_set_object (value, self->client);
      break;

    case PROP_REAUTHENTICATION_ONLY:
      g_value_set_boolean (value, self->reauth_only);
      break;

    case PROP_HAS_PENDING_MESSAGES:
      g_value_set_boolean (value, shell_gdm_user_verifier_get_has_pending_messages (self));
      break;

    case PROP_ALLOWED_FAILURES:
      g_value_set_int (value, shell_gdm_user_verifier_get_allowed_failures (self));
      break;

    case PROP_PREEMPTIVE_INPUT:
      g_value_set_boolean (value, shell_gdm_user_verifier_get_preemptive_input (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
on_shutdown (ShellGlobal          *global,
             ShellGdmUserVerifier *self)
{
  self->shutting_down = TRUE;

  g_clear_handle_id (&self->message_queue_timeout_id, g_source_remove);
  g_clear_pointer (&self->awaited_message, queued_message_unref);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  complete_pending_message_tasks (self);
}

static void
shell_gdm_user_verifier_constructed (GObject *object)
{
  ShellGdmUserVerifier *self = SHELL_GDM_USER_VERIFIER (object);
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = NULL;

  G_OBJECT_CLASS (shell_gdm_user_verifier_parent_class)->constructed (object);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  if (source != NULL)
    schema = g_settings_schema_source_lookup (source, SHELL_GDM_LOGIN_SCREEN_SCHEMA, TRUE);

  if (schema != NULL)
    {
      self->settings = g_settings_new_full (schema, NULL, NULL);
      self->settings_changed_id =
        g_signal_connect (self->settings, "changed", G_CALLBACK (on_settings_changed), self);
    }
  else
    {
      g_message ("The schema %s is not installed, no authentication service is available",
                 SHELL_GDM_LOGIN_SCREEN_SCHEMA);
    }

  update_auth_services (self);
}

static void
shell_gdm_user_verifier_dispose (GObject *object)
{
  ShellGdmUserVerifier *self = SHELL_GDM_USER_VERIFIER (object);

  if (!self->destroyed)
    shell_gdm_user_verifier_destroy (self);

  g_clear_handle_id (&self->message_queue_timeout_id, g_source_remove);
  g_clear_pointer (&self->awaited_message, queued_message_unref);
  complete_pending_message_tasks (self);

  g_clear_pointer (&self->auth_services, g_ptr_array_unref);
  g_clear_pointer (&self->message_queue, g_ptr_array_unref);
  clear_user_verifier (self);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->client);

  G_OBJECT_CLASS (shell_gdm_user_verifier_parent_class)->dispose (object);
}

static void
shell_gdm_user_verifier_finalize (GObject *object)
{
  ShellGdmUserVerifier *self = SHELL_GDM_USER_VERIFIER (object);

  g_clear_pointer (&self->driver_service, g_free);

  G_OBJECT_CLASS (shell_gdm_user_verifier_parent_class)->finalize (object);
}

static void
shell_gdm_user_verifier_class_init (ShellGdmUserVerifierClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = shell_gdm_user_verifier_set_property;
  object_class->get_property = shell_gdm_user_verifier_get_property;
  object_class->constructed = shell_gdm_user_verifier_constructed;
  object_class->dispose = shell_gdm_user_verifier_dispose;
  object_class->finalize = shell_gdm_user_verifier_finalize;

  /**
   * ShellGdmUserVerifier:client:
   *
   * The #GdmClient that provides the user verifier.
   */
  props[PROP_CLIENT] =
    g_param_spec_object ("client", NULL, NULL,
                         GDM_TYPE_CLIENT,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellGdmUserVerifier:reauthentication-only:
   *
   * Whether to verify only users who already have a session (the unlock
   * dialog), instead of falling back to the login session (`reauthenticationOnly`
   * of the JS constructor).
   */
  props[PROP_REAUTHENTICATION_ONLY] =
    g_param_spec_boolean ("reauthentication-only", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellGdmUserVerifier:has-pending-messages:
   *
   * Whether messages are waiting to be shown or are being shown (`hasPendingMessages`
   * of the JS class). It does not notify.
   */
  props[PROP_HAS_PENDING_MESSAGES] =
    g_param_spec_boolean ("has-pending-messages", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellGdmUserVerifier:allowed-failures:
   *
   * The `allowed-failures` key of org.gnome.login-screen (`allowedFailures`
   * of the JS class).
   */
  props[PROP_ALLOWED_FAILURES] =
    g_param_spec_int ("allowed-failures", NULL, NULL,
                      0, G_MAXINT, 3,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellGdmUserVerifier:preemptive-input:
   *
   * Whether the user can type before the selected mechanism asks
   * (`selectedMechanism?.preemptiveInput` of the JS class). It does not notify.
   */
  props[PROP_PREEMPTIVE_INPUT] =
    g_param_spec_boolean ("preemptive-input", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellGdmUserVerifier::ask-question:
   * @verifier: the #ShellGdmUserVerifier
   * @service_name: the PAM service that asks
   * @question: the question, as PAM phrased it
   * @secret: whether the answer is a secret
   * @answer_handler: call shell_gdm_auth_handler_invoke () with the answer
   *
   * Emitted when a service wants the user to type something.
   */
  signals[ASK_QUESTION] =
    g_signal_new ("ask-question",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 4,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN,
                  SHELL_TYPE_GDM_AUTH_HANDLER);

  /**
   * ShellGdmUserVerifier::show-message:
   * @verifier: the #ShellGdmUserVerifier
   * @message: (nullable): the message, or %NULL to clear the current one
   * @type: the #ShellGdmMessageType of the message
   * @should_wiggle: whether the prompt should wiggle the message; the prompt
   *   then calls shell_gdm_user_verifier_message_shown () when it is done
   *
   * Emitted when a queued message is to be shown.
   */
  signals[SHOW_MESSAGE] =
    g_signal_new ("show-message",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_STRING, SHELL_TYPE_GDM_MESSAGE_TYPE, G_TYPE_BOOLEAN);

  /**
   * ShellGdmUserVerifier::show-choice-list:
   * @verifier: the #ShellGdmUserVerifier
   * @service_name: the PAM service that asks
   * @prompt_message: (nullable): the title of the list
   * @choice_list: (nullable): an `a(sa{sv})` of the keys of the choices and their
   *   content (`title`, `subtitle`, `iconName`, `iconTitle` and `iconSubtitle`)
   * @choice_handler: (nullable): call shell_gdm_auth_handler_invoke () with the key
   *
   * Emitted when a service wants the user to choose from a list. Without a
   * handler it is a message to show instead of the list.
   */
  signals[SHOW_CHOICE_LIST] =
    g_signal_new ("show-choice-list",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 4,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_VARIANT,
                  SHELL_TYPE_GDM_AUTH_HANDLER);

  /**
   * ShellGdmUserVerifier::show-button:
   * @verifier: the #ShellGdmUserVerifier
   * @service_name: the PAM service that asks
   * @label: the label of the button
   * @handler: call shell_gdm_auth_handler_invoke () when the button is clicked
   *
   * Emitted when a service wants the user to click a button.
   */
  signals[SHOW_BUTTON] =
    g_signal_new ("show-button",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_STRING, G_TYPE_STRING, SHELL_TYPE_GDM_AUTH_HANDLER);

  /**
   * ShellGdmUserVerifier::mechanisms-changed:
   * @verifier: the #ShellGdmUserVerifier
   * @mechanisms: an `aa{sv}` with the mechanisms the user can pick, with the
   *   keys of the JS objects: `serviceName`, `role`, `id`, `name`, `selectable`,
   *   `preemptiveInput`, `hint`, `iconName` and `description`
   * @selected_mechanism: an `a{sv}` with the selected mechanism, empty if none
   *
   * Emitted when the mechanisms change. Call GLib.Variant.recursiveUnpack ()
   * to get the objects of the JS.
   */
  signals[MECHANISMS_CHANGED] =
    g_signal_new ("mechanisms-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_VARIANT, G_TYPE_VARIANT);

  /**
   * ShellGdmUserVerifier::web-login:
   * @verifier: the #ShellGdmUserVerifier
   * @service_name: the PAM service that asks
   * @message: the instructions
   * @url: the address the user has to open
   * @code: (nullable): the code to type there
   * @done_label: the label of the button that says the user is done
   * @done_handler: call shell_gdm_auth_handler_invoke () when the user is done
   *
   * Emitted when a service wants the user to log in on a web page.
   */
  signals[WEB_LOGIN] =
    g_signal_new ("web-login",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 6,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                  G_TYPE_STRING, SHELL_TYPE_GDM_AUTH_HANDLER);

  /**
   * ShellGdmUserVerifier::verification-failed:
   * @verifier: the #ShellGdmUserVerifier
   * @service_name: (nullable): the PAM service that failed
   * @can_retry: whether the user can try again
   *
   * Emitted when a verification failed.
   */
  signals[VERIFICATION_FAILED] =
    g_signal_new ("verification-failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING, G_TYPE_BOOLEAN);

  /**
   * ShellGdmUserVerifier::verification-complete:
   * @verifier: the #ShellGdmUserVerifier
   *
   * Emitted when the user is verified.
   */
  signals[VERIFICATION_COMPLETE] =
    g_signal_new ("verification-complete",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellGdmUserVerifier::reset:
   * @verifier: the #ShellGdmUserVerifier
   * @soft_reset: whether to go back to the prompt of the current user
   * @reuse_entry_text: whether to keep what the user typed
   *
   * Emitted when the prompt has to start again.
   */
  signals[RESET] =
    g_signal_new ("reset",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);

  /**
   * ShellGdmUserVerifier::no-more-messages:
   * @verifier: the #ShellGdmUserVerifier
   *
   * Emitted when the queue of messages ran out.
   */
  signals[NO_MORE_MESSAGES] =
    g_signal_new ("no-more-messages",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_gdm_user_verifier_init (ShellGdmUserVerifier *self)
{
  self->auth_services = g_ptr_array_new_with_free_func (g_object_unref);
  self->message_queue = g_ptr_array_new_with_free_func ((GDestroyNotify) queued_message_unref);
  self->enabled_kinds = -1;
}
