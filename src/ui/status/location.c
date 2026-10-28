/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * location.c: The location agent of the shell for GeoClue, the dialog which
 * asks whether an application may know the location and the indicator of the
 * top bar that shows while the location is in use. Port of
 * js/ui/status/location.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The agent interface is a gdbus-codegen skeleton and the manager a
 *    gdbus-codegen proxy. The system bus is obtained asynchronously, and the
 *    agent is exported, the name of GeoClue watched and the agent added to
 *    GeoClue as soon as it is there; without a system bus or without GeoClue
 *    this is silent (debug messages), as GeoClue is not required.
 *  - The MaxAccuracyLevel property of the skeleton is updated whenever the
 *    setting changes and PropertiesChanged is only emitted when the value
 *    differs from the previous one (the JS emitted it for every call).
 *  - A failed AddAgent () or a failed manager proxy does not stop the agent
 *    from connecting to GeoClue the next time its name appears or the
 *    accuracy level changes (the JS kept `_connecting` set).
 *  - An application the shell does not know (no desktop file) is refused
 *    without writing it to the permission store; the JS replaced the whole
 *    table of the store with that single entry.
 *  - When a request for authorization is still open as the shell shuts down
 *    it is answered with an error, and the lookups of the permission store
 *    and the dialogs are not completed afterwards.
 *  - A dialog which is destroyed without an answer leaves the request open,
 *    as the JS promise stayed pending.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <geoclue.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gi18n-lib.h>

#include "misc/permission-store.h"
#include "shell-app.h"
#include "shell-app-system.h"
#include "shell-global.h"
#include "ui/dialog.h"
#include "ui/status/location.h"
#include "ui/status/status-private.h"

#include "geoclue-dbus.h"

#define LOCATION_SCHEMA "org.gnome.system.location"
#define MAX_ACCURACY_LEVEL_KEY "max-accuracy-level"
#define ENABLED_KEY "enabled"

#define APP_PERMISSIONS_TABLE "location"
#define APP_PERMISSIONS_ID "location"

#define GEOCLUE_BUS_NAME "org.freedesktop.GeoClue2"
#define GEOCLUE_MANAGER_PATH "/org/freedesktop/GeoClue2/Manager"
#define GEOCLUE_AGENT_PATH "/org/freedesktop/GeoClue2/Agent"

/* GeoclueAccuracyLevel */
static const struct
{
  const char *name;
  GClueAccuracyLevel level;
} accuracy_levels[] = {
  { "NONE", GCLUE_ACCURACY_LEVEL_NONE },
  { "COUNTRY", GCLUE_ACCURACY_LEVEL_COUNTRY },
  { "CITY", GCLUE_ACCURACY_LEVEL_CITY },
  { "NEIGHBORHOOD", GCLUE_ACCURACY_LEVEL_NEIGHBORHOOD },
  { "STREET", GCLUE_ACCURACY_LEVEL_STREET },
  { "EXACT", GCLUE_ACCURACY_LEVEL_EXACT },
};

/* accuracyLevelToString () */
static const char *
accuracy_level_to_string (guint accuracy_level)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (accuracy_levels); i++)
    {
      if ((guint) accuracy_levels[i].level == accuracy_level)
        return accuracy_levels[i].name;
    }

  return "NONE";
}

/* GeoclueAccuracyLevel[name] || GeoclueAccuracyLevel.NONE; @name is upper case */
static guint
accuracy_level_from_string (const char *name)
{
  guint i;

  if (name == NULL)
    return GCLUE_ACCURACY_LEVEL_NONE;

  for (i = 0; i < G_N_ELEMENTS (accuracy_levels); i++)
    {
      if (g_str_equal (accuracy_levels[i].name, name))
        return accuracy_levels[i].level;
    }

  return GCLUE_ACCURACY_LEVEL_NONE;
}

/* ======================================================================== */
/* ShellGeolocationDialog                                                   */
/* ======================================================================== */

enum
{
  DIALOG_PROP_0,

  DIALOG_PROP_APP_NAME,
  DIALOG_PROP_REASON,
  DIALOG_PROP_REQ_ACCURACY_LEVEL,

  N_DIALOG_PROPS
};

static GParamSpec *dialog_props[N_DIALOG_PROPS] = { NULL, };

enum
{
  RESPONSE,

  N_DIALOG_SIGNALS
};

static guint dialog_signals[N_DIALOG_SIGNALS] = { 0, };

struct _ShellGeolocationDialog
{
  ShellModalDialog parent;

  char *app_name;
  char *reason;
  guint req_accuracy_level;
};

G_DEFINE_FINAL_TYPE (ShellGeolocationDialog, shell_geolocation_dialog,
                     SHELL_TYPE_MODAL_DIALOG)

/* _onGrantClicked () */
static void
geolocation_dialog_grant_clicked (gpointer user_data)
{
  ShellGeolocationDialog *self = user_data;

  g_signal_emit (self, dialog_signals[RESPONSE], 0, self->req_accuracy_level);
  shell_modal_dialog_close (SHELL_MODAL_DIALOG (self));
}

/* _onDenyClicked () */
static void
geolocation_dialog_deny_clicked (gpointer user_data)
{
  ShellGeolocationDialog *self = user_data;

  g_signal_emit (self, dialog_signals[RESPONSE], 0, (guint) GCLUE_ACCURACY_LEVEL_NONE);
  shell_modal_dialog_close (SHELL_MODAL_DIALOG (self));
}

static void
shell_geolocation_dialog_constructed (GObject *object)
{
  ShellGeolocationDialog *self = SHELL_GEOLOCATION_DIALOG (object);
  ShellModalDialog *dialog = SHELL_MODAL_DIALOG (self);
  g_autofree char *description = NULL;
  ClutterActor *content;
  ClutterActor *reason_label;
  ClutterActor *info_label;
  StButton *button;

  G_OBJECT_CLASS (shell_geolocation_dialog_parent_class)->constructed (object);

  /* Translators: %s is an application name */
  description = g_strdup_printf (_("The app %s wants to access your location"),
                                 self->app_name != NULL ? self->app_name : "");
  content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT,
                          "title", _("Allow location access"),
                          "description", description,
                          NULL);

  reason_label = g_object_new (ST_TYPE_LABEL,
                               "text", self->reason != NULL ? self->reason : "",
                               "style-class", "message-dialog-description",
                               NULL);
  clutter_actor_add_child (content, reason_label);

  info_label = g_object_new (ST_TYPE_LABEL,
                             "text", _("Location access can be changed at any time from the privacy settings"),
                             "style-class", "message-dialog-description",
                             NULL);
  clutter_actor_add_child (content, info_label);

  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (dialog)),
                           content);

  button = shell_modal_dialog_add_button (dialog, _("Deny Access"), CLUTTER_KEY_Escape,
                                          FALSE, geolocation_dialog_deny_clicked,
                                          self, NULL);
  shell_modal_dialog_add_button (dialog, _("Grant Access"), 0, FALSE,
                                 geolocation_dialog_grant_clicked, self, NULL);

  shell_modal_dialog_set_initial_key_focus (dialog, CLUTTER_ACTOR (button));
}

static void
shell_geolocation_dialog_finalize (GObject *object)
{
  ShellGeolocationDialog *self = SHELL_GEOLOCATION_DIALOG (object);

  g_free (self->app_name);
  g_free (self->reason);

  G_OBJECT_CLASS (shell_geolocation_dialog_parent_class)->finalize (object);
}

static void
shell_geolocation_dialog_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellGeolocationDialog *self = SHELL_GEOLOCATION_DIALOG (object);

  switch (prop_id)
    {
    case DIALOG_PROP_APP_NAME:
      g_value_set_string (value, self->app_name);
      break;

    case DIALOG_PROP_REASON:
      g_value_set_string (value, self->reason);
      break;

    case DIALOG_PROP_REQ_ACCURACY_LEVEL:
      g_value_set_uint (value, self->req_accuracy_level);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_geolocation_dialog_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellGeolocationDialog *self = SHELL_GEOLOCATION_DIALOG (object);

  switch (prop_id)
    {
    case DIALOG_PROP_APP_NAME:
      g_free (self->app_name);
      self->app_name = g_value_dup_string (value);
      break;

    case DIALOG_PROP_REASON:
      g_free (self->reason);
      self->reason = g_value_dup_string (value);
      break;

    case DIALOG_PROP_REQ_ACCURACY_LEVEL:
      self->req_accuracy_level = g_value_get_uint (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_geolocation_dialog_class_init (ShellGeolocationDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_geolocation_dialog_constructed;
  object_class->finalize = shell_geolocation_dialog_finalize;
  object_class->get_property = shell_geolocation_dialog_get_property;
  object_class->set_property = shell_geolocation_dialog_set_property;

  /**
   * ShellGeolocationDialog:app-name:
   *
   * The name of the application which wants to know the location.
   */
  dialog_props[DIALOG_PROP_APP_NAME] =
    g_param_spec_string ("app-name", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellGeolocationDialog:reason:
   *
   * Why the application wants the location (`X-Geoclue-Reason`).
   */
  dialog_props[DIALOG_PROP_REASON] =
    g_param_spec_string ("reason", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellGeolocationDialog:req-accuracy-level:
   *
   * The accuracy level which the application asked for (`reqAccuracyLevel`).
   */
  dialog_props[DIALOG_PROP_REQ_ACCURACY_LEVEL] =
    g_param_spec_uint ("req-accuracy-level", NULL, NULL,
                       0, G_MAXUINT, 0,
                       G_PARAM_READWRITE |
                       G_PARAM_CONSTRUCT_ONLY |
                       G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_DIALOG_PROPS, dialog_props);

  /**
   * ShellGeolocationDialog::response:
   * @self: the #ShellGeolocationDialog
   * @accuracy_level: the accuracy level which is granted, 0 if the access is
   *   denied
   *
   * Emitted when the user answered, before the dialog closes.
   */
  dialog_signals[RESPONSE] =
    g_signal_new ("response",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_UINT);
}

static void
shell_geolocation_dialog_init (ShellGeolocationDialog *self)
{
}

/**
 * shell_geolocation_dialog_new:
 * @name: the name of the application
 * @reason: (nullable): the reason the application gave for needing the location
 * @req_accuracy_level: the accuracy level which the application requested
 *
 * Creates the dialog which asks whether the application may know the
 * location (`new GeolocationDialog (name, reason, reqAccuracyLevel)`). Open it
 * with shell_modal_dialog_open () and wait for the ::response signal.
 *
 * Returns: (transfer none): a new #ShellGeolocationDialog, which belongs to the
 *   container of the modal dialogs
 */
ShellGeolocationDialog *
shell_geolocation_dialog_new (const char *name,
                              const char *reason,
                              guint       req_accuracy_level)
{
  return g_object_new (SHELL_TYPE_GEOLOCATION_DIALOG,
                       "dialog-style-class", "geolocation-dialog",
                       "app-name", name,
                       "reason", reason,
                       "req-accuracy-level", req_accuracy_level,
                       NULL);
}

/* ======================================================================== */
/* ShellGeoclueAgent                                                        */
/* ======================================================================== */

enum
{
  AGENT_PROP_0,

  AGENT_PROP_ENABLED,
  AGENT_PROP_IN_USE,
  AGENT_PROP_MAX_ACCURACY_LEVEL,

  N_AGENT_PROPS
};

static GParamSpec *agent_props[N_AGENT_PROPS] = { NULL, };

typedef struct _AppAuthorizer AppAuthorizer;

struct _ShellGeoclueAgent
{
  GObject parent;

  GSettings *settings;                 /* nullable */
  ShellGeoClue2Agent *agent;           /* the skeleton */
  GDBusConnection *system_bus;         /* NULL until it is there */
  guint watch_id;
  ShellGeoClue2Manager *manager_proxy; /* nullable */
  gboolean connecting;
  ShellPortalPermissionStore *perm_store; /* nullable */
  GCancellable *cancellable;
  gboolean shutting_down;

  GPtrArray *authorizers;              /* AppAuthorizer */
};

G_DEFINE_FINAL_TYPE (ShellGeoclueAgent, shell_geoclue_agent, G_TYPE_OBJECT)

/* ------------------------------------------------------------------------ */
/* AppAuthorizer                                                            */
/* ------------------------------------------------------------------------ */

struct _AppAuthorizer
{
  int ref_count;

  ShellGeoclueAgent *agent;            /* NULL once detached */
  GDBusMethodInvocation *invocation;   /* NULL once answered */
  char *desktop_id;
  guint req_accuracy_level;
  ShellPortalPermissionStore *perm_store; /* nullable */
  guint max_accuracy_level;
  GVariant *permissions;               /* a{sas}, nullable */
  guint accuracy_level;
  ShellApp *app;

  ShellGeolocationDialog *dialog;      /* weak */
  gulong response_id;
};

static AppAuthorizer *
app_authorizer_ref (AppAuthorizer *self)
{
  self->ref_count++;
  return self;
}

static void
app_authorizer_unref (AppAuthorizer *self)
{
  if (--self->ref_count > 0)
    return;

  g_clear_weak_pointer (&self->dialog);
  g_clear_object (&self->invocation);
  g_clear_object (&self->perm_store);
  g_clear_object (&self->app);
  g_clear_pointer (&self->permissions, g_variant_unref);
  g_free (self->desktop_id);
  g_free (self);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AppAuthorizer, app_authorizer_unref)

static AppAuthorizer *
app_authorizer_new (ShellGeoclueAgent    *agent,
                    GDBusMethodInvocation *invocation,
                    const char           *desktop_id,
                    guint                 req_accuracy_level)
{
  AppAuthorizer *self = g_new0 (AppAuthorizer, 1);

  self->ref_count = 1;
  self->agent = agent;
  self->invocation = g_object_ref (invocation);
  self->desktop_id = g_strdup (desktop_id);
  self->req_accuracy_level = req_accuracy_level;
  self->perm_store = agent->perm_store != NULL ? g_object_ref (agent->perm_store) : NULL;
  self->max_accuracy_level = shell_geoclue_agent_get_max_accuracy_level (agent);
  self->accuracy_level = GCLUE_ACCURACY_LEVEL_NONE;

  return self;
}

static void
app_authorizer_disconnect_dialog (AppAuthorizer *self)
{
  if (self->dialog != NULL && self->response_id != 0)
    g_signal_handler_disconnect (self->dialog, self->response_id);
  self->response_id = 0;
}

/* The agent does not wait for the authorizer any more; the request of the
 * client stays unanswered unless @error_message is given */
static void
app_authorizer_detach (AppAuthorizer *self,
                       const char    *error_message)
{
  app_authorizer_disconnect_dialog (self);
  self->agent = NULL;

  if (self->invocation != NULL && error_message != NULL)
    g_dbus_method_invocation_return_error_literal (g_steal_pointer (&self->invocation),
                                                   G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                   error_message);
}

static void
app_authorizer_set_finish_cb (GObject      *source,
                              GAsyncResult *res,
                              gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  if (!shell_portal_permission_store_call_set_finish (SHELL_PORTAL_PERMISSION_STORE (source),
                                                      res, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_debug ("Failed to save the location permission: %s", error->message);
}

/* _saveToPermissionStore (): does not wait for the answer */
static void
app_authorizer_save_to_permission_store (AppAuthorizer *self)
{
  GVariantBuilder builder;
  GVariantIter iter;
  GVariant *entry;
  g_autofree char *date_str = NULL;
  const char *strv[3];

  if (self->perm_store == NULL || self->agent == NULL)
    return;

  date_str = g_strdup_printf ("%" G_GINT64_FORMAT,
                              (g_get_real_time () / 1000 + 500) / 1000);
  strv[0] = accuracy_level_to_string (self->accuracy_level);
  strv[1] = date_str;
  strv[2] = NULL;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sas}"));
  if (self->permissions != NULL)
    {
      g_variant_iter_init (&iter, self->permissions);
      while ((entry = g_variant_iter_next_value (&iter)) != NULL)
        {
          const char *key = NULL;

          g_variant_get_child (entry, 0, "&s", &key);
          if (g_strcmp0 (key, self->desktop_id) != 0)
            g_variant_builder_add_value (&builder, entry);
          g_variant_unref (entry);
        }
    }
  g_variant_builder_add (&builder, "{s@as}", self->desktop_id,
                         g_variant_new_strv (strv, -1));

  shell_portal_permission_store_call_set (self->perm_store,
                                          APP_PERMISSIONS_TABLE,
                                          TRUE,
                                          APP_PERMISSIONS_ID,
                                          g_variant_builder_end (&builder),
                                          g_variant_new_array (G_VARIANT_TYPE_VARIANT, NULL, 0),
                                          self->agent->cancellable,
                                          app_authorizer_set_finish_cb,
                                          NULL);
}

/* _completeAuth () and the reply of AuthorizeAppAsync () */
static void
app_authorizer_complete (AppAuthorizer *self)
{
  ShellGeoclueAgent *agent = self->agent;
  g_autoptr (AppAuthorizer) keep_alive = NULL;
  GDBusMethodInvocation *invocation;

  if (agent == NULL)
    return;

  /* Removing the authorizer from the agent drops the reference of the agent */
  keep_alive = app_authorizer_ref (self);

  if (self->accuracy_level != GCLUE_ACCURACY_LEVEL_NONE)
    self->accuracy_level = MIN (self->accuracy_level, self->max_accuracy_level);

  if (self->app != NULL)
    app_authorizer_save_to_permission_store (self);

  invocation = g_steal_pointer (&self->invocation);
  app_authorizer_disconnect_dialog (self);
  self->agent = NULL;
  g_ptr_array_remove (agent->authorizers, self);

  if (invocation != NULL)
    shell_geo_clue2_agent_complete_authorize_app (agent->agent, invocation,
                                                  self->accuracy_level != GCLUE_ACCURACY_LEVEL_NONE,
                                                  self->accuracy_level);
}

static void
app_authorizer_response_cb (ShellGeolocationDialog *dialog,
                            guint                   accuracy_level,
                            AppAuthorizer          *self)
{
  app_authorizer_disconnect_dialog (self);
  self->accuracy_level = accuracy_level;
  app_authorizer_complete (self);
}

/* _userAuthorizeApp () */
static void
app_authorizer_user_authorize_app (AppAuthorizer *self)
{
  GDesktopAppInfo *app_info = shell_app_get_app_info (self->app);
  g_autofree char *reason = NULL;
  ShellGeolocationDialog *dialog;

  if (app_info != NULL)
    reason = g_desktop_app_info_get_locale_string (app_info, "X-Geoclue-Reason");

  dialog = shell_geolocation_dialog_new (shell_app_get_name (self->app),
                                         reason, self->req_accuracy_level);
  g_set_weak_pointer (&self->dialog, dialog);
  self->response_id =
    g_signal_connect_data (dialog, "response",
                           G_CALLBACK (app_authorizer_response_cb),
                           app_authorizer_ref (self),
                           (GClosureNotify) app_authorizer_unref, 0);
  shell_modal_dialog_open (SHELL_MODAL_DIALOG (dialog));
}

static void
app_authorizer_lookup_cb (GObject      *source,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  AppAuthorizer *self = user_data;
  g_autoptr (GVariant) permissions = NULL;
  g_autoptr (GVariant) data = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) permission = NULL;

  if (!shell_portal_permission_store_call_lookup_finish (SHELL_PORTAL_PERMISSION_STORE (source),
                                                         &permissions, &data, res, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) || self->agent == NULL)
        {
          app_authorizer_unref (self);
          return;
        }

      if (error->domain == G_DBUS_ERROR)
        {
          /* Likely no xdg-app installed, just authorize the app */
          self->accuracy_level = self->req_accuracy_level;
          g_clear_object (&self->perm_store);
          app_authorizer_complete (self);
          app_authorizer_unref (self);
          return;
        }

      /* Currently xdg-app throws an error if we lookup for
       * unknown ID (which would be the case first time this code
       * runs) so we continue with user authorization as normal
       * and ID is added to the store if user says "yes". */
      g_debug ("Failed to look up the location permission: %s", error->message);
    }

  if (self->agent == NULL)
    {
      app_authorizer_unref (self);
      return;
    }

  g_clear_pointer (&self->permissions, g_variant_unref);
  self->permissions = g_steal_pointer (&permissions);

  if (self->permissions != NULL)
    permission = g_variant_lookup_value (self->permissions, self->desktop_id,
                                         G_VARIANT_TYPE ("as"));

  if (permission == NULL)
    {
      app_authorizer_user_authorize_app (self);
    }
  else
    {
      const char *level_str = NULL;

      if (g_variant_n_children (permission) > 0)
        g_variant_get_child (permission, 0, "&s", &level_str);

      self->accuracy_level = accuracy_level_from_string (level_str);
      app_authorizer_complete (self);
    }

  app_authorizer_unref (self);
}

/* authorize () */
static void
app_authorizer_authorize (AppAuthorizer *self)
{
  g_autofree char *app_id = g_strdup_printf ("%s.desktop", self->desktop_id);
  ShellApp *app = shell_app_system_lookup_app (shell_app_system_get_default (), app_id);

  if (app != NULL)
    self->app = g_object_ref (app);

  if (self->app == NULL || self->perm_store == NULL)
    {
      app_authorizer_complete (self);
      return;
    }

  shell_portal_permission_store_call_lookup (self->perm_store,
                                             APP_PERMISSIONS_TABLE,
                                             APP_PERMISSIONS_ID,
                                             self->agent->cancellable,
                                             app_authorizer_lookup_cb,
                                             app_authorizer_ref (self));
}

/* ------------------------------------------------------------------------ */
/* The agent                                                                */
/* ------------------------------------------------------------------------ */

gboolean
shell_geoclue_agent_get_enabled (ShellGeoclueAgent *self)
{
  g_return_val_if_fail (SHELL_IS_GEOCLUE_AGENT (self), FALSE);

  return self->settings != NULL &&
         g_settings_get_boolean (self->settings, ENABLED_KEY);
}

/**
 * shell_geoclue_agent_set_enabled:
 * @self: a #ShellGeoclueAgent
 * @enabled: whether location services are enabled
 *
 * Sets #ShellGeoclueAgent:enabled, the `enabled` key of org.gnome.system.location.
 */
void
shell_geoclue_agent_set_enabled (ShellGeoclueAgent *self,
                                 gboolean           enabled)
{
  g_return_if_fail (SHELL_IS_GEOCLUE_AGENT (self));

  if (self->settings != NULL)
    g_settings_set_boolean (self->settings, ENABLED_KEY, enabled);
}

/**
 * shell_geoclue_agent_get_in_use:
 * @self: a #ShellGeoclueAgent
 *
 * Gets whether an application is using the location (`inUse`).
 *
 * Returns: %TRUE if GeoClue is running and says the location is in use
 */
gboolean
shell_geoclue_agent_get_in_use (ShellGeoclueAgent *self)
{
  g_return_val_if_fail (SHELL_IS_GEOCLUE_AGENT (self), FALSE);

  return self->manager_proxy != NULL &&
         shell_geo_clue2_manager_get_in_use (self->manager_proxy);
}

/**
 * shell_geoclue_agent_get_max_accuracy_level:
 * @self: a #ShellGeoclueAgent
 *
 * Gets the highest accuracy level which applications can be given
 * (`maxAccuracyLevel`): 0 if location services are disabled, otherwise the
 * one of the `max-accuracy-level` setting.
 *
 * Returns: a `GClueAccuracyLevel`
 */
int
shell_geoclue_agent_get_max_accuracy_level (ShellGeoclueAgent *self)
{
  g_autofree char *level = NULL;
  g_autofree char *upper = NULL;

  g_return_val_if_fail (SHELL_IS_GEOCLUE_AGENT (self), GCLUE_ACCURACY_LEVEL_NONE);

  if (!shell_geoclue_agent_get_enabled (self))
    return GCLUE_ACCURACY_LEVEL_NONE;

  level = g_settings_get_string (self->settings, MAX_ACCURACY_LEVEL_KEY);
  upper = g_ascii_strup (level, -1);
  return accuracy_level_from_string (upper);
}

/* _notifyMaxAccuracyLevel () */
static void
agent_notify_max_accuracy_level (ShellGeoclueAgent *self)
{
  shell_geo_clue2_agent_set_max_accuracy_level (self->agent,
                                                shell_geoclue_agent_get_max_accuracy_level (self));
}

static void
agent_manager_proxy_notify_in_use_cb (GObject           *proxy,
                                      GParamSpec        *pspec,
                                      ShellGeoclueAgent *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), agent_props[AGENT_PROP_IN_USE]);
}

static void
agent_add_agent_cb (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  ShellGeoclueAgent *self;
  gboolean ok;

  ok = shell_geo_clue2_manager_call_add_agent_finish (SHELL_GEO_CLUE2_MANAGER (source),
                                                      res, &error);
  if (!ok && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  self = user_data;
  self->connecting = FALSE;

  if (ok)
    agent_notify_max_accuracy_level (self);
  else
    g_debug ("Failed to add the location agent to GeoClue: %s", error->message);
}

static void
agent_manager_proxy_ready_cb (GObject      *source,
                              GAsyncResult *res,
                              gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  ShellGeoClue2Manager *proxy;
  ShellGeoclueAgent *self;

  proxy = shell_geo_clue2_manager_proxy_new_finish (res, &error);
  if (proxy == NULL && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  self = user_data;
  if (proxy == NULL)
    {
      self->connecting = FALSE;
      g_debug ("Failed to connect to GeoClue: %s", error->message);
      return;
    }

  self->manager_proxy = proxy;
  g_signal_connect_object (proxy, "notify::in-use",
                           G_CALLBACK (agent_manager_proxy_notify_in_use_cb), self, 0);

  g_object_notify_by_pspec (G_OBJECT (self), agent_props[AGENT_PROP_IN_USE]);

  shell_geo_clue2_manager_call_add_agent (proxy, "gnome-shell",
                                          self->cancellable,
                                          agent_add_agent_cb, self);
}

/* _connectToGeoclue (): returns whether it started to connect */
static gboolean
agent_connect_to_geoclue (ShellGeoclueAgent *self)
{
  if (self->manager_proxy != NULL || self->connecting ||
      self->system_bus == NULL || self->shutting_down)
    return FALSE;

  self->connecting = TRUE;
  shell_geo_clue2_manager_proxy_new (self->system_bus,
                                     G_DBUS_PROXY_FLAGS_NONE,
                                     GEOCLUE_BUS_NAME,
                                     GEOCLUE_MANAGER_PATH,
                                     self->cancellable,
                                     agent_manager_proxy_ready_cb,
                                     self);
  return TRUE;
}

/* _onMaxAccuracyLevelChanged () */
static void
agent_max_accuracy_level_changed (ShellGeoclueAgent *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), agent_props[AGENT_PROP_MAX_ACCURACY_LEVEL]);

  /* Gotta ensure geoclue is up and we are registered as agent to it
   * before we emit the notify for this property change. */
  if (!agent_connect_to_geoclue (self))
    agent_notify_max_accuracy_level (self);
}

static void
agent_settings_enabled_changed_cb (ShellGeoclueAgent *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), agent_props[AGENT_PROP_ENABLED]);
  agent_max_accuracy_level_changed (self);
}

/* _onGeoclueVanished () */
static void
agent_geoclue_vanished_cb (GDBusConnection *connection,
                           const char      *name,
                           gpointer         user_data)
{
  ShellGeoclueAgent *self = user_data;

  if (self->manager_proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->manager_proxy, self);
  g_clear_object (&self->manager_proxy);

  g_object_notify_by_pspec (G_OBJECT (self), agent_props[AGENT_PROP_IN_USE]);
}

static void
agent_geoclue_appeared_cb (GDBusConnection *connection,
                           const char      *name,
                           const char      *name_owner,
                           gpointer         user_data)
{
  agent_connect_to_geoclue (user_data);
}

static gboolean
agent_handle_authorize_app (ShellGeoClue2Agent    *skeleton,
                            GDBusMethodInvocation *invocation,
                            const char            *desktop_id,
                            guint                  req_accuracy_level,
                            ShellGeoclueAgent     *self)
{
  AppAuthorizer *authorizer;

  if (self->shutting_down)
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                     "The shell is shutting down");
      return TRUE;
    }

  authorizer = app_authorizer_new (self, invocation, desktop_id, req_accuracy_level);
  g_ptr_array_add (self->authorizers, authorizer);
  app_authorizer_authorize (authorizer);

  return TRUE;
}

static void
agent_system_bus_ready_cb (GObject      *source,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) connection = NULL;
  ShellGeoclueAgent *self;

  connection = g_bus_get_finish (res, &error);
  if (connection == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("No system bus for the location agent: %s", error->message);
      return;
    }

  self = user_data;
  self->system_bus = g_steal_pointer (&connection);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->agent),
                                         self->system_bus, GEOCLUE_AGENT_PATH, &error))
    g_warning ("Failed to export the location agent: %s", error->message);

  self->watch_id = g_bus_watch_name_on_connection (self->system_bus,
                                                   GEOCLUE_BUS_NAME,
                                                   G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                   agent_geoclue_appeared_cb,
                                                   agent_geoclue_vanished_cb,
                                                   self, NULL);
  agent_connect_to_geoclue (self);
}

static void
agent_perm_store_ready_cb (GObject      *source,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  ShellPortalPermissionStore *store;
  ShellGeoclueAgent *self;

  store = shell_permission_store_new_finish (res, &error);
  if (store == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("No permission store for the location agent: %s", error->message);
      return;
    }

  self = user_data;
  g_clear_object (&self->perm_store);
  self->perm_store = store;
}

static void
agent_shutdown_cb (ShellGlobal       *global,
                   ShellGeoclueAgent *self)
{
  guint i;

  self->shutting_down = TRUE;
  g_cancellable_cancel (self->cancellable);
  g_clear_handle_id (&self->watch_id, g_bus_unwatch_name);

  for (i = 0; i < self->authorizers->len; i++)
    app_authorizer_detach (g_ptr_array_index (self->authorizers, i),
                           "The shell is shutting down");
  g_ptr_array_set_size (self->authorizers, 0);
}

static void
shell_geoclue_agent_constructed (GObject *object)
{
  ShellGeoclueAgent *self = SHELL_GEOCLUE_AGENT (object);

  G_OBJECT_CLASS (shell_geoclue_agent_parent_class)->constructed (object);

  self->settings = shell_status_settings_new (LOCATION_SCHEMA);
  if (self->settings != NULL)
    {
      g_signal_connect_object (self->settings, "changed::" ENABLED_KEY,
                               G_CALLBACK (agent_settings_enabled_changed_cb),
                               self, G_CONNECT_SWAPPED);
      g_signal_connect_object (self->settings, "changed::" MAX_ACCURACY_LEVEL_KEY,
                               G_CALLBACK (agent_max_accuracy_level_changed),
                               self, G_CONNECT_SWAPPED);
    }

  self->agent = shell_geo_clue2_agent_skeleton_new ();
  g_signal_connect_object (self->agent, "handle-authorize-app",
                           G_CALLBACK (agent_handle_authorize_app), self, 0);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (agent_shutdown_cb), self, 0);

  agent_max_accuracy_level_changed (self);

  g_bus_get (G_BUS_TYPE_SYSTEM, self->cancellable, agent_system_bus_ready_cb, self);
  shell_permission_store_new (self->cancellable, agent_perm_store_ready_cb, self);
}

static void
shell_geoclue_agent_dispose (GObject *object)
{
  ShellGeoclueAgent *self = SHELL_GEOCLUE_AGENT (object);
  guint i;

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_handle_id (&self->watch_id, g_bus_unwatch_name);

  for (i = 0; i < self->authorizers->len; i++)
    app_authorizer_detach (g_ptr_array_index (self->authorizers, i), NULL);
  g_ptr_array_set_size (self->authorizers, 0);

  if (self->manager_proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->manager_proxy, self);
  g_clear_object (&self->manager_proxy);

  if (self->agent != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->agent, self);
      if (g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (self->agent)) != NULL)
        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->agent));
    }
  g_clear_object (&self->agent);

  g_clear_object (&self->perm_store);
  g_clear_object (&self->system_bus);
  g_clear_object (&self->settings);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (shell_geoclue_agent_parent_class)->dispose (object);
}

static void
shell_geoclue_agent_finalize (GObject *object)
{
  ShellGeoclueAgent *self = SHELL_GEOCLUE_AGENT (object);

  g_clear_pointer (&self->authorizers, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_geoclue_agent_parent_class)->finalize (object);
}

static void
shell_geoclue_agent_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellGeoclueAgent *self = SHELL_GEOCLUE_AGENT (object);

  switch (prop_id)
    {
    case AGENT_PROP_ENABLED:
      g_value_set_boolean (value, shell_geoclue_agent_get_enabled (self));
      break;

    case AGENT_PROP_IN_USE:
      g_value_set_boolean (value, shell_geoclue_agent_get_in_use (self));
      break;

    case AGENT_PROP_MAX_ACCURACY_LEVEL:
      g_value_set_int (value, shell_geoclue_agent_get_max_accuracy_level (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_geoclue_agent_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  ShellGeoclueAgent *self = SHELL_GEOCLUE_AGENT (object);

  switch (prop_id)
    {
    case AGENT_PROP_ENABLED:
      shell_geoclue_agent_set_enabled (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_geoclue_agent_class_init (ShellGeoclueAgentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_geoclue_agent_constructed;
  object_class->dispose = shell_geoclue_agent_dispose;
  object_class->finalize = shell_geoclue_agent_finalize;
  object_class->get_property = shell_geoclue_agent_get_property;
  object_class->set_property = shell_geoclue_agent_set_property;

  /**
   * ShellGeoclueAgent:enabled:
   *
   * Whether location services are enabled (the `enabled` key of
   * org.gnome.system.location).
   */
  agent_props[AGENT_PROP_ENABLED] =
    g_param_spec_boolean ("enabled", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellGeoclueAgent:in-use:
   *
   * Whether an application is using the location.
   */
  agent_props[AGENT_PROP_IN_USE] =
    g_param_spec_boolean ("in-use", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellGeoclueAgent:max-accuracy-level:
   *
   * The highest accuracy level which applications are given, a
   * `GClueAccuracyLevel`.
   */
  agent_props[AGENT_PROP_MAX_ACCURACY_LEVEL] =
    g_param_spec_int ("max-accuracy-level", NULL, NULL,
                      0, 8, 0,
                      G_PARAM_READABLE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_AGENT_PROPS, agent_props);
}

static void
shell_geoclue_agent_init (ShellGeoclueAgent *self)
{
  self->cancellable = g_cancellable_new ();
  self->authorizers = g_ptr_array_new_with_free_func ((GDestroyNotify) app_authorizer_unref);
}

static ShellGeoclueAgent *geoclue_agent = NULL;

/**
 * shell_geoclue_agent_get_default:
 *
 * Gets the location agent of the shell, which is created the first time this
 * is called (`getGeoclueAgent ()`).
 *
 * Returns: (transfer none): the #ShellGeoclueAgent singleton
 */
ShellGeoclueAgent *
shell_geoclue_agent_get_default (void)
{
  if (geoclue_agent == NULL)
    geoclue_agent = g_object_new (SHELL_TYPE_GEOCLUE_AGENT, NULL);

  return geoclue_agent;
}

/* ======================================================================== */
/* ShellLocationIndicator                                                   */
/* ======================================================================== */

struct _ShellLocationIndicator
{
  ShellSystemIndicator parent;
};

G_DEFINE_FINAL_TYPE (ShellLocationIndicator, shell_location_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static void
shell_location_indicator_constructed (GObject *object)
{
  ShellLocationIndicator *self = SHELL_LOCATION_INDICATOR (object);
  ShellGeoclueAgent *agent;
  StIcon *indicator;

  G_OBJECT_CLASS (shell_location_indicator_parent_class)->constructed (object);

  agent = shell_geoclue_agent_get_default ();

  indicator = shell_system_indicator_add_indicator (SHELL_SYSTEM_INDICATOR (self));
  st_icon_set_icon_name (indicator, "location-services-active-symbolic");
  g_object_bind_property (agent, "in-use",
                          indicator, "visible",
                          G_BINDING_SYNC_CREATE);
}

static void
shell_location_indicator_class_init (ShellLocationIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_location_indicator_constructed;
}

static void
shell_location_indicator_init (ShellLocationIndicator *self)
{
}

/**
 * shell_location_indicator_new:
 *
 * Creates the location indicator: the icon of the top bar, which is shown
 * while the location is in use (`new Indicator ()`).
 *
 * Returns: (transfer floating): a new #ShellLocationIndicator
 */
ShellLocationIndicator *
shell_location_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_LOCATION_INDICATOR, NULL);
}
