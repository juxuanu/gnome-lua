/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * parental-controls-manager.c: A GNOME Shell component to wrap the
 * interactions over D-Bus with the malcontent library. Port of
 * js/misc/parentalControlsManager.js.
 *
 * Copyright (C) 2018, 2019, 2020 Endless Mobile, Inc.
 *
 * It is possible for the user's parental controls to change at runtime if
 * the Parental Controls application is used by an administrator from within
 * the user's session.
 *
 * The JS found out at run time whether libmalcontent (>= 0.6.0, and >= 0.14.0
 * for the session limits) was available. Here that is decided at build time
 * (the `malcontent` Meson feature, HAVE_MALCONTENT and HAVE_MALCONTENT_0_14).
 * Without libmalcontent the manager is initialized from the start, no app is
 * filtered and no limits are enabled, like the JS without the typelib.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#ifdef HAVE_MALCONTENT
#include <libmalcontent/malcontent.h>
#endif

#include "misc/parental-controls-manager.h"
#include "shell-global.h"
#include "shell-util.h"

enum
{
  PROP_0,

  PROP_INITIALIZED,
  PROP_ANY_PARENTAL_CONTROLS_ENABLED,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  APP_FILTER_CHANGED,
  SESSION_LIMITS_CHANGED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellParentalControlsManager
{
  GObject parent;

  GCancellable *cancellable;

  gboolean initialized;
  gboolean disabled;

#ifdef HAVE_MALCONTENT
  MctManager *manager;
  MctAppFilter *app_filter;
  MctSessionLimits *session_limits;
#endif
};

G_DEFINE_FINAL_TYPE (ShellParentalControlsManager,
                     shell_parental_controls_manager,
                     G_TYPE_OBJECT)

static void
emit_initialized (ShellParentalControlsManager *self)
{
  self->initialized = TRUE;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_INITIALIZED]);

  g_signal_emit (self, signals[APP_FILTER_CHANGED], 0);
  g_signal_emit (self, signals[SESSION_LIMITS_CHANGED], 0);
  g_object_notify_by_pspec (G_OBJECT (self),
                            props[PROP_ANY_PARENTAL_CONTROLS_ENABLED]);
}

#ifdef HAVE_MALCONTENT

static gboolean
is_disabled_error (GError *error)
{
  return g_error_matches (error, MCT_MANAGER_ERROR, MCT_MANAGER_ERROR_DISABLED);
}

static gboolean
is_cancelled_error (ShellParentalControlsManager *self,
                    GError                       *error)
{
  return g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
         g_cancellable_is_cancelled (self->cancellable);
}

static void
on_app_filter_changed_ready (GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  g_autoptr (ShellParentalControlsManager) self = user_data;
  g_autoptr (MctAppFilter) app_filter = NULL;
  g_autoptr (GError) error = NULL;

  app_filter = mct_manager_get_app_filter_finish (MCT_MANAGER (source), res, &error);
  if (app_filter == NULL)
    {
      if (is_cancelled_error (self, error))
        return;

      if (!is_disabled_error (error))
        {
          /* Log an error and keep the old app filter. */
          g_warning ("Failed to get new MctAppFilter for uid %d on app-filter-changed: %s",
                     shell_util_get_uid (), error->message);
          return;
        }

      g_debug ("Parental controls globally disabled");
      self->disabled = TRUE;
    }

  g_clear_pointer (&self->app_filter, mct_app_filter_unref);
  self->app_filter = g_steal_pointer (&app_filter);

  g_signal_emit (self, signals[APP_FILTER_CHANGED], 0);
  g_object_notify_by_pspec (G_OBJECT (self),
                            props[PROP_ANY_PARENTAL_CONTROLS_ENABLED]);
}

static void
on_app_filter_changed (MctManager                   *manager,
                       uint64_t                      uid,
                       ShellParentalControlsManager *self)
{
  /* Emit 'changed' signal only if app-filter is changed for currently
   * logged-in user. */
  if ((uint64_t) shell_util_get_uid () != uid)
    return;

  mct_manager_get_app_filter_async (manager, shell_util_get_uid (),
                                    MCT_MANAGER_GET_VALUE_FLAGS_NONE,
                                    self->cancellable,
                                    on_app_filter_changed_ready,
                                    g_object_ref (self));
}

#ifdef HAVE_MALCONTENT_0_14

static void
on_session_limits_changed_ready (GObject      *source,
                                 GAsyncResult *res,
                                 gpointer      user_data)
{
  g_autoptr (ShellParentalControlsManager) self = user_data;
  g_autoptr (MctSessionLimits) session_limits = NULL;
  g_autoptr (GError) error = NULL;

  session_limits = mct_manager_get_session_limits_finish (MCT_MANAGER (source),
                                                          res, &error);
  if (session_limits == NULL)
    {
      if (is_cancelled_error (self, error))
        return;

      if (!is_disabled_error (error))
        {
          /* Keep the old session limits */
          g_warning ("Failed to get new MctSessionLimits for uid %d on session-limits-changed: %s",
                     shell_util_get_uid (), error->message);
          return;
        }

      g_debug ("Parental controls globally disabled");
      self->disabled = TRUE;
    }

  g_clear_pointer (&self->session_limits, mct_session_limits_unref);
  self->session_limits = g_steal_pointer (&session_limits);

  g_signal_emit (self, signals[SESSION_LIMITS_CHANGED], 0);
  g_object_notify_by_pspec (G_OBJECT (self),
                            props[PROP_ANY_PARENTAL_CONTROLS_ENABLED]);
}

static void
on_session_limits_changed (MctManager                   *manager,
                           uint64_t                      uid,
                           ShellParentalControlsManager *self)
{
  /* Emit 'changed' signal only if session-limits are changed for currently
   * logged-in user */
  if ((uint64_t) shell_util_get_uid () != uid)
    return;

  mct_manager_get_session_limits_async (manager, shell_util_get_uid (),
                                        MCT_MANAGER_GET_VALUE_FLAGS_NONE,
                                        self->cancellable,
                                        on_session_limits_changed_ready,
                                        g_object_ref (self));
}

static void
on_session_limits_ready (GObject      *source,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  g_autoptr (ShellParentalControlsManager) self = user_data;
  g_autoptr (MctSessionLimits) session_limits = NULL;
  g_autoptr (GError) error = NULL;

  session_limits = mct_manager_get_session_limits_finish (MCT_MANAGER (source),
                                                          res, &error);
  if (session_limits == NULL)
    {
      if (is_cancelled_error (self, error))
        return;

      if (!is_disabled_error (error))
        {
          g_warning ("Failed to get parental controls settings: %s",
                     error->message);
          return;
        }

      g_debug ("Parental controls globally disabled");
      self->disabled = TRUE;
    }

  self->session_limits = g_steal_pointer (&session_limits);

  g_signal_connect_object (self->manager, "session-limits-changed",
                           G_CALLBACK (on_session_limits_changed), self, 0);
  g_signal_connect_object (self->manager, "app-filter-changed",
                           G_CALLBACK (on_app_filter_changed), self, 0);

  /* Signal initialisation is complete. */
  emit_initialized (self);
}

#endif /* HAVE_MALCONTENT_0_14 */

static void
on_app_filter_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  g_autoptr (ShellParentalControlsManager) self = user_data;
  g_autoptr (MctAppFilter) app_filter = NULL;
  g_autoptr (GError) error = NULL;

  app_filter = mct_manager_get_app_filter_finish (MCT_MANAGER (source), res, &error);
  if (app_filter == NULL)
    {
      if (is_cancelled_error (self, error))
        return;

      if (!is_disabled_error (error))
        {
          g_warning ("Failed to get parental controls settings: %s",
                     error->message);
          return;
        }

      g_debug ("Parental controls globally disabled");
      self->disabled = TRUE;
    }

  self->app_filter = g_steal_pointer (&app_filter);

#ifdef HAVE_MALCONTENT_0_14
  mct_manager_get_session_limits_async (self->manager, shell_util_get_uid (),
                                        MCT_MANAGER_GET_VALUE_FLAGS_NONE,
                                        self->cancellable,
                                        on_session_limits_ready,
                                        g_object_ref (self));
#else
  g_signal_connect_object (self->manager, "app-filter-changed",
                           G_CALLBACK (on_app_filter_changed), self, 0);

  /* Signal initialisation is complete. */
  emit_initialized (self);
#endif
}

static void
on_system_bus_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  g_autoptr (ShellParentalControlsManager) self = user_data;
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GError) error = NULL;

  connection = g_bus_get_finish (res, &error);
  if (connection == NULL)
    {
      if (!is_cancelled_error (self, error))
        {
          g_warning ("Failed to get parental controls settings: %s",
                     error->message);
        }
      return;
    }

  self->manager = mct_manager_new (connection);
  mct_manager_get_app_filter_async (self->manager, shell_util_get_uid (),
                                    MCT_MANAGER_GET_VALUE_FLAGS_NONE,
                                    self->cancellable,
                                    on_app_filter_ready,
                                    g_object_ref (self));
}

#endif /* HAVE_MALCONTENT */

static void
initialize_manager (ShellParentalControlsManager *self)
{
#ifdef HAVE_MALCONTENT
  g_bus_get (G_BUS_TYPE_SYSTEM, self->cancellable,
             on_system_bus_ready, g_object_ref (self));
#else
  g_debug ("Skipping parental controls support, malcontent not found");
  emit_initialized (self);
#endif
}

static void
on_shutdown (ShellGlobal                  *global,
             ShellParentalControlsManager *self)
{
  /* Nothing may reach the callers once the shell is going away. */
  g_cancellable_cancel (self->cancellable);

#ifdef HAVE_MALCONTENT
  if (self->manager != NULL)
    g_signal_handlers_disconnect_by_data (self->manager, self);
#endif
}

/**
 * shell_parental_controls_manager_get_default:
 *
 * Gets the #ShellParentalControlsManager singleton. It starts initializing
 * on first use; #ShellParentalControlsManager:initialized tells when that is
 * done.
 *
 * Returns: (transfer none): the #ShellParentalControlsManager
 */
ShellParentalControlsManager *
shell_parental_controls_manager_get_default (void)
{
  static ShellParentalControlsManager *default_manager = NULL;

  if (default_manager == NULL)
    default_manager = g_object_new (SHELL_TYPE_PARENTAL_CONTROLS_MANAGER, NULL);

  return default_manager;
}

/**
 * shell_parental_controls_manager_get_initialized:
 * @manager: a #ShellParentalControlsManager
 *
 * Returns: whether the parental controls settings were retrieved (or
 *   libmalcontent is not available)
 */
gboolean
shell_parental_controls_manager_get_initialized (ShellParentalControlsManager *manager)
{
  g_return_val_if_fail (SHELL_IS_PARENTAL_CONTROLS_MANAGER (manager), FALSE);

  return manager->initialized;
}

/**
 * shell_parental_controls_manager_should_show_app:
 * @manager: a #ShellParentalControlsManager
 * @app_info: a #GAppInfo, usually a #GDesktopAppInfo
 *
 * Calculates whether the given app should be shown on the desktop, in search
 * results, etc. The app should be shown if:
 *
 *  - The .desktop file doesn't say it should be hidden.
 *  - The executable from the .desktop file's Exec line isn't denied in the
 *    user's parental controls.
 *  - None of the flatpak app IDs from the X-Flatpak and the
 *    X-Flatpak-RenamedFrom lines are denied in the user's parental controls.
 *
 * Until the manager is initialized no app is shown.
 *
 * Returns: %TRUE if the app should be shown
 */
gboolean
shell_parental_controls_manager_should_show_app (ShellParentalControlsManager *manager,
                                                 GAppInfo                     *app_info)
{
  g_return_val_if_fail (SHELL_IS_PARENTAL_CONTROLS_MANAGER (manager), FALSE);
  g_return_val_if_fail (G_IS_APP_INFO (app_info), FALSE);

  /* Quick decision? */
  if (!g_app_info_should_show (app_info))
    return FALSE;

#ifdef HAVE_MALCONTENT
  /* Are parental controls enabled (at configure time or runtime)? */
  if (manager->disabled)
    return TRUE;

  /* Have we finished initialising yet? */
  if (!manager->initialized)
    {
      g_debug ("Hiding app because parental controls not yet initialised: %s",
               g_app_info_get_id (app_info));
      return FALSE;
    }

  return manager->app_filter == NULL ||
         mct_app_filter_is_appinfo_allowed (manager->app_filter, app_info);
#else
  return TRUE;
#endif
}

/**
 * shell_parental_controls_manager_app_filter_enabled:
 * @manager: a #ShellParentalControlsManager
 *
 * Checks whether parental controls app filtering is enabled for the account,
 * and if the library supports the features required for the functionality.
 *
 * Typically you should check whether a certain app is filtered
 * (shell_parental_controls_manager_should_show_app ()), rather than checking
 * whether filtering is enabled at all.
 *
 * Returns: whether app filtering is supported and enabled
 */
gboolean
shell_parental_controls_manager_app_filter_enabled (ShellParentalControlsManager *manager)
{
  g_return_val_if_fail (SHELL_IS_PARENTAL_CONTROLS_MANAGER (manager), FALSE);

#ifdef HAVE_MALCONTENT_0_14
  /* Are latest parental controls enabled (at configure or runtime)? */
  if (manager->disabled)
    return FALSE;

  /* Have we finished initialising yet? */
  if (!manager->initialized)
    {
      g_debug ("Ignoring app filter because parental controls not yet initialised");
      return FALSE;
    }

  return manager->app_filter != NULL &&
         mct_app_filter_is_enabled (manager->app_filter);
#else
  return FALSE;
#endif
}

/**
 * shell_parental_controls_manager_session_limits_enabled:
 * @manager: a #ShellParentalControlsManager
 *
 * Checks whether parental controls session limits are enabled for the
 * account, and if the library supports the features required for the
 * functionality.
 *
 * Returns: whether session limits are supported and enabled
 */
gboolean
shell_parental_controls_manager_session_limits_enabled (ShellParentalControlsManager *manager)
{
  g_return_val_if_fail (SHELL_IS_PARENTAL_CONTROLS_MANAGER (manager), FALSE);

#ifdef HAVE_MALCONTENT_0_14
  /* Are latest parental controls enabled (at configure or runtime)? */
  if (manager->disabled)
    return FALSE;

  /* Have we finished initialising yet? */
  if (!manager->initialized)
    {
      g_debug ("Ignoring session limits because parental controls not yet initialised");
      return FALSE;
    }

  return manager->session_limits != NULL &&
         mct_session_limits_is_enabled (manager->session_limits);
#else
  return FALSE;
#endif
}

/**
 * shell_parental_controls_manager_get_any_parental_controls_enabled:
 * @manager: a #ShellParentalControlsManager
 *
 * Whether any parental controls are supported and enabled for the current
 * user.
 *
 * Returns: %TRUE if app filtering or session limits are enabled
 */
gboolean
shell_parental_controls_manager_get_any_parental_controls_enabled (ShellParentalControlsManager *manager)
{
  g_return_val_if_fail (SHELL_IS_PARENTAL_CONTROLS_MANAGER (manager), FALSE);

  return shell_parental_controls_manager_app_filter_enabled (manager) ||
         shell_parental_controls_manager_session_limits_enabled (manager);
}

static void
shell_parental_controls_manager_get_property (GObject    *object,
                                              guint       prop_id,
                                              GValue     *value,
                                              GParamSpec *pspec)
{
  ShellParentalControlsManager *self = SHELL_PARENTAL_CONTROLS_MANAGER (object);

  switch (prop_id)
    {
    case PROP_INITIALIZED:
      g_value_set_boolean (value, self->initialized);
      break;

    case PROP_ANY_PARENTAL_CONTROLS_ENABLED:
      g_value_set_boolean (value,
                           shell_parental_controls_manager_get_any_parental_controls_enabled (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_parental_controls_manager_dispose (GObject *object)
{
  ShellParentalControlsManager *self = SHELL_PARENTAL_CONTROLS_MANAGER (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

#ifdef HAVE_MALCONTENT
  if (self->manager != NULL)
    g_signal_handlers_disconnect_by_data (self->manager, self);
  g_clear_object (&self->manager);
  g_clear_pointer (&self->app_filter, mct_app_filter_unref);
  g_clear_pointer (&self->session_limits, mct_session_limits_unref);
#endif

  G_OBJECT_CLASS (shell_parental_controls_manager_parent_class)->dispose (object);
}

static void
shell_parental_controls_manager_class_init (ShellParentalControlsManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_parental_controls_manager_get_property;
  object_class->dispose = shell_parental_controls_manager_dispose;

  /**
   * ShellParentalControlsManager:initialized:
   *
   * Whether the parental controls settings were retrieved. Until then no app
   * is shown by shell_parental_controls_manager_should_show_app ().
   */
  props[PROP_INITIALIZED] =
    g_param_spec_boolean ("initialized", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellParentalControlsManager:any-parental-controls-enabled:
   *
   * Whether any parental controls are supported and enabled for the current
   * user.
   */
  props[PROP_ANY_PARENTAL_CONTROLS_ENABLED] =
    g_param_spec_boolean ("any-parental-controls-enabled", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellParentalControlsManager::app-filter-changed:
   * @manager: the #ShellParentalControlsManager
   *
   * Emitted when the app filter of the current user changed, and once when
   * the manager is initialized.
   */
  signals[APP_FILTER_CHANGED] =
    g_signal_new ("app-filter-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellParentalControlsManager::session-limits-changed:
   * @manager: the #ShellParentalControlsManager
   *
   * Emitted when the session limits of the current user changed, and once
   * when the manager is initialized.
   */
  signals[SESSION_LIMITS_CHANGED] =
    g_signal_new ("session-limits-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_parental_controls_manager_init (ShellParentalControlsManager *self)
{
  self->cancellable = g_cancellable_new ();

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  initialize_manager (self);
}
