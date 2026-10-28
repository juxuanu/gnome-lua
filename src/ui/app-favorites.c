/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * app-favorites.c: The applications pinned to the dash, kept in the
 * `favorite-apps` key of the shell settings. Port of js/ui/appFavorites.js.
 *
 * The JS class showed a "Pinned to Dash" / "Unpinned From Dash" notification
 * with an Undo button, which needs js/ui/messageTray.js (M4). Here the
 * #ShellAppFavorites::favorite-added and ::favorite-removed signals say what
 * happened and main.js shows the notification; its Undo button calls
 * shell_app_favorites_remove_favorite_silently () or
 * shell_app_favorites_add_favorite_silently (), the JS `_removeFavorite ()` and
 * `_addFavorite ()`.
 *
 * `getFavoriteMap ()` is gone, shell_app_favorites_get_favorites () and
 * shell_app_favorites_is_favorite () cover what its callers did.
 */

#include "config.h"

#include <gio/gio.h>

#include "shell-app-system.h"
#include "shell-global.h"
#include "ui/app-favorites.h"
#include "misc/parental-controls-manager.h"

#define FAVORITE_APPS_KEY "favorite-apps"

enum
{
  CHANGED,
  FAVORITE_ADDED,
  FAVORITE_REMOVED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellAppFavorites
{
  GObject parent;

  ShellParentalControlsManager *parental_controls;
  ShellAppSystem *app_system;
  GSettings *settings;

  GPtrArray *favorites;         /* of ShellApp, in the order of the setting */
};

G_DEFINE_FINAL_TYPE (ShellAppFavorites, shell_app_favorites, G_TYPE_OBJECT)

static ShellAppFavorites *default_instance = NULL;

/* The ids of the favorites, as the JS `_getIds ()` */
static GStrv
get_ids (ShellAppFavorites *self)
{
  g_auto (GStrv) ids = g_new0 (char *, self->favorites->len + 1);
  guint i;

  for (i = 0; i < self->favorites->len; i++)
    ids[i] = g_strdup (shell_app_get_id (g_ptr_array_index (self->favorites, i)));

  return g_steal_pointer (&ids);
}

static int
find_favorite (ShellAppFavorites *self,
               const char        *app_id)
{
  guint i;

  if (app_id == NULL)
    return -1;

  for (i = 0; i < self->favorites->len; i++)
    {
      ShellApp *app = g_ptr_array_index (self->favorites, i);

      if (g_strcmp0 (shell_app_get_id (app), app_id) == 0)
        return i;
    }

  return -1;
}

/**
 * shell_app_favorites_reload:
 * @self: a #ShellAppFavorites
 *
 * Reads the favorites from the settings again, leaving out the applications
 * which are not installed or which the parental controls hide. Does not emit
 * #ShellAppFavorites::changed.
 */
void
shell_app_favorites_reload (ShellAppFavorites *self)
{
  g_auto (GStrv) ids = NULL;
  guint i;

  g_return_if_fail (SHELL_IS_APP_FAVORITES (self));

  g_ptr_array_set_size (self->favorites, 0);

  ids = g_settings_get_strv (self->settings, FAVORITE_APPS_KEY);
  for (i = 0; ids[i] != NULL; i++)
    {
      ShellApp *app = shell_app_system_lookup_app (self->app_system, ids[i]);
      int pos;

      if (app == NULL ||
          !shell_parental_controls_manager_should_show_app (self->parental_controls,
                                                            G_APP_INFO (shell_app_get_app_info (app))))
        continue;

      /* The JS kept them in an object keyed by id: a second entry for the same
       * application takes the place of the first one without moving it */
      pos = find_favorite (self, shell_app_get_id (app));
      if (pos >= 0)
        {
          g_clear_object (&self->favorites->pdata[pos]);
          self->favorites->pdata[pos] = g_object_ref (app);
        }
      else
        {
          g_ptr_array_add (self->favorites, g_object_ref (app));
        }
    }
}

static void
update_favorites (ShellAppFavorites *self)
{
  g_auto (GStrv) old_ids = get_ids (self);
  g_auto (GStrv) new_ids = NULL;
  gboolean have_changed = FALSE;
  guint i;

  shell_app_favorites_reload (self);
  new_ids = get_ids (self);

  if (g_strv_length (old_ids) != g_strv_length (new_ids))
    {
      have_changed = TRUE;
    }
  else
    {
      for (i = 0; old_ids[i] != NULL; i++)
        {
          if (g_strcmp0 (old_ids[i], new_ids[i]) != 0 ||
              !shell_app_favorites_is_favorite (self, old_ids[i]))
            {
              have_changed = TRUE;
              break;
            }
        }
    }

  if (have_changed)
    g_signal_emit (self, signals[CHANGED], 0);
}

static void
app_filter_changed_cb (ShellParentalControlsManager *manager,
                       ShellAppFavorites            *self)
{
  update_favorites (self);
}

static void
installed_changed_cb (ShellAppSystem    *app_system,
                      ShellAppFavorites *self)
{
  update_favorites (self);
}

static void
favorite_apps_changed_cb (GSettings         *settings,
                          const char        *key,
                          ShellAppFavorites *self)
{
  update_favorites (self);
}

static void
disconnect_sources (ShellAppFavorites *self)
{
  if (self->parental_controls != NULL)
    g_signal_handlers_disconnect_by_data (self->parental_controls, self);
  if (self->app_system != NULL)
    g_signal_handlers_disconnect_by_data (self->app_system, self);
  if (self->settings != NULL)
    g_signal_handlers_disconnect_by_data (self->settings, self);
}

static void
on_shutdown (ShellGlobal       *global,
             ShellAppFavorites *self)
{
  /* Nothing may reach the callers once the shell is going away. */
  disconnect_sources (self);
}

static void
shell_app_favorites_constructed (GObject *object)
{
  ShellAppFavorites *self = SHELL_APP_FAVORITES (object);
  ShellGlobal *global = shell_global_get ();

  G_OBJECT_CLASS (shell_app_favorites_parent_class)->constructed (object);

  /* Filter the apps through the user’s parental controls. */
  self->parental_controls = g_object_ref (shell_parental_controls_manager_get_default ());
  g_signal_connect (self->parental_controls, "app-filter-changed",
                    G_CALLBACK (app_filter_changed_cb), self);

  self->app_system = g_object_ref (shell_app_system_get_default ());
  g_signal_connect (self->app_system, "installed-changed",
                    G_CALLBACK (installed_changed_cb), self);

  self->settings = g_object_ref (shell_global_get_settings (global));
  g_signal_connect (self->settings, "changed::" FAVORITE_APPS_KEY,
                    G_CALLBACK (favorite_apps_changed_cb), self);

  g_signal_connect_object (global, "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  shell_app_favorites_reload (self);
}

static void
shell_app_favorites_dispose (GObject *object)
{
  ShellAppFavorites *self = SHELL_APP_FAVORITES (object);

  disconnect_sources (self);
  g_clear_object (&self->parental_controls);
  g_clear_object (&self->app_system);
  g_clear_object (&self->settings);
  g_clear_pointer (&self->favorites, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_app_favorites_parent_class)->dispose (object);
}

static void
shell_app_favorites_class_init (ShellAppFavoritesClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_app_favorites_constructed;
  object_class->dispose = shell_app_favorites_dispose;

  /**
   * ShellAppFavorites::changed:
   * @self: the #ShellAppFavorites
   *
   * Emitted when the list of favorites changed, whoever changed it: the
   * settings, an application being (un)installed or the parental controls.
   */
  signals[CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellAppFavorites::favorite-added:
   * @self: the #ShellAppFavorites
   * @app: the application which was pinned
   *
   * Emitted after shell_app_favorites_add_favorite () or
   * shell_app_favorites_add_favorite_at_pos () pinned an application, but not
   * after it was moved or pinned silently. main.js shows a notification with an
   * Undo button for it.
   */
  signals[FAVORITE_ADDED] =
    g_signal_new ("favorite-added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, SHELL_TYPE_APP);

  /**
   * ShellAppFavorites::favorite-removed:
   * @self: the #ShellAppFavorites
   * @app: the application which was unpinned
   * @pos: the position it had among the favorites
   *
   * Emitted after shell_app_favorites_remove_favorite () unpinned an
   * application, but not after it was moved or unpinned silently. main.js
   * shows a notification with an Undo button for it, which pins @app at @pos
   * again.
   */
  signals[FAVORITE_REMOVED] =
    g_signal_new ("favorite-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, SHELL_TYPE_APP, G_TYPE_INT);
}

static void
shell_app_favorites_init (ShellAppFavorites *self)
{
  self->favorites = g_ptr_array_new_with_free_func (g_object_unref);
}

/**
 * shell_app_favorites_get_default:
 *
 * Gets the #ShellAppFavorites singleton (`getAppFavorites ()`). It is created
 * on first use and reads the favorites from the shell settings.
 *
 * Returns: (transfer none): the #ShellAppFavorites
 */
ShellAppFavorites *
shell_app_favorites_get_default (void)
{
  if (default_instance == NULL)
    default_instance = g_object_new (SHELL_TYPE_APP_FAVORITES, NULL);

  return default_instance;
}

/**
 * shell_app_favorites_get_favorites:
 * @self: a #ShellAppFavorites
 *
 * Returns: (transfer container) (element-type ShellApp): the favorites, in
 *   the order they appear in the dash
 */
GList *
shell_app_favorites_get_favorites (ShellAppFavorites *self)
{
  GList *list = NULL;
  guint i;

  g_return_val_if_fail (SHELL_IS_APP_FAVORITES (self), NULL);

  for (i = self->favorites->len; i > 0; i--)
    list = g_list_prepend (list, g_ptr_array_index (self->favorites, i - 1));

  return list;
}

/**
 * shell_app_favorites_is_favorite:
 * @self: a #ShellAppFavorites
 * @app_id: a desktop file ID
 *
 * Returns: whether the application is one of the favorites
 */
gboolean
shell_app_favorites_is_favorite (ShellAppFavorites *self,
                                 const char        *app_id)
{
  g_return_val_if_fail (SHELL_IS_APP_FAVORITES (self), FALSE);

  return find_favorite (self, app_id) >= 0;
}

/**
 * shell_app_favorites_add_favorite_silently:
 * @self: a #ShellAppFavorites
 * @app_id: a desktop file ID
 * @pos: the position to put the application at, or -1 for the end
 *
 * Pins an application without emitting #ShellAppFavorites::favorite-added
 * (`_addFavorite ()`). A @pos after the end puts it at the end.
 *
 * Returns: %FALSE if the application is a favorite already, is not installed
 *   or is hidden by the parental controls
 */
gboolean
shell_app_favorites_add_favorite_silently (ShellAppFavorites *self,
                                           const char        *app_id,
                                           int                pos)
{
  g_auto (GStrv) ids = NULL;
  g_autoptr (GStrvBuilder) builder = NULL;
  g_auto (GStrv) new_ids = NULL;
  ShellApp *app;
  guint i, n;

  g_return_val_if_fail (SHELL_IS_APP_FAVORITES (self), FALSE);
  g_return_val_if_fail (app_id != NULL, FALSE);

  if (find_favorite (self, app_id) >= 0)
    return FALSE;

  app = shell_app_system_lookup_app (self->app_system, app_id);
  if (app == NULL)
    return FALSE;

  if (!shell_parental_controls_manager_should_show_app (self->parental_controls,
                                                        G_APP_INFO (shell_app_get_app_info (app))))
    return FALSE;

  ids = get_ids (self);
  n = g_strv_length (ids);
  if (pos < 0 || (guint) pos > n)
    pos = n;

  builder = g_strv_builder_new ();
  for (i = 0; i < n; i++)
    {
      if (i == (guint) pos)
        g_strv_builder_add (builder, app_id);
      g_strv_builder_add (builder, ids[i]);
    }
  if ((guint) pos == n)
    g_strv_builder_add (builder, app_id);

  new_ids = g_strv_builder_end (builder);
  g_settings_set_strv (self->settings, FAVORITE_APPS_KEY,
                       (const char * const *) new_ids);

  return TRUE;
}

/**
 * shell_app_favorites_remove_favorite_silently:
 * @self: a #ShellAppFavorites
 * @app_id: a desktop file ID
 *
 * Unpins an application without emitting #ShellAppFavorites::favorite-removed
 * (`_removeFavorite ()`).
 *
 * Returns: %FALSE if the application is not a favorite
 */
gboolean
shell_app_favorites_remove_favorite_silently (ShellAppFavorites *self,
                                              const char        *app_id)
{
  g_autoptr (GStrvBuilder) builder = NULL;
  g_auto (GStrv) new_ids = NULL;
  guint i;

  g_return_val_if_fail (SHELL_IS_APP_FAVORITES (self), FALSE);
  g_return_val_if_fail (app_id != NULL, FALSE);

  if (find_favorite (self, app_id) < 0)
    return FALSE;

  builder = g_strv_builder_new ();
  for (i = 0; i < self->favorites->len; i++)
    {
      const char *id = shell_app_get_id (g_ptr_array_index (self->favorites, i));

      if (g_strcmp0 (id, app_id) != 0)
        g_strv_builder_add (builder, id);
    }

  new_ids = g_strv_builder_end (builder);
  g_settings_set_strv (self->settings, FAVORITE_APPS_KEY,
                       (const char * const *) new_ids);

  return TRUE;
}

/**
 * shell_app_favorites_add_favorite_at_pos:
 * @self: a #ShellAppFavorites
 * @app_id: a desktop file ID
 * @pos: the position to put the application at, or -1 for the end
 *
 * Pins an application and emits #ShellAppFavorites::favorite-added
 * (`addFavoriteAtPos ()`). Does nothing if it is a favorite already, is not
 * installed or is hidden by the parental controls.
 */
void
shell_app_favorites_add_favorite_at_pos (ShellAppFavorites *self,
                                         const char        *app_id,
                                         int                pos)
{
  ShellApp *app;

  g_return_if_fail (SHELL_IS_APP_FAVORITES (self));
  g_return_if_fail (app_id != NULL);

  if (!shell_app_favorites_add_favorite_silently (self, app_id, pos))
    return;

  app = shell_app_system_lookup_app (self->app_system, app_id);
  if (app != NULL)
    g_signal_emit (self, signals[FAVORITE_ADDED], 0, app);
}

/**
 * shell_app_favorites_add_favorite:
 * @self: a #ShellAppFavorites
 * @app_id: a desktop file ID
 *
 * Pins an application at the end of the favorites
 * (shell_app_favorites_add_favorite_at_pos () with a position of -1).
 */
void
shell_app_favorites_add_favorite (ShellAppFavorites *self,
                                  const char        *app_id)
{
  shell_app_favorites_add_favorite_at_pos (self, app_id, -1);
}

/**
 * shell_app_favorites_move_favorite_to_pos:
 * @self: a #ShellAppFavorites
 * @app_id: a desktop file ID
 * @pos: the new position of the application
 *
 * Moves an application, which may not be a favorite yet, to a position
 * without emitting #ShellAppFavorites::favorite-added or ::favorite-removed.
 */
void
shell_app_favorites_move_favorite_to_pos (ShellAppFavorites *self,
                                          const char        *app_id,
                                          int                pos)
{
  g_return_if_fail (SHELL_IS_APP_FAVORITES (self));
  g_return_if_fail (app_id != NULL);

  shell_app_favorites_remove_favorite_silently (self, app_id);
  shell_app_favorites_add_favorite_silently (self, app_id, pos);
}

/**
 * shell_app_favorites_remove_favorite:
 * @self: a #ShellAppFavorites
 * @app_id: a desktop file ID
 *
 * Unpins an application and emits #ShellAppFavorites::favorite-removed
 * (`removeFavorite ()`). Does nothing if it is not a favorite.
 */
void
shell_app_favorites_remove_favorite (ShellAppFavorites *self,
                                     const char        *app_id)
{
  g_autoptr (ShellApp) app = NULL;
  int pos;

  g_return_if_fail (SHELL_IS_APP_FAVORITES (self));
  g_return_if_fail (app_id != NULL);

  pos = find_favorite (self, app_id);
  if (pos < 0)
    return;

  app = g_object_ref (g_ptr_array_index (self->favorites, pos));
  if (!shell_app_favorites_remove_favorite_silently (self, app_id))
    return;

  g_signal_emit (self, signals[FAVORITE_REMOVED], 0, app, pos);
}
