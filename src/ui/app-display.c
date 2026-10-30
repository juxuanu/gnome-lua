/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * app-display.c: The application grid of the overview, its folders and the
 * search provider of the installed applications. Port of js/ui/appDisplay.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The overview, `Main.activateWindow ()` and the other singletons that are
 *    still JS are reached through a #ShellWindowManager, which every item,
 *    view and the provider get as a construct property (`window-manager`, may
 *    be %NULL, then nothing happens where the JS reached for the overview):
 *    `Main.overview.hide ()` is its `overview-hide-requested` signal,
 *    `Main.overview.visible` and friends are its `overview-*` properties, and
 *    `Main.overview.beginItemDrag ()`, `cancelledItemDrag ()` and
 *    `endItemDrag ()` are its `item-drag-begin`, `item-drag-cancelled` and
 *    `item-drag-end` signals, which the views and the dash listen to
 *    themselves (they used to connect to the overview while they were
 *    mapped, and still do). The overview (overview.c) hands the signals on.
 *  - `Main.overview.dash.iconSize`, the size of the drag actor of an
 *    application, is the `drag-icon-size` property of the item. The views
 *    and the search provider have the property too, and overview-controls.c
 *    binds it to the `icon-size` of the dash.
 *  - `Main.initializeDeferredWork ()` is shell_deferred_work_new ()
 *    (deferred-work.h).
 *  - `Config.SYSTEM_FOLDER_APPS` and `UTILITIES_FOLDER_APPS` are macros of
 *    config.h.
 *  - `AppViewItem.isDraggable`, `expandTitleOnHover`, `AppIcon.app`,
 *    `popupMenuSide` and `showLabel` are construct properties.
 *  - The drop target of the grid and of the scroll container of a view is a
 *    private object (the JS `BaseAppViewDragTarget`) so that dropping on the
 *    page indicator dots is not accepted.
 *  - A folder that has become empty is not destroyed by `_loadApps ()` but
 *    dropped by `_redisplay ()` with the other removed items.
 *  - `AppDisplay._onDestroy ()` removed the scroll timeout only for the
 *    application grid; every view does it here.
 *  - `AppDisplay.getAppInfos ()` is shell_app_display_get_app_infos ().
 *    The folder dialog of `AppFolderDialog.popdown (callback)` is
 *    shell_app_folder_dialog_popdown () with a callback and its data.
 *  - Errors that the JS logged with `log ()` are g_debug () messages
 *    (selecting an app that does not exist) or g_warning () messages.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <atk/atk.h>
#include <clutter/clutter-pango.h>
#include <glib/gi18n-lib.h>
#include <math.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-later.h>
#include <meta/meta-workspace-manager.h>
#include <meta/window.h>
#include <meta/workspace.h>

#include "misc/parental-controls-manager.h"
#include "misc/system-actions.h"
#include "shell-app-system.h"
#include "shell-app-usage.h"
/* The generated St enum types (ST_TYPE_SIDE) are not part of st.h */
#define ST_H_INSIDE 1
#include "st-enum-types.h"
#undef ST_H_INSIDE
#include "shell-global.h"
#include "shell-stack.h"
#include "shell-util.h"
#include "ui/app-display.h"
#include "ui/app-favorites.h"
#include "ui/app-menu.h"
#include "ui/deferred-work.h"
#include "ui/environment.h"
#include "ui/grab-helper.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/page-indicators.h"
#include "ui/panel.h"
#include "ui/popup-menu.h"
#include "ui/search.h"
#include "ui/swipe-tracker.h"

#define POPDOWN_DIALOG_TIMEOUT 500

#define FOLDER_SUBICON_FRACTION .4

#define SCROLL_TIMEOUT_TIME 150

#define APP_ICON_SCALE_IN_TIME 500
#define APP_ICON_SCALE_IN_DELAY 700

#define APP_ICON_TITLE_EXPAND_TIME 200
#define APP_ICON_TITLE_COLLAPSE_TIME 100

#define FOLDER_DIALOG_ANIMATION_TIME 200

#define PAGE_PREVIEW_ANIMATION_TIME 150
#define PAGE_INDICATOR_FADE_TIME 200
#define PAGE_PREVIEW_RATIO 0.20

#define DRAG_PAGE_SWITCH_INITIAL_TIMEOUT 1000
#define DRAG_PAGE_SWITCH_IMMEDIATELY_THRESHOLD_PX 20

#define DRAG_PAGE_SWITCH_REPEAT_TIMEOUT 1000

#define DELAYED_MOVE_TIMEOUT 200

/* The scale of the position of a folder preview and the icons of a dropped
 * application, and the shade of the dialog (alpha 204 and 85 of 255) */
#define DIALOG_SHADE_NORMAL_ALPHA 204
#define DIALOG_SHADE_HIGHLIGHT_ALPHA 85

typedef struct
{
  const char *id;
  const char *name;
  const char * const *apps;
  const char * const *categories;
} DefaultFolder;

static const char * const system_folder_apps[] = { SYSTEM_FOLDER_APPS, NULL };
static const char * const utilities_folder_apps[] = { UTILITIES_FOLDER_APPS, NULL };
static const char * const yast_categories[] = { "X-SuSE-YaST", NULL };
static const char * const pardus_categories[] = { "X-Pardus-Apps", NULL };

static const DefaultFolder default_folders[] = {
  { "System", "X-GNOME-Shell-System.directory", system_folder_apps, NULL },
  { "Utilities", "X-GNOME-Shell-Utilities.directory", utilities_folder_apps, NULL },
  { "YaST", "suse-yast.directory", NULL, yast_categories },
  { "Pardus", "X-Pardus-Apps.directory", NULL, pardus_categories },
};

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static GSettings *
get_shell_settings (void)
{
  return shell_global_get_settings (shell_global_get ());
}

/* Info.get_categories () split at the semicolons, empty when it has none */
static char **
get_categories (GAppInfo *info)
{
  const char *categories = NULL;

  if (G_IS_DESKTOP_APP_INFO (info))
    categories = g_desktop_app_info_get_categories (G_DESKTOP_APP_INFO (info));

  if (categories == NULL || categories[0] == '\0')
    return g_new0 (char *, 1);

  return g_strsplit (categories, ";", -1);
}

static gboolean
lists_intersect (const char * const *a,
                 const char * const *b)
{
  for (; a != NULL && *a != NULL; a++)
    {
      if (b != NULL && g_strv_contains (b, *a))
        return TRUE;
    }

  return FALSE;
}

static char *
get_folder_name (GSettings *folder)
{
  g_autofree char *name = g_settings_get_string (folder, "name");

  if (g_settings_get_boolean (folder, "translate"))
    {
      char *translated = shell_util_get_translated_folder_name (name);

      if (translated != NULL)
        return translated;
    }

  return g_steal_pointer (&name);
}

static ShellBaseAppView *
get_view_from_icon (ClutterActor *icon)
{
  ClutterActor *parent;

  for (parent = clutter_actor_get_parent (icon);
       parent != NULL;
       parent = clutter_actor_get_parent (parent))
    {
      if (SHELL_IS_BASE_APP_VIEW (parent))
        return SHELL_BASE_APP_VIEW (parent);
    }

  return NULL;
}

/* @apps: a list of #ShellApp */
static char *
find_best_folder_name (GList *apps)
{
  g_autoptr (GHashTable) counter = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (GPtrArray) common = g_ptr_array_new_with_free_func (g_free);
  guint n_apps = g_list_length (apps);
  GList *l;
  guint i;

  for (l = apps; l != NULL; l = l->next)
    {
      GAppInfo *info = G_APP_INFO (shell_app_get_app_info (l->data));
      g_auto (GStrv) categories = get_categories (info);
      char **category;

      for (category = categories; *category != NULL; category++)
        {
          guint count = GPOINTER_TO_UINT (g_hash_table_lookup (counter, *category)) + 1;

          g_hash_table_insert (counter, g_strdup (*category), GUINT_TO_POINTER (count));

          /* If a category is present in all apps, its counter will
           * reach the number of apps */
          if ((*category)[0] != '\0' && count == n_apps)
            g_ptr_array_add (common, g_strdup (*category));
        }
    }

  for (i = 0; i < common->len; i++)
    {
      g_autofree char *directory = g_strdup_printf ("%s.directory",
                                                    (char *) g_ptr_array_index (common, i));
      char *translated = shell_util_get_translated_folder_name (directory);

      if (translated != NULL)
        return translated;
    }

  return NULL;
}

/* App.get_app_info () can be NULL for window backed apps */
static GAppInfo *
app_info_of (ShellApp *app)
{
  return G_APP_INFO (shell_app_get_app_info (app));
}

typedef struct
{
  ClutterActor *actor;
  gboolean hide;
} HideAfterEase;

static void
hide_after_ease_cb (gboolean finished,
                    gpointer user_data)
{
  HideAfterEase *data = user_data;

  if (finished && data->hide)
    clutter_actor_hide (data->actor);
}

static void
hide_after_ease_free (gpointer user_data)
{
  HideAfterEase *data = user_data;

  g_object_unref (data->actor);
  g_free (data);
}

/* actor.ease ({opacity, duration, onComplete: () => { if (hide) actor.hide () }}) */
static void
ease_opacity (ClutterActor *actor,
              int           opacity,
              int           duration,
              gboolean      hide_when_done)
{
  ShellEaseParams params = {
    .duration = duration,
    .delay = -1,
    .mode = 0,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };
  HideAfterEase *data = g_new0 (HideAfterEase, 1);

  data->actor = g_object_ref (actor);
  data->hide = hide_when_done;

  shell_actor_ease (actor, &params,
                    hide_after_ease_cb, data, hide_after_ease_free,
                    "opacity", opacity,
                    NULL);
}

/* adjustment.ease (target, {mode, duration, onComplete}) */
static void
ease_adjustment (StAdjustment         *adjustment,
                 double                target,
                 int                   duration,
                 ClutterAnimationMode  mode,
                 ShellEaseCallback     callback,
                 gpointer              user_data,
                 GDestroyNotify        destroy)
{
  ShellEaseParams params = {
    .duration = duration,
    .delay = -1,
    .mode = mode,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };
  g_auto (GValue) value = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;

  g_value_init (&value, G_TYPE_DOUBLE);
  g_value_set_double (&value, target);

  /* On failure the callback is dropped and the data freed */
  shell_animatable_ease_property (CLUTTER_ANIMATABLE (adjustment), "value", &value,
                                  &params, callback, user_data, destroy, &error);
}

static ShellEaseParams
default_ease_params (void)
{
  ShellEaseParams params = {
    .duration = -1,
    .delay = -1,
    .mode = 0,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  return params;
}

static ClutterTextDirection
text_direction_of (ClutterActor *actor)
{
  return clutter_actor_get_text_direction (actor);
}

static ShellLayoutManager *
get_layout_manager (void)
{
  return shell_main_get_layout_manager (shell_main_get_default ());
}

/* Main.layoutManager.primary_index */
static int
get_primary_index (void)
{
  ShellLayoutManager *layout_manager = get_layout_manager ();

  return layout_manager != NULL ? shell_layout_manager_get_primary_index (layout_manager) : 0;
}

/* Main.layoutManager.n_monitors */
static int
get_n_monitors (void)
{
  ShellLayoutManager *layout_manager = get_layout_manager ();

  return layout_manager != NULL ? shell_layout_manager_get_n_monitors (layout_manager) : 1;
}

/* Main.overview.hide () */
static void
request_overview_hide (ShellWindowManager *window_manager)
{
  if (window_manager != NULL)
    g_signal_emit_by_name (window_manager, "overview-hide-requested");
}

/* global.settings.is_writable ('favorite-apps' / 'app-picker-layout') */
static gboolean
is_settings_writable (const char *key)
{
  return g_settings_is_writable (get_shell_settings (), key);
}

/* ------------------------------------------------------------------------ */
/* ShellAppGrid (private, AppGrid and FolderGrid)                            */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_APP_GRID (shell_app_grid_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellAppGrid, shell_app_grid,
                          SHELL, APP_GRID, ShellIconGrid)

struct _ShellAppGridClass
{
  ShellIconGridClass parent_class;
};

typedef struct
{
  ClutterMargin indicators_padding;
} ShellAppGridPrivate;

enum
{
  GRID_PROP_0,
  GRID_PROP_INDICATORS_PADDING,
  GRID_N_PROPS
};

static GParamSpec *grid_props[GRID_N_PROPS] = { NULL, };

G_DEFINE_TYPE_WITH_PRIVATE (ShellAppGrid, shell_app_grid, SHELL_TYPE_ICON_GRID)

static void
app_grid_update_padding (ShellAppGrid *self)
{
  ShellAppGridPrivate *priv = shell_app_grid_get_instance_private (self);
  StThemeNode *node = st_widget_get_theme_node (ST_WIDGET (self));
  ClutterMargin padding = priv->indicators_padding;

  padding.top += st_theme_node_get_length (node, "page-padding-top");
  padding.right += st_theme_node_get_length (node, "page-padding-right");
  padding.bottom += st_theme_node_get_length (node, "page-padding-bottom");
  padding.left += st_theme_node_get_length (node, "page-padding-left");

  g_object_set (self, "page-padding", &padding, NULL);
}

static void
shell_app_grid_style_changed (StWidget *widget)
{
  ST_WIDGET_CLASS (shell_app_grid_parent_class)->style_changed (widget);
  app_grid_update_padding (SHELL_APP_GRID (widget));
}

static const ClutterMargin *
app_grid_get_indicators_padding (ShellAppGrid *self)
{
  ShellAppGridPrivate *priv = shell_app_grid_get_instance_private (self);

  return &priv->indicators_padding;
}

static void
app_grid_set_indicators_padding (ShellAppGrid        *self,
                                 const ClutterMargin *padding)
{
  ShellAppGridPrivate *priv = shell_app_grid_get_instance_private (self);
  ClutterMargin value = { 0, };

  if (padding != NULL)
    value = *padding;

  if (priv->indicators_padding.left == value.left &&
      priv->indicators_padding.right == value.right &&
      priv->indicators_padding.top == value.top &&
      priv->indicators_padding.bottom == value.bottom)
    return;

  priv->indicators_padding = value;
  g_object_notify_by_pspec (G_OBJECT (self), grid_props[GRID_PROP_INDICATORS_PADDING]);
  app_grid_update_padding (self);
}

static void
shell_app_grid_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  ShellAppGrid *self = SHELL_APP_GRID (object);

  switch (prop_id)
    {
    case GRID_PROP_INDICATORS_PADDING:
      app_grid_set_indicators_padding (self, g_value_get_boxed (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_app_grid_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  ShellAppGrid *self = SHELL_APP_GRID (object);

  switch (prop_id)
    {
    case GRID_PROP_INDICATORS_PADDING:
      g_value_set_boxed (value, app_grid_get_indicators_padding (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_app_grid_constructed (GObject *object)
{
  G_OBJECT_CLASS (shell_app_grid_parent_class)->constructed (object);

  clutter_actor_set_accessible_name (CLUTTER_ACTOR (object), _("App grid"));
  clutter_actor_set_accessible_role (CLUTTER_ACTOR (object), ATK_ROLE_GROUPING);
}

static void
shell_app_grid_class_init (ShellAppGridClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);

  object_class->constructed = shell_app_grid_constructed;
  object_class->set_property = shell_app_grid_set_property;
  object_class->get_property = shell_app_grid_get_property;

  widget_class->style_changed = shell_app_grid_style_changed;

  grid_props[GRID_PROP_INDICATORS_PADDING] =
    g_param_spec_boxed ("indicators-padding", NULL, NULL,
                        CLUTTER_TYPE_MARGIN,
                        G_PARAM_READWRITE |
                        G_PARAM_EXPLICIT_NOTIFY |
                        G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, GRID_N_PROPS, grid_props);
}

static void
shell_app_grid_init (ShellAppGrid *self)
{
}

#define SHELL_TYPE_FOLDER_GRID (shell_folder_grid_get_type ())
G_DECLARE_FINAL_TYPE (ShellFolderGrid, shell_folder_grid,
                      SHELL, FOLDER_GRID, ShellAppGrid)

struct _ShellFolderGrid
{
  ShellAppGrid parent;
};

G_DEFINE_FINAL_TYPE (ShellFolderGrid, shell_folder_grid, SHELL_TYPE_APP_GRID)

static void
shell_folder_grid_constructed (GObject *object)
{
  g_autoptr (GVariant) modes = NULL;

  G_OBJECT_CLASS (shell_folder_grid_parent_class)->constructed (object);

  /* (rows, columns) */
  modes = g_variant_ref_sink (g_variant_new_parsed ("[(3, 3)]"));
  shell_icon_grid_set_grid_modes (SHELL_ICON_GRID (object), modes);
}

static void
shell_folder_grid_class_init (ShellFolderGridClass *klass)
{
  G_OBJECT_CLASS (klass)->constructed = shell_folder_grid_constructed;
}

static void
shell_folder_grid_init (ShellFolderGrid *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellAppViewGridLayout (private, BaseAppViewGridLayout)                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_APP_VIEW_GRID_LAYOUT (shell_app_view_grid_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellAppViewGridLayout, shell_app_view_grid_layout,
                      SHELL, APP_VIEW_GRID_LAYOUT, ClutterBinLayout)

struct _ShellAppViewGridLayout
{
  ClutterBinLayout parent;

  ShellAppGrid *grid;
  ClutterActor *scroll_view;
  ClutterActor *previous_page_indicator;
  ClutterActor *previous_page_arrow;
  ClutterActor *next_page_indicator;
  ClutterActor *next_page_arrow;

  StAdjustment *page_indicators_adjustment;
  ClutterActor *container;

  gboolean show_indicators;
  int current_page;
  float page_width;
};

G_DEFINE_FINAL_TYPE (ShellAppViewGridLayout, shell_app_view_grid_layout,
                     CLUTTER_TYPE_BIN_LAYOUT)

static float
layout_get_indicators_width (ShellAppViewGridLayout *self,
                             const ClutterActorBox  *box)
{
  float width = clutter_actor_box_get_width (box);
  float height = clutter_actor_box_get_height (box);
  ClutterActor *arrows[] = { self->next_page_arrow, self->previous_page_arrow };
  float min_arrows_width = 0;
  float ideal_indicator_width = (width * PAGE_PREVIEW_RATIO) / 2;
  guint i;

  for (i = 0; i < G_N_ELEMENTS (arrows); i++)
    {
      float min;

      clutter_actor_get_preferred_width (arrows[i], height, &min, NULL);
      min_arrows_width = MAX (min_arrows_width, min);
    }

  return MAX (ideal_indicator_width, min_arrows_width);
}

static void
layout_sync_page_indicators_visibility (ShellAppViewGridLayout *self,
                                        gboolean                animate)
{
  gboolean previous_indicators_visible = self->current_page > 0 && self->show_indicators;
  gboolean previous_arrow_visible;
  gboolean allow_incomplete_pages;
  gboolean next_indicators_visible;
  gboolean next_arrow_visible;
  int n_pages;
  int duration = animate ? PAGE_INDICATOR_FADE_TIME : 0;

  if (previous_indicators_visible)
    clutter_actor_show (self->previous_page_indicator);

  ease_opacity (self->previous_page_indicator,
                previous_indicators_visible ? 255 : 0, duration,
                !previous_indicators_visible);

  previous_arrow_visible = self->current_page > 0 && !previous_indicators_visible;

  if (previous_arrow_visible)
    clutter_actor_show (self->previous_page_arrow);

  ease_opacity (self->previous_page_arrow,
                previous_arrow_visible ? 255 : 0, duration,
                !previous_arrow_visible);

  /* Always show the next page indicator to allow dropping
   * icons into new pages */
  g_object_get (self->grid,
                "allow-incomplete-pages", &allow_incomplete_pages,
                "n-pages", &n_pages,
                NULL);
  next_indicators_visible = self->show_indicators &&
    (allow_incomplete_pages ? TRUE : self->current_page < n_pages - 1);

  if (next_indicators_visible)
    clutter_actor_show (self->next_page_indicator);

  ease_opacity (self->next_page_indicator,
                next_indicators_visible ? 255 : 0, duration,
                !next_indicators_visible);

  next_arrow_visible = self->current_page < n_pages - 1 && !next_indicators_visible;

  if (next_arrow_visible)
    clutter_actor_show (self->next_page_arrow);

  ease_opacity (self->next_page_arrow,
                next_arrow_visible ? 255 : 0, duration,
                !next_arrow_visible);
}

static ClutterActor *
layout_get_end_icon (ShellAppViewGridLayout *self,
                     GList                  *icons)
{
  int columns_per_page;
  int n = (int) g_list_length (icons);
  int index;

  g_object_get (self->grid, "columns-per-page", &columns_per_page, NULL);
  index = MIN (n, columns_per_page);

  return g_list_nth_data (icons, MAX (index - 1, 0));
}

/* grid.get_items_at_page (page).filter (i => i.visible) */
static GList *
get_visible_items_at_page (ShellIconGrid *grid,
                           int            page)
{
  g_autoptr (GError) error = NULL;
  GList *items = shell_icon_grid_get_items_at_page (grid, page, &error);
  GList *l = items;

  if (error != NULL)
    return NULL;

  while (l != NULL)
    {
      GList *next = l->next;

      if (!clutter_actor_is_visible (l->data))
        items = g_list_delete_link (items, l);

      l = next;
    }

  return items;
}

static void
layout_translate_previous_page_icons (ShellAppViewGridLayout *self,
                                      double                  value,
                                      gboolean                ltr)
{
  const ClutterMargin *padding;
  GList *icons, *l;
  int previous_page;
  int column_spacing;
  ClutterActor *end_icon;
  ClutterActorBox box;
  double icon_offset;
  int n_pages;

  if (self->current_page == 0)
    return;

  previous_page = self->current_page - 1;
  icons = get_visible_items_at_page (SHELL_ICON_GRID (self->grid), previous_page);
  if (icons == NULL)
    return;

  padding = app_grid_get_indicators_padding (self->grid);
  g_object_get (self->grid, "column-spacing", &column_spacing, "n-pages", &n_pages, NULL);
  end_icon = layout_get_end_icon (self, icons);
  clutter_actor_get_allocation_box (end_icon, &box);

  if (ltr)
    {
      double current_page_offset = self->page_width * self->current_page;

      icon_offset = current_page_offset - box.x2 + padding->left - column_spacing;
    }
  else
    {
      int rtl_page = n_pages - previous_page - 1;
      double page_offset = self->page_width * rtl_page;

      icon_offset = page_offset - box.x1 - padding->right + column_spacing;
    }

  for (l = icons; l != NULL; l = l->next)
    clutter_actor_set_translation (l->data, icon_offset * value, 0, 0);

  g_list_free (icons);
}

static void
layout_translate_next_page_icons (ShellAppViewGridLayout *self,
                                  double                  value,
                                  gboolean                ltr)
{
  const ClutterMargin *padding;
  GList *icons, *l;
  int next_page;
  int column_spacing;
  int n_pages;
  ClutterActorBox box;
  double icon_offset;

  g_object_get (self->grid, "n-pages", &n_pages, NULL);
  if (self->current_page >= n_pages - 1)
    return;

  next_page = self->current_page + 1;
  icons = get_visible_items_at_page (SHELL_ICON_GRID (self->grid), next_page);
  if (icons == NULL)
    return;

  padding = app_grid_get_indicators_padding (self->grid);
  g_object_get (self->grid, "column-spacing", &column_spacing, NULL);
  clutter_actor_get_allocation_box (icons->data, &box);

  if (ltr)
    {
      double page_offset = self->page_width * next_page;

      icon_offset = page_offset - box.x1 - padding->right + column_spacing;
    }
  else
    {
      int rtl_page = n_pages - self->current_page - 1;
      double current_page_offset = self->page_width * rtl_page;

      icon_offset = current_page_offset - box.x2 + padding->left - column_spacing;
    }

  for (l = icons; l != NULL; l = l->next)
    clutter_actor_set_translation (l->data, icon_offset * value, 0, 0);

  g_list_free (icons);
}

static void
layout_sync_page_indicators (ShellAppViewGridLayout *self)
{
  const ClutterMargin *padding;
  double value;
  gboolean ltr;
  float left, right;
  float left_indicator_offset, right_indicator_offset;
  float left_arrow_offset, right_arrow_offset;
  int n_pages;

  if (self->container == NULL)
    return;

  value = st_adjustment_get_value (self->page_indicators_adjustment);

  ltr = clutter_actor_get_text_direction (self->container) != CLUTTER_TEXT_DIRECTION_RTL;
  padding = app_grid_get_indicators_padding (self->grid);
  left = padding->left;
  right = padding->right;
  left_indicator_offset = -left * (1 - value);
  right_indicator_offset = right * (1 - value);

  clutter_actor_set_translation (self->previous_page_indicator,
                                 ltr ? left_indicator_offset : right_indicator_offset, 0, 0);
  clutter_actor_set_translation (self->next_page_indicator,
                                 ltr ? right_indicator_offset : left_indicator_offset, 0, 0);

  left_arrow_offset = -left * value;
  right_arrow_offset = right * value;

  clutter_actor_set_translation (self->previous_page_arrow,
                                 ltr ? left_arrow_offset : right_arrow_offset, 0, 0);
  clutter_actor_set_translation (self->next_page_arrow,
                                 ltr ? right_arrow_offset : left_arrow_offset, 0, 0);

  /* Page icons */
  layout_translate_previous_page_icons (self, value, ltr);
  layout_translate_next_page_icons (self, value, ltr);

  g_object_get (self->grid, "n-pages", &n_pages, NULL);
  if (n_pages > 0)
    {
      /* The JS reset every icon of the page, visible or not */
      GList *icons = shell_icon_grid_get_items_at_page (SHELL_ICON_GRID (self->grid),
                                                        self->current_page, NULL);
      GList *l;

      for (l = icons; l != NULL; l = l->next)
        clutter_actor_set_translation (l->data, 0, 0, 0);

      g_list_free (icons);
    }
}

static void
layout_pages_changed_cb (ShellIconGrid          *grid,
                         ShellAppViewGridLayout *self)
{
  layout_sync_page_indicators_visibility (self, TRUE);
}

static void
layout_adjustment_value_changed_cb (StAdjustment           *adjustment,
                                    GParamSpec             *pspec,
                                    ShellAppViewGridLayout *self)
{
  layout_sync_page_indicators (self);
}

static void
shell_app_view_grid_layout_set_container (ClutterLayoutManager *manager,
                                          ClutterActor         *container)
{
  ShellAppViewGridLayout *self = SHELL_APP_VIEW_GRID_LAYOUT (manager);
  ClutterLayoutManagerClass *parent_class =
    CLUTTER_LAYOUT_MANAGER_CLASS (shell_app_view_grid_layout_parent_class);

  if (parent_class->set_container != NULL)
    parent_class->set_container (manager, container);

  g_set_weak_pointer (&self->container, container);
  st_adjustment_set_actor (self->page_indicators_adjustment, container);
  layout_sync_page_indicators (self);
}

static void
shell_app_view_grid_layout_allocate (ClutterLayoutManager *manager,
                                     ClutterActor         *container,
                                     const ClutterActorBox *box)
{
  ShellAppViewGridLayout *self = SHELL_APP_VIEW_GRID_LAYOUT (manager);
  gboolean ltr = clutter_actor_get_text_direction (container) != CLUTTER_TEXT_DIRECTION_RTL;
  float indicators_width = layout_get_indicators_width (self, box);
  ClutterMargin padding = { 0, };
  ClutterActorBox left_box = *box;
  ClutterActorBox right_box = *box;

  padding.left = indicators_width;
  padding.right = indicators_width;
  g_object_set (self->grid, "indicators-padding", &padding, NULL);

  clutter_actor_allocate (self->scroll_view, box);

  left_box.x2 = left_box.x1 + indicators_width;
  right_box.x1 = right_box.x2 - indicators_width;

  clutter_actor_allocate (self->previous_page_indicator, ltr ? &left_box : &right_box);
  clutter_actor_allocate_align_fill (self->previous_page_arrow,
                                     ltr ? &left_box : &right_box,
                                     0.5, 0.5, FALSE, FALSE);
  clutter_actor_allocate (self->next_page_indicator, ltr ? &right_box : &left_box);
  clutter_actor_allocate_align_fill (self->next_page_arrow,
                                     ltr ? &right_box : &left_box,
                                     0.5, 0.5, FALSE, FALSE);

  self->page_width = clutter_actor_box_get_width (box);
}

static void
layout_go_to_page (ShellAppViewGridLayout *self,
                   int                     page,
                   gboolean                animate)
{
  if (self->current_page == page)
    return;

  self->current_page = page;
  layout_sync_page_indicators_visibility (self, animate);
  layout_sync_page_indicators (self);
}

static void
layout_show_page_indicators (ShellAppViewGridLayout *self)
{
  if (self->show_indicators)
    return;

  ease_adjustment (self->page_indicators_adjustment, 1,
                   PAGE_PREVIEW_ANIMATION_TIME, CLUTTER_EASE_OUT_CUBIC,
                   NULL, NULL, NULL);

  st_viewport_set_clip_to_view (ST_VIEWPORT (self->grid), FALSE);
  self->show_indicators = TRUE;
  layout_sync_page_indicators_visibility (self, TRUE);
}

static void
layout_hide_done_cb (gboolean finished,
                     gpointer user_data)
{
  GWeakRef *ref = user_data;
  g_autoptr (ShellAppViewGridLayout) self = g_weak_ref_get (ref);

  if (finished && self != NULL && self->grid != NULL)
    st_viewport_set_clip_to_view (ST_VIEWPORT (self->grid), TRUE);
}

static void
weak_ref_free (gpointer data)
{
  GWeakRef *ref = data;

  g_weak_ref_clear (ref);
  g_free (ref);
}

static void
layout_hide_page_indicators (ShellAppViewGridLayout *self)
{
  GWeakRef *ref;

  if (!self->show_indicators)
    return;

  ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, self);
  ease_adjustment (self->page_indicators_adjustment, 0,
                   PAGE_PREVIEW_ANIMATION_TIME, CLUTTER_EASE_OUT_CUBIC,
                   layout_hide_done_cb, ref, weak_ref_free);

  self->show_indicators = FALSE;
  layout_sync_page_indicators_visibility (self, TRUE);
}

static void
shell_app_view_grid_layout_dispose (GObject *object)
{
  ShellAppViewGridLayout *self = SHELL_APP_VIEW_GRID_LAYOUT (object);

  if (self->grid != NULL)
    g_signal_handlers_disconnect_by_func (self->grid, layout_pages_changed_cb, self);

  if (self->page_indicators_adjustment != NULL)
    g_signal_handlers_disconnect_by_func (self->page_indicators_adjustment,
                                          layout_adjustment_value_changed_cb, self);

  g_clear_weak_pointer (&self->grid);
  g_clear_weak_pointer (&self->scroll_view);
  g_clear_weak_pointer (&self->previous_page_indicator);
  g_clear_weak_pointer (&self->previous_page_arrow);
  g_clear_weak_pointer (&self->next_page_indicator);
  g_clear_weak_pointer (&self->next_page_arrow);
  g_clear_weak_pointer (&self->container);
  g_clear_object (&self->page_indicators_adjustment);

  G_OBJECT_CLASS (shell_app_view_grid_layout_parent_class)->dispose (object);
}

static void
shell_app_view_grid_layout_class_init (ShellAppViewGridLayoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);

  object_class->dispose = shell_app_view_grid_layout_dispose;

  layout_class->set_container = shell_app_view_grid_layout_set_container;
  layout_class->allocate = shell_app_view_grid_layout_allocate;
}

static void
shell_app_view_grid_layout_init (ShellAppViewGridLayout *self)
{
  self->page_indicators_adjustment = st_adjustment_new (NULL, 0, 0, 1, 0, 0, 0);
  g_signal_connect (self->page_indicators_adjustment, "notify::value",
                    G_CALLBACK (layout_adjustment_value_changed_cb), self);
}

static ShellAppViewGridLayout *
shell_app_view_grid_layout_new (ShellAppGrid *grid,
                                ClutterActor *scroll_view,
                                ClutterActor *next_page_indicator,
                                ClutterActor *next_page_arrow,
                                ClutterActor *previous_page_indicator,
                                ClutterActor *previous_page_arrow)
{
  ShellAppViewGridLayout *self = g_object_new (SHELL_TYPE_APP_VIEW_GRID_LAYOUT, NULL);

  g_set_weak_pointer (&self->grid, grid);
  g_set_weak_pointer (&self->scroll_view, scroll_view);
  g_set_weak_pointer (&self->previous_page_indicator, previous_page_indicator);
  g_set_weak_pointer (&self->previous_page_arrow, previous_page_arrow);
  g_set_weak_pointer (&self->next_page_indicator, next_page_indicator);
  g_set_weak_pointer (&self->next_page_arrow, next_page_arrow);

  g_signal_connect (grid, "pages-changed", G_CALLBACK (layout_pages_changed_cb), self);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellPageManager                                                          */
/* ------------------------------------------------------------------------ */

enum
{
  PAGE_MANAGER_PROP_0,
  PAGE_MANAGER_PROP_SETTINGS,
  PAGE_MANAGER_N_PROPS
};

static GParamSpec *page_manager_props[PAGE_MANAGER_N_PROPS] = { NULL, };

enum
{
  PAGE_MANAGER_LAYOUT_CHANGED,
  PAGE_MANAGER_LAST_SIGNAL
};

static guint page_manager_signals[PAGE_MANAGER_LAST_SIGNAL] = { 0, };

struct _ShellPageManager
{
  GObject parent;

  GSettings *settings;
  GVariant *pages;
  gboolean updating_pages;
};

G_DEFINE_FINAL_TYPE (ShellPageManager, shell_page_manager, G_TYPE_OBJECT)

static void
page_manager_load_pages (ShellPageManager *self)
{
  g_clear_pointer (&self->pages, g_variant_unref);
  self->pages = g_settings_get_value (self->settings, "app-picker-layout");

  if (!self->updating_pages)
    g_signal_emit (self, page_manager_signals[PAGE_MANAGER_LAYOUT_CHANGED], 0);
}

static void
page_manager_settings_changed_cb (GSettings        *settings,
                                  const char       *key,
                                  ShellPageManager *self)
{
  page_manager_load_pages (self);
}

/**
 * shell_page_manager_new:
 * @settings: the `org.gnome.shell` settings, which have `app-picker-layout`
 *
 * Creates the object which reads and writes the layout of the application
 * grid (`new PageManager ()`).
 *
 * Returns: (transfer full): a new #ShellPageManager
 */
ShellPageManager *
shell_page_manager_new (GSettings *settings)
{
  g_return_val_if_fail (G_IS_SETTINGS (settings), NULL);

  return g_object_new (SHELL_TYPE_PAGE_MANAGER,
                       "settings", settings,
                       NULL);
}

/**
 * shell_page_manager_get_app_position:
 * @self: a #ShellPageManager
 * @app_id: the id of an application or folder
 * @page: (out): return location for the page, -1 if it is not in the layout
 * @position: (out): return location for the position, -1 if it is not in the
 *   layout
 *
 * Looks up where the item @app_id is on the pages (`getAppPosition ()`).
 */
void
shell_page_manager_get_app_position (ShellPageManager *self,
                                     const char       *app_id,
                                     int              *page,
                                     int              *position)
{
  gsize n_pages, i;

  g_return_if_fail (SHELL_IS_PAGE_MANAGER (self));
  g_return_if_fail (app_id != NULL);

  if (page != NULL)
    *page = -1;
  if (position != NULL)
    *position = -1;

  n_pages = self->pages != NULL ? g_variant_n_children (self->pages) : 0;
  for (i = 0; i < n_pages; i++)
    {
      g_autoptr (GVariant) page_data = g_variant_get_child_value (self->pages, i);
      g_autoptr (GVariant) properties = NULL;
      int pos = 0;

      properties = g_variant_lookup_value (page_data, app_id, G_VARIANT_TYPE_VARDICT);
      if (properties == NULL)
        continue;

      if (page != NULL)
        *page = (int) i;

      /* position is an int32, a missing one is the undefined of the JS */
      if (g_variant_lookup (properties, "position", "i", &pos) && position != NULL)
        *position = pos;

      return;
    }
}

/**
 * shell_page_manager_set_pages:
 * @self: a #ShellPageManager
 * @pages: an `aa{sv}` variant: for each page a dictionary of the ids of the
 *   items on it to an `a{sv}` with their `position`
 *
 * Saves the layout in the settings (`set pages ()`). ::layout-changed is not
 * emitted for this change.
 */
void
shell_page_manager_set_pages (ShellPageManager *self,
                              GVariant         *pages)
{
  g_return_if_fail (SHELL_IS_PAGE_MANAGER (self));
  g_return_if_fail (pages != NULL);
  g_return_if_fail (g_variant_is_of_type (pages, G_VARIANT_TYPE ("aa{sv}")));

  self->updating_pages = TRUE;
  g_settings_set_value (self->settings, "app-picker-layout", pages);
  self->updating_pages = FALSE;
}

/**
 * shell_page_manager_get_pages:
 * @self: a #ShellPageManager
 *
 * Returns: (transfer none): the layout as an `aa{sv}` variant (`pages`)
 */
GVariant *
shell_page_manager_get_pages (ShellPageManager *self)
{
  g_return_val_if_fail (SHELL_IS_PAGE_MANAGER (self), NULL);

  return self->pages;
}

static void
shell_page_manager_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellPageManager *self = SHELL_PAGE_MANAGER (object);

  switch (prop_id)
    {
    case PAGE_MANAGER_PROP_SETTINGS:
      self->settings = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_page_manager_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellPageManager *self = SHELL_PAGE_MANAGER (object);

  switch (prop_id)
    {
    case PAGE_MANAGER_PROP_SETTINGS:
      g_value_set_object (value, self->settings);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_page_manager_constructed (GObject *object)
{
  ShellPageManager *self = SHELL_PAGE_MANAGER (object);

  G_OBJECT_CLASS (shell_page_manager_parent_class)->constructed (object);

  g_return_if_fail (self->settings != NULL);

  g_signal_connect_object (self->settings, "changed::app-picker-layout",
                           G_CALLBACK (page_manager_settings_changed_cb), self, 0);
  page_manager_load_pages (self);
}

static void
shell_page_manager_dispose (GObject *object)
{
  ShellPageManager *self = SHELL_PAGE_MANAGER (object);

  g_clear_object (&self->settings);
  g_clear_pointer (&self->pages, g_variant_unref);

  G_OBJECT_CLASS (shell_page_manager_parent_class)->dispose (object);
}

static void
shell_page_manager_class_init (ShellPageManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_page_manager_constructed;
  object_class->dispose = shell_page_manager_dispose;
  object_class->set_property = shell_page_manager_set_property;
  object_class->get_property = shell_page_manager_get_property;

  /**
   * ShellPageManager:settings:
   *
   * The `org.gnome.shell` settings. The JS read them from `global.settings`.
   */
  page_manager_props[PAGE_MANAGER_PROP_SETTINGS] =
    g_param_spec_object ("settings", NULL, NULL,
                         G_TYPE_SETTINGS,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PAGE_MANAGER_N_PROPS,
                                     page_manager_props);

  /**
   * ShellPageManager::layout-changed:
   * @self: the #ShellPageManager
   *
   * Emitted when the layout was changed by something else than
   * shell_page_manager_set_pages ().
   */
  page_manager_signals[PAGE_MANAGER_LAYOUT_CHANGED] =
    g_signal_new ("layout-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_page_manager_init (ShellPageManager *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellAppViewItem                                                          */
/* ------------------------------------------------------------------------ */

enum
{
  ITEM_PROP_0,
  ITEM_PROP_ID,
  ITEM_PROP_NAME,
  ITEM_PROP_IS_DRAGGABLE,
  ITEM_PROP_EXPAND_TITLE_ON_HOVER,
  ITEM_PROP_WINDOW_MANAGER,
  ITEM_PROP_DRAG_ICON_SIZE,
  ITEM_N_PROPS
};

static GParamSpec *item_props[ITEM_N_PROPS] = { NULL, };

typedef struct
{
  char *id;
  char *name;

  gboolean is_draggable;
  gboolean expand_title_on_hover;
  ShellWindowManager *window_manager; /* weak */
  int drag_icon_size;

  ShellBaseIcon *icon; /* weak */

  gboolean dragging;
  gboolean other_icon_is_hovering;
  guint drag_monitor;

  gboolean forced_highlight;
  int expand; /* -1 until the title was expanded or collapsed */
} ShellAppViewItemPrivate;

static void shell_app_view_item_drag_target_iface_init (ShellDragTargetInterface *iface);

G_DEFINE_TYPE_WITH_CODE (ShellAppViewItem, shell_app_view_item, ST_TYPE_BUTTON,
                         G_ADD_PRIVATE (ShellAppViewItem)
                         G_IMPLEMENT_INTERFACE (SHELL_TYPE_DRAG_TARGET,
                                                shell_app_view_item_drag_target_iface_init))

/**
 * shell_app_view_item_get_id:
 * @self: a #ShellAppViewItem
 *
 * Returns: (transfer none) (nullable): the id of the application or folder
 */
const char *
shell_app_view_item_get_id (ShellAppViewItem *self)
{
  ShellAppViewItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_APP_VIEW_ITEM (self), NULL);

  priv = shell_app_view_item_get_instance_private (self);
  return priv->id;
}

/**
 * shell_app_view_item_set_id:
 * @self: a #ShellAppViewItem
 * @id: the id of the application or folder
 *
 * For subclasses: sets the id (`_id`).
 */
void
shell_app_view_item_set_id (ShellAppViewItem *self,
                            const char       *id)
{
  ShellAppViewItemPrivate *priv;

  g_return_if_fail (SHELL_IS_APP_VIEW_ITEM (self));

  priv = shell_app_view_item_get_instance_private (self);
  if (g_strcmp0 (priv->id, id) == 0)
    return;

  g_free (priv->id);
  priv->id = g_strdup (id);
  g_object_notify_by_pspec (G_OBJECT (self), item_props[ITEM_PROP_ID]);
}

/**
 * shell_app_view_item_get_name:
 * @self: a #ShellAppViewItem
 *
 * Returns: (transfer none) (nullable): the name of the application or folder
 */
const char *
shell_app_view_item_get_name (ShellAppViewItem *self)
{
  ShellAppViewItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_APP_VIEW_ITEM (self), NULL);

  priv = shell_app_view_item_get_instance_private (self);
  return priv->name;
}

/**
 * shell_app_view_item_set_name:
 * @self: a #ShellAppViewItem
 * @name: the name of the application or folder
 *
 * For subclasses: sets the name (`_name`).
 */
void
shell_app_view_item_set_name (ShellAppViewItem *self,
                              const char       *name)
{
  ShellAppViewItemPrivate *priv;

  g_return_if_fail (SHELL_IS_APP_VIEW_ITEM (self));

  priv = shell_app_view_item_get_instance_private (self);
  if (g_strcmp0 (priv->name, name) == 0)
    return;

  g_free (priv->name);
  priv->name = g_strdup (name);
  g_object_notify_by_pspec (G_OBJECT (self), item_props[ITEM_PROP_NAME]);
}

/**
 * shell_app_view_item_get_icon:
 * @self: a #ShellAppViewItem
 *
 * Returns: (transfer none) (nullable): the icon of the item (`icon`)
 */
ShellBaseIcon *
shell_app_view_item_get_icon (ShellAppViewItem *self)
{
  ShellAppViewItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_APP_VIEW_ITEM (self), NULL);

  priv = shell_app_view_item_get_instance_private (self);
  return priv->icon;
}

/**
 * shell_app_view_item_set_icon:
 * @self: a #ShellAppViewItem
 * @icon: (nullable): the icon, which has to be a descendant of @self
 *
 * For subclasses: sets the icon (`icon`).
 */
void
shell_app_view_item_set_icon (ShellAppViewItem *self,
                              ShellBaseIcon    *icon)
{
  ShellAppViewItemPrivate *priv;

  g_return_if_fail (SHELL_IS_APP_VIEW_ITEM (self));
  g_return_if_fail (icon == NULL || SHELL_IS_BASE_ICON (icon));

  priv = shell_app_view_item_get_instance_private (self);
  g_clear_weak_pointer (&priv->icon);
  g_set_weak_pointer (&priv->icon, icon);
}

/**
 * shell_app_view_item_get_window_manager:
 * @self: a #ShellAppViewItem
 *
 * Returns: (transfer none) (nullable): the window manager the item reaches
 *   the overview through
 */
ShellWindowManager *
shell_app_view_item_get_window_manager (ShellAppViewItem *self)
{
  ShellAppViewItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_APP_VIEW_ITEM (self), NULL);

  priv = shell_app_view_item_get_instance_private (self);
  return priv->window_manager;
}

/**
 * shell_app_view_item_get_drag_icon_size:
 * @self: a #ShellAppViewItem
 *
 * Returns: the size of the icon that is dragged, in logical pixels
 *   (`Main.overview.dash.iconSize`)
 */
int
shell_app_view_item_get_drag_icon_size (ShellAppViewItem *self)
{
  ShellAppViewItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_APP_VIEW_ITEM (self), 0);

  priv = shell_app_view_item_get_instance_private (self);
  return priv->drag_icon_size;
}

/**
 * shell_app_view_item_set_drag_icon_size:
 * @self: a #ShellAppViewItem
 * @size: the size of the icon that is dragged, in logical pixels
 *
 * Sets the size of the icon that is dragged, which is the icon size of the
 * dash.
 */
void
shell_app_view_item_set_drag_icon_size (ShellAppViewItem *self,
                                        int               size)
{
  ShellAppViewItemPrivate *priv;

  g_return_if_fail (SHELL_IS_APP_VIEW_ITEM (self));

  priv = shell_app_view_item_get_instance_private (self);
  if (priv->drag_icon_size == size)
    return;

  priv->drag_icon_size = size;
  g_object_notify_by_pspec (G_OBJECT (self), item_props[ITEM_PROP_DRAG_ICON_SIZE]);
}

/**
 * shell_app_view_item_get_dragging:
 * @self: a #ShellAppViewItem
 *
 * Returns: whether the item is being dragged
 */
gboolean
shell_app_view_item_get_dragging (ShellAppViewItem *self)
{
  ShellAppViewItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_APP_VIEW_ITEM (self), FALSE);

  priv = shell_app_view_item_get_instance_private (self);
  return priv->dragging;
}

static void
restore_easing_cb (ClutterActor *actor,
                   GParamSpec   *pspec,
                   gpointer      user_data)
{
  gulong *handler_id = user_data;

  clutter_actor_restore_easing_state (actor);
  g_signal_handler_disconnect (actor, *handler_id);
}

static void
app_view_item_update_multiline (ShellAppViewItem *self)
{
  ShellAppViewItemPrivate *priv = shell_app_view_item_get_instance_private (self);
  StLabel *label;
  ClutterText *clutter_text;
  PangoLayout *layout;
  gboolean expand;
  ClutterActor *leader;
  gulong *handler_id;

  if (!priv->expand_title_on_hover || priv->icon == NULL)
    return;

  label = shell_base_icon_get_label (priv->icon);
  if (label == NULL)
    return;

  clutter_text = st_label_get_clutter_text (label);
  layout = clutter_text_get_layout (clutter_text);
  if (!pango_layout_is_wrapped (layout) && !pango_layout_is_ellipsized (layout))
    return;

  expand = priv->forced_highlight ||
           st_widget_get_hover (ST_WIDGET (self)) ||
           clutter_actor_has_key_focus (CLUTTER_ACTOR (self));
  if (priv->expand == expand)
    return;
  priv->expand = expand;

  leader = expand ? CLUTTER_ACTOR (self) : CLUTTER_ACTOR (label);
  clutter_actor_remove_transition (leader, "allocation");

  handler_id = g_new0 (gulong, 1);
  *handler_id = g_signal_connect_data (leader, "notify::allocation",
                                       G_CALLBACK (restore_easing_cb),
                                       handler_id, (GClosureNotify) g_free, 0);

  clutter_actor_set_clip_to_allocation (CLUTTER_ACTOR (priv->icon), expand);
  clutter_actor_save_easing_state (leader);
  clutter_actor_set_easing_duration (leader, expand
                                    ? APP_ICON_TITLE_EXPAND_TIME
                                    : APP_ICON_TITLE_COLLAPSE_TIME);
  g_object_set (clutter_text,
                "line-wrap", expand,
                "line-wrap-mode", expand ? PANGO_WRAP_WORD_CHAR : PANGO_WRAP_WORD,
                "ellipsize", expand ? PANGO_ELLIPSIZE_NONE : PANGO_ELLIPSIZE_END,
                NULL);
}

static void
item_hover_changed_cb (GObject    *object,
                       GParamSpec *pspec,
                       gpointer    user_data)
{
  app_view_item_update_multiline (SHELL_APP_VIEW_ITEM (object));
}

/**
 * shell_app_view_item_scale_in:
 * @self: a #ShellAppViewItem
 *
 * Zooms the item in, after a delay (`scaleIn ()`).
 */
void
shell_app_view_item_scale_in (ShellAppViewItem *self)
{
  ShellEaseParams params = {
    .duration = APP_ICON_SCALE_IN_TIME,
    .delay = APP_ICON_SCALE_IN_DELAY,
    .mode = CLUTTER_EASE_OUT_QUINT,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  g_return_if_fail (SHELL_IS_APP_VIEW_ITEM (self));

  clutter_actor_set_scale (CLUTTER_ACTOR (self), 0, 0);

  shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                    "scale-x", 1.0,
                    "scale-y", 1.0,
                    NULL);
}

static void
item_real_scale_and_fade (ShellAppViewItem *self)
{
  ShellEaseParams params = default_ease_params ();

  clutter_actor_set_reactive (CLUTTER_ACTOR (self), FALSE);
  shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                    "scale-x", 0.5,
                    "scale-y", 0.5,
                    "opacity", 0,
                    NULL);
}

static void
item_real_undo_scale_and_fade (ShellAppViewItem *self)
{
  ShellEaseParams params = default_ease_params ();

  clutter_actor_set_reactive (CLUTTER_ACTOR (self), TRUE);
  shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                    "scale-x", 1.0,
                    "scale-y", 1.0,
                    "opacity", 255,
                    NULL);
}

/**
 * shell_app_view_item_scale_and_fade:
 * @self: a #ShellAppViewItem
 *
 * Shrinks and hides the item, as long as it is dragged (`scaleAndFade ()`).
 */
void
shell_app_view_item_scale_and_fade (ShellAppViewItem *self)
{
  g_return_if_fail (SHELL_IS_APP_VIEW_ITEM (self));

  SHELL_APP_VIEW_ITEM_GET_CLASS (self)->scale_and_fade (self);
}

/**
 * shell_app_view_item_undo_scale_and_fade:
 * @self: a #ShellAppViewItem
 *
 * Shows the item again after it was dragged (`undoScaleAndFade ()`).
 */
void
shell_app_view_item_undo_scale_and_fade (ShellAppViewItem *self)
{
  g_return_if_fail (SHELL_IS_APP_VIEW_ITEM (self));

  SHELL_APP_VIEW_ITEM_GET_CLASS (self)->undo_scale_and_fade (self);
}

static gboolean
item_real_can_accept (ShellAppViewItem *self,
                 GObject          *source)
{
  return source != G_OBJECT (self);
}

static ShellDragMotionResult
app_view_item_drag_motion_cb (ShellDragEvent *event,
                              gpointer        user_data)
{
  ShellAppViewItem *self = user_data;

  return SHELL_APP_VIEW_ITEM_GET_CLASS (self)->drag_motion (self, event);
}

static void
item_real_set_hovering_by_dnd (ShellAppViewItem *self,
                          gboolean          hovering)
{
  ShellAppViewItemPrivate *priv = shell_app_view_item_get_instance_private (self);

  if (priv->other_icon_is_hovering == hovering)
    return;

  priv->other_icon_is_hovering = hovering;

  if (hovering)
    {
      priv->drag_monitor = shell_dnd_add_drag_monitor (app_view_item_drag_motion_cb, self, NULL,
                                                       NULL, NULL, NULL);
    }
  else if (priv->drag_monitor != 0)
    {
      shell_dnd_remove_drag_monitor (priv->drag_monitor);
      priv->drag_monitor = 0;
    }
}

static void
app_view_item_set_hovering_by_dnd (ShellAppViewItem *self,
                                   gboolean          hovering)
{
  SHELL_APP_VIEW_ITEM_GET_CLASS (self)->set_hovering_by_dnd (self, hovering);
}

static ShellDragMotionResult
item_real_drag_motion (ShellAppViewItem *self,
                  ShellDragEvent   *event)
{
  ClutterActor *target_actor = shell_drag_event_get_target_actor (event);

  if (target_actor == NULL || !clutter_actor_contains (CLUTTER_ACTOR (self), target_actor))
    app_view_item_set_hovering_by_dnd (self, FALSE);

  return SHELL_DRAG_MOTION_RESULT_CONTINUE;
}

static ShellDragMotionResult
item_real_handle_drag_over (ShellAppViewItem *self,
                       GObject          *source,
                       ClutterActor     *actor,
                       float             x,
                       float             y,
                       uint32_t          time)
{
  if (source == G_OBJECT (self))
    return SHELL_DRAG_MOTION_RESULT_NO_DROP;

  if (!SHELL_APP_VIEW_ITEM_GET_CLASS (self)->can_accept (self, source))
    return SHELL_DRAG_MOTION_RESULT_CONTINUE;

  /* _withinLeeways () was always false */
  app_view_item_set_hovering_by_dnd (self, TRUE);

  return SHELL_DRAG_MOTION_RESULT_MOVE_DROP;
}

static gboolean
item_real_accept_drop (ShellAppViewItem *self,
                  GObject          *source,
                  ClutterActor     *actor,
                  float             x,
                  float             y,
                  uint32_t          time)
{
  app_view_item_set_hovering_by_dnd (self, FALSE);

  return SHELL_APP_VIEW_ITEM_GET_CLASS (self)->can_accept (self, source);
}

static ShellDragMotionResult
item_iface_handle_drag_over (ShellDragTarget *target,
                             GObject         *source,
                             ClutterActor    *actor,
                             float            x,
                             float            y,
                             uint32_t         time)
{
  ShellAppViewItem *self = SHELL_APP_VIEW_ITEM (target);

  return SHELL_APP_VIEW_ITEM_GET_CLASS (self)->handle_drag_over (self, source, actor, x, y, time);
}

static gboolean
item_iface_accept_drop (ShellDragTarget *target,
                        GObject         *source,
                        ClutterActor    *actor,
                        float            x,
                        float            y,
                        uint32_t         time)
{
  ShellAppViewItem *self = SHELL_APP_VIEW_ITEM (target);

  return SHELL_APP_VIEW_ITEM_GET_CLASS (self)->accept_drop (self, source, actor, x, y, time);
}

static void
shell_app_view_item_drag_target_iface_init (ShellDragTargetInterface *iface)
{
  iface->handle_drag_over = item_iface_handle_drag_over;
  iface->accept_drop = item_iface_accept_drop;
}

/**
 * shell_app_view_item_set_forced_highlight:
 * @self: a #ShellAppViewItem
 * @highlighted: whether to show the item as hovered whatever the pointer does
 *
 * Highlights the item while its menu is open (`setForcedHighlight ()`).
 */
void
shell_app_view_item_set_forced_highlight (ShellAppViewItem *self,
                                          gboolean          highlighted)
{
  ShellAppViewItemPrivate *priv;

  g_return_if_fail (SHELL_IS_APP_VIEW_ITEM (self));

  priv = shell_app_view_item_get_instance_private (self);
  priv->forced_highlight = highlighted;

  g_object_freeze_notify (G_OBJECT (self));
  st_widget_set_track_hover (ST_WIDGET (self), !highlighted);
  st_widget_set_hover (ST_WIDGET (self), highlighted);
  g_object_thaw_notify (G_OBJECT (self));
}

static void
app_view_item_draggable_begin_cb (ShellDraggable *draggable,
                                  uint32_t        time,
                                  ShellAppViewItem *self)
{
  ShellAppViewItemPrivate *priv = shell_app_view_item_get_instance_private (self);

  priv->dragging = TRUE;
  shell_app_view_item_scale_and_fade (self);

  if (priv->window_manager != NULL)
    shell_window_manager_begin_item_drag (priv->window_manager, G_OBJECT (self));
}

static void
app_view_item_draggable_cancelled_cb (ShellDraggable *draggable,
                                      uint32_t        time,
                                      ShellAppViewItem *self)
{
  ShellAppViewItemPrivate *priv = shell_app_view_item_get_instance_private (self);

  priv->dragging = FALSE;

  if (priv->window_manager != NULL)
    shell_window_manager_cancelled_item_drag (priv->window_manager, G_OBJECT (self));
}

static void
app_view_item_draggable_end_cb (ShellDraggable *draggable,
                                uint32_t        time,
                                gboolean        snapback,
                                ShellAppViewItem *self)
{
  ShellAppViewItemPrivate *priv = shell_app_view_item_get_instance_private (self);

  priv->dragging = FALSE;
  shell_app_view_item_undo_scale_and_fade (self);

  if (priv->window_manager != NULL)
    shell_window_manager_end_item_drag (priv->window_manager, G_OBJECT (self));
}

static void
app_view_item_key_focus_in (ClutterActor *actor)
{
  app_view_item_update_multiline (SHELL_APP_VIEW_ITEM (actor));
  CLUTTER_ACTOR_CLASS (shell_app_view_item_parent_class)->key_focus_in (actor);
}

static void
app_view_item_key_focus_out (ClutterActor *actor)
{
  app_view_item_update_multiline (SHELL_APP_VIEW_ITEM (actor));
  CLUTTER_ACTOR_CLASS (shell_app_view_item_parent_class)->key_focus_out (actor);
}

static void
app_view_item_release (ShellAppViewItem *self)
{
  ShellAppViewItemPrivate *priv = shell_app_view_item_get_instance_private (self);

  if (priv->drag_monitor != 0)
    {
      shell_dnd_remove_drag_monitor (priv->drag_monitor);
      priv->drag_monitor = 0;
    }
}

static void
app_view_item_destroy (ClutterActor *actor)
{
  ShellAppViewItem *self = SHELL_APP_VIEW_ITEM (actor);
  ShellAppViewItemPrivate *priv = shell_app_view_item_get_instance_private (self);

  app_view_item_release (self);

  if (priv->is_draggable && priv->dragging)
    {
      priv->dragging = FALSE;

      if (priv->window_manager != NULL)
        shell_window_manager_end_item_drag (priv->window_manager, G_OBJECT (self));
    }

  CLUTTER_ACTOR_CLASS (shell_app_view_item_parent_class)->destroy (actor);
}

static void
shell_app_view_item_constructed (GObject *object)
{
  ShellAppViewItem *self = SHELL_APP_VIEW_ITEM (object);
  ShellAppViewItemPrivate *priv = shell_app_view_item_get_instance_private (self);

  G_OBJECT_CLASS (shell_app_view_item_parent_class)->constructed (object);

  if (priv->is_draggable)
    {
      g_autoptr (ShellDraggable) draggable = NULL;

      /* The item owns the draggable */
      draggable = g_object_new (SHELL_TYPE_DRAGGABLE,
                                "actor", self,
                                "timeout-threshold", 200u,
                                NULL);
      g_signal_connect_object (draggable, "drag-begin",
                               G_CALLBACK (app_view_item_draggable_begin_cb), self, 0);
      g_signal_connect_object (draggable, "drag-cancelled",
                               G_CALLBACK (app_view_item_draggable_cancelled_cb), self, 0);
      g_signal_connect_object (draggable, "drag-end",
                               G_CALLBACK (app_view_item_draggable_end_cb), self, 0);
    }

  if (priv->expand_title_on_hover)
    g_signal_connect (self, "notify::hover", G_CALLBACK (item_hover_changed_cb), NULL);
}

static void
shell_app_view_item_dispose (GObject *object)
{
  ShellAppViewItem *self = SHELL_APP_VIEW_ITEM (object);
  ShellAppViewItemPrivate *priv = shell_app_view_item_get_instance_private (self);

  app_view_item_release (self);

  /* dispose runs before the destroy vfunc and clears window_manager, so end
   * a running drag here */
  if (priv->is_draggable && priv->dragging)
    {
      priv->dragging = FALSE;

      if (priv->window_manager != NULL)
        shell_window_manager_end_item_drag (priv->window_manager, G_OBJECT (self));
    }

  g_clear_weak_pointer (&priv->window_manager);
  g_clear_weak_pointer (&priv->icon);

  G_OBJECT_CLASS (shell_app_view_item_parent_class)->dispose (object);
}

static void
shell_app_view_item_finalize (GObject *object)
{
  ShellAppViewItem *self = SHELL_APP_VIEW_ITEM (object);
  ShellAppViewItemPrivate *priv = shell_app_view_item_get_instance_private (self);

  g_free (priv->id);
  g_free (priv->name);

  G_OBJECT_CLASS (shell_app_view_item_parent_class)->finalize (object);
}

static void
shell_app_view_item_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  ShellAppViewItem *self = SHELL_APP_VIEW_ITEM (object);
  ShellAppViewItemPrivate *priv = shell_app_view_item_get_instance_private (self);

  switch (prop_id)
    {
    case ITEM_PROP_IS_DRAGGABLE:
      priv->is_draggable = g_value_get_boolean (value);
      break;

    case ITEM_PROP_EXPAND_TITLE_ON_HOVER:
      priv->expand_title_on_hover = g_value_get_boolean (value);
      break;

    case ITEM_PROP_WINDOW_MANAGER:
      g_clear_weak_pointer (&priv->window_manager);
      g_set_weak_pointer (&priv->window_manager, g_value_get_object (value));
      break;

    case ITEM_PROP_DRAG_ICON_SIZE:
      shell_app_view_item_set_drag_icon_size (self, g_value_get_int (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_app_view_item_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellAppViewItem *self = SHELL_APP_VIEW_ITEM (object);
  ShellAppViewItemPrivate *priv = shell_app_view_item_get_instance_private (self);

  switch (prop_id)
    {
    case ITEM_PROP_ID:
      g_value_set_string (value, priv->id);
      break;

    case ITEM_PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case ITEM_PROP_IS_DRAGGABLE:
      g_value_set_boolean (value, priv->is_draggable);
      break;

    case ITEM_PROP_EXPAND_TITLE_ON_HOVER:
      g_value_set_boolean (value, priv->expand_title_on_hover);
      break;

    case ITEM_PROP_WINDOW_MANAGER:
      g_value_set_object (value, priv->window_manager);
      break;

    case ITEM_PROP_DRAG_ICON_SIZE:
      g_value_set_int (value, priv->drag_icon_size);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_app_view_item_class_init (ShellAppViewItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_app_view_item_constructed;
  object_class->dispose = shell_app_view_item_dispose;
  object_class->finalize = shell_app_view_item_finalize;
  object_class->set_property = shell_app_view_item_set_property;
  object_class->get_property = shell_app_view_item_get_property;

  actor_class->destroy = app_view_item_destroy;
  actor_class->key_focus_in = app_view_item_key_focus_in;
  actor_class->key_focus_out = app_view_item_key_focus_out;

  klass->can_accept = item_real_can_accept;
  klass->set_hovering_by_dnd = item_real_set_hovering_by_dnd;
  klass->handle_drag_over = item_real_handle_drag_over;
  klass->accept_drop = item_real_accept_drop;
  klass->scale_and_fade = item_real_scale_and_fade;
  klass->undo_scale_and_fade = item_real_undo_scale_and_fade;
  klass->drag_motion = item_real_drag_motion;

  /**
   * ShellAppViewItem:id:
   *
   * The id of the application or folder (`id`).
   */
  item_props[ITEM_PROP_ID] =
    g_param_spec_string ("id", NULL, NULL, NULL,
                         G_PARAM_READABLE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellAppViewItem:name:
   *
   * The name of the application or folder (`name`).
   */
  item_props[ITEM_PROP_NAME] =
    g_param_spec_string ("name", NULL, NULL, NULL,
                         G_PARAM_READABLE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellAppViewItem:is-draggable:
   *
   * Whether the item can be dragged (`isDraggable`).
   */
  item_props[ITEM_PROP_IS_DRAGGABLE] =
    g_param_spec_boolean ("is-draggable", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellAppViewItem:expand-title-on-hover:
   *
   * Whether the title wraps while the item is hovered or focused, instead of
   * being cut off (`expandTitleOnHover`).
   */
  item_props[ITEM_PROP_EXPAND_TITLE_ON_HOVER] =
    g_param_spec_boolean ("expand-title-on-hover", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellAppViewItem:window-manager:
   *
   * The window manager that tells the drags of the item to the overview and
   * hides the overview (`Main.overview`).
   */
  item_props[ITEM_PROP_WINDOW_MANAGER] =
    g_param_spec_object ("window-manager", NULL, NULL,
                         SHELL_TYPE_WINDOW_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellAppViewItem:drag-icon-size:
   *
   * The size of the icon of the application that is dragged, in logical
   * pixels (`Main.overview.dash.iconSize`).
   */
  item_props[ITEM_PROP_DRAG_ICON_SIZE] =
    g_param_spec_int ("drag-icon-size", NULL, NULL,
                      1, G_MAXINT, 64,
                      G_PARAM_READWRITE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, ITEM_N_PROPS, item_props);
}

static void
shell_app_view_item_init (ShellAppViewItem *self)
{
  ShellAppViewItemPrivate *priv = shell_app_view_item_get_instance_private (self);

  priv->is_draggable = TRUE;
  priv->expand_title_on_hover = TRUE;
  priv->drag_icon_size = 64;
  priv->expand = -1;

  clutter_actor_set_pivot_point (CLUTTER_ACTOR (self), 0.5f, 0.5f);
  clutter_actor_set_reactive (CLUTTER_ACTOR (self), TRUE);
  st_button_set_button_mask (ST_BUTTON (self), ST_BUTTON_PRIMARY | ST_BUTTON_MIDDLE);
  st_widget_set_can_focus (ST_WIDGET (self), TRUE);
}

/* ------------------------------------------------------------------------ */
/* ShellAppIcon                                                              */
/* ------------------------------------------------------------------------ */

enum
{
  APP_ICON_PROP_0,
  APP_ICON_PROP_APP,
  APP_ICON_PROP_POPUP_MENU_SIDE,
  APP_ICON_PROP_SHOW_LABEL,
  APP_ICON_N_PROPS
};

static GParamSpec *app_icon_props[APP_ICON_N_PROPS] = { NULL, };

enum
{
  APP_ICON_MENU_STATE_CHANGED,
  APP_ICON_SYNC_TOOLTIP,
  APP_ICON_LAST_SIGNAL
};

static guint app_icon_signals[APP_ICON_LAST_SIGNAL] = { 0, };

typedef struct
{
  ShellApp *app;
  StSide popup_menu_side;
  gboolean show_label;

  ClutterActor *icon_container; /* weak */
  ClutterActor *dot; /* weak */

  guint folder_preview_id;

  ShellPopupMenuBase *menu; /* weak, the actor of the menu owns it */
  ShellPopupMenuManager *menu_manager;
} ShellAppIconPrivate;

static void shell_app_icon_drag_source_iface_init (ShellDragSourceInterface *iface);

G_DEFINE_TYPE_WITH_CODE (ShellAppIcon, shell_app_icon, SHELL_TYPE_APP_VIEW_ITEM,
                         G_ADD_PRIVATE (ShellAppIcon)
                         G_IMPLEMENT_INTERFACE (SHELL_TYPE_DRAG_SOURCE,
                                                shell_app_icon_drag_source_iface_init))

static ShellAppViewItemPrivate *
item_priv (ShellAppViewItem *item)
{
  return shell_app_view_item_get_instance_private (item);
}

static ClutterActor *
app_icon_create_icon_cb (int      size,
                         gpointer user_data)
{
  ShellAppIcon *self = user_data;
  ShellAppIconPrivate *priv = shell_app_icon_get_instance_private (self);

  return shell_app_create_icon_texture (priv->app, size);
}

static void
app_icon_update_dot_style (ShellAppIcon *self)
{
  ShellAppIconPrivate *priv = shell_app_icon_get_instance_private (self);
  StThemeNode *theme_node;
  float x, y, z;

  if (priv->dot == NULL)
    return;

  theme_node = st_widget_get_theme_node (ST_WIDGET (priv->dot));
  clutter_actor_get_translation (priv->dot, &x, &y, &z);
  clutter_actor_set_translation (priv->dot, x,
                                 st_theme_node_get_length (theme_node, "offset-y"), z);
}

static void
app_icon_dot_style_changed_cb (StWidget     *dot,
                               ShellAppIcon *self)
{
  app_icon_update_dot_style (self);
}

static void
app_icon_update_running_style (ShellAppIcon *self)
{
  ShellAppIconPrivate *priv = shell_app_icon_get_instance_private (self);

  if (priv->dot == NULL)
    return;

  if (shell_app_get_state (priv->app) != SHELL_APP_STATE_STOPPED)
    clutter_actor_show (priv->dot);
  else
    clutter_actor_hide (priv->dot);
}

static void
app_icon_app_state_changed_cb (GObject      *app,
                               GParamSpec   *pspec,
                               ShellAppIcon *self)
{
  app_icon_update_running_style (self);
}

/**
 * shell_app_icon_get_app:
 * @self: a #ShellAppIcon
 *
 * Returns: (transfer none): the application of the icon (`app`)
 */
ShellApp *
shell_app_icon_get_app (ShellAppIcon *self)
{
  ShellAppIconPrivate *priv;

  g_return_val_if_fail (SHELL_IS_APP_ICON (self), NULL);

  priv = shell_app_icon_get_instance_private (self);
  return priv->app;
}

/**
 * shell_app_icon_animate_launch:
 * @self: a #ShellAppIcon
 *
 * Zooms out the icon, as the application starts (`animateLaunch ()`).
 */
void
shell_app_icon_animate_launch (ShellAppIcon *self)
{
  ShellAppViewItemPrivate *item = NULL;

  g_return_if_fail (SHELL_IS_APP_ICON (self));

  item = item_priv (SHELL_APP_VIEW_ITEM (self));
  if (item->icon != NULL)
    shell_base_icon_animate_zoom_out (item->icon);
}

/**
 * shell_app_icon_animate_launch_at_pos:
 * @self: a #ShellAppIcon
 * @x: x coordinate on the stage
 * @y: y coordinate on the stage
 *
 * Like shell_app_icon_animate_launch (), with the icon at (@x, @y)
 * (`animateLaunchAtPos ()`).
 */
void
shell_app_icon_animate_launch_at_pos (ShellAppIcon *self,
                                      float         x,
                                      float         y)
{
  ShellAppViewItemPrivate *item = NULL;

  g_return_if_fail (SHELL_IS_APP_ICON (self));

  item = item_priv (SHELL_APP_VIEW_ITEM (self));
  if (item->icon != NULL)
    shell_base_icon_animate_zoom_out_at_pos (item->icon, x, y);
}

/**
 * shell_app_icon_activate:
 * @self: a #ShellAppIcon
 * @button: the mouse button which activated the icon, 0 for none
 *
 * Starts or shows the application, or opens a new window of it for the
 * middle button or with Ctrl (`activate ()`), and leaves the overview.
 */
void
shell_app_icon_activate (ShellAppIcon *self,
                         uint32_t      button)
{
  ShellAppIconPrivate *priv;
  const ClutterEvent *event;
  ClutterModifierType modifiers = 0;
  gboolean is_middle_button, is_ctrl_pressed, open_new_window;

  g_return_if_fail (SHELL_IS_APP_ICON (self));

  priv = shell_app_icon_get_instance_private (self);

  event = clutter_get_current_event ();
  if (event != NULL)
    modifiers = clutter_event_get_state (event);

  is_middle_button = button != 0 && button == CLUTTER_BUTTON_MIDDLE;
  is_ctrl_pressed = (modifiers & CLUTTER_CONTROL_MASK) != 0;
  open_new_window = shell_app_can_open_new_window (priv->app) &&
                    shell_app_get_state (priv->app) == SHELL_APP_STATE_RUNNING &&
                    (is_ctrl_pressed || is_middle_button);

  if (shell_app_get_state (priv->app) == SHELL_APP_STATE_STOPPED || open_new_window)
    shell_app_icon_animate_launch (self);

  if (open_new_window)
    shell_app_open_new_window (priv->app, -1);
  else
    shell_app_activate (priv->app);

  request_overview_hide (item_priv (SHELL_APP_VIEW_ITEM (self))->window_manager);
}

static void
app_icon_clicked (StButton *button,
                  int       clicked_button)
{
  shell_app_icon_activate (SHELL_APP_ICON (button), clicked_button);
}

/**
 * shell_app_icon_should_show_tooltip:
 * @self: a #ShellAppIcon
 *
 * Returns: whether the label of the dash should be shown for the icon
 *   (`shouldShowTooltip ()`)
 */
gboolean
shell_app_icon_should_show_tooltip (ShellAppIcon *self)
{
  ShellAppIconPrivate *priv;

  g_return_val_if_fail (SHELL_IS_APP_ICON (self), FALSE);

  priv = shell_app_icon_get_instance_private (self);
  return st_widget_get_hover (ST_WIDGET (self)) &&
         (priv->menu == NULL || !shell_popup_menu_base_get_is_open (priv->menu));
}

/* Main.activateWindow () */
static void
app_icon_activate_window (ShellAppIcon *self,
                          MetaWindow   *window,
                          uint32_t      time)
{
  ShellGlobal *global = shell_global_get ();
  MetaWorkspaceManager *workspace_manager = shell_global_get_workspace_manager (global);
  MetaWorkspace *active = meta_workspace_manager_get_active_workspace (workspace_manager);
  MetaWorkspace *workspace = meta_window_get_workspace (window);
  ShellPanel *panel;

  if (time == 0)
    time = shell_global_get_current_time (global);

  if (workspace != NULL && workspace != active)
    meta_workspace_activate_with_focus (workspace, window, time);
  else
    meta_window_activate (window, time);

  request_overview_hide (item_priv (SHELL_APP_VIEW_ITEM (self))->window_manager);

  panel = shell_main_get_panel (shell_main_get_default ());
  if (panel != NULL)
    shell_panel_close_calendar (panel);
}

static void
app_icon_menu_animate_launch_cb (ShellAppMenu *menu,
                                 ShellAppIcon *self)
{
  shell_app_icon_animate_launch (self);
}

static void
app_icon_menu_hide_requested_cb (ShellAppMenu *menu,
                                 ShellAppIcon *self)
{
  request_overview_hide (item_priv (SHELL_APP_VIEW_ITEM (self))->window_manager);
}

static void
app_icon_menu_window_activate_requested_cb (ShellAppMenu *menu,
                                            MetaWindow   *window,
                                            guint         time,
                                            ShellAppIcon *self)
{
  app_icon_activate_window (self, window, time);
}

static void
app_icon_menu_open_state_changed_cb (ShellPopupMenuBase *menu,
                                     gboolean            is_popped_up,
                                     ShellAppIcon       *self)
{
  if (!is_popped_up)
    {
      shell_app_view_item_set_forced_highlight (SHELL_APP_VIEW_ITEM (self), FALSE);
      g_signal_emit (self, app_icon_signals[APP_ICON_MENU_STATE_CHANGED], 0, FALSE);
    }
}

static void
app_icon_overview_closing_changed_cb (GObject      *wm,
                                      GParamSpec   *pspec,
                                      ShellAppIcon *self)
{
  ShellAppIconPrivate *priv = shell_app_icon_get_instance_private (self);

  /* The overview's `hiding` */
  if (priv->menu != NULL && shell_window_manager_get_overview_closing (SHELL_WINDOW_MANAGER (wm)))
    shell_popup_menu_base_close (priv->menu, SHELL_POPUP_ANIMATION_NONE);
}

/**
 * shell_app_icon_popup_menu:
 * @self: a #ShellAppIcon
 *
 * Opens the menu of the application (`popupMenu ()`).
 */
void
shell_app_icon_popup_menu (ShellAppIcon *self)
{
  ShellAppIconPrivate *priv;
  ShellAppViewItemPrivate *item;

  g_return_if_fail (SHELL_IS_APP_ICON (self));

  priv = shell_app_icon_get_instance_private (self);
  item = item_priv (SHELL_APP_VIEW_ITEM (self));

  shell_app_view_item_set_forced_highlight (SHELL_APP_VIEW_ITEM (self), TRUE);

  if (priv->menu == NULL)
    {
      ShellAppMenu *menu = shell_app_menu_new (CLUTTER_ACTOR (self),
                                               priv->popup_menu_side, TRUE, TRUE);
      ShellLayoutManager *layout_manager = get_layout_manager ();

      g_signal_connect_object (menu, "animate-launch",
                               G_CALLBACK (app_icon_menu_animate_launch_cb), self, 0);
      g_signal_connect_object (menu, "overview-hide-requested",
                               G_CALLBACK (app_icon_menu_hide_requested_cb), self, 0);
      g_signal_connect_object (menu, "window-activate-requested",
                               G_CALLBACK (app_icon_menu_window_activate_requested_cb), self, 0);
      shell_app_menu_set_app (menu, priv->app);
      g_signal_connect_object (menu, "open-state-changed",
                               G_CALLBACK (app_icon_menu_open_state_changed_cb), self, 0);

      if (item->window_manager != NULL)
        g_signal_connect_object (item->window_manager, "notify::overview-closing",
                                 G_CALLBACK (app_icon_overview_closing_changed_cb), self, 0);

      if (layout_manager != NULL)
        clutter_actor_add_child (CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager)),
                                 shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (menu)));
      shell_popup_menu_manager_add_menu (priv->menu_manager, G_OBJECT (menu));

      /* The actor of the menu owns it */
      g_set_weak_pointer (&priv->menu, SHELL_POPUP_MENU_BASE (menu));
      g_object_unref (menu);
    }

  g_signal_emit (self, app_icon_signals[APP_ICON_MENU_STATE_CHANGED], 0, TRUE);

  shell_popup_menu_base_open (priv->menu, SHELL_POPUP_ANIMATION_FULL);
  g_signal_emit (self, app_icon_signals[APP_ICON_SYNC_TOOLTIP], 0);
}

static void
app_icon_long_press_recognize_cb (ClutterGesture *gesture,
                                  ShellAppIcon   *self)
{
  shell_app_icon_popup_menu (self);
}

static void
app_icon_keyboard_popup_menu_cb (StWidget     *widget,
                                 ShellAppIcon *self)
{
  ShellAppIconPrivate *priv = shell_app_icon_get_instance_private (self);

  shell_app_icon_popup_menu (self);

  if (priv->menu != NULL)
    st_widget_navigate_focus (ST_WIDGET (shell_popup_menu_base_get_actor (priv->menu)),
                              NULL, ST_DIR_TAB_FORWARD, FALSE);
}

static ClutterActor *
app_icon_get_drag_actor (ShellDragSource *source)
{
  ShellAppIcon *self = SHELL_APP_ICON (source);
  ShellAppIconPrivate *priv = shell_app_icon_get_instance_private (self);

  return shell_app_create_icon_texture (priv->app,
                                        item_priv (SHELL_APP_VIEW_ITEM (self))->drag_icon_size);
}

/* The original actor that should align with the actor we show as the item
 * is being dragged */
static ClutterActor *
app_icon_get_drag_actor_source (ShellDragSource *source)
{
  ShellAppViewItemPrivate *item = item_priv (SHELL_APP_VIEW_ITEM (source));

  return item->icon != NULL ? shell_base_icon_get_icon (item->icon) : NULL;
}

/* Read by the workspaces, which accept the drop of an app to open a new
 * window of it */
static ShellApp *
app_icon_get_app (ShellDragSource *source)
{
  return shell_app_icon_get_app (SHELL_APP_ICON (source));
}

static void
app_icon_animate_launch_at_pos (ShellDragSource *source,
                                float            x,
                                float            y)
{
  shell_app_icon_animate_launch_at_pos (SHELL_APP_ICON (source), x, y);
}

static void
shell_app_icon_drag_source_iface_init (ShellDragSourceInterface *iface)
{
  iface->get_drag_actor = app_icon_get_drag_actor;
  iface->get_drag_actor_source = app_icon_get_drag_actor_source;
  iface->get_app = app_icon_get_app;
  iface->animate_launch_at_pos = app_icon_animate_launch_at_pos;
}

static void
app_icon_show_folder_preview (ShellAppIcon *self)
{
  ShellAppViewItemPrivate *item = item_priv (SHELL_APP_VIEW_ITEM (self));
  ShellEaseParams params = default_ease_params ();
  StLabel *label;
  ClutterActor *icon;

  if (item->icon == NULL)
    return;

  label = shell_base_icon_get_label (item->icon);
  if (label != NULL)
    clutter_actor_set_opacity (CLUTTER_ACTOR (label), 0);

  icon = shell_base_icon_get_icon (item->icon);
  if (icon != NULL)
    shell_actor_ease (icon, &params, NULL, NULL, NULL,
                      "scale-x", (double) FOLDER_SUBICON_FRACTION,
                      "scale-y", (double) FOLDER_SUBICON_FRACTION,
                      NULL);
}

static void
app_icon_hide_folder_preview (ShellAppIcon *self)
{
  ShellAppViewItemPrivate *item = item_priv (SHELL_APP_VIEW_ITEM (self));
  ShellEaseParams params = default_ease_params ();
  StLabel *label;
  ClutterActor *icon;

  if (item->icon == NULL)
    return;

  label = shell_base_icon_get_label (item->icon);
  if (label != NULL)
    clutter_actor_set_opacity (CLUTTER_ACTOR (label), 255);

  icon = shell_base_icon_get_icon (item->icon);
  if (icon != NULL)
    shell_actor_ease (icon, &params, NULL, NULL, NULL,
                      "scale-x", 1.0,
                      "scale-y", 1.0,
                      NULL);
}

static gboolean
app_icon_can_accept (ShellAppViewItem *item,
                     GObject          *source)
{
  ShellBaseAppView *view;

  if (!CLUTTER_IS_ACTOR (source))
    return FALSE;

  view = get_view_from_icon (CLUTTER_ACTOR (source));

  return source != G_OBJECT (item) &&
         G_TYPE_CHECK_INSTANCE_TYPE (source, G_OBJECT_TYPE (item)) &&
         SHELL_IS_APP_DISPLAY (view);
}

static void
app_icon_folder_preview_timeout_cb (gpointer user_data)
{
  ShellAppIcon *self = user_data;
  ShellAppIconPrivate *priv = shell_app_icon_get_instance_private (self);

  priv->folder_preview_id = 0;
  st_widget_add_style_pseudo_class (ST_WIDGET (self), "drop");
  app_icon_show_folder_preview (self);
}

static void
app_icon_set_hovering_by_dnd (ShellAppViewItem *item,
                              gboolean          hovering)
{
  ShellAppIcon *self = SHELL_APP_ICON (item);
  ShellAppIconPrivate *priv = shell_app_icon_get_instance_private (self);
  ShellAppViewItemClass *parent_class = SHELL_APP_VIEW_ITEM_CLASS (shell_app_icon_parent_class);

  if (item_priv (item)->other_icon_is_hovering == hovering)
    return;

  parent_class->set_hovering_by_dnd (item, hovering);

  if (hovering)
    {
      if (priv->folder_preview_id > 0)
        return;

      priv->folder_preview_id = g_timeout_add_once (500, app_icon_folder_preview_timeout_cb, self);
    }
  else
    {
      g_clear_handle_id (&priv->folder_preview_id, g_source_remove);
      app_icon_hide_folder_preview (self);
      st_widget_remove_style_pseudo_class (ST_WIDGET (self), "drop");
    }
}

static gboolean
app_icon_accept_drop (ShellAppViewItem *item,
                      GObject          *source,
                      ClutterActor     *actor,
                      float             x,
                      float             y,
                      uint32_t          time)
{
  ShellAppViewItemClass *parent_class = SHELL_APP_VIEW_ITEM_CLASS (shell_app_icon_parent_class);
  ShellBaseAppView *view;
  const char *apps[3];

  if (!parent_class->accept_drop (item, source, actor, x, y, time))
    return FALSE;

  view = get_view_from_icon (CLUTTER_ACTOR (item));
  if (!SHELL_IS_APP_DISPLAY (view))
    return FALSE;

  apps[0] = shell_app_view_item_get_id (item);
  apps[1] = shell_app_view_item_get_id (SHELL_APP_VIEW_ITEM (source));
  apps[2] = NULL;

  return shell_app_display_create_folder (SHELL_APP_DISPLAY (view), apps);
}

static void
app_icon_destroy (ClutterActor *actor)
{
  ShellAppIcon *self = SHELL_APP_ICON (actor);
  ShellAppIconPrivate *priv = shell_app_icon_get_instance_private (self);

  g_clear_handle_id (&priv->folder_preview_id, g_source_remove);

  if (priv->menu != NULL)
    {
      g_autoptr (ShellPopupMenuBase) menu = g_object_ref (priv->menu);

      g_clear_weak_pointer (&priv->menu);
      shell_popup_menu_base_destroy (menu);
    }

  CLUTTER_ACTOR_CLASS (shell_app_icon_parent_class)->destroy (actor);
}

static void
shell_app_icon_constructed (GObject *object)
{
  ShellAppIcon *self = SHELL_APP_ICON (object);
  ShellAppIconPrivate *priv = shell_app_icon_get_instance_private (self);
  ShellAppViewItem *item = SHELL_APP_VIEW_ITEM (self);
  ClutterActor *icon_container, *dot;
  ShellBaseIcon *icon;
  ClutterAction *action;

  G_OBJECT_CLASS (shell_app_icon_parent_class)->constructed (object);

  g_return_if_fail (SHELL_IS_APP (priv->app));

  st_widget_set_style_class_name (ST_WIDGET (self), "overview-tile");

  shell_app_view_item_set_id (item, shell_app_get_id (priv->app));
  shell_app_view_item_set_name (item, shell_app_get_name (priv->app));

  icon_container = g_object_new (ST_TYPE_WIDGET,
                                 "layout-manager", clutter_bin_layout_new (),
                                 "x-expand", TRUE,
                                 "y-expand", TRUE,
                                 NULL);
  g_set_weak_pointer (&priv->icon_container, icon_container);
  st_bin_set_child (ST_BIN (self), icon_container);

  icon = g_object_new (SHELL_TYPE_BASE_ICON,
                       "label-text", shell_app_get_name (priv->app),
                       "show-label", priv->show_label,
                       "set-size-manually", TRUE,
                       NULL);
  shell_base_icon_set_create_icon_func (icon, app_icon_create_icon_cb, self, NULL);
  shell_app_view_item_set_icon (item, icon);
  clutter_actor_add_child (icon_container, CLUTTER_ACTOR (icon));

  dot = g_object_new (ST_TYPE_WIDGET,
                      "style-class", "app-grid-running-dot",
                      "layout-manager", clutter_bin_layout_new (),
                      "x-expand", TRUE,
                      "y-expand", TRUE,
                      "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                      "y-align", CLUTTER_ACTOR_ALIGN_END,
                      NULL);
  g_set_weak_pointer (&priv->dot, dot);
  g_signal_connect (dot, "style-changed", G_CALLBACK (app_icon_dot_style_changed_cb), self);
  clutter_actor_add_child (icon_container, dot);

  if (shell_base_icon_get_label (icon) != NULL)
    st_widget_set_label_actor (ST_WIDGET (self),
                               CLUTTER_ACTOR (shell_base_icon_get_label (icon)));

  g_signal_connect (self, "popup-menu",
                    G_CALLBACK (app_icon_keyboard_popup_menu_cb), self);

  priv->menu_manager = shell_popup_menu_manager_new (SHELL_ACTION_MODE_POPUP);

  g_signal_connect_object (priv->app, "notify::state",
                           G_CALLBACK (app_icon_app_state_changed_cb), self, 0);
  app_icon_update_running_style (self);

  action = clutter_long_press_gesture_new ();
  g_signal_connect_object (action, "recognize",
                           G_CALLBACK (app_icon_long_press_recognize_cb), self, 0);
  clutter_actor_add_action (CLUTTER_ACTOR (self), action);

  action = clutter_click_gesture_new ();
  clutter_press_gesture_set_required_button (CLUTTER_PRESS_GESTURE (action),
                                             CLUTTER_BUTTON_SECONDARY);
  clutter_click_gesture_set_recognize_on_press (CLUTTER_CLICK_GESTURE (action), TRUE);
  g_signal_connect_object (action, "recognize",
                           G_CALLBACK (app_icon_long_press_recognize_cb), self, 0);
  clutter_actor_add_action (CLUTTER_ACTOR (self), action);
}

static void
shell_app_icon_dispose (GObject *object)
{
  ShellAppIcon *self = SHELL_APP_ICON (object);
  ShellAppIconPrivate *priv = shell_app_icon_get_instance_private (self);

  g_clear_handle_id (&priv->folder_preview_id, g_source_remove);

  /* dispose runs before the destroy vfunc, which would find priv->menu
   * already cleared and leave the menu alive without its icon */
  if (priv->menu != NULL)
    {
      g_autoptr (ShellPopupMenuBase) menu = g_object_ref (priv->menu);

      g_clear_weak_pointer (&priv->menu);
      shell_popup_menu_base_destroy (menu);
    }

  g_clear_weak_pointer (&priv->icon_container);
  g_clear_weak_pointer (&priv->dot);
  g_clear_object (&priv->menu_manager);

  G_OBJECT_CLASS (shell_app_icon_parent_class)->dispose (object);

  g_clear_object (&priv->app);
}

static void
shell_app_icon_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  ShellAppIcon *self = SHELL_APP_ICON (object);
  ShellAppIconPrivate *priv = shell_app_icon_get_instance_private (self);

  switch (prop_id)
    {
    case APP_ICON_PROP_APP:
      priv->app = g_value_dup_object (value);
      break;

    case APP_ICON_PROP_POPUP_MENU_SIDE:
      priv->popup_menu_side = g_value_get_enum (value);
      break;

    case APP_ICON_PROP_SHOW_LABEL:
      priv->show_label = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_app_icon_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  ShellAppIcon *self = SHELL_APP_ICON (object);
  ShellAppIconPrivate *priv = shell_app_icon_get_instance_private (self);

  switch (prop_id)
    {
    case APP_ICON_PROP_APP:
      g_value_set_object (value, priv->app);
      break;

    case APP_ICON_PROP_POPUP_MENU_SIDE:
      g_value_set_enum (value, priv->popup_menu_side);
      break;

    case APP_ICON_PROP_SHOW_LABEL:
      g_value_set_boolean (value, priv->show_label);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_app_icon_class_init (ShellAppIconClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);
  ShellAppViewItemClass *item_class = SHELL_APP_VIEW_ITEM_CLASS (klass);

  object_class->constructed = shell_app_icon_constructed;
  object_class->dispose = shell_app_icon_dispose;
  object_class->set_property = shell_app_icon_set_property;
  object_class->get_property = shell_app_icon_get_property;

  actor_class->destroy = app_icon_destroy;

  button_class->clicked = app_icon_clicked;

  item_class->can_accept = app_icon_can_accept;
  item_class->set_hovering_by_dnd = app_icon_set_hovering_by_dnd;
  item_class->accept_drop = app_icon_accept_drop;

  /**
   * ShellAppIcon:app:
   *
   * The application of the icon (`app`).
   */
  app_icon_props[APP_ICON_PROP_APP] =
    g_param_spec_object ("app", NULL, NULL,
                         SHELL_TYPE_APP,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellAppIcon:popup-menu-side:
   *
   * The side of the icon the menu opens at (`popupMenuSide`).
   */
  app_icon_props[APP_ICON_PROP_POPUP_MENU_SIDE] =
    g_param_spec_enum ("popup-menu-side", NULL, NULL,
                       ST_TYPE_SIDE, ST_SIDE_LEFT,
                       G_PARAM_READWRITE |
                       G_PARAM_CONSTRUCT_ONLY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellAppIcon:show-label:
   *
   * Whether the icon has a label (`showLabel`).
   */
  app_icon_props[APP_ICON_PROP_SHOW_LABEL] =
    g_param_spec_boolean ("show-label", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, APP_ICON_N_PROPS, app_icon_props);

  /**
   * ShellAppIcon::menu-state-changed:
   * @self: the #ShellAppIcon
   * @opened: whether the menu opened or closed
   *
   * Emitted when the menu is opened or closed.
   */
  app_icon_signals[APP_ICON_MENU_STATE_CHANGED] =
    g_signal_new ("menu-state-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN);

  /**
   * ShellAppIcon::sync-tooltip:
   * @self: the #ShellAppIcon
   *
   * Emitted when the label of the dash should be shown or hidden.
   */
  app_icon_signals[APP_ICON_SYNC_TOOLTIP] =
    g_signal_new ("sync-tooltip",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_app_icon_init (ShellAppIcon *self)
{
  ShellAppIconPrivate *priv = shell_app_icon_get_instance_private (self);

  priv->popup_menu_side = ST_SIDE_LEFT;
  priv->show_label = TRUE;
}

/**
 * shell_app_icon_new:
 * @app: the application
 * @window_manager: (nullable): the window manager the overview is reached
 *   through
 *
 * Creates the icon of an application which can be dragged, with a label
 * (`new AppIcon (app)`).
 *
 * Returns: (transfer full): a new #ShellAppIcon
 */
ShellAppIcon *
shell_app_icon_new (ShellApp           *app,
                    ShellWindowManager *window_manager)
{
  g_return_val_if_fail (SHELL_IS_APP (app), NULL);

  return g_object_new (SHELL_TYPE_APP_ICON,
                       "app", app,
                       "window-manager", window_manager,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellBaseAppViewDragTarget (private, BaseAppViewDragTarget)               */
/* ------------------------------------------------------------------------ */

/* The drop target of the grid and the scroll container of a view. The view
 * itself is not registered as a target, so that dropping on its page
 * indicator dots is not accepted */
#define SHELL_TYPE_BASE_APP_VIEW_DRAG_TARGET (shell_base_app_view_drag_target_get_type ())
G_DECLARE_FINAL_TYPE (ShellBaseAppViewDragTarget, shell_base_app_view_drag_target,
                      SHELL, BASE_APP_VIEW_DRAG_TARGET, GObject)

struct _ShellBaseAppViewDragTarget
{
  GObject parent;

  ShellBaseAppView *view; /* weak */
};

static void shell_base_app_view_drag_target_iface_init (ShellDragTargetInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellBaseAppViewDragTarget, shell_base_app_view_drag_target,
                               G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (SHELL_TYPE_DRAG_TARGET,
                                                      shell_base_app_view_drag_target_iface_init))

static ShellDragMotionResult base_app_view_handle_drag_over (ShellBaseAppView *self,
                                                             GObject          *source);
static gboolean              base_app_view_accept_drop      (ShellBaseAppView *self,
                                                             GObject          *source);

static ShellDragMotionResult
view_target_handle_drag_over (ShellDragTarget *target,
                              GObject         *source,
                              ClutterActor    *actor,
                              float            x,
                              float            y,
                              uint32_t         time)
{
  ShellBaseAppViewDragTarget *self = SHELL_BASE_APP_VIEW_DRAG_TARGET (target);

  if (self->view == NULL)
    return SHELL_DRAG_MOTION_RESULT_CONTINUE;

  return base_app_view_handle_drag_over (self->view, source);
}

static gboolean
view_target_accept_drop (ShellDragTarget *target,
                         GObject         *source,
                         ClutterActor    *actor,
                         float            x,
                         float            y,
                         uint32_t         time)
{
  ShellBaseAppViewDragTarget *self = SHELL_BASE_APP_VIEW_DRAG_TARGET (target);

  if (self->view == NULL)
    return FALSE;

  return base_app_view_accept_drop (self->view, source);
}

static void
shell_base_app_view_drag_target_iface_init (ShellDragTargetInterface *iface)
{
  iface->handle_drag_over = view_target_handle_drag_over;
  iface->accept_drop = view_target_accept_drop;
}

static void
shell_base_app_view_drag_target_dispose (GObject *object)
{
  ShellBaseAppViewDragTarget *self = SHELL_BASE_APP_VIEW_DRAG_TARGET (object);

  g_clear_weak_pointer (&self->view);

  G_OBJECT_CLASS (shell_base_app_view_drag_target_parent_class)->dispose (object);
}

static void
shell_base_app_view_drag_target_class_init (ShellBaseAppViewDragTargetClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = shell_base_app_view_drag_target_dispose;
}

static void
shell_base_app_view_drag_target_init (ShellBaseAppViewDragTarget *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellBaseAppView                                                          */
/* ------------------------------------------------------------------------ */

enum
{
  VIEW_PROP_0,
  VIEW_PROP_WINDOW_MANAGER,
  VIEW_PROP_DRAG_ICON_SIZE,
  VIEW_N_PROPS
};

static GParamSpec *view_props[VIEW_N_PROPS] = { NULL, };

enum
{
  VIEW_LOADED,
  VIEW_LAST_SIGNAL
};

static guint view_signals[VIEW_LAST_SIGNAL] = { 0, };

typedef struct
{
  int page;
  int position;
  ShellAppViewItem *source; /* no reference, removed with the "destroy" handler */
  gulong destroy_id;
  guint timeout_id;
} DelayedMoveData;

typedef struct
{
  ShellWindowManager *window_manager; /* weak */
  int drag_icon_size;

  ShellAppGrid *grid; /* weak */
  ClutterActor *scroll_view; /* weak */
  StAdjustment *adjustment; /* weak, the horizontal adjustment of the scroll view */
  ShellPageIndicators *page_indicators; /* weak */
  ClutterActor *next_page_indicator; /* weak */
  ClutterActor *prev_page_indicator; /* weak */
  ClutterActor *next_page_arrow; /* weak */
  ClutterActor *prev_page_arrow; /* weak */
  ShellAppViewGridLayout *app_grid_layout; /* weak */
  ClutterActor *box; /* weak */
  ShellSwipeTracker *swipe_tracker;
  ShellBaseAppViewDragTarget *drag_target;

  gboolean can_scroll;
  guint scroll_timeout_id;

  ClutterOrientation orientation;

  GHashTable *items; /* id -> ShellAppViewItem, with a reference */
  GPtrArray *ordered_items; /* ShellAppViewItem, with a reference */

  ShellParentalControlsManager *parental_controls_manager; /* not owned */
  ShellAppFavorites *app_favorites; /* not owned */

  int last_overshoot_coord;
  DelayedMoveData *delayed_move_data;

  gulong drag_begin_id;
  gulong drag_end_id;
  gulong drag_cancelled_id;
  guint drag_monitor;
  ClutterActor *drop_target; /* weak */

  guint drag_page_switch_initial_timeout_id;
  guint drag_page_switch_repeat_timeout_id;
} ShellBaseAppViewPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (ShellBaseAppView, shell_base_app_view, ST_TYPE_WIDGET)

static void base_app_view_move_item (ShellBaseAppView *self,
                                     ShellAppViewItem *item,
                                     int               new_page,
                                     int               new_position);
static void base_app_view_remove_delayed_move (ShellBaseAppView *self);
static void base_app_view_go_to_page (ShellBaseAppView *self,
                                      int               page,
                                      gboolean          animate);

static ShellBaseAppViewPrivate *
view_priv (ShellBaseAppView *self)
{
  return shell_base_app_view_get_instance_private (self);
}

/**
 * shell_base_app_view_get_grid:
 * @self: a #ShellBaseAppView
 *
 * Returns: (transfer none): the grid of the view (`_grid`)
 */
ShellIconGrid *
shell_base_app_view_get_grid (ShellBaseAppView *self)
{
  g_return_val_if_fail (SHELL_IS_BASE_APP_VIEW (self), NULL);

  return SHELL_ICON_GRID (view_priv (self)->grid);
}

/**
 * shell_base_app_view_get_scroll_view:
 * @self: a #ShellBaseAppView
 *
 * Returns: (transfer none): the scroll view of the grid (`_scrollView`)
 */
StScrollView *
shell_base_app_view_get_scroll_view (ShellBaseAppView *self)
{
  g_return_val_if_fail (SHELL_IS_BASE_APP_VIEW (self), NULL);

  return ST_SCROLL_VIEW (view_priv (self)->scroll_view);
}

/**
 * shell_base_app_view_get_window_manager:
 * @self: a #ShellBaseAppView
 *
 * Returns: (transfer none) (nullable): the window manager the overview is
 *   reached through
 */
ShellWindowManager *
shell_base_app_view_get_window_manager (ShellBaseAppView *self)
{
  g_return_val_if_fail (SHELL_IS_BASE_APP_VIEW (self), NULL);

  return view_priv (self)->window_manager;
}

/**
 * shell_base_app_view_get_drag_icon_size:
 * @self: a #ShellBaseAppView
 *
 * Returns: the size of the icon that is dragged, in logical pixels
 */
int
shell_base_app_view_get_drag_icon_size (ShellBaseAppView *self)
{
  g_return_val_if_fail (SHELL_IS_BASE_APP_VIEW (self), 0);

  return view_priv (self)->drag_icon_size;
}

/**
 * shell_base_app_view_set_drag_icon_size:
 * @self: a #ShellBaseAppView
 * @size: the size of the icon that is dragged, in logical pixels
 *
 * Sets the drag icon size of the view and all its icons, which is the icon
 * size of the dash.
 */
void
shell_base_app_view_set_drag_icon_size (ShellBaseAppView *self,
                                        int               size)
{
  ShellBaseAppViewPrivate *priv;
  guint i;

  g_return_if_fail (SHELL_IS_BASE_APP_VIEW (self));

  priv = view_priv (self);
  if (priv->drag_icon_size == size)
    return;

  priv->drag_icon_size = size;

  for (i = 0; i < priv->ordered_items->len; i++)
    {
      ShellAppViewItem *item = g_ptr_array_index (priv->ordered_items, i);

      shell_app_view_item_set_drag_icon_size (item, size);

      /* The icons in the dialog of a folder */
      if (SHELL_IS_FOLDER_ICON (item) && shell_folder_icon_get_view (SHELL_FOLDER_ICON (item)) != NULL)
        shell_base_app_view_set_drag_icon_size (SHELL_BASE_APP_VIEW (shell_folder_icon_get_view (SHELL_FOLDER_ICON (item))),
                                                size);
    }

  g_object_notify_by_pspec (G_OBJECT (self), view_props[VIEW_PROP_DRAG_ICON_SIZE]);
}

/**
 * shell_base_app_view_get_all_items:
 * @self: a #ShellBaseAppView
 *
 * Returns: (transfer container) (element-type ShellAppViewItem): the items
 *   of the view in the order they are shown (`getAllItems ()`)
 */
GPtrArray *
shell_base_app_view_get_all_items (ShellBaseAppView *self)
{
  ShellBaseAppViewPrivate *priv;
  GPtrArray *items = g_ptr_array_new ();
  guint i;

  g_return_val_if_fail (SHELL_IS_BASE_APP_VIEW (self), NULL);

  priv = view_priv (self);
  for (i = 0; i < priv->ordered_items->len; i++)
    g_ptr_array_add (items, g_ptr_array_index (priv->ordered_items, i));

  return items;
}

static ShellAppViewItem *
base_app_view_lookup_item (ShellBaseAppView *self,
                           const char       *id)
{
  return g_hash_table_lookup (view_priv (self)->items, id);
}

/* grid.get_items_at_page (page).filter (c => c.visible).length */
static int
count_visible_items_at_page (ShellIconGrid *grid,
                             int            page)
{
  GList *items = get_visible_items_at_page (grid, page);
  int n = (int) g_list_length (items);

  g_list_free (items);
  return n;
}

static int
base_app_view_get_linear_position (ShellBaseAppView *self,
                                   ShellAppViewItem *item)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  ShellIconGrid *grid = SHELL_ICON_GRID (priv->grid);
  int page, position;
  int item_index = 0;
  int i;

  shell_icon_grid_get_item_position (grid, CLUTTER_ACTOR (item), &page, &position);
  if (page == -1 || position == -1)
    {
      g_warning ("%s: Item not found in grid", G_STRFUNC);
      return -1;
    }

  if (shell_icon_grid_get_n_pages (grid) > 0)
    {
      for (i = 0; i < page; i++)
        item_index += count_visible_items_at_page (grid, i);

      item_index += position;
    }

  return item_index;
}

static void
insert_ordered_item (ShellBaseAppView *self,
                     ShellAppViewItem *item,
                     int               index)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);

  /* Array.splice () appended past the end */
  if (index < 0 || index > (int) priv->ordered_items->len)
    index = (int) priv->ordered_items->len;

  g_ptr_array_insert (priv->ordered_items, index, g_object_ref (item));
}

static void
base_app_view_add_item (ShellBaseAppView *self,
                        ShellAppViewItem *item,
                        int               page,
                        int               position)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  g_autoptr (GError) error = NULL;

  g_hash_table_replace (priv->items, g_strdup (shell_app_view_item_get_id (item)),
                        g_object_ref (item));

  if (!shell_icon_grid_add_item (SHELL_ICON_GRID (priv->grid), CLUTTER_ACTOR (item),
                                 page, position, &error))
    {
      g_autoptr (GError) append_error = NULL;

      /* The saved layout can name a page that does not exist; the JS
       * threw here and the item was never shown */
      g_debug ("Could not add %s to page %d: %s", shell_app_view_item_get_id (item),
               page, error->message);

      if (!shell_icon_grid_add_item (SHELL_ICON_GRID (priv->grid), CLUTTER_ACTOR (item),
                                     -1, -1, &append_error))
        {
          g_warning ("%s: %s", G_STRFUNC, append_error->message);
          g_hash_table_remove (priv->items, shell_app_view_item_get_id (item));
          return;
        }
    }

  insert_ordered_item (self, item, base_app_view_get_linear_position (self, item));
}

static void
base_app_view_remove_item (ShellBaseAppView *self,
                           ShellAppViewItem *item)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  g_autoptr (ShellAppViewItem) keep_alive = g_object_ref (item);
  g_autoptr (GError) error = NULL;

  g_ptr_array_remove (priv->ordered_items, item);
  g_hash_table_remove (priv->items, shell_app_view_item_get_id (item));

  if (!shell_icon_grid_remove_item (SHELL_ICON_GRID (priv->grid), CLUTTER_ACTOR (item), &error))
    g_debug ("%s: %s", G_STRFUNC, error->message);
}

static void
base_app_view_move_item (ShellBaseAppView *self,
                         ShellAppViewItem *item,
                         int               new_page,
                         int               new_position)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  g_autoptr (ShellAppViewItem) keep_alive = g_object_ref (item);
  g_autoptr (GError) error = NULL;

  if (!shell_icon_grid_move_item (SHELL_ICON_GRID (priv->grid), CLUTTER_ACTOR (item),
                                  new_page, new_position, &error))
    {
      g_warning ("%s: %s", G_STRFUNC, error->message);
      return;
    }

  /* Update the ordered items */
  g_ptr_array_remove (priv->ordered_items, item);
  insert_ordered_item (self, item, base_app_view_get_linear_position (self, item));
}

static void
view_real_get_item_position (ShellBaseAppView *self,
                        ShellAppViewItem *item,
                        int              *page,
                        int              *position)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  int items_per_page = shell_icon_grid_get_items_per_page (SHELL_ICON_GRID (priv->grid));
  guint index = 0;
  int icon_index;

  if (!g_ptr_array_find (priv->ordered_items, item, &index))
    icon_index = (int) priv->ordered_items->len - 1;
  else
    icon_index = (int) index;

  if (icon_index < 0)
    {
      *page = -1;
      *position = -1;
      return;
    }

  if (items_per_page <= 0)
    {
      *page = 0;
      *position = icon_index;
      return;
    }

  *page = icon_index / items_per_page;
  *position = icon_index % items_per_page;
}

static int
view_real_compare_items (ShellBaseAppView *self,
                    ShellAppViewItem *a,
                    ShellAppViewItem *b)
{
  return g_utf8_collate (shell_app_view_item_get_name (a) != NULL ? shell_app_view_item_get_name (a) : "",
                         shell_app_view_item_get_name (b) != NULL ? shell_app_view_item_get_name (b) : "");
}

static int
compare_items_cb (gconstpointer a,
                  gconstpointer b,
                  gpointer      user_data)
{
  ShellBaseAppView *self = user_data;
  ShellAppViewItem *item_a = *(ShellAppViewItem * const *) a;
  ShellAppViewItem *item_b = *(ShellAppViewItem * const *) b;

  return SHELL_BASE_APP_VIEW_GET_CLASS (self)->compare_items (self, item_a, item_b);
}

static GPtrArray *
view_real_load_apps (ShellBaseAppView *self)
{
  return g_ptr_array_new_with_free_func (g_object_unref);
}

static void
view_real_redisplay (ShellBaseAppView *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  ShellBaseAppViewClass *klass = SHELL_BASE_APP_VIEW_GET_CLASS (self);
  g_autoptr (GPtrArray) old_apps = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (GPtrArray) new_apps = NULL;
  g_autoptr (GHashTable) old_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (GHashTable) new_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (GHashTable) added_apps = g_hash_table_new (NULL, NULL);
  guint i;

  for (i = 0; i < priv->ordered_items->len; i++)
    {
      ShellAppViewItem *icon = g_ptr_array_index (priv->ordered_items, i);

      g_ptr_array_add (old_apps, g_object_ref (icon));
      g_hash_table_add (old_ids, g_strdup (shell_app_view_item_get_id (icon)));
    }

  new_apps = klass->load_apps (self);
  g_ptr_array_sort_with_data (new_apps, compare_items_cb, self);

  for (i = 0; i < new_apps->len; i++)
    {
      ShellAppViewItem *icon = g_ptr_array_index (new_apps, i);

      g_hash_table_add (new_ids, g_strdup (shell_app_view_item_get_id (icon)));
      if (!g_hash_table_contains (old_ids, shell_app_view_item_get_id (icon)))
        g_hash_table_add (added_apps, icon);
    }

  /* Remove old app icons */
  for (i = 0; i < old_apps->len; i++)
    {
      ShellAppViewItem *icon = g_ptr_array_index (old_apps, i);

      if (g_hash_table_contains (new_ids, shell_app_view_item_get_id (icon)))
        continue;

      base_app_view_remove_item (self, icon);
      clutter_actor_destroy (CLUTTER_ACTOR (icon));
    }

  /* Add new app icons, or move existing ones */
  for (i = 0; i < new_apps->len; i++)
    {
      ShellAppViewItem *icon = g_ptr_array_index (new_apps, i);
      int page, position;

      klass->get_item_position (self, icon, &page, &position);

      if (g_hash_table_contains (added_apps, icon))
        {
          /* If there's two pages, newly installed apps should not appear
           * on page 0 */
          if (page == -1 && position == -1 &&
              shell_icon_grid_get_n_pages (SHELL_ICON_GRID (priv->grid)) > 1)
            base_app_view_add_item (self, icon, 1, -1);
          else
            base_app_view_add_item (self, icon, page, position);
        }
      else if (page != -1 && position != -1)
        {
          base_app_view_move_item (self, icon, page, position);
        }
      else
        {
          /* App is part of a folder */
        }
    }

  g_signal_emit (self, view_signals[VIEW_LOADED], 0);
}

/**
 * shell_base_app_view_redisplay:
 * @self: a #ShellBaseAppView
 *
 * Loads the items again, adds the new ones, removes the ones which are gone
 * and moves the others to their positions (`_redisplay ()`).
 */
void
shell_base_app_view_redisplay (ShellBaseAppView *self)
{
  g_return_if_fail (SHELL_IS_BASE_APP_VIEW (self));

  SHELL_BASE_APP_VIEW_GET_CLASS (self)->redisplay (self);
}

static void
base_app_view_select_app_internal (ShellBaseAppView *self,
                                   const char       *id)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  ShellAppViewItem *item = g_hash_table_lookup (priv->items, id);

  if (item != NULL)
    st_widget_navigate_focus (ST_WIDGET (item), NULL, ST_DIR_TAB_FORWARD, FALSE);
  else
    g_debug ("No such app %s", id);
}

typedef struct
{
  GWeakRef view;
  char *id;
  gulong handler_id;
} SelectAppData;

static void
select_app_data_free (gpointer data,
                      GClosure *closure)
{
  SelectAppData *select = data;

  g_weak_ref_clear (&select->view);
  g_free (select->id);
  g_free (select);
}

static void
select_app_item_mapped_cb (GObject    *object,
                           GParamSpec *pspec,
                           gpointer    user_data)
{
  SelectAppData *data = user_data;
  g_autoptr (ShellBaseAppView) view = g_weak_ref_get (&data->view);
  g_autofree char *id = NULL;

  if (!clutter_actor_is_mapped (CLUTTER_ACTOR (object)))
    return;

  id = g_strdup (data->id);
  g_signal_handler_disconnect (object, data->handler_id);

  if (view != NULL)
    base_app_view_select_app_internal (view, id);
}

static void
select_app_view_loaded_cb (ShellBaseAppView *self,
                           gpointer          user_data)
{
  SelectAppData *data = user_data;
  g_autofree char *id = g_strdup (data->id);

  g_signal_handler_disconnect (self, data->handler_id);
  shell_base_app_view_select_app (self, id);
}

/**
 * shell_base_app_view_select_app:
 * @self: a #ShellBaseAppView
 * @id: the id of an application or folder
 *
 * Moves the keyboard focus to the item @id, once it exists and is shown
 * (`selectApp ()`).
 */
void
shell_base_app_view_select_app (ShellBaseAppView *self,
                                const char       *id)
{
  ShellBaseAppViewPrivate *priv;
  ShellAppViewItem *item;
  SelectAppData *data;

  g_return_if_fail (SHELL_IS_BASE_APP_VIEW (self));
  g_return_if_fail (id != NULL);

  priv = view_priv (self);
  item = g_hash_table_lookup (priv->items, id);

  if (item != NULL)
    {
      if (clutter_actor_is_mapped (CLUTTER_ACTOR (item)))
        {
          base_app_view_select_app_internal (self, id);
        }
      else
        {
          /* Need to wait until the view is mapped */
          data = g_new0 (SelectAppData, 1);
          g_weak_ref_init (&data->view, self);
          data->id = g_strdup (id);
          data->handler_id = g_signal_connect_data (item, "notify::mapped",
                                                    G_CALLBACK (select_app_item_mapped_cb),
                                                    data, select_app_data_free, 0);
        }
    }
  else
    {
      /* Need to wait until the view is built */
      data = g_new0 (SelectAppData, 1);
      g_weak_ref_init (&data->view, self);
      data->id = g_strdup (id);
      data->handler_id = g_signal_connect_data (self, "view-loaded",
                                                G_CALLBACK (select_app_view_loaded_cb),
                                                data, select_app_data_free, 0);
    }
}

static void
base_app_view_get_drop_target (ShellBaseAppView          *self,
                               double                     x,
                               double                     y,
                               ShellAppViewItem          *source,
                               int                       *target_page,
                               int                       *target_position,
                               ShellIconGridDragLocation *drag_location)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  ShellIconGrid *grid = SHELL_ICON_GRID (priv->grid);
  gboolean allow_incomplete_pages;
  int source_page, source_position;
  int reflow_direction = CLUTTER_ACTOR_ALIGN_END;
  int columns_per_page;

  shell_icon_grid_get_item_position (grid, CLUTTER_ACTOR (source), &source_page, &source_position);
  shell_icon_grid_get_drop_target (grid, x, y, target_page, target_position, drag_location);

  g_object_get (grid,
                "allow-incomplete-pages", &allow_incomplete_pages,
                "columns-per-page", &columns_per_page,
                NULL);

  if (source_position == *target_position)
    reflow_direction = -1;

  if (source_page == *target_page && source_position < *target_position)
    reflow_direction = CLUTTER_ACTOR_ALIGN_START;
  if (!allow_incomplete_pages && source_page < *target_page)
    reflow_direction = CLUTTER_ACTOR_ALIGN_START;

  /* In case we're hovering over the edge of an item but the
   * reflow will happen in the opposite direction (the drag
   * can't "naturally push the item away"), we instead set the
   * drop target to the adjacent item that can be pushed away
   * in the reflow-direction.
   *
   * We must avoid doing that if we're hovering over the first
   * or last column though, in that case there is no adjacent
   * icon we could push away. */
  if (*drag_location == SHELL_ICON_GRID_DRAG_LOCATION_START_EDGE &&
      reflow_direction == CLUTTER_ACTOR_ALIGN_START)
    {
      int target_column = *target_position % columns_per_page;

      if (target_column > 0)
        {
          *target_position -= 1;
          *drag_location = SHELL_ICON_GRID_DRAG_LOCATION_END_EDGE;
        }
    }
  else if (*drag_location == SHELL_ICON_GRID_DRAG_LOCATION_END_EDGE &&
           reflow_direction == CLUTTER_ACTOR_ALIGN_END)
    {
      int target_column = *target_position % columns_per_page;

      if (target_column < columns_per_page - 1)
        {
          *target_position += 1;
          *drag_location = SHELL_ICON_GRID_DRAG_LOCATION_START_EDGE;
        }
    }
}

static void
base_app_view_remove_delayed_move (ShellBaseAppView *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  DelayedMoveData *data = priv->delayed_move_data;

  if (data == NULL)
    return;

  priv->delayed_move_data = NULL;

  g_clear_handle_id (&data->timeout_id, g_source_remove);

  if (data->destroy_id > 0)
    g_signal_handler_disconnect (data->source, data->destroy_id);

  g_free (data);
}

static void
delayed_move_source_destroyed_cb (ClutterActor     *source,
                                  ShellBaseAppView *self)
{
  /* The handler is gone with the source */
  if (view_priv (self)->delayed_move_data != NULL)
    view_priv (self)->delayed_move_data->destroy_id = 0;

  base_app_view_remove_delayed_move (self);
}

static void
delayed_move_timeout_cb (gpointer user_data)
{
  ShellBaseAppView *self = user_data;
  ShellBaseAppViewPrivate *priv = view_priv (self);
  DelayedMoveData *data = priv->delayed_move_data;
  ShellAppViewItem *source;
  int page, position;

  if (data == NULL)
    return;

  data->timeout_id = 0;
  source = data->source;
  page = data->page;
  position = data->position;

  base_app_view_move_item (self, source, page, position);
  base_app_view_remove_delayed_move (self);
}

static void
base_app_view_maybe_move_item_for (ShellBaseAppView *self,
                                   ShellDragEvent   *event,
                                   ShellAppViewItem *source)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  ShellIconGrid *grid = SHELL_ICON_GRID (priv->grid);
  ClutterActor *item = NULL;
  ShellIconGridDragLocation drag_location;
  int page, position;
  float x, y;

  if (!clutter_actor_transform_stage_point (CLUTTER_ACTOR (priv->grid),
                                            shell_drag_event_get_x (event),
                                            shell_drag_event_get_y (event),
                                            &x, &y))
    return;

  base_app_view_get_drop_target (self, x, y, source, &page, &position, &drag_location);
  if (position != -1)
    item = shell_icon_grid_get_item_at (grid, page, position);

  /* Dragging over invalid parts of the grid cancels the timeout */
  if (item == CLUTTER_ACTOR (source) ||
      st_adjustment_get_transition (priv->adjustment, "value") != NULL ||
      page != shell_icon_grid_get_current_page (grid) ||
      drag_location == SHELL_ICON_GRID_DRAG_LOCATION_INVALID ||
      drag_location == SHELL_ICON_GRID_DRAG_LOCATION_ON_ICON)
    {
      base_app_view_remove_delayed_move (self);
      return;
    }

  if (priv->delayed_move_data == NULL ||
      priv->delayed_move_data->page != page ||
      priv->delayed_move_data->position != position)
    {
      DelayedMoveData *data;

      /* Update the item with a small delay */
      base_app_view_remove_delayed_move (self);

      data = g_new0 (DelayedMoveData, 1);
      data->page = page;
      data->position = position;
      data->source = source;
      data->destroy_id = g_signal_connect (source, "destroy",
                                           G_CALLBACK (delayed_move_source_destroyed_cb), self);
      data->timeout_id = g_timeout_add_once (DELAYED_MOVE_TIMEOUT,
                                             delayed_move_timeout_cb, self);
      priv->delayed_move_data = data;
    }
}

static void
view_real_maybe_move_item (ShellBaseAppView *self,
                      ShellDragEvent   *event)
{
  GObject *source = shell_drag_event_get_source (event);

  if (SHELL_IS_APP_VIEW_ITEM (source))
    base_app_view_maybe_move_item_for (self, event, SHELL_APP_VIEW_ITEM (source));
}

static void
base_app_view_reset_drag_page_switch (ShellBaseAppView *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);

  g_clear_handle_id (&priv->drag_page_switch_initial_timeout_id, g_source_remove);
  g_clear_handle_id (&priv->drag_page_switch_repeat_timeout_id, g_source_remove);

  priv->last_overshoot_coord = -1;
}

static gboolean
drag_page_switch_repeat_cb (gpointer user_data)
{
  ShellBaseAppView *self = user_data;
  ShellBaseAppViewPrivate *priv = view_priv (self);
  /* The direction is kept in the data of the source */
  int direction = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (self), "shell-drag-page-switch-direction"));

  base_app_view_go_to_page (self,
                            shell_icon_grid_get_current_page (SHELL_ICON_GRID (priv->grid)) + direction,
                            TRUE);

  return G_SOURCE_CONTINUE;
}

static void
base_app_view_setup_drag_page_switch_repeat (ShellBaseAppView *self,
                                             int               direction)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);

  base_app_view_reset_drag_page_switch (self);

  g_object_set_data (G_OBJECT (self), "shell-drag-page-switch-direction",
                     GINT_TO_POINTER (direction));
  priv->drag_page_switch_repeat_timeout_id =
    g_timeout_add (DRAG_PAGE_SWITCH_REPEAT_TIMEOUT, drag_page_switch_repeat_cb, self);
  g_source_set_name_by_id (priv->drag_page_switch_repeat_timeout_id,
                           "[gnome-shell] this._dragPageSwitchRepeatTimeoutId");
}

static gboolean
base_app_view_drag_maybe_switch_page_immediately (ShellBaseAppView *self,
                                                  ShellDragEvent   *event)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  gboolean vertical = priv->orientation == CLUTTER_ORIENTATION_VERTICAL;
  float grid_x, grid_y, grid_width, grid_height;
  float grid_start, grid_end, drag_coord;
  int direction;

  /* When there's an adjacent monitor, this gesture conflicts with
   * dragging to the adjacent monitor, so disable in multi-monitor setups */
  if (get_n_monitors () > 1)
    return FALSE;

  /* Already animating */
  if (st_adjustment_get_transition (priv->adjustment, "value") != NULL)
    return FALSE;

  clutter_actor_get_transformed_position (CLUTTER_ACTOR (self), &grid_x, &grid_y);
  clutter_actor_get_transformed_size (CLUTTER_ACTOR (self), &grid_width, &grid_height);

  grid_start = vertical
    ? grid_y + DRAG_PAGE_SWITCH_IMMEDIATELY_THRESHOLD_PX
    : grid_x + DRAG_PAGE_SWITCH_IMMEDIATELY_THRESHOLD_PX;
  grid_end = vertical
    ? grid_y + grid_height - DRAG_PAGE_SWITCH_IMMEDIATELY_THRESHOLD_PX
    : grid_x + grid_width - DRAG_PAGE_SWITCH_IMMEDIATELY_THRESHOLD_PX;

  drag_coord = vertical ? shell_drag_event_get_y (event) : shell_drag_event_get_x (event);
  if (drag_coord > grid_start && drag_coord < grid_end)
    {
      float move_delta = fabsf ((float) priv->last_overshoot_coord - drag_coord);

      /* We moved back out of the overshoot region into the grid. If the
       * move was sufficiently large, reset the overshoot so that it's
       * possible to repeatedly bump against the edge and quickly switch
       * pages. */
      if (priv->last_overshoot_coord >= 0 &&
          move_delta > DRAG_PAGE_SWITCH_IMMEDIATELY_THRESHOLD_PX)
        base_app_view_reset_drag_page_switch (self);

      return FALSE;
    }

  /* Still in the area of the previous overshoot */
  if (priv->last_overshoot_coord >= 0)
    return FALSE;

  direction = drag_coord <= grid_start ? -1 : 1;
  if (text_direction_of (CLUTTER_ACTOR (self)) == CLUTTER_TEXT_DIRECTION_RTL)
    direction *= -1;

  base_app_view_go_to_page (self,
                            shell_icon_grid_get_current_page (SHELL_ICON_GRID (priv->grid)) + direction,
                            TRUE);
  base_app_view_setup_drag_page_switch_repeat (self, direction);

  /* The JS compared the float coordinate, which can be fractional; the
   * only use is the >= 0 test and the distance of the next move */
  priv->last_overshoot_coord = (int) drag_coord;

  return TRUE;
}

static void
drag_page_switch_initial_cb (gpointer user_data)
{
  ShellBaseAppView *self = user_data;
  ShellBaseAppViewPrivate *priv = view_priv (self);
  int direction = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (self), "shell-drag-page-switch-direction"));

  priv->drag_page_switch_initial_timeout_id = 0;

  base_app_view_go_to_page (self,
                            shell_icon_grid_get_current_page (SHELL_ICON_GRID (priv->grid)) + direction,
                            TRUE);
  base_app_view_setup_drag_page_switch_repeat (self, direction);
}

static void
base_app_view_maybe_setup_drag_page_switch_initial_timeout (ShellBaseAppView *self,
                                                            ShellDragEvent   *event)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  ClutterActor *target_actor = shell_drag_event_get_target_actor (event);

  if (priv->drag_page_switch_initial_timeout_id != 0 ||
      priv->drag_page_switch_repeat_timeout_id != 0)
    return;

  g_object_set_data (G_OBJECT (self), "shell-drag-page-switch-direction",
                     GINT_TO_POINTER (target_actor == priv->prev_page_indicator ? -1 : 1));
  priv->drag_page_switch_initial_timeout_id =
    g_timeout_add_once (DRAG_PAGE_SWITCH_INITIAL_TIMEOUT, drag_page_switch_initial_cb, self);
}

static ShellDragMotionResult
base_app_view_drag_motion_cb (ShellDragEvent *event,
                              gpointer        user_data)
{
  ShellBaseAppView *self = user_data;

  return SHELL_BASE_APP_VIEW_GET_CLASS (self)->drag_motion (self, event);
}

static ShellDragDropResult
base_app_view_drag_drop_cb (ShellDropEvent *event,
                            gpointer        user_data)
{
  ShellBaseAppView *self = user_data;
  ShellBaseAppViewPrivate *priv = view_priv (self);

  /* Because acceptDrop () does not receive the target actor, store it
   * here and use this value in the accept_drop () implementation below. */
  g_clear_weak_pointer (&priv->drop_target);
  g_set_weak_pointer (&priv->drop_target, shell_drop_event_get_target_actor (event));

  return SHELL_DRAG_DROP_RESULT_CONTINUE;
}

static void
view_real_drag_begin (ShellBaseAppView *self,
                 GObject          *source)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);

  priv->drag_monitor = shell_dnd_add_drag_monitor (base_app_view_drag_motion_cb, self, NULL,
                                                   base_app_view_drag_drop_cb, self, NULL);
  layout_show_page_indicators (priv->app_grid_layout);
}

static ShellDragMotionResult
view_real_drag_motion (ShellBaseAppView *self,
                  ShellDragEvent   *event)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  ClutterActor *target_actor;

  if (!SHELL_IS_APP_VIEW_ITEM (shell_drag_event_get_source (event)))
    return SHELL_DRAG_MOTION_RESULT_CONTINUE;

  /* Two ways of switching pages during DND:
   * 1) When "bumping" the cursor against the monitor edge, we switch
   *    page immediately.
   * 2) When hovering over the next-page indicator for a certain time,
   *    we also switch page. */
  if (!base_app_view_drag_maybe_switch_page_immediately (self, event))
    {
      target_actor = shell_drag_event_get_target_actor (event);

      if (target_actor == priv->prev_page_indicator ||
          target_actor == priv->next_page_indicator)
        base_app_view_maybe_setup_drag_page_switch_initial_timeout (self, event);
      else
        base_app_view_reset_drag_page_switch (self);
    }

  SHELL_BASE_APP_VIEW_GET_CLASS (self)->maybe_move_item (self, event);

  return SHELL_DRAG_MOTION_RESULT_CONTINUE;
}

static void
view_real_drag_end (ShellBaseAppView *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);

  if (priv->drag_monitor != 0)
    {
      shell_dnd_remove_drag_monitor (priv->drag_monitor);
      priv->drag_monitor = 0;
    }

  base_app_view_reset_drag_page_switch (self);

  layout_hide_page_indicators (priv->app_grid_layout);
}

static void
view_real_drag_cancelled (ShellBaseAppView *self,
                     GObject          *source)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);

  /* At this point, the positions aren't stored yet, thus redisplay ()
   * will move all items to their original positions */
  shell_base_app_view_redisplay (self);
  layout_hide_page_indicators (priv->app_grid_layout);
}

static gboolean
view_real_can_accept (ShellBaseAppView *self,
                 GObject          *source)
{
  return SHELL_IS_APP_VIEW_ITEM (source);
}

static gboolean
view_real_can_scroll (ShellBaseAppView *self)
{
  return TRUE;
}

static ShellDragMotionResult
base_app_view_handle_drag_over (ShellBaseAppView *self,
                                GObject          *source)
{
  if (!SHELL_BASE_APP_VIEW_GET_CLASS (self)->can_accept (self, source))
    return SHELL_DRAG_MOTION_RESULT_NO_DROP;

  return SHELL_DRAG_MOTION_RESULT_MOVE_DROP;
}

static gboolean
view_real_accept_drop (ShellBaseAppView *self,
                  GObject          *source)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  ShellIconGrid *grid = SHELL_ICON_GRID (priv->grid);
  g_autoptr (ClutterActor) drop_target = NULL;

  /* The target of the drop event, see the drop callback of the monitor */
  if (priv->drop_target != NULL)
    drop_target = g_object_ref (priv->drop_target);
  g_clear_weak_pointer (&priv->drop_target);

  if (!SHELL_BASE_APP_VIEW_GET_CLASS (self)->can_accept (self, source))
    return FALSE;

  if (drop_target != NULL &&
      (drop_target == priv->prev_page_indicator ||
       drop_target == priv->next_page_indicator))
    {
      int increment = drop_target == priv->prev_page_indicator ? -1 : 1;
      int current_page = shell_icon_grid_get_current_page (grid);
      int n_pages = shell_icon_grid_get_n_pages (grid);
      int page = MIN (current_page + increment, n_pages);
      int position = page < n_pages ? -1 : 0;
      int view_real_page;

      base_app_view_move_item (self, SHELL_APP_VIEW_ITEM (source), page, position);
      /* could be the next page, if the requested one was full */
      view_real_page = shell_icon_grid_get_item_page (grid, CLUTTER_ACTOR (source));
      base_app_view_go_to_page (self, view_real_page, TRUE);
    }
  else if (priv->delayed_move_data != NULL)
    {
      /* Dropped before the icon was moved */
      int page = priv->delayed_move_data->page;
      int position = priv->delayed_move_data->position;

      base_app_view_move_item (self, SHELL_APP_VIEW_ITEM (source), page, position);
      base_app_view_remove_delayed_move (self);
    }

  return TRUE;
}

static gboolean
base_app_view_accept_drop (ShellBaseAppView *self,
                           GObject          *source)
{
  return SHELL_BASE_APP_VIEW_GET_CLASS (self)->accept_drop (self, source);
}

static void
view_real_go_to_page (ShellBaseAppView *self,
                 int               page_number,
                 gboolean          animate)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  ShellIconGrid *grid = SHELL_ICON_GRID (priv->grid);
  g_autoptr (GError) error = NULL;

  page_number = CLAMP (page_number, 0, MAX (shell_icon_grid_get_n_pages (grid) - 1, 0));

  if (shell_icon_grid_get_current_page (grid) == page_number)
    return;

  layout_go_to_page (priv->app_grid_layout, page_number, animate);
  if (!shell_icon_grid_go_to_page (grid, page_number, animate, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

static void
base_app_view_go_to_page (ShellBaseAppView *self,
                          int               page,
                          gboolean          animate)
{
  SHELL_BASE_APP_VIEW_GET_CLASS (self)->go_to_page (self, page, animate);
}

/**
 * shell_base_app_view_go_to_page:
 * @self: a #ShellBaseAppView
 * @page: the page, which is clamped to the pages that exist
 * @animate: whether to animate the change
 *
 * Shows a page of the view (`goToPage ()`).
 */
void
shell_base_app_view_go_to_page (ShellBaseAppView *self,
                                int               page,
                                gboolean          animate)
{
  g_return_if_fail (SHELL_IS_BASE_APP_VIEW (self));

  base_app_view_go_to_page (self, page, animate);
}

/**
 * shell_base_app_view_drag_begin:
 * @self: a #ShellBaseAppView
 * @source: the item which is dragged
 *
 * Tells the view that the drag of an item started. The view does this itself
 * while it is mapped, from the #ShellWindowManager::item-drag-begin signal of
 * its window manager.
 */
void
shell_base_app_view_drag_begin (ShellBaseAppView *self,
                                GObject          *source)
{
  g_return_if_fail (SHELL_IS_BASE_APP_VIEW (self));

  SHELL_BASE_APP_VIEW_GET_CLASS (self)->drag_begin (self, source);
}

/**
 * shell_base_app_view_drag_end:
 * @self: a #ShellBaseAppView
 *
 * Tells the view that the drag of an item ended.
 */
void
shell_base_app_view_drag_end (ShellBaseAppView *self)
{
  g_return_if_fail (SHELL_IS_BASE_APP_VIEW (self));

  SHELL_BASE_APP_VIEW_GET_CLASS (self)->drag_end (self);
}

/**
 * shell_base_app_view_drag_cancelled:
 * @self: a #ShellBaseAppView
 * @source: the item which was dragged
 *
 * Tells the view that the drag of an item was cancelled.
 */
void
shell_base_app_view_drag_cancelled (ShellBaseAppView *self,
                                    GObject          *source)
{
  g_return_if_fail (SHELL_IS_BASE_APP_VIEW (self));

  SHELL_BASE_APP_VIEW_GET_CLASS (self)->drag_cancelled (self, source);
}

static void
view_item_drag_begin_cb (ShellWindowManager *wm,
                         GObject            *source,
                         ShellBaseAppView   *self)
{
  SHELL_BASE_APP_VIEW_GET_CLASS (self)->drag_begin (self, source);
}

static void
view_item_drag_end_cb (ShellWindowManager *wm,
                       GObject            *source,
                       ShellBaseAppView   *self)
{
  SHELL_BASE_APP_VIEW_GET_CLASS (self)->drag_end (self);
}

static void
view_item_drag_cancelled_cb (ShellWindowManager *wm,
                             GObject            *source,
                             ShellBaseAppView   *self)
{
  SHELL_BASE_APP_VIEW_GET_CLASS (self)->drag_cancelled (self, source);
}

static void
base_app_view_connect_dnd (ShellBaseAppView *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);

  if (priv->window_manager == NULL)
    return;

  if (priv->drag_begin_id == 0)
    priv->drag_begin_id = g_signal_connect (priv->window_manager, "item-drag-begin",
                                            G_CALLBACK (view_item_drag_begin_cb), self);
  if (priv->drag_end_id == 0)
    priv->drag_end_id = g_signal_connect (priv->window_manager, "item-drag-end",
                                          G_CALLBACK (view_item_drag_end_cb), self);
  if (priv->drag_cancelled_id == 0)
    priv->drag_cancelled_id = g_signal_connect (priv->window_manager, "item-drag-cancelled",
                                                G_CALLBACK (view_item_drag_cancelled_cb), self);
}

static void
base_app_view_disconnect_dnd (ShellBaseAppView *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);

  if (priv->window_manager != NULL)
    {
      g_clear_signal_handler (&priv->drag_begin_id, priv->window_manager);
      g_clear_signal_handler (&priv->drag_end_id, priv->window_manager);
      g_clear_signal_handler (&priv->drag_cancelled_id, priv->window_manager);
    }
  else
    {
      priv->drag_begin_id = 0;
      priv->drag_end_id = 0;
      priv->drag_cancelled_id = 0;
    }

  if (priv->drag_monitor != 0)
    {
      shell_dnd_remove_drag_monitor (priv->drag_monitor);
      priv->drag_monitor = 0;
    }
}

static void
base_app_view_scroll_timeout_cb (gpointer user_data)
{
  ShellBaseAppView *self = user_data;
  ShellBaseAppViewPrivate *priv = view_priv (self);

  priv->can_scroll = TRUE;
  priv->scroll_timeout_id = 0;
}

static void
base_app_view_scroll_cb (ClutterScrollController *controller,
                         ClutterSprite           *sprite,
                         ClutterScrollSource      source,
                         double                   dx,
                         double                   dy,
                         ShellBaseAppView        *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  gboolean rtl = text_direction_of (CLUTTER_ACTOR (self)) == CLUTTER_TEXT_DIRECTION_RTL;
  gboolean vertical = priv->orientation == CLUTTER_ORIENTATION_VERTICAL;
  int next_page;

  if (!SHELL_BASE_APP_VIEW_GET_CLASS (self)->can_scroll (self))
    return;

  if (!priv->can_scroll)
    return;

  next_page = shell_icon_grid_get_current_page (SHELL_ICON_GRID (priv->grid));
  if (dy < 0)
    {
      next_page -= 1;
    }
  else if (dy > 0)
    {
      next_page += 1;
    }
  else if (dx < 0)
    {
      if (vertical)
        return;
      next_page += rtl ? 1 : -1;
    }
  else if (dx > 0)
    {
      if (vertical)
        return;
      next_page += rtl ? -1 : 1;
    }
  else
    {
      return;
    }

  base_app_view_go_to_page (self, next_page, TRUE);

  priv->can_scroll = FALSE;
  priv->scroll_timeout_id = g_timeout_add_once (SCROLL_TIMEOUT_TIME,
                                                base_app_view_scroll_timeout_cb, self);
}

static void
base_app_view_swipe_begin_cb (ShellSwipeTracker *tracker,
                              guint              monitor,
                              ShellBaseAppView  *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  g_autofree double *points = NULL;
  ClutterActorBox box;
  double progress;
  double size;
  int n_pages = shell_icon_grid_get_n_pages (SHELL_ICON_GRID (priv->grid));
  int i;

  if ((int) monitor != get_primary_index ())
    return;

  st_adjustment_remove_transition (priv->adjustment, "value");

  progress = st_adjustment_get_value (priv->adjustment) /
             st_adjustment_get_page_size (priv->adjustment);
  points = g_new0 (double, MAX (n_pages, 1));
  for (i = 0; i < n_pages; i++)
    points[i] = i;

  clutter_actor_get_allocation_box (CLUTTER_ACTOR (priv->grid), &box);
  size = shell_swipe_tracker_get_orientation (tracker) == CLUTTER_ORIENTATION_VERTICAL
    ? clutter_actor_box_get_height (&box) : clutter_actor_box_get_width (&box);

  shell_swipe_tracker_confirm_swipe (tracker, size, points, n_pages, progress, round (progress));
}

static void
base_app_view_swipe_update_cb (ShellSwipeTracker *tracker,
                               double             progress,
                               ShellBaseAppView  *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);

  st_adjustment_set_value (priv->adjustment,
                           progress * st_adjustment_get_page_size (priv->adjustment));
}

static void
swipe_end_done_cb (gboolean finished,
                   gpointer user_data)
{
  GWeakRef *ref = user_data;
  g_autoptr (ShellBaseAppView) self = g_weak_ref_get (ref);
  int page;

  if (!finished || self == NULL)
    return;

  page = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (self), "shell-swipe-end-page"));
  base_app_view_go_to_page (self, page, FALSE);
}

static void
base_app_view_swipe_end_cb (ShellSwipeTracker *tracker,
                            guint64            duration,
                            double             end_progress,
                            ShellBaseAppView  *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  double value = end_progress * st_adjustment_get_page_size (priv->adjustment);
  GWeakRef *ref = g_new0 (GWeakRef, 1);

  g_weak_ref_init (ref, self);
  g_object_set_data (G_OBJECT (self), "shell-swipe-end-page",
                     GINT_TO_POINTER ((int) end_progress));

  ease_adjustment (priv->adjustment, value, (int) duration, CLUTTER_EASE_OUT_CUBIC,
                   swipe_end_done_cb, ref, weak_ref_free);
}

static void
base_app_view_grid_pages_changed_cb (ShellIconGrid    *grid,
                                     ShellBaseAppView *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);

  if (priv->page_indicators == NULL)
    return;

  base_app_view_go_to_page (self, shell_icon_grid_get_current_page (grid), TRUE);
  shell_page_indicators_set_n_pages (priv->page_indicators, shell_icon_grid_get_n_pages (grid));
  shell_page_indicators_set_current_position (priv->page_indicators,
                                              shell_icon_grid_get_current_page (grid));
}

static void
base_app_view_adjustment_value_changed_cb (StAdjustment     *adjustment,
                                           GParamSpec       *pspec,
                                           ShellBaseAppView *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);
  double value = st_adjustment_get_value (adjustment) / st_adjustment_get_page_size (adjustment);

  shell_page_indicators_set_current_position (priv->page_indicators, value);
}

static void
base_app_view_page_activated_cb (ShellPageIndicators *indicators,
                                 int                  page_index,
                                 ShellBaseAppView    *self)
{
  base_app_view_go_to_page (self, page_index, TRUE);
}

static void
base_app_view_next_arrow_clicked_cb (StButton         *button,
                                     int               clicked_button,
                                     ShellBaseAppView *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);

  base_app_view_go_to_page (self,
                            shell_icon_grid_get_current_page (SHELL_ICON_GRID (priv->grid)) + 1,
                            TRUE);
}

static void
base_app_view_prev_arrow_clicked_cb (StButton         *button,
                                     int               clicked_button,
                                     ShellBaseAppView *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);

  base_app_view_go_to_page (self,
                            shell_icon_grid_get_current_page (SHELL_ICON_GRID (priv->grid)) - 1,
                            TRUE);
}

static void
base_app_view_app_filter_changed_cb (ShellParentalControlsManager *manager,
                                     ShellBaseAppView             *self)
{
  shell_base_app_view_redisplay (self);
}

static void
base_app_view_favorites_changed_cb (ShellAppFavorites *favorites,
                                    ShellBaseAppView  *self)
{
  shell_base_app_view_redisplay (self);
}

static ShellIconGrid *
view_real_create_grid (ShellBaseAppView *self)
{
  return g_object_new (SHELL_TYPE_APP_GRID,
                       "allow-incomplete-pages", TRUE,
                       NULL);
}

static void
shell_base_app_view_map (ClutterActor *actor)
{
  ShellBaseAppView *self = SHELL_BASE_APP_VIEW (actor);
  ShellBaseAppViewPrivate *priv = view_priv (self);

  shell_swipe_tracker_set_enabled (priv->swipe_tracker, TRUE);
  base_app_view_connect_dnd (self);

  CLUTTER_ACTOR_CLASS (shell_base_app_view_parent_class)->map (actor);
}

static void
shell_base_app_view_unmap (ClutterActor *actor)
{
  ShellBaseAppView *self = SHELL_BASE_APP_VIEW (actor);
  ShellBaseAppViewPrivate *priv = view_priv (self);

  if (priv->swipe_tracker != NULL)
    shell_swipe_tracker_set_enabled (priv->swipe_tracker, FALSE);
  base_app_view_disconnect_dnd (self);

  CLUTTER_ACTOR_CLASS (shell_base_app_view_parent_class)->unmap (actor);
}

static void
shell_base_app_view_destroy (ClutterActor *actor)
{
  ShellBaseAppView *self = SHELL_BASE_APP_VIEW (actor);
  ShellBaseAppViewPrivate *priv = view_priv (self);

  if (priv->swipe_tracker != NULL)
    {
      shell_swipe_tracker_destroy (priv->swipe_tracker);
      g_clear_object (&priv->swipe_tracker);
    }

  base_app_view_remove_delayed_move (self);
  base_app_view_disconnect_dnd (self);
  base_app_view_reset_drag_page_switch (self);
  g_clear_handle_id (&priv->scroll_timeout_id, g_source_remove);

  CLUTTER_ACTOR_CLASS (shell_base_app_view_parent_class)->destroy (actor);
}

static void
shell_base_app_view_constructed (GObject *object)
{
  ShellBaseAppView *self = SHELL_BASE_APP_VIEW (object);
  ShellBaseAppViewPrivate *priv = view_priv (self);
  ShellBaseAppViewClass *klass = SHELL_BASE_APP_VIEW_GET_CLASS (self);
  ShellIconGrid *grid;
  ClutterActor *scroll_view, *scroll_container;
  ClutterAction *scroll_controller;
  ClutterActor *page_indicators, *next_indicator, *prev_indicator;
  ClutterActor *next_arrow, *prev_arrow, *box;
  ShellAppViewGridLayout *layout;
  gboolean rtl;
  StAdjustment *adjustment;

  G_OBJECT_CLASS (shell_base_app_view_parent_class)->constructed (object);

  clutter_actor_set_layout_manager (CLUTTER_ACTOR (self), clutter_bin_layout_new ());
  clutter_actor_set_x_expand (CLUTTER_ACTOR (self), TRUE);
  clutter_actor_set_y_expand (CLUTTER_ACTOR (self), TRUE);

  priv->drag_target = g_object_new (SHELL_TYPE_BASE_APP_VIEW_DRAG_TARGET, NULL);
  g_set_weak_pointer (&priv->drag_target->view, self);

  grid = klass->create_grid (self);
  g_return_if_fail (SHELL_IS_APP_GRID (grid));
  g_object_ref_sink (grid);
  g_set_weak_pointer (&priv->grid, SHELL_APP_GRID (grid));
  shell_dnd_set_drag_target (CLUTTER_ACTOR (grid), SHELL_DRAG_TARGET (priv->drag_target));
  /* Standard hack for ClutterBinLayout */
  clutter_actor_set_x_expand (CLUTTER_ACTOR (grid), TRUE);
  g_signal_connect (grid, "pages-changed",
                    G_CALLBACK (base_app_view_grid_pages_changed_cb), self);

  /* Scroll View */
  scroll_view = g_object_new (ST_TYPE_SCROLL_VIEW,
                              "style-class", "apps-scroll-view",
                              "clip-to-allocation", TRUE,
                              "x-expand", TRUE,
                              "y-expand", TRUE,
                              "reactive", TRUE,
                              "enable-mouse-scrolling", FALSE,
                              "enable-touch-scrolling", FALSE,
                              "hscrollbar-policy", ST_POLICY_EXTERNAL,
                              "vscrollbar-policy", ST_POLICY_NEVER,
                              "child", grid,
                              NULL);
  g_object_unref (grid);
  g_set_weak_pointer (&priv->scroll_view, scroll_view);

  priv->can_scroll = TRUE; /* limiting scrolling speed */

  scroll_controller = clutter_scroll_controller_new (CLUTTER_SCROLL_CONTROLLER_FLAG_DISCRETE |
                                                     CLUTTER_SCROLL_CONTROLLER_FLAG_SCROLL_VERTICAL);
  g_signal_connect (scroll_controller, "scroll",
                    G_CALLBACK (base_app_view_scroll_cb), self);
  clutter_actor_add_action (scroll_view, scroll_controller);

  adjustment = st_scroll_view_get_hadjustment (ST_SCROLL_VIEW (scroll_view));
  g_set_weak_pointer (&priv->adjustment, adjustment);
  g_signal_connect (adjustment, "notify::value",
                    G_CALLBACK (base_app_view_adjustment_value_changed_cb), self);

  /* Page Indicators */
  page_indicators = CLUTTER_ACTOR (shell_page_indicators_new (CLUTTER_ORIENTATION_HORIZONTAL));
  g_set_weak_pointer (&priv->page_indicators, SHELL_PAGE_INDICATORS (page_indicators));
  clutter_actor_set_y_expand (page_indicators, FALSE);
  g_signal_connect (page_indicators, "page-activated",
                    G_CALLBACK (base_app_view_page_activated_cb), self);

  scroll_controller = clutter_scroll_controller_new (CLUTTER_SCROLL_CONTROLLER_FLAG_DISCRETE |
                                                     CLUTTER_SCROLL_CONTROLLER_FLAG_SCROLL_VERTICAL);
  g_signal_connect (scroll_controller, "scroll",
                    G_CALLBACK (base_app_view_scroll_cb), self);
  clutter_actor_add_action (page_indicators, scroll_controller);

  /* Navigation indicators */
  next_indicator = g_object_new (ST_TYPE_WIDGET,
                                 "style-class", "page-navigation-hint next",
                                 "opacity", 0,
                                 "visible", FALSE,
                                 "reactive", FALSE,
                                 "x-expand", TRUE,
                                 "y-expand", TRUE,
                                 "x-align", CLUTTER_ACTOR_ALIGN_FILL,
                                 "y-align", CLUTTER_ACTOR_ALIGN_FILL,
                                 NULL);
  g_set_weak_pointer (&priv->next_page_indicator, next_indicator);

  prev_indicator = g_object_new (ST_TYPE_WIDGET,
                                 "style-class", "page-navigation-hint previous",
                                 "opacity", 0,
                                 "visible", FALSE,
                                 "reactive", FALSE,
                                 "x-expand", TRUE,
                                 "y-expand", TRUE,
                                 "x-align", CLUTTER_ACTOR_ALIGN_FILL,
                                 "y-align", CLUTTER_ACTOR_ALIGN_FILL,
                                 NULL);
  g_set_weak_pointer (&priv->prev_page_indicator, prev_indicator);

  /* Next/prev page arrows */
  rtl = text_direction_of (CLUTTER_ACTOR (self)) == CLUTTER_TEXT_DIRECTION_RTL;
  next_arrow = g_object_new (ST_TYPE_BUTTON,
                             "style-class", "page-navigation-arrow",
                             "icon-name", rtl
                               ? "carousel-arrow-previous-symbolic"
                               : "carousel-arrow-next-symbolic",
                             "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             NULL);
  g_set_weak_pointer (&priv->next_page_arrow, next_arrow);
  g_signal_connect (next_arrow, "clicked",
                    G_CALLBACK (base_app_view_next_arrow_clicked_cb), self);

  prev_arrow = g_object_new (ST_TYPE_BUTTON,
                             "style-class", "page-navigation-arrow",
                             "icon-name", rtl
                               ? "carousel-arrow-next-symbolic"
                               : "carousel-arrow-previous-symbolic",
                             "opacity", 0,
                             "visible", FALSE,
                             "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             NULL);
  g_set_weak_pointer (&priv->prev_page_arrow, prev_arrow);
  g_signal_connect (prev_arrow, "clicked",
                    G_CALLBACK (base_app_view_prev_arrow_clicked_cb), self);

  scroll_container = g_object_new (ST_TYPE_WIDGET,
                                   "clip-to-allocation", TRUE,
                                   "y-expand", TRUE,
                                   NULL);
  clutter_actor_add_child (scroll_container, scroll_view);
  clutter_actor_add_child (scroll_container, prev_indicator);
  clutter_actor_add_child (scroll_container, next_indicator);
  clutter_actor_add_child (scroll_container, next_arrow);
  clutter_actor_add_child (scroll_container, prev_arrow);

  layout = shell_app_view_grid_layout_new (SHELL_APP_GRID (grid),
                                           scroll_view,
                                           next_indicator,
                                           next_arrow,
                                           prev_indicator,
                                           prev_arrow);
  clutter_actor_set_layout_manager (scroll_container, CLUTTER_LAYOUT_MANAGER (layout));
  g_set_weak_pointer (&priv->app_grid_layout, layout);
  shell_dnd_set_drag_target (scroll_container, SHELL_DRAG_TARGET (priv->drag_target));

  box = g_object_new (ST_TYPE_BOX_LAYOUT,
                      "orientation", CLUTTER_ORIENTATION_VERTICAL,
                      "x-expand", TRUE,
                      "y-expand", TRUE,
                      NULL);
  g_set_weak_pointer (&priv->box, box);
  clutter_actor_add_child (box, scroll_container);
  clutter_actor_add_child (box, page_indicators);
  clutter_actor_add_child (CLUTTER_ACTOR (self), box);

  /* Swipe */
  priv->swipe_tracker = shell_swipe_tracker_new (scroll_view,
                                                 CLUTTER_ORIENTATION_HORIZONTAL,
                                                 SHELL_ACTION_MODE_ALL);
  g_signal_connect (priv->swipe_tracker, "begin",
                    G_CALLBACK (base_app_view_swipe_begin_cb), self);
  g_signal_connect (priv->swipe_tracker, "update",
                    G_CALLBACK (base_app_view_swipe_update_cb), self);
  g_signal_connect (priv->swipe_tracker, "end",
                    G_CALLBACK (base_app_view_swipe_end_cb), self);

  priv->orientation = CLUTTER_ORIENTATION_HORIZONTAL;

  /* Filter the apps through the user's parental controls. */
  priv->parental_controls_manager = shell_parental_controls_manager_get_default ();
  g_signal_connect_object (priv->parental_controls_manager, "app-filter-changed",
                           G_CALLBACK (base_app_view_app_filter_changed_cb), self, 0);

  /* Don't duplicate favorites */
  priv->app_favorites = shell_app_favorites_get_default ();
  g_signal_connect_object (priv->app_favorites, "changed",
                           G_CALLBACK (base_app_view_favorites_changed_cb), self, 0);

  /* Drag n' Drop */
  priv->last_overshoot_coord = -1;
}

static void
shell_base_app_view_dispose (GObject *object)
{
  ShellBaseAppView *self = SHELL_BASE_APP_VIEW (object);
  ShellBaseAppViewPrivate *priv = view_priv (self);

  base_app_view_remove_delayed_move (self);
  base_app_view_disconnect_dnd (self);
  base_app_view_reset_drag_page_switch (self);
  g_clear_handle_id (&priv->scroll_timeout_id, g_source_remove);

  if (priv->swipe_tracker != NULL)
    {
      shell_swipe_tracker_destroy (priv->swipe_tracker);
      g_clear_object (&priv->swipe_tracker);
    }

  g_clear_object (&priv->drag_target);

  G_OBJECT_CLASS (shell_base_app_view_parent_class)->dispose (object);

  g_clear_weak_pointer (&priv->grid);
  g_clear_weak_pointer (&priv->scroll_view);
  g_clear_weak_pointer (&priv->adjustment);
  g_clear_weak_pointer (&priv->page_indicators);
  g_clear_weak_pointer (&priv->next_page_indicator);
  g_clear_weak_pointer (&priv->prev_page_indicator);
  g_clear_weak_pointer (&priv->next_page_arrow);
  g_clear_weak_pointer (&priv->prev_page_arrow);
  g_clear_weak_pointer (&priv->app_grid_layout);
  g_clear_weak_pointer (&priv->box);
  g_clear_weak_pointer (&priv->drop_target);
  g_clear_weak_pointer (&priv->window_manager);
}

static void
shell_base_app_view_finalize (GObject *object)
{
  ShellBaseAppView *self = SHELL_BASE_APP_VIEW (object);
  ShellBaseAppViewPrivate *priv = view_priv (self);

  g_clear_pointer (&priv->items, g_hash_table_unref);
  g_clear_pointer (&priv->ordered_items, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_base_app_view_parent_class)->finalize (object);
}

static void
shell_base_app_view_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  ShellBaseAppView *self = SHELL_BASE_APP_VIEW (object);
  ShellBaseAppViewPrivate *priv = view_priv (self);

  switch (prop_id)
    {
    case VIEW_PROP_WINDOW_MANAGER:
      g_clear_weak_pointer (&priv->window_manager);
      g_set_weak_pointer (&priv->window_manager, g_value_get_object (value));
      break;

    case VIEW_PROP_DRAG_ICON_SIZE:
      shell_base_app_view_set_drag_icon_size (self, g_value_get_int (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_base_app_view_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellBaseAppView *self = SHELL_BASE_APP_VIEW (object);
  ShellBaseAppViewPrivate *priv = view_priv (self);

  switch (prop_id)
    {
    case VIEW_PROP_WINDOW_MANAGER:
      g_value_set_object (value, priv->window_manager);
      break;

    case VIEW_PROP_DRAG_ICON_SIZE:
      g_value_set_int (value, priv->drag_icon_size);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_base_app_view_class_init (ShellBaseAppViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_base_app_view_constructed;
  object_class->dispose = shell_base_app_view_dispose;
  object_class->finalize = shell_base_app_view_finalize;
  object_class->set_property = shell_base_app_view_set_property;
  object_class->get_property = shell_base_app_view_get_property;

  actor_class->map = shell_base_app_view_map;
  actor_class->unmap = shell_base_app_view_unmap;
  actor_class->destroy = shell_base_app_view_destroy;

  klass->create_grid = view_real_create_grid;
  klass->load_apps = view_real_load_apps;
  klass->compare_items = view_real_compare_items;
  klass->get_item_position = view_real_get_item_position;
  klass->can_accept = view_real_can_accept;
  klass->drag_begin = view_real_drag_begin;
  klass->drag_motion = view_real_drag_motion;
  klass->drag_end = view_real_drag_end;
  klass->drag_cancelled = view_real_drag_cancelled;
  klass->accept_drop = view_real_accept_drop;
  klass->maybe_move_item = view_real_maybe_move_item;
  klass->go_to_page = view_real_go_to_page;
  klass->redisplay = view_real_redisplay;
  klass->can_scroll = view_real_can_scroll;

  /**
   * ShellBaseAppView:window-manager:
   *
   * The window manager whose drag signals the view listens to while it is
   * mapped (`Main.overview`).
   */
  view_props[VIEW_PROP_WINDOW_MANAGER] =
    g_param_spec_object ("window-manager", NULL, NULL,
                         SHELL_TYPE_WINDOW_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellBaseAppView:drag-icon-size:
   *
   * The size of the icon of an application while it is dragged, in logical
   * pixels. It is given to the icons of the view.
   */
  view_props[VIEW_PROP_DRAG_ICON_SIZE] =
    g_param_spec_int ("drag-icon-size", NULL, NULL,
                      1, G_MAXINT, 64,
                      G_PARAM_READWRITE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, VIEW_N_PROPS, view_props);

  /**
   * ShellBaseAppView::view-loaded:
   * @self: the #ShellBaseAppView
   *
   * Emitted when the items of the view were loaded.
   */
  view_signals[VIEW_LOADED] =
    g_signal_new ("view-loaded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_base_app_view_init (ShellBaseAppView *self)
{
  ShellBaseAppViewPrivate *priv = view_priv (self);

  priv->drag_icon_size = 64;
  priv->items = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  priv->ordered_items = g_ptr_array_new_with_free_func (g_object_unref);
  priv->last_overshoot_coord = -1;
}

/* ------------------------------------------------------------------------ */
/* ShellFolderView                                                           */
/* ------------------------------------------------------------------------ */

enum
{
  FOLDER_VIEW_PROP_0,
  FOLDER_VIEW_PROP_FOLDER,
  FOLDER_VIEW_PROP_FOLDER_ID,
  FOLDER_VIEW_PROP_PARENT_VIEW,
  FOLDER_VIEW_N_PROPS
};

static GParamSpec *folder_view_props[FOLDER_VIEW_N_PROPS] = { NULL, };

struct _ShellFolderView
{
  ShellBaseAppView parent;

  GSettings *folder;
  char *id;
  ShellAppDisplay *parent_view; /* weak */

  gboolean deleting_folder;
  GPtrArray *apps; /* ShellApp */
};

G_DEFINE_FINAL_TYPE (ShellFolderView, shell_folder_view, SHELL_TYPE_BASE_APP_VIEW)

static ShellIconGrid *
folder_view_create_grid (ShellBaseAppView *view)
{
  return g_object_new (SHELL_TYPE_FOLDER_GRID,
                       "allow-incomplete-pages", FALSE,
                       "columns-per-page", 3,
                       "rows-per-page", 3,
                       "page-halign", CLUTTER_ACTOR_ALIGN_CENTER,
                       "page-valign", CLUTTER_ACTOR_ALIGN_CENTER,
                       NULL);
}

/* The index of the application of an item in the applications of the folder */
static int
folder_view_index_of_app (ShellFolderView *self,
                          ShellApp        *app)
{
  guint index;

  if (app != NULL && g_ptr_array_find (self->apps, app, &index))
    return (int) index;

  return -1;
}

static ShellApp *
item_get_app (ShellAppViewItem *item)
{
  return SHELL_IS_APP_ICON (item) ? shell_app_icon_get_app (SHELL_APP_ICON (item)) : NULL;
}

static void
folder_view_get_item_position (ShellBaseAppView *view,
                               ShellAppViewItem *item,
                               int              *page,
                               int              *position)
{
  ShellFolderView *self = SHELL_FOLDER_VIEW (view);
  int app_index = folder_view_index_of_app (self, item_get_app (item));
  int items_per_page;

  if (app_index == -1)
    {
      *page = -1;
      *position = -1;
      return;
    }

  items_per_page = shell_icon_grid_get_items_per_page (shell_base_app_view_get_grid (view));
  if (items_per_page <= 0)
    {
      *page = 0;
      *position = app_index;
      return;
    }

  *page = app_index / items_per_page;
  *position = app_index % items_per_page;
}

static int
folder_view_compare_items (ShellBaseAppView *view,
                           ShellAppViewItem *a,
                           ShellAppViewItem *b)
{
  ShellFolderView *self = SHELL_FOLDER_VIEW (view);
  int a_position = folder_view_index_of_app (self, item_get_app (a));
  int b_position = folder_view_index_of_app (self, item_get_app (b));

  if (a_position == -1 && b_position == -1)
    return g_utf8_collate (shell_app_view_item_get_name (a), shell_app_view_item_get_name (b));
  else if (a_position == -1)
    return 1;
  else if (b_position == -1)
    return -1;

  return a_position - b_position;
}

/**
 * shell_folder_view_create_folder_icon:
 * @self: a #ShellFolderView
 * @size: the size of the icon in logical pixels
 *
 * Makes the icon of the folder: the first four icons of its applications
 * (`createFolderIcon ()`).
 *
 * Returns: (transfer full): a new actor
 */
ClutterActor *
shell_folder_view_create_folder_icon (ShellFolderView *self,
                                      int              size)
{
  g_autoptr (GPtrArray) items = NULL;
  g_autofree char *style = NULL;
  ClutterLayoutManager *layout;
  ClutterActor *icon;
  int sub_size;
  gboolean rtl;
  guint num_items;
  int i;

  g_return_val_if_fail (SHELL_IS_FOLDER_VIEW (self), NULL);

  layout = clutter_grid_layout_new ();
  clutter_grid_layout_set_row_homogeneous (CLUTTER_GRID_LAYOUT (layout), TRUE);
  clutter_grid_layout_set_column_homogeneous (CLUTTER_GRID_LAYOUT (layout), TRUE);

  style = g_strdup_printf ("width: %dpx; height: %dpx;", size, size);
  icon = g_object_new (ST_TYPE_WIDGET,
                       "layout-manager", layout,
                       "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       "style", style,
                       NULL);

  sub_size = (int) floor (FOLDER_SUBICON_FRACTION * size);

  items = shell_base_app_view_get_all_items (SHELL_BASE_APP_VIEW (self));
  num_items = items->len;
  rtl = text_direction_of (icon) == CLUTTER_TEXT_DIRECTION_RTL;
  for (i = 0; i < 4; i++)
    {
      g_autofree char *sub_style = g_strdup_printf ("width: %dpx; height: %dpx;",
                                                    sub_size, sub_size);
      ClutterActor *bin = g_object_new (ST_TYPE_BIN, "style", sub_style, NULL);

      if ((guint) i < num_items)
        {
          ShellApp *app = item_get_app (g_ptr_array_index (items, i));

          if (app != NULL)
            st_bin_set_child (ST_BIN (bin), shell_app_create_icon_texture (app, sub_size));
        }

      clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), bin,
                                  rtl ? (i + 1) % 2 : i % 2, i / 2, 1, 1);
    }

  return icon;
}

static GPtrArray *
folder_view_load_apps (ShellBaseAppView *view)
{
  ShellFolderView *self = SHELL_FOLDER_VIEW (view);
  ShellWindowManager *window_manager = shell_base_app_view_get_window_manager (view);
  GPtrArray *items = g_ptr_array_new_with_free_func (g_object_unref);
  ShellAppSystem *app_system = shell_app_system_get_default ();
  ShellAppFavorites *app_favorites = shell_app_favorites_get_default ();
  ShellParentalControlsManager *parental = shell_parental_controls_manager_get_default ();
  g_auto (GStrv) excluded_apps = g_settings_get_strv (self->folder, "excluded-apps");
  g_auto (GStrv) folder_apps = g_settings_get_strv (self->folder, "apps");
  g_auto (GStrv) folder_categories = g_settings_get_strv (self->folder, "categories");
  GList *app_infos = self->parent_view != NULL
    ? shell_app_display_get_app_infos (self->parent_view) : NULL;
  GList *l;
  guint i;
  char **id;

  g_ptr_array_set_size (self->apps, 0);

  /* addAppId () */
#define ADD_APP_ID(app_id)                                                   \
  G_STMT_START {                                                             \
    const char *_id = (app_id);                                              \
    ShellApp *_app = NULL;                                                   \
                                                                             \
    if (!g_strv_contains ((const char * const *) excluded_apps, _id) &&      \
        !shell_app_favorites_is_favorite (app_favorites, _id) &&             \
        (_app = shell_app_system_lookup_app (app_system, _id)) != NULL &&    \
        shell_parental_controls_manager_should_show_app (parental,           \
                                                         app_info_of (_app)) && \
        !g_ptr_array_find (self->apps, _app, NULL))                          \
      g_ptr_array_add (self->apps, g_object_ref (_app));                     \
  } G_STMT_END

  for (id = folder_apps; id != NULL && *id != NULL; id++)
    ADD_APP_ID (*id);

  for (l = app_infos; l != NULL; l = l->next)
    {
      g_auto (GStrv) app_categories = get_categories (l->data);

      if (!lists_intersect ((const char * const *) folder_categories,
                            (const char * const *) app_categories))
        continue;

      if (g_app_info_get_id (l->data) != NULL)
        ADD_APP_ID (g_app_info_get_id (l->data));
    }

#undef ADD_APP_ID

  for (i = 0; i < self->apps->len; i++)
    {
      ShellApp *app = g_ptr_array_index (self->apps, i);
      ShellAppViewItem *icon = base_app_view_lookup_item (view, shell_app_get_id (app));

      if (icon != NULL)
        {
          g_ptr_array_add (items, g_object_ref (icon));
        }
      else
        {
          icon = g_object_ref_sink (g_object_new (SHELL_TYPE_APP_ICON,
                                                  "app", app,
                                                  "window-manager", window_manager,
                                                  "drag-icon-size", shell_base_app_view_get_drag_icon_size (view),
                                                  NULL));
          g_ptr_array_add (items, icon);
        }
    }

  return items;
}

static gboolean
folder_view_accept_drop (ShellBaseAppView *view,
                         GObject          *source)
{
  ShellFolderView *self = SHELL_FOLDER_VIEW (view);
  ShellBaseAppViewClass *parent_class = SHELL_BASE_APP_VIEW_CLASS (shell_folder_view_parent_class);
  g_autoptr (GPtrArray) items = NULL;
  g_autoptr (GPtrArray) folder_apps = g_ptr_array_new ();
  guint i;

  if (!parent_class->accept_drop (view, source))
    return FALSE;

  items = shell_base_app_view_get_all_items (view);
  for (i = 0; i < items->len; i++)
    g_ptr_array_add (folder_apps, (gpointer) shell_app_view_item_get_id (g_ptr_array_index (items, i)));
  g_ptr_array_add (folder_apps, NULL);

  g_settings_set_strv (self->folder, "apps", (const char * const *) folder_apps->pdata);

  return TRUE;
}

/**
 * shell_folder_view_add_app:
 * @self: a #ShellFolderView
 * @app: the application to add to the folder
 *
 * Adds an application to the folder (`addApp ()`).
 */
void
shell_folder_view_add_app (ShellFolderView *self,
                           ShellApp        *app)
{
  g_autoptr (GPtrArray) folder_apps = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) excluded = g_ptr_array_new_with_free_func (g_free);
  g_auto (GStrv) strv = NULL;
  const char *app_id;
  gboolean found = FALSE;
  char **id;

  g_return_if_fail (SHELL_IS_FOLDER_VIEW (self));
  g_return_if_fail (SHELL_IS_APP (app));

  app_id = shell_app_get_id (app);

  strv = g_settings_get_strv (self->folder, "apps");
  for (id = strv; *id != NULL; id++)
    g_ptr_array_add (folder_apps, g_strdup (*id));
  g_ptr_array_add (folder_apps, g_strdup (app_id));
  g_ptr_array_add (folder_apps, NULL);

  g_settings_set_strv (self->folder, "apps", (const char * const *) folder_apps->pdata);

  /* Also remove from 'excluded-apps' if the app id is listed
   * there. This is only possible on categories-based folders. */
  g_strfreev (strv);
  strv = g_settings_get_strv (self->folder, "excluded-apps");
  for (id = strv; *id != NULL; id++)
    {
      if (!found && g_strcmp0 (*id, app_id) == 0)
        {
          found = TRUE;
          continue;
        }

      g_ptr_array_add (excluded, g_strdup (*id));
    }

  if (found)
    {
      g_ptr_array_add (excluded, NULL);
      g_settings_set_strv (self->folder, "excluded-apps", (const char * const *) excluded->pdata);
    }
}

/**
 * shell_folder_view_remove_app:
 * @self: a #ShellFolderView
 * @app: the application to remove from the folder
 *
 * Removes an application from the folder, and the folder if it was the last
 * (`removeApp ()`).
 */
void
shell_folder_view_remove_app (ShellFolderView *self,
                              ShellApp        *app)
{
  g_autoptr (GPtrArray) folder_apps = g_ptr_array_new_with_free_func (g_free);
  g_auto (GStrv) strv = NULL;
  const char *app_id;
  gboolean found = FALSE;
  char **id;

  g_return_if_fail (SHELL_IS_FOLDER_VIEW (self));
  g_return_if_fail (SHELL_IS_APP (app));

  app_id = shell_app_get_id (app);

  strv = g_settings_get_strv (self->folder, "apps");
  for (id = strv; *id != NULL; id++)
    {
      if (!found && g_strcmp0 (*id, app_id) == 0)
        {
          found = TRUE;
          continue;
        }

      g_ptr_array_add (folder_apps, g_strdup (*id));
    }

  /* Remove the folder if this is the last app icon; otherwise,
   * just remove the icon */
  if (folder_apps->len == 0)
    {
      g_autoptr (GSettingsSchema) schema = NULL;
      g_autoptr (GSettings) settings = NULL;
      g_auto (GStrv) keys = NULL;
      g_auto (GStrv) folders = NULL;
      g_autoptr (GPtrArray) remaining = g_ptr_array_new_with_free_func (g_free);
      char **key;

      self->deleting_folder = TRUE;

      /* Resetting all keys deletes the relocatable schema */
      g_object_get (self->folder, "settings-schema", &schema, NULL);
      keys = g_settings_schema_list_keys (schema);
      for (key = keys; *key != NULL; key++)
        g_settings_reset (self->folder, *key);

      settings = g_settings_new ("org.gnome.desktop.app-folders");
      folders = g_settings_get_strv (settings, "folder-children");
      for (id = folders; *id != NULL; id++)
        {
          if (g_strcmp0 (*id, self->id) != 0)
            g_ptr_array_add (remaining, g_strdup (*id));
        }
      g_ptr_array_add (remaining, NULL);
      g_settings_set_strv (settings, "folder-children", (const char * const *) remaining->pdata);

      self->deleting_folder = FALSE;
    }
  else
    {
      g_auto (GStrv) categories = NULL;

      /* If this is a categories-based folder, also add it to
       * the list of excluded apps */
      categories = g_settings_get_strv (self->folder, "categories");
      if (g_strv_length (categories) > 0)
        {
          g_autoptr (GPtrArray) excluded = g_ptr_array_new_with_free_func (g_free);
          g_auto (GStrv) excluded_strv = g_settings_get_strv (self->folder, "excluded-apps");

          for (id = excluded_strv; *id != NULL; id++)
            g_ptr_array_add (excluded, g_strdup (*id));
          g_ptr_array_add (excluded, g_strdup (app_id));
          g_ptr_array_add (excluded, NULL);
          g_settings_set_strv (self->folder, "excluded-apps", (const char * const *) excluded->pdata);
        }

      g_ptr_array_add (folder_apps, NULL);
      g_settings_set_strv (self->folder, "apps", (const char * const *) folder_apps->pdata);
    }
}

/**
 * shell_folder_view_get_deleting_folder:
 * @self: a #ShellFolderView
 *
 * Returns: whether the folder is being removed (`deletingFolder`)
 */
gboolean
shell_folder_view_get_deleting_folder (ShellFolderView *self)
{
  g_return_val_if_fail (SHELL_IS_FOLDER_VIEW (self), FALSE);

  return self->deleting_folder;
}

static void
shell_folder_view_constructed (GObject *object)
{
  ShellFolderView *self = SHELL_FOLDER_VIEW (object);

  G_OBJECT_CLASS (shell_folder_view_parent_class)->constructed (object);

  shell_base_app_view_redisplay (SHELL_BASE_APP_VIEW (self));
}

static void
shell_folder_view_dispose (GObject *object)
{
  ShellFolderView *self = SHELL_FOLDER_VIEW (object);

  g_clear_weak_pointer (&self->parent_view);

  G_OBJECT_CLASS (shell_folder_view_parent_class)->dispose (object);
}

static void
shell_folder_view_finalize (GObject *object)
{
  ShellFolderView *self = SHELL_FOLDER_VIEW (object);

  g_clear_object (&self->folder);
  g_clear_pointer (&self->id, g_free);
  g_clear_pointer (&self->apps, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_folder_view_parent_class)->finalize (object);
}

static void
shell_folder_view_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  ShellFolderView *self = SHELL_FOLDER_VIEW (object);

  switch (prop_id)
    {
    case FOLDER_VIEW_PROP_FOLDER:
      self->folder = g_value_dup_object (value);
      break;

    case FOLDER_VIEW_PROP_FOLDER_ID:
      self->id = g_value_dup_string (value);
      break;

    case FOLDER_VIEW_PROP_PARENT_VIEW:
      g_clear_weak_pointer (&self->parent_view);
      g_set_weak_pointer (&self->parent_view, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_folder_view_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  ShellFolderView *self = SHELL_FOLDER_VIEW (object);

  switch (prop_id)
    {
    case FOLDER_VIEW_PROP_FOLDER:
      g_value_set_object (value, self->folder);
      break;

    case FOLDER_VIEW_PROP_FOLDER_ID:
      g_value_set_string (value, self->id);
      break;

    case FOLDER_VIEW_PROP_PARENT_VIEW:
      g_value_set_object (value, self->parent_view);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_folder_view_class_init (ShellFolderViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellBaseAppViewClass *view_class = SHELL_BASE_APP_VIEW_CLASS (klass);

  object_class->constructed = shell_folder_view_constructed;
  object_class->dispose = shell_folder_view_dispose;
  object_class->finalize = shell_folder_view_finalize;
  object_class->set_property = shell_folder_view_set_property;
  object_class->get_property = shell_folder_view_get_property;

  view_class->create_grid = folder_view_create_grid;
  view_class->get_item_position = folder_view_get_item_position;
  view_class->compare_items = folder_view_compare_items;
  view_class->load_apps = folder_view_load_apps;
  view_class->accept_drop = folder_view_accept_drop;

  /**
   * ShellFolderView:folder:
   *
   * The settings of the folder.
   */
  folder_view_props[FOLDER_VIEW_PROP_FOLDER] =
    g_param_spec_object ("folder", NULL, NULL,
                         G_TYPE_SETTINGS,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellFolderView:folder-id:
   *
   * The id of the folder.
   */
  folder_view_props[FOLDER_VIEW_PROP_FOLDER_ID] =
    g_param_spec_string ("folder-id", NULL, NULL, NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellFolderView:parent-view:
   *
   * The application grid the folder is part of, which knows the installed
   * applications.
   */
  folder_view_props[FOLDER_VIEW_PROP_PARENT_VIEW] =
    g_param_spec_object ("parent-view", NULL, NULL,
                         SHELL_TYPE_APP_DISPLAY,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, FOLDER_VIEW_N_PROPS, folder_view_props);
}

static void
shell_folder_view_init (ShellFolderView *self)
{
  self->apps = g_ptr_array_new_with_free_func (g_object_unref);
}

/**
 * shell_folder_view_new:
 * @folder: the settings of the folder
 * @id: the id of the folder
 * @parent_view: the application grid the folder is part of
 * @window_manager: (nullable): the window manager the overview is reached
 *   through
 *
 * Creates the view of the applications of a folder (`new FolderView ()`).
 *
 * Returns: (transfer full): a new #ShellFolderView
 */
ShellFolderView *
shell_folder_view_new (GSettings          *folder,
                       const char         *id,
                       ShellAppDisplay    *parent_view,
                       ShellWindowManager *window_manager)
{
  return g_object_new (SHELL_TYPE_FOLDER_VIEW,
                       "folder", folder,
                       "folder-id", id,
                       "parent-view", parent_view,
                       "window-manager", window_manager,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellAppFolderDialog                                                      */
/* ------------------------------------------------------------------------ */

enum
{
  DIALOG_PROP_0,
  DIALOG_PROP_SOURCE,
  DIALOG_PROP_FOLDER,
  DIALOG_PROP_APP_DISPLAY,
  DIALOG_N_PROPS
};

static GParamSpec *dialog_props[DIALOG_N_PROPS] = { NULL, };

enum
{
  DIALOG_OPEN_STATE_CHANGED,
  DIALOG_LAST_SIGNAL
};

static guint dialog_signals[DIALOG_LAST_SIGNAL] = { 0, };

typedef struct
{
  ShellAppFolderDialogPopdownFunc func;
  gpointer user_data;
  GDestroyNotify destroy;
} PopdownCallback;

struct _ShellAppFolderDialog
{
  StBin parent;

  ShellFolderIcon *source; /* weak */
  GSettings *folder;
  ShellFolderView *view; /* weak, it is shown by the dialog */
  ShellAppDisplay *app_display; /* weak */

  gboolean is_open;

  ClutterActor *view_box; /* weak */
  ClutterActor *entry_box; /* weak */
  ClutterActor *folder_name_label; /* weak */
  ClutterActor *entry; /* weak */
  ClutterActor *edit_button; /* weak */

  ShellGrabHelper *grab_helper;

  guint drag_monitor;
  gulong source_mapped_id;
  guint popdown_timeout_id;
  gboolean needs_zoom_and_fade;

  GPtrArray *popdown_callbacks; /* PopdownCallback */
};

static void shell_app_folder_dialog_drag_target_iface_init (ShellDragTargetInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellAppFolderDialog, shell_app_folder_dialog, ST_TYPE_BIN,
                               G_IMPLEMENT_INTERFACE (SHELL_TYPE_DRAG_TARGET,
                                                      shell_app_folder_dialog_drag_target_iface_init))

static void zoom_and_fade_out (ShellAppFolderDialog *self);

static void
popdown_callback_free (gpointer data)
{
  PopdownCallback *callback = data;

  if (callback->destroy != NULL)
    callback->destroy (callback->user_data);

  g_free (callback);
}

static CoglColor
shade_color (guint8 alpha)
{
  CoglColor color = { .red = 0, .green = 0, .blue = 0, .alpha = alpha };

  return color;
}

static void
make_unreactive_cb (gboolean finished,
                    gpointer user_data)
{
  ClutterActor *actor = user_data;

  if (finished)
    clutter_actor_set_reactive (actor, FALSE);
}

static void
dialog_switch_actor (ClutterActor *from,
                     ClutterActor *to)
{
  ShellEaseParams params = {
    .duration = 300,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  clutter_actor_set_reactive (to, TRUE);
  st_widget_set_can_focus (ST_WIDGET (to), TRUE);
  shell_actor_ease (to, &params, NULL, NULL, NULL,
                    "opacity", 255,
                    NULL);

  st_widget_set_can_focus (ST_WIDGET (from), FALSE);
  shell_actor_ease (from, &params, make_unreactive_cb, g_object_ref (from), g_object_unref,
                    "opacity", 0,
                    NULL);
}

static void
dialog_maybe_update_folder_name (ShellAppFolderDialog *self)
{
  g_autofree char *folder_name = get_folder_name (self->folder);
  g_autofree char *new_folder_name = g_strstrip (g_strdup (clutter_text_get_text (st_entry_get_clutter_text (ST_ENTRY (self->entry)))));

  if (new_folder_name[0] == '\0' || g_strcmp0 (new_folder_name, folder_name) == 0)
    return;

  g_settings_set_string (self->folder, "name", new_folder_name);
  g_settings_set_boolean (self->folder, "translate", FALSE);
}

static void
dialog_show_folder_label (ShellAppFolderDialog *self)
{
  if (st_button_get_checked (ST_BUTTON (self->edit_button)))
    st_button_set_checked (ST_BUTTON (self->edit_button), FALSE);

  dialog_maybe_update_folder_name (self);
  dialog_switch_actor (self->entry, self->folder_name_label);
  st_widget_navigate_focus (ST_WIDGET (self), CLUTTER_ACTOR (self), ST_DIR_TAB_FORWARD, FALSE);
}

static void
dialog_show_folder_entry (ShellAppFolderDialog *self)
{
  ClutterText *text = st_entry_get_clutter_text (ST_ENTRY (self->entry));

  dialog_switch_actor (self->folder_name_label, self->entry);

  clutter_text_set_selection (text, 0, -1);
  clutter_actor_grab_key_focus (CLUTTER_ACTOR (text));
}

static void
dialog_sync_folder_name (ShellAppFolderDialog *self)
{
  g_autofree char *new_name = get_folder_name (self->folder);

  st_label_set_text (ST_LABEL (self->folder_name_label), new_name);
  st_entry_set_text (ST_ENTRY (self->entry), new_name);
}

static void
dialog_entry_activate_cb (ClutterText          *text,
                          ShellAppFolderDialog *self)
{
  dialog_show_folder_label (self);
}

static void
dialog_edit_button_checked_cb (GObject              *object,
                               GParamSpec           *pspec,
                               ShellAppFolderDialog *self)
{
  if (st_button_get_checked (ST_BUTTON (self->edit_button)))
    dialog_show_folder_entry (self);
  else
    dialog_show_folder_label (self);
}

static void
dialog_folder_name_changed_cb (GSettings            *settings,
                               const char           *key,
                               ShellAppFolderDialog *self)
{
  dialog_sync_folder_name (self);
}

static void
dialog_add_folder_name_entry (ShellAppFolderDialog *self)
{
  ClutterActor *entry_box, *ghost_button, *stack, *label, *entry, *edit_button;
  ClutterText *text;

  entry_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "style-class", "folder-name-container",
                            NULL);
  g_set_weak_pointer (&self->entry_box, entry_box);
  clutter_actor_add_child (self->view_box, entry_box);

  /* Empty actor to center the title */
  ghost_button = clutter_actor_new ();
  clutter_actor_add_child (entry_box, ghost_button);

  stack = g_object_new (SHELL_TYPE_STACK,
                        "x-expand", TRUE,
                        "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  clutter_actor_add_child (entry_box, stack);

  /* Folder name label */
  label = g_object_new (ST_TYPE_LABEL,
                        "style-class", "folder-name-label",
                        "x-expand", TRUE,
                        "y-expand", TRUE,
                        "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  g_set_weak_pointer (&self->folder_name_label, label);
  clutter_actor_add_child (stack, label);

  /* Folder name entry */
  entry = g_object_new (ST_TYPE_ENTRY,
                        "style-class", "folder-name-entry",
                        "opacity", 0,
                        "reactive", FALSE,
                        NULL);
  g_set_weak_pointer (&self->entry, entry);
  clutter_actor_set_accessible_name (entry, _("Folder name"));
  text = st_entry_get_clutter_text (ST_ENTRY (entry));
  clutter_actor_set_x_expand (CLUTTER_ACTOR (text), TRUE);
  clutter_actor_set_x_align (CLUTTER_ACTOR (text), CLUTTER_ACTOR_ALIGN_CENTER);
  g_signal_connect (text, "activate", G_CALLBACK (dialog_entry_activate_cb), self);

  clutter_actor_add_child (stack, entry);

  /* Edit button */
  edit_button = g_object_new (ST_TYPE_BUTTON,
                              "style-class", "icon-button",
                              "button-mask", ST_BUTTON_PRIMARY,
                              "toggle-mode", TRUE,
                              "reactive", TRUE,
                              "can-focus", TRUE,
                              "x-align", CLUTTER_ACTOR_ALIGN_END,
                              "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                              "icon-name", "document-edit-symbolic",
                              NULL);
  g_set_weak_pointer (&self->edit_button, edit_button);
  clutter_actor_set_accessible_name (edit_button, _("Edit folder name"));

  g_signal_connect (edit_button, "notify::checked",
                    G_CALLBACK (dialog_edit_button_checked_cb), self);

  clutter_actor_add_child (entry_box, edit_button);

  clutter_actor_add_constraint (ghost_button,
                                clutter_bind_constraint_new (edit_button,
                                                             CLUTTER_BIND_SIZE, 0));

  g_signal_connect_object (self->folder, "changed::name",
                           G_CALLBACK (dialog_folder_name_changed_cb), self, 0);
  dialog_sync_folder_name (self);
}

static void
dialog_zoom_and_fade_out_cb (ClutterActor *actor,
                             GParamSpec   *pspec,
                             gpointer      user_data)
{
  zoom_and_fade_out (SHELL_APP_FOLDER_DIALOG (user_data));
}

static void
dialog_zoom_and_fade_in (ShellAppFolderDialog *self)
{
  ShellEaseParams params = {
    .duration = FOLDER_DIALOG_ANIMATION_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };
  ClutterActor *child = st_bin_get_child (ST_BIN (self));
  CoglColor color = shade_color (DIALOG_SHADE_NORMAL_ALPHA);
  float source_x, source_y, dialog_x, dialog_y;

  if (self->source == NULL || child == NULL)
    return;

  clutter_actor_get_transformed_position (CLUTTER_ACTOR (self->source), &source_x, &source_y);
  clutter_actor_get_transformed_position (child, &dialog_x, &dialog_y);

  clutter_actor_set_translation (child, source_x - dialog_x, source_y - dialog_y, 0);
  clutter_actor_set_scale (child,
                           clutter_actor_get_width (CLUTTER_ACTOR (self->source)) /
                           clutter_actor_get_width (child),
                           clutter_actor_get_height (CLUTTER_ACTOR (self->source)) /
                           clutter_actor_get_height (child));
  clutter_actor_set_opacity (child, 0);

  shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                    "background-color", &color,
                    NULL);

  params.mode = CLUTTER_EASE_OUT_EXPO;
  shell_actor_ease (child, &params, NULL, NULL, NULL,
                    "translation-x", 0.0,
                    "translation-y", 0.0,
                    "scale-x", 1.0,
                    "scale-y", 1.0,
                    "opacity", 255,
                    NULL);

  self->needs_zoom_and_fade = FALSE;

  if (self->source_mapped_id == 0)
    self->source_mapped_id = g_signal_connect (self->source, "notify::mapped",
                                               G_CALLBACK (dialog_zoom_and_fade_out_cb), self);
}

typedef struct
{
  GWeakRef dialog;
} ZoomOutData;

static void
zoom_out_data_free (gpointer data)
{
  ZoomOutData *zoom = data;

  g_weak_ref_clear (&zoom->dialog);
  g_free (zoom);
}

static void
zoom_out_done_cb (gboolean finished,
                  gpointer user_data)
{
  ZoomOutData *zoom = user_data;
  g_autoptr (ShellAppFolderDialog) self = g_weak_ref_get (&zoom->dialog);
  g_autoptr (GPtrArray) callbacks = NULL;
  ClutterActor *child;
  guint i;

  if (!finished || self == NULL)
    return;

  child = st_bin_get_child (ST_BIN (self));
  if (child != NULL)
    {
      clutter_actor_set_translation (child, 0, 0, 0);
      clutter_actor_set_scale (child, 1, 1);
      clutter_actor_set_opacity (child, 255);
    }
  clutter_actor_hide (CLUTTER_ACTOR (self));

  callbacks = g_steal_pointer (&self->popdown_callbacks);
  self->popdown_callbacks = g_ptr_array_new_with_free_func (popdown_callback_free);
  for (i = 0; i < callbacks->len; i++)
    {
      PopdownCallback *callback = g_ptr_array_index (callbacks, i);

      callback->func (callback->user_data);
    }
}

static void
zoom_and_fade_out (ShellAppFolderDialog *self)
{
  ShellEaseParams params = {
    .duration = FOLDER_DIALOG_ANIMATION_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };
  ClutterActor *child = st_bin_get_child (ST_BIN (self));
  CoglColor color = shade_color (0);
  float source_x, source_y, dialog_x, dialog_y;
  ZoomOutData *zoom;

  if (!self->is_open)
    return;

  if (self->source == NULL || !clutter_actor_is_mapped (CLUTTER_ACTOR (self->source)) ||
      child == NULL)
    {
      clutter_actor_hide (CLUTTER_ACTOR (self));
      return;
    }

  clutter_actor_get_transformed_position (CLUTTER_ACTOR (self->source), &source_x, &source_y);
  clutter_actor_get_transformed_position (child, &dialog_x, &dialog_y);

  shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                    "background-color", &color,
                    NULL);

  shell_actor_ease (child, &params, NULL, NULL, NULL,
                    "opacity", 0,
                    NULL);

  params.mode = CLUTTER_EASE_OUT_EXPO;
  zoom = g_new0 (ZoomOutData, 1);
  g_weak_ref_init (&zoom->dialog, self);
  shell_actor_ease (child, &params, zoom_out_done_cb, zoom, zoom_out_data_free,
                    "translation-x", (double) (source_x - dialog_x),
                    "translation-y", (double) (source_y - dialog_y),
                    "scale-x", (double) (clutter_actor_get_width (CLUTTER_ACTOR (self->source)) /
                                         clutter_actor_get_width (child)),
                    "scale-y", (double) (clutter_actor_get_height (CLUTTER_ACTOR (self->source)) /
                                         clutter_actor_get_height (child)),
                    NULL);

  self->needs_zoom_and_fade = FALSE;
}

static void
dialog_remove_drag_monitor (ShellAppFolderDialog *self)
{
  if (self->drag_monitor == 0)
    return;

  shell_dnd_remove_drag_monitor (self->drag_monitor);
  self->drag_monitor = 0;
}

static void
dialog_remove_popdown_timeout (ShellAppFolderDialog *self)
{
  g_clear_handle_id (&self->popdown_timeout_id, g_source_remove);
}

static void
dialog_allocate (ClutterActor          *actor,
                 const ClutterActorBox *box)
{
  ShellAppFolderDialog *self = SHELL_APP_FOLDER_DIALOG (actor);

  CLUTTER_ACTOR_CLASS (shell_app_folder_dialog_parent_class)->allocate (actor, box);

  /* We can only start zooming after receiving an allocation */
  if (self->needs_zoom_and_fade)
    dialog_zoom_and_fade_in (self);
}

static void
dialog_set_lighter_background (ShellAppFolderDialog *self,
                               gboolean              lighter)
{
  ShellEaseParams params = {
    .duration = FOLDER_DIALOG_ANIMATION_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };
  CoglColor color = shade_color (lighter
                                 ? DIALOG_SHADE_HIGHLIGHT_ALPHA
                                 : DIALOG_SHADE_NORMAL_ALPHA);

  shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                    "background-color", &color,
                    NULL);
}

static gboolean
dialog_within_dialog (ShellAppFolderDialog *self,
                      float                 x,
                      float                 y)
{
  ClutterActor *child = st_bin_get_child (ST_BIN (self));
  graphene_rect_t extents;
  graphene_point_t point = GRAPHENE_POINT_INIT (x, y);

  if (child == NULL)
    return FALSE;

  clutter_actor_get_transformed_extents (child, &extents);
  return graphene_rect_contains_point (&extents, &point);
}

static ShellDragMotionResult
dialog_drag_monitor_cb (ShellDragEvent *event,
                        gpointer        user_data)
{
  ShellAppFolderDialog *self = user_data;
  gboolean within_dialog = dialog_within_dialog (self,
                                                 shell_drag_event_get_x (event),
                                                 shell_drag_event_get_y (event));

  dialog_set_lighter_background (self, !within_dialog);

  if (within_dialog)
    {
      dialog_remove_popdown_timeout (self);
      dialog_remove_drag_monitor (self);
    }

  return SHELL_DRAG_MOTION_RESULT_CONTINUE;
}

static void
dialog_setup_drag_monitor (ShellAppFolderDialog *self)
{
  if (self->drag_monitor != 0)
    return;

  self->drag_monitor = shell_dnd_add_drag_monitor (dialog_drag_monitor_cb, self, NULL,
                                                   NULL, NULL, NULL);
}

static void
dialog_popdown_timeout_cb (gpointer user_data)
{
  ShellAppFolderDialog *self = user_data;

  self->popdown_timeout_id = 0;
  shell_app_folder_dialog_popdown (self, NULL, NULL, NULL);
}

static void
dialog_setup_popdown_timeout (ShellAppFolderDialog *self)
{
  if (self->popdown_timeout_id > 0)
    return;

  self->popdown_timeout_id = g_timeout_add_once (POPDOWN_DIALOG_TIMEOUT,
                                                 dialog_popdown_timeout_cb, self);
}

static ShellDragMotionResult
dialog_handle_drag_over (ShellDragTarget *target,
                         GObject         *source,
                         ClutterActor    *actor,
                         float            x,
                         float            y,
                         uint32_t         time)
{
  ShellAppFolderDialog *self = SHELL_APP_FOLDER_DIALOG (target);

  if (dialog_within_dialog (self, x, y))
    {
      dialog_set_lighter_background (self, FALSE);
      dialog_remove_popdown_timeout (self);
      dialog_remove_drag_monitor (self);
    }
  else
    {
      dialog_setup_popdown_timeout (self);
      dialog_setup_drag_monitor (self);
    }

  return SHELL_DRAG_MOTION_RESULT_MOVE_DROP;
}

typedef struct
{
  GWeakRef dialog;
  GObject *source;
  char *app_id;
} RemoveAppData;

static void
remove_app_data_free (gpointer user_data)
{
  RemoveAppData *data = user_data;

  g_weak_ref_clear (&data->dialog);
  g_clear_object (&data->source);
  g_free (data->app_id);
  g_free (data);
}

static void
remove_app_popdown_cb (gpointer user_data)
{
  RemoveAppData *data = user_data;
  g_autoptr (ShellAppFolderDialog) self = g_weak_ref_get (&data->dialog);

  if (self == NULL)
    return;

  if (self->view != NULL && SHELL_IS_APP_ICON (data->source))
    shell_folder_view_remove_app (self->view, shell_app_icon_get_app (SHELL_APP_ICON (data->source)));

  if (self->app_display != NULL)
    shell_base_app_view_select_app (SHELL_BASE_APP_VIEW (self->app_display), data->app_id);
}

static gboolean
dialog_accept_drop (ShellDragTarget *target,
                    GObject         *source,
                    ClutterActor    *actor,
                    float            x,
                    float            y,
                    uint32_t         time)
{
  ShellAppFolderDialog *self = SHELL_APP_FOLDER_DIALOG (target);
  RemoveAppData *data;

  if (!SHELL_IS_APP_VIEW_ITEM (source))
    return FALSE;

  data = g_new0 (RemoveAppData, 1);
  g_weak_ref_init (&data->dialog, self);
  data->source = g_object_ref (source);
  data->app_id = g_strdup (shell_app_view_item_get_id (SHELL_APP_VIEW_ITEM (source)));

  shell_app_folder_dialog_popdown (self, remove_app_popdown_cb, data, remove_app_data_free);

  return TRUE;
}

static void
shell_app_folder_dialog_drag_target_iface_init (ShellDragTargetInterface *iface)
{
  iface->handle_drag_over = dialog_handle_drag_over;
  iface->accept_drop = dialog_accept_drop;
}

/**
 * shell_app_folder_dialog_toggle:
 * @self: a #ShellAppFolderDialog
 *
 * Opens the dialog, or closes it if it is open (`toggle ()`).
 */
void
shell_app_folder_dialog_toggle (ShellAppFolderDialog *self)
{
  g_return_if_fail (SHELL_IS_APP_FOLDER_DIALOG (self));

  if (self->is_open)
    shell_app_folder_dialog_popdown (self, NULL, NULL, NULL);
  else
    shell_app_folder_dialog_popup (self);
}

static void
dialog_ungrab_cb (gboolean is_user,
                  gpointer user_data)
{
  GWeakRef *ref = user_data;
  g_autoptr (ShellAppFolderDialog) self = g_weak_ref_get (ref);

  if (self != NULL)
    shell_app_folder_dialog_popdown (self, NULL, NULL, NULL);
}

/**
 * shell_app_folder_dialog_popup:
 * @self: a #ShellAppFolderDialog
 *
 * Opens the dialog (`popup ()`).
 */
void
shell_app_folder_dialog_popup (ShellAppFolderDialog *self)
{
  GWeakRef *ref;
  ClutterActor *parent;

  g_return_if_fail (SHELL_IS_APP_FOLDER_DIALOG (self));

  if (self->is_open)
    return;

  ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, self);
  self->is_open = shell_grab_helper_grab (self->grab_helper, CLUTTER_ACTOR (self),
                                          self->edit_button,
                                          dialog_ungrab_cb, ref, weak_ref_free);

  if (!self->is_open)
    return;

  parent = clutter_actor_get_parent (CLUTTER_ACTOR (self));
  if (parent != NULL)
    clutter_actor_set_child_above_sibling (parent, CLUTTER_ACTOR (self), NULL);

  self->needs_zoom_and_fade = TRUE;
  clutter_actor_show (CLUTTER_ACTOR (self));

  g_signal_emit (self, dialog_signals[DIALOG_OPEN_STATE_CHANGED], 0, TRUE);
}

/**
 * shell_app_folder_dialog_popdown:
 * @self: a #ShellAppFolderDialog
 * @callback: (nullable) (scope notified) (closure user_data) (destroy destroy):
 *   called once the dialog is closed, or right away if it is not shown
 * @user_data: data for @callback
 * @destroy: called on @user_data after @callback
 *
 * Closes the dialog (`popdown (callback)`).
 */
void
shell_app_folder_dialog_popdown (ShellAppFolderDialog         *self,
                                 ShellAppFolderDialogPopdownFunc callback,
                                 gpointer                      user_data,
                                 GDestroyNotify                destroy)
{
  g_return_if_fail (SHELL_IS_APP_FOLDER_DIALOG (self));

  /* Either call the callback right away, or wait for the zoom out
   * animation to finish */
  if (callback != NULL)
    {
      if (clutter_actor_is_visible (CLUTTER_ACTOR (self)))
        {
          PopdownCallback *entry = g_new0 (PopdownCallback, 1);

          entry->func = callback;
          entry->user_data = user_data;
          entry->destroy = destroy;
          g_ptr_array_add (self->popdown_callbacks, entry);
        }
      else
        {
          callback (user_data);
          if (destroy != NULL)
            destroy (user_data);
        }
    }

  if (!self->is_open)
    return;

  zoom_and_fade_out (self);
  dialog_show_folder_label (self);

  self->is_open = FALSE;
  shell_grab_helper_ungrab_actor (self->grab_helper, CLUTTER_ACTOR (self), FALSE);
  g_signal_emit (self, dialog_signals[DIALOG_OPEN_STATE_CHANGED], 0, FALSE);
}

static gboolean
dialog_click_may_recognize_cb (ClutterGesture        *gesture,
                               ShellAppFolderDialog  *self)
{
  graphene_point_t coords;
  ClutterActor *child = st_bin_get_child (ST_BIN (self));
  ClutterActorBox box;
  float x, y;

  clutter_press_gesture_get_coords_abs (CLUTTER_PRESS_GESTURE (gesture), &coords);
  if (child == NULL || self->view_box == NULL ||
      !clutter_actor_transform_stage_point (child, coords.x, coords.y, &x, &y))
    return TRUE;

  clutter_actor_get_allocation_box (self->view_box, &box);
  return !(x >= box.x1 && x <= box.x2 && y >= box.y1 && y <= box.y2);
}

static void
dialog_click_recognize_cb (ClutterGesture        *gesture,
                           ShellAppFolderDialog  *self)
{
  shell_app_folder_dialog_popdown (self, NULL, NULL, NULL);
}

static void
shell_app_folder_dialog_destroy (ClutterActor *actor)
{
  ShellAppFolderDialog *self = SHELL_APP_FOLDER_DIALOG (actor);

  if (self->is_open)
    {
      self->is_open = FALSE;
      if (self->grab_helper != NULL)
        shell_grab_helper_ungrab_actor (self->grab_helper, actor, FALSE);
      g_clear_object (&self->grab_helper);
    }

  if (self->source_mapped_id != 0)
    {
      if (self->source != NULL)
        g_signal_handler_disconnect (self->source, self->source_mapped_id);
      self->source_mapped_id = 0;
    }

  dialog_remove_popdown_timeout (self);
  dialog_remove_drag_monitor (self);

  CLUTTER_ACTOR_CLASS (shell_app_folder_dialog_parent_class)->destroy (actor);
}

static void
shell_app_folder_dialog_constructed (GObject *object)
{
  ShellAppFolderDialog *self = SHELL_APP_FOLDER_DIALOG (object);
  ClutterActor *container, *view_box;
  ClutterAction *click_gesture;
  ShellMonitorConstraint *constraint;

  G_OBJECT_CLASS (shell_app_folder_dialog_parent_class)->constructed (object);

  g_return_if_fail (self->source != NULL && self->folder != NULL);

  clutter_actor_hide (CLUTTER_ACTOR (self));
  clutter_actor_set_x_expand (CLUTTER_ACTOR (self), TRUE);
  clutter_actor_set_y_expand (CLUTTER_ACTOR (self), TRUE);
  clutter_actor_set_reactive (CLUTTER_ACTOR (self), TRUE);
  clutter_actor_set_accessible_name (CLUTTER_ACTOR (self),
                                     shell_app_view_item_get_name (SHELL_APP_VIEW_ITEM (self->source)));
  clutter_actor_set_accessible_role (CLUTTER_ACTOR (self), ATK_ROLE_PANEL);

  constraint = shell_monitor_constraint_new ();
  shell_monitor_constraint_set_primary (constraint, TRUE);
  clutter_actor_add_constraint (CLUTTER_ACTOR (self), CLUTTER_CONSTRAINT (constraint));

  click_gesture = clutter_click_gesture_new ();
  g_signal_connect (click_gesture, "may-recognize",
                    G_CALLBACK (dialog_click_may_recognize_cb), self);
  g_signal_connect (click_gesture, "recognize",
                    G_CALLBACK (dialog_click_recognize_cb), self);
  clutter_actor_add_action (CLUTTER_ACTOR (self), click_gesture);

  g_set_weak_pointer (&self->view, shell_folder_icon_get_view (self->source));

  view_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                             "style-class", "app-folder-dialog",
                             "x-expand", TRUE,
                             "y-expand", TRUE,
                             "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             "orientation", CLUTTER_ORIENTATION_VERTICAL,
                             NULL);
  g_set_weak_pointer (&self->view_box, view_box);

  container = g_object_new (ST_TYPE_BIN,
                            "style-class", "app-folder-dialog-container",
                            "x-align", CLUTTER_ACTOR_ALIGN_FILL,
                            "y-align", CLUTTER_ACTOR_ALIGN_FILL,
                            NULL);
  st_bin_set_child (ST_BIN (container), view_box);
  st_bin_set_child (ST_BIN (self), container);

  dialog_add_folder_name_entry (self);
  clutter_actor_add_child (self->view_box, CLUTTER_ACTOR (self->view));

  st_focus_manager_add_group (shell_global_get_focus_manager (shell_global_get ()),
                              ST_WIDGET (self));

  self->grab_helper = shell_grab_helper_new (CLUTTER_ACTOR (self), SHELL_ACTION_MODE_POPUP);
}

static void
shell_app_folder_dialog_dispose (GObject *object)
{
  ShellAppFolderDialog *self = SHELL_APP_FOLDER_DIALOG (object);

  dialog_remove_popdown_timeout (self);
  dialog_remove_drag_monitor (self);
  g_clear_object (&self->grab_helper);
  g_clear_pointer (&self->popdown_callbacks, g_ptr_array_unref);

  /* dispose runs before the destroy vfunc and clears self->source below, so
   * the vfunc could no longer find the source to disconnect from */
  if (self->source_mapped_id != 0)
    {
      if (self->source != NULL)
        g_signal_handler_disconnect (self->source, self->source_mapped_id);
      self->source_mapped_id = 0;
    }

  g_clear_weak_pointer (&self->source);
  g_clear_weak_pointer (&self->view);
  g_clear_weak_pointer (&self->app_display);
  g_clear_weak_pointer (&self->view_box);
  g_clear_weak_pointer (&self->entry_box);
  g_clear_weak_pointer (&self->folder_name_label);
  g_clear_weak_pointer (&self->entry);
  g_clear_weak_pointer (&self->edit_button);

  G_OBJECT_CLASS (shell_app_folder_dialog_parent_class)->dispose (object);
}

static void
shell_app_folder_dialog_finalize (GObject *object)
{
  ShellAppFolderDialog *self = SHELL_APP_FOLDER_DIALOG (object);

  g_clear_object (&self->folder);

  G_OBJECT_CLASS (shell_app_folder_dialog_parent_class)->finalize (object);
}

static void
shell_app_folder_dialog_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  ShellAppFolderDialog *self = SHELL_APP_FOLDER_DIALOG (object);

  switch (prop_id)
    {
    case DIALOG_PROP_SOURCE:
      g_clear_weak_pointer (&self->source);
      g_set_weak_pointer (&self->source, g_value_get_object (value));
      break;

    case DIALOG_PROP_FOLDER:
      self->folder = g_value_dup_object (value);
      break;

    case DIALOG_PROP_APP_DISPLAY:
      g_clear_weak_pointer (&self->app_display);
      g_set_weak_pointer (&self->app_display, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_app_folder_dialog_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  ShellAppFolderDialog *self = SHELL_APP_FOLDER_DIALOG (object);

  switch (prop_id)
    {
    case DIALOG_PROP_SOURCE:
      g_value_set_object (value, self->source);
      break;

    case DIALOG_PROP_FOLDER:
      g_value_set_object (value, self->folder);
      break;

    case DIALOG_PROP_APP_DISPLAY:
      g_value_set_object (value, self->app_display);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_app_folder_dialog_class_init (ShellAppFolderDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_app_folder_dialog_constructed;
  object_class->dispose = shell_app_folder_dialog_dispose;
  object_class->finalize = shell_app_folder_dialog_finalize;
  object_class->set_property = shell_app_folder_dialog_set_property;
  object_class->get_property = shell_app_folder_dialog_get_property;

  actor_class->allocate = dialog_allocate;
  actor_class->destroy = shell_app_folder_dialog_destroy;

  /**
   * ShellAppFolderDialog:source:
   *
   * The icon of the folder, which the dialog zooms out of.
   */
  dialog_props[DIALOG_PROP_SOURCE] =
    g_param_spec_object ("source", NULL, NULL,
                         SHELL_TYPE_FOLDER_ICON,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellAppFolderDialog:folder:
   *
   * The settings of the folder.
   */
  dialog_props[DIALOG_PROP_FOLDER] =
    g_param_spec_object ("folder", NULL, NULL,
                         G_TYPE_SETTINGS,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellAppFolderDialog:app-display:
   *
   * The application grid, which selects the application that is dragged out
   * of the dialog.
   */
  dialog_props[DIALOG_PROP_APP_DISPLAY] =
    g_param_spec_object ("app-display", NULL, NULL,
                         SHELL_TYPE_APP_DISPLAY,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, DIALOG_N_PROPS, dialog_props);

  /**
   * ShellAppFolderDialog::open-state-changed:
   * @self: the #ShellAppFolderDialog
   * @is_open: whether the dialog opened or closed
   *
   * Emitted when the dialog is opened or closed.
   */
  dialog_signals[DIALOG_OPEN_STATE_CHANGED] =
    g_signal_new ("open-state-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN);
}

static void
shell_app_folder_dialog_init (ShellAppFolderDialog *self)
{
  self->popdown_callbacks = g_ptr_array_new_with_free_func (popdown_callback_free);
}

/* ------------------------------------------------------------------------ */
/* ShellFolderIcon                                                           */
/* ------------------------------------------------------------------------ */

enum
{
  FOLDER_ICON_PROP_0,
  FOLDER_ICON_PROP_FOLDER_ID,
  FOLDER_ICON_PROP_FOLDER_PATH,
  FOLDER_ICON_PROP_PARENT_VIEW,
  FOLDER_ICON_PROP_VIEW,
  FOLDER_ICON_N_PROPS
};

static GParamSpec *folder_icon_props[FOLDER_ICON_N_PROPS] = { NULL, };

enum
{
  FOLDER_ICON_APPS_CHANGED,
  FOLDER_ICON_LAST_SIGNAL
};

static guint folder_icon_signals[FOLDER_ICON_LAST_SIGNAL] = { 0, };

struct _ShellFolderIcon
{
  ShellAppViewItem parent;

  char *folder_id;
  char *folder_path;
  ShellAppDisplay *parent_view; /* weak */

  GSettings *folder;
  ShellFolderView *view;
  ShellAppFolderDialog *dialog; /* weak */
};

static void shell_folder_icon_drag_source_iface_init (ShellDragSourceInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellFolderIcon, shell_folder_icon, SHELL_TYPE_APP_VIEW_ITEM,
                               G_IMPLEMENT_INTERFACE (SHELL_TYPE_DRAG_SOURCE,
                                                      shell_folder_icon_drag_source_iface_init))

static ClutterActor *
folder_icon_create_icon_cb (int      size,
                            gpointer user_data)
{
  ShellFolderIcon *self = user_data;

  if (self->view == NULL)
    return NULL;

  return shell_folder_view_create_folder_icon (self->view, size);
}

/**
 * shell_folder_icon_get_view:
 * @self: a #ShellFolderIcon
 *
 * Returns: (transfer none): the view of the applications of the folder
 *   (`view`)
 */
ShellFolderView *
shell_folder_icon_get_view (ShellFolderIcon *self)
{
  g_return_val_if_fail (SHELL_IS_FOLDER_ICON (self), NULL);

  return self->view;
}

/**
 * shell_folder_icon_get_app_ids:
 * @self: a #ShellFolderIcon
 *
 * Returns: (transfer full) (array zero-terminated=1): the ids of the
 *   applications in the folder (`getAppIds ()`)
 */
char **
shell_folder_icon_get_app_ids (ShellFolderIcon *self)
{
  g_autoptr (GPtrArray) items = NULL;
  GStrvBuilder *builder = g_strv_builder_new ();
  guint i;

  g_return_val_if_fail (SHELL_IS_FOLDER_ICON (self), NULL);

  items = shell_base_app_view_get_all_items (SHELL_BASE_APP_VIEW (self->view));
  for (i = 0; i < items->len; i++)
    g_strv_builder_add (builder, shell_app_view_item_get_id (g_ptr_array_index (items, i)));

  return g_strv_builder_end (builder);
}

static void
folder_icon_dialog_open_state_changed_cb (ShellAppFolderDialog *dialog,
                                          gboolean              is_open,
                                          ShellFolderIcon      *self)
{
  ShellEaseParams params = {
    .duration = FOLDER_DIALOG_ANIMATION_TIME / 2,
    .delay = is_open ? 0 : FOLDER_DIALOG_ANIMATION_TIME - FOLDER_DIALOG_ANIMATION_TIME / 2,
    .mode = is_open ? CLUTTER_EASE_OUT_QUAD : CLUTTER_EASE_IN_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                    "opacity", is_open ? 0 : 255,
                    NULL);

  if (is_open)
    clutter_actor_add_accessible_state (CLUTTER_ACTOR (self), ATK_STATE_EXPANDED);
  else
    clutter_actor_remove_accessible_state (CLUTTER_ACTOR (self), ATK_STATE_EXPANDED);
}

static void
folder_icon_ensure_folder_dialog (ShellFolderIcon *self)
{
  ShellAppFolderDialog *dialog;

  if (self->dialog != NULL || self->parent_view == NULL)
    return;

  dialog = g_object_new (SHELL_TYPE_APP_FOLDER_DIALOG,
                         "source", self,
                         "folder", self->folder,
                         "app-display", self->parent_view,
                         NULL);
  g_set_weak_pointer (&self->dialog, dialog);
  shell_app_display_add_folder_dialog (self->parent_view, dialog);
  g_signal_connect_object (dialog, "open-state-changed",
                           G_CALLBACK (folder_icon_dialog_open_state_changed_cb), self, 0);
}

/**
 * shell_folder_icon_open:
 * @self: a #ShellFolderIcon
 *
 * Opens the folder (`open ()`).
 */
void
shell_folder_icon_open (ShellFolderIcon *self)
{
  g_return_if_fail (SHELL_IS_FOLDER_ICON (self));

  folder_icon_ensure_folder_dialog (self);
  if (self->dialog == NULL)
    return;

  st_adjustment_set_value (st_scroll_view_get_vadjustment (shell_base_app_view_get_scroll_view (SHELL_BASE_APP_VIEW (self->view))),
                           0);
  shell_app_folder_dialog_popup (self->dialog);
}

static void
folder_icon_clicked (StButton *button,
                     int       clicked_button)
{
  shell_folder_icon_open (SHELL_FOLDER_ICON (button));
}

static void
folder_icon_unmap (ClutterActor *actor)
{
  ShellFolderIcon *self = SHELL_FOLDER_ICON (actor);

  if (self->dialog != NULL)
    shell_app_folder_dialog_popdown (self->dialog, NULL, NULL, NULL);

  CLUTTER_ACTOR_CLASS (shell_folder_icon_parent_class)->unmap (actor);
}

static void
folder_icon_set_hovering_by_dnd (ShellAppViewItem *item,
                                 gboolean          hovering)
{
  ShellAppViewItemClass *parent_class = SHELL_APP_VIEW_ITEM_CLASS (shell_folder_icon_parent_class);

  if (item_priv (item)->other_icon_is_hovering == hovering)
    return;

  parent_class->set_hovering_by_dnd (item, hovering);

  if (hovering)
    st_widget_add_style_pseudo_class (ST_WIDGET (item), "drop");
  else
    st_widget_remove_style_pseudo_class (ST_WIDGET (item), "drop");
}

static gboolean
folder_icon_can_accept (ShellAppViewItem *item,
                        GObject          *source)
{
  ShellFolderIcon *self = SHELL_FOLDER_ICON (item);
  ShellBaseAppView *view;
  g_auto (GStrv) apps = NULL;
  const char *source_id;

  if (!SHELL_IS_APP_ICON (source))
    return FALSE;

  view = get_view_from_icon (CLUTTER_ACTOR (source));
  if (!SHELL_IS_APP_DISPLAY (view))
    return FALSE;

  source_id = shell_app_view_item_get_id (SHELL_APP_VIEW_ITEM (source));
  apps = g_settings_get_strv (self->folder, "apps");
  if (g_strv_contains ((const char * const *) apps, source_id))
    return FALSE;

  return TRUE;
}

static gboolean
folder_icon_accept_drop (ShellAppViewItem *item,
                         GObject          *source,
                         ClutterActor     *actor,
                         float             x,
                         float             y,
                         uint32_t          time)
{
  ShellFolderIcon *self = SHELL_FOLDER_ICON (item);
  ShellAppViewItemClass *parent_class = SHELL_APP_VIEW_ITEM_CLASS (shell_folder_icon_parent_class);

  if (!parent_class->accept_drop (item, source, actor, x, y, time))
    return FALSE;

  shell_folder_view_add_app (self->view, shell_app_icon_get_app (SHELL_APP_ICON (source)));

  return TRUE;
}

static ShellDragMotionResult
folder_icon_drag_motion (ShellAppViewItem *item,
                         ShellDragEvent   *event)
{
  ShellAppViewItemClass *parent_class = SHELL_APP_VIEW_ITEM_CLASS (shell_folder_icon_parent_class);
  GObject *source = shell_drag_event_get_source (event);

  if (!SHELL_APP_VIEW_ITEM_GET_CLASS (item)->can_accept (item, source))
    SHELL_APP_VIEW_ITEM_GET_CLASS (item)->set_hovering_by_dnd (item, FALSE);

  return parent_class->drag_motion (item, event);
}

static ClutterActor *
folder_icon_get_drag_actor (ShellDragSource *source)
{
  ShellFolderIcon *self = SHELL_FOLDER_ICON (source);
  ShellAppViewItem *item = SHELL_APP_VIEW_ITEM (self);
  ShellBaseIcon *icon;

  icon = g_object_new (SHELL_TYPE_BASE_ICON,
                       "label-text", shell_app_view_item_get_name (item),
                       "show-label", item_priv (item)->icon != NULL &&
                                     shell_base_icon_get_label (item_priv (item)->icon) != NULL,
                       NULL);
  shell_base_icon_set_create_icon_func (icon, folder_icon_create_icon_cb,
                                        g_object_ref (self), g_object_unref);
  st_widget_set_style_class_name (ST_WIDGET (icon),
                                  st_widget_get_style_class_name (ST_WIDGET (self)));

  return CLUTTER_ACTOR (icon);
}

static ClutterActor *
folder_icon_get_drag_actor_source (ShellDragSource *source)
{
  return CLUTTER_ACTOR (source);
}

static void
shell_folder_icon_drag_source_iface_init (ShellDragSourceInterface *iface)
{
  iface->get_drag_actor = folder_icon_get_drag_actor;
  iface->get_drag_actor_source = folder_icon_get_drag_actor_source;
}

static void
folder_icon_update_name (ShellFolderIcon *self)
{
  ShellAppViewItem *item = SHELL_APP_VIEW_ITEM (self);
  g_autofree char *name = get_folder_name (self->folder);
  StLabel *label;

  if (g_strcmp0 (shell_app_view_item_get_name (item), name) == 0)
    return;

  shell_app_view_item_set_name (item, name);

  label = item_priv (item)->icon != NULL ? shell_base_icon_get_label (item_priv (item)->icon) : NULL;
  if (label != NULL)
    st_label_set_text (label, name);
}

static void
folder_icon_sync (ShellFolderIcon *self)
{
  g_autoptr (GPtrArray) items = NULL;
  ShellAppViewItemPrivate *item = item_priv (SHELL_APP_VIEW_ITEM (self));

  if (self->view == NULL || shell_folder_view_get_deleting_folder (self->view))
    return;

  g_signal_emit (self, folder_icon_signals[FOLDER_ICON_APPS_CHANGED], 0);
  folder_icon_update_name (self);

  items = shell_base_app_view_get_all_items (SHELL_BASE_APP_VIEW (self->view));
  if (items->len > 0)
    clutter_actor_show (CLUTTER_ACTOR (self));
  else
    clutter_actor_hide (CLUTTER_ACTOR (self));

  if (item->icon != NULL)
    shell_base_icon_update (item->icon);
}

static void
folder_icon_settings_changed_cb (GSettings       *settings,
                                 const char      *key,
                                 ShellFolderIcon *self)
{
  folder_icon_sync (self);
}

static void
folder_icon_favorites_changed_cb (ShellAppFavorites *favorites,
                                  ShellFolderIcon   *self)
{
  folder_icon_sync (self);
}

static void
folder_icon_destroy (ClutterActor *actor)
{
  ShellFolderIcon *self = SHELL_FOLDER_ICON (actor);

  if (self->dialog != NULL)
    clutter_actor_destroy (CLUTTER_ACTOR (self->dialog));
  else if (self->view != NULL)
    clutter_actor_destroy (CLUTTER_ACTOR (self->view));

  CLUTTER_ACTOR_CLASS (shell_folder_icon_parent_class)->destroy (actor);
}

static void
shell_folder_icon_constructed (GObject *object)
{
  ShellFolderIcon *self = SHELL_FOLDER_ICON (object);
  ShellAppViewItem *item = SHELL_APP_VIEW_ITEM (self);
  ShellWindowManager *window_manager;
  ShellBaseIcon *icon;

  G_OBJECT_CLASS (shell_folder_icon_parent_class)->constructed (object);

  g_return_if_fail (self->folder_id != NULL && self->folder_path != NULL);

  st_widget_set_style_class_name (ST_WIDGET (self), "overview-tile app-folder");
  st_button_set_button_mask (ST_BUTTON (self), ST_BUTTON_PRIMARY);
  st_widget_set_can_focus (ST_WIDGET (self), TRUE);

  clutter_actor_add_accessible_state (CLUTTER_ACTOR (self), ATK_STATE_EXPANDABLE);

  shell_app_view_item_set_id (item, self->folder_id);
  shell_app_view_item_set_name (item, "");

  self->folder = g_settings_new_with_path ("org.gnome.desktop.app-folders.folder",
                                           self->folder_path);

  icon = g_object_new (SHELL_TYPE_BASE_ICON,
                       "set-size-manually", TRUE,
                       NULL);
  shell_base_icon_set_create_icon_func (icon, folder_icon_create_icon_cb, self, NULL);
  shell_app_view_item_set_icon (item, icon);
  st_bin_set_child (ST_BIN (self), CLUTTER_ACTOR (icon));
  if (shell_base_icon_get_label (icon) != NULL)
    st_widget_set_label_actor (ST_WIDGET (self),
                               CLUTTER_ACTOR (shell_base_icon_get_label (icon)));

  window_manager = shell_app_view_item_get_window_manager (item);
  self->view = g_object_ref_sink (shell_folder_view_new (self->folder, self->folder_id,
                                                         self->parent_view, window_manager));
  if (self->parent_view != NULL)
    shell_base_app_view_set_drag_icon_size (SHELL_BASE_APP_VIEW (self->view),
                                            shell_base_app_view_get_drag_icon_size (SHELL_BASE_APP_VIEW (self->parent_view)));

  g_signal_connect_object (self->folder, "changed",
                           G_CALLBACK (folder_icon_settings_changed_cb), self, 0);
  g_signal_connect_object (shell_app_favorites_get_default (), "changed",
                           G_CALLBACK (folder_icon_favorites_changed_cb), self, 0);
  folder_icon_sync (self);
}

static void
shell_folder_icon_dispose (GObject *object)
{
  ShellFolderIcon *self = SHELL_FOLDER_ICON (object);

  g_clear_weak_pointer (&self->parent_view);

  G_OBJECT_CLASS (shell_folder_icon_parent_class)->dispose (object);

  /* after the chain-up: the destroy vfunc, run by it, destroys the dialog */
  g_clear_weak_pointer (&self->dialog);
  g_clear_object (&self->view);
}

static void
shell_folder_icon_finalize (GObject *object)
{
  ShellFolderIcon *self = SHELL_FOLDER_ICON (object);

  g_clear_object (&self->folder);
  g_clear_pointer (&self->folder_id, g_free);
  g_clear_pointer (&self->folder_path, g_free);

  G_OBJECT_CLASS (shell_folder_icon_parent_class)->finalize (object);
}

static void
shell_folder_icon_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  ShellFolderIcon *self = SHELL_FOLDER_ICON (object);

  switch (prop_id)
    {
    case FOLDER_ICON_PROP_FOLDER_ID:
      self->folder_id = g_value_dup_string (value);
      break;

    case FOLDER_ICON_PROP_FOLDER_PATH:
      self->folder_path = g_value_dup_string (value);
      break;

    case FOLDER_ICON_PROP_PARENT_VIEW:
      g_clear_weak_pointer (&self->parent_view);
      g_set_weak_pointer (&self->parent_view, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_folder_icon_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  ShellFolderIcon *self = SHELL_FOLDER_ICON (object);

  switch (prop_id)
    {
    case FOLDER_ICON_PROP_FOLDER_ID:
      g_value_set_string (value, self->folder_id);
      break;

    case FOLDER_ICON_PROP_FOLDER_PATH:
      g_value_set_string (value, self->folder_path);
      break;

    case FOLDER_ICON_PROP_PARENT_VIEW:
      g_value_set_object (value, self->parent_view);
      break;

    case FOLDER_ICON_PROP_VIEW:
      g_value_set_object (value, self->view);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_folder_icon_class_init (ShellFolderIconClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);
  ShellAppViewItemClass *item_class = SHELL_APP_VIEW_ITEM_CLASS (klass);

  object_class->constructed = shell_folder_icon_constructed;
  object_class->dispose = shell_folder_icon_dispose;
  object_class->finalize = shell_folder_icon_finalize;
  object_class->set_property = shell_folder_icon_set_property;
  object_class->get_property = shell_folder_icon_get_property;

  actor_class->destroy = folder_icon_destroy;
  actor_class->unmap = folder_icon_unmap;

  button_class->clicked = folder_icon_clicked;

  item_class->can_accept = folder_icon_can_accept;
  item_class->set_hovering_by_dnd = folder_icon_set_hovering_by_dnd;
  item_class->accept_drop = folder_icon_accept_drop;
  item_class->drag_motion = folder_icon_drag_motion;

  /**
   * ShellFolderIcon:folder-id:
   *
   * The id of the folder (`id`).
   */
  folder_icon_props[FOLDER_ICON_PROP_FOLDER_ID] =
    g_param_spec_string ("folder-id", NULL, NULL, NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellFolderIcon:folder-path:
   *
   * The GSettings path of the folder (`path`).
   */
  folder_icon_props[FOLDER_ICON_PROP_FOLDER_PATH] =
    g_param_spec_string ("folder-path", NULL, NULL, NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellFolderIcon:parent-view:
   *
   * The application grid the folder is part of (`parentView`).
   */
  folder_icon_props[FOLDER_ICON_PROP_PARENT_VIEW] =
    g_param_spec_object ("parent-view", NULL, NULL,
                         SHELL_TYPE_APP_DISPLAY,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellFolderIcon:view:
   *
   * The view of the applications of the folder (`view`).
   */
  folder_icon_props[FOLDER_ICON_PROP_VIEW] =
    g_param_spec_object ("view", NULL, NULL,
                         SHELL_TYPE_FOLDER_VIEW,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, FOLDER_ICON_N_PROPS, folder_icon_props);

  /**
   * ShellFolderIcon::apps-changed:
   * @self: the #ShellFolderIcon
   *
   * Emitted when the applications of the folder or the favorites changed.
   */
  folder_icon_signals[FOLDER_ICON_APPS_CHANGED] =
    g_signal_new ("apps-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_folder_icon_init (ShellFolderIcon *self)
{
}

/**
 * shell_folder_icon_new:
 * @folder_id: the id of the folder
 * @path: the GSettings path of the folder
 * @parent_view: the application grid the folder is part of
 * @window_manager: (nullable): the window manager the overview is reached
 *   through
 *
 * Creates the icon of an application folder (`new FolderIcon ()`). It can
 * be dragged if the layout of the grid can be changed.
 *
 * Returns: (transfer full): a new #ShellFolderIcon
 */
ShellFolderIcon *
shell_folder_icon_new (const char         *folder_id,
                       const char         *path,
                       ShellAppDisplay    *parent_view,
                       ShellWindowManager *window_manager)
{
  return g_object_new (SHELL_TYPE_FOLDER_ICON,
                       "folder-id", folder_id,
                       "folder-path", path,
                       "parent-view", parent_view,
                       "window-manager", window_manager,
                       "is-draggable", is_settings_writable ("app-picker-layout"),
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellAppDisplay                                                           */
/* ------------------------------------------------------------------------ */

struct _ShellAppDisplay
{
  ShellBaseAppView parent;

  ShellPageManager *page_manager;
  GSettings *folder_settings;
  GPtrArray *folder_icons; /* ShellFolderIcon */
  GList *app_info_list; /* GAppInfo */

  ShellAppFolderDialog *current_dialog; /* weak */
  gboolean displaying_dialog;

  ShellAppIcon *placeholder;

  ShellDeferredWork *redisplay_work;
  gboolean overview_hidden_connected;
};

G_DEFINE_FINAL_TYPE (ShellAppDisplay, shell_app_display, SHELL_TYPE_BASE_APP_VIEW)

static gboolean
app_display_binding_next_cb (GObject             *object,
                             const char          *action_name,
                             guint                key_val,
                             ClutterModifierType  modifiers,
                             gpointer             user_data)
{
  ShellIconGrid *grid = shell_base_app_view_get_grid (SHELL_BASE_APP_VIEW (object));

  shell_base_app_view_go_to_page (SHELL_BASE_APP_VIEW (object),
                                  shell_icon_grid_get_current_page (grid) + 1, TRUE);
  return CLUTTER_EVENT_STOP;
}

static gboolean
app_display_binding_prev_cb (GObject             *object,
                             const char          *action_name,
                             guint                key_val,
                             ClutterModifierType  modifiers,
                             gpointer             user_data)
{
  ShellIconGrid *grid = shell_base_app_view_get_grid (SHELL_BASE_APP_VIEW (object));

  shell_base_app_view_go_to_page (SHELL_BASE_APP_VIEW (object),
                                  shell_icon_grid_get_current_page (grid) - 1, TRUE);
  return CLUTTER_EVENT_STOP;
}

static gboolean
app_display_binding_first_cb (GObject             *object,
                              const char          *action_name,
                              guint                key_val,
                              ClutterModifierType  modifiers,
                              gpointer             user_data)
{
  shell_base_app_view_go_to_page (SHELL_BASE_APP_VIEW (object), 0, TRUE);
  return CLUTTER_EVENT_STOP;
}

static gboolean
app_display_binding_last_cb (GObject             *object,
                             const char          *action_name,
                             guint                key_val,
                             ClutterModifierType  modifiers,
                             gpointer             user_data)
{
  ShellIconGrid *grid = shell_base_app_view_get_grid (SHELL_BASE_APP_VIEW (object));

  shell_base_app_view_go_to_page (SHELL_BASE_APP_VIEW (object),
                                  shell_icon_grid_get_n_pages (grid) - 1, TRUE);
  return CLUTTER_EVENT_STOP;
}

static gboolean
app_display_navigate_focus (StWidget        *widget,
                            ClutterActor    *from,
                            StDirectionType  direction)
{
  StWidgetClass *parent_class = ST_WIDGET_CLASS (shell_app_display_parent_class);

  if (from == CLUTTER_ACTOR (widget) && direction == ST_DIR_DOWN)
    return parent_class->navigate_focus (widget, NULL, ST_DIR_TAB_FORWARD);

  return parent_class->navigate_focus (widget, from, direction);
}

/**
 * shell_app_display_get_app_infos:
 * @self: a #ShellAppDisplay
 *
 * Returns: (transfer none) (element-type GAppInfo): the installed
 *   applications which are not favorites and are shown to the user
 *   (`getAppInfos ()`)
 */
GList *
shell_app_display_get_app_infos (ShellAppDisplay *self)
{
  g_return_val_if_fail (SHELL_IS_APP_DISPLAY (self), NULL);

  return self->app_info_list;
}

static void
app_display_redisplay (ShellBaseAppView *view)
{
  ShellAppDisplay *self = SHELL_APP_DISPLAY (view);
  guint i;

  for (i = 0; self->folder_icons != NULL && i < self->folder_icons->len; i++)
    {
      ShellFolderIcon *icon = g_ptr_array_index (self->folder_icons, i);

      if (icon->view != NULL)
        shell_base_app_view_redisplay (SHELL_BASE_APP_VIEW (icon->view));
    }

  SHELL_BASE_APP_VIEW_CLASS (shell_app_display_parent_class)->redisplay (view);
}

static void
app_display_save_pages (ShellAppDisplay *self)
{
  ShellIconGrid *grid = shell_base_app_view_get_grid (SHELL_BASE_APP_VIEW (self));
  GVariantBuilder pages;
  int n_pages = shell_icon_grid_get_n_pages (grid);
  int i;

  g_variant_builder_init (&pages, G_VARIANT_TYPE ("aa{sv}"));

  for (i = 0; i < n_pages; i++)
    {
      GList *page_items = get_visible_items_at_page (grid, i);
      GVariantBuilder page_data;
      GList *l;
      int index = 0;

      g_variant_builder_init (&page_data, G_VARIANT_TYPE ("a{sv}"));

      for (l = page_items; l != NULL; l = l->next, index++)
        {
          GVariantBuilder properties;

          g_variant_builder_init (&properties, G_VARIANT_TYPE ("a{sv}"));
          g_variant_builder_add (&properties, "{sv}", "position", g_variant_new_int32 (index));
          g_variant_builder_add (&page_data, "{sv}",
                                 shell_app_view_item_get_id (SHELL_APP_VIEW_ITEM (l->data)),
                                 g_variant_builder_end (&properties));
        }

      g_list_free (page_items);
      g_variant_builder_add_value (&pages, g_variant_builder_end (&page_data));
    }

  shell_page_manager_set_pages (self->page_manager, g_variant_builder_end (&pages));
}

static void
app_display_ensure_default_folders (ShellAppDisplay *self)
{
  g_autoptr (GVariant) user_value = NULL;
  g_auto (GStrv) children = NULL;
  g_autofree char *path = NULL;
  ShellAppSystem *app_system;
  g_autoptr (GPtrArray) folders = NULL;
  guint i;

  user_value = g_settings_get_user_value (self->folder_settings, "folder-children");
  children = g_settings_get_strv (self->folder_settings, "folder-children");
  if (user_value != NULL || g_strv_length (children) > 0)
    return;

  app_system = shell_app_system_get_default ();

  folders = g_ptr_array_new ();
  for (i = 0; i < G_N_ELEMENTS (default_folders); i++)
    g_ptr_array_add (folders, (gpointer) default_folders[i].id);
  g_ptr_array_add (folders, NULL);
  g_settings_set_strv (self->folder_settings, "folder-children",
                       (const char * const *) folders->pdata);

  g_object_get (self->folder_settings, "path", &path, NULL);
  for (i = 0; i < G_N_ELEMENTS (default_folders); i++)
    {
      const DefaultFolder *folder = &default_folders[i];
      g_autofree char *child_path = g_strdup_printf ("%sfolders/%s/", path, folder->id);
      g_autoptr (GSettings) child = NULL;
      g_autoptr (GPtrArray) filtered_apps = g_ptr_array_new ();
      const char * const *app;

      child = g_settings_new_with_path ("org.gnome.desktop.app-folders.folder", child_path);
      g_settings_set_string (child, "name", folder->name);
      g_settings_set_boolean (child, "translate", TRUE);
      if (folder->categories != NULL)
        g_settings_set_strv (child, "categories", folder->categories);

      for (app = folder->apps; app != NULL && *app != NULL; app++)
        {
          if (shell_app_system_lookup_app (app_system, *app) != NULL)
            g_ptr_array_add (filtered_apps, (gpointer) *app);
        }

      if (filtered_apps->len > 0)
        {
          g_ptr_array_add (filtered_apps, NULL);
          g_settings_set_strv (child, "apps", (const char * const *) filtered_apps->pdata);
        }
    }
}

static void
app_display_ensure_placeholder (ShellAppDisplay *self,
                                ShellAppViewItem *source)
{
  ShellBaseAppView *view = SHELL_BASE_APP_VIEW (self);
  ShellApp *app;
  gboolean is_draggable;

  if (self->placeholder != NULL)
    return;

  app = shell_app_system_lookup_app (shell_app_system_get_default (),
                                     shell_app_view_item_get_id (source));
  if (app == NULL)
    return;

  is_draggable = is_settings_writable ("favorite-apps") ||
                 is_settings_writable ("app-picker-layout");

  self->placeholder = g_object_ref_sink (g_object_new (SHELL_TYPE_APP_ICON,
                                                       "app", app,
                                                       "is-draggable", is_draggable,
                                                       "window-manager", shell_base_app_view_get_window_manager (view),
                                                       "drag-icon-size", shell_base_app_view_get_drag_icon_size (view),
                                                       NULL));
  shell_app_view_item_scale_and_fade (SHELL_APP_VIEW_ITEM (self->placeholder));
  shell_base_app_view_redisplay (view);
}

static void
app_display_remove_placeholder (ShellAppDisplay *self)
{
  g_autoptr (ShellAppIcon) placeholder = NULL;

  if (self->placeholder == NULL)
    return;

  placeholder = g_steal_pointer (&self->placeholder);
  shell_app_view_item_undo_scale_and_fade (SHELL_APP_VIEW_ITEM (placeholder));
  shell_base_app_view_redisplay (SHELL_BASE_APP_VIEW (self));
}

static void
app_display_get_item_position (ShellBaseAppView *view,
                               ShellAppViewItem *item,
                               int              *page,
                               int              *position)
{
  ShellAppDisplay *self = SHELL_APP_DISPLAY (view);

  if (self->placeholder != NULL && item == SHELL_APP_VIEW_ITEM (self->placeholder))
    {
      shell_icon_grid_get_item_position (shell_base_app_view_get_grid (view),
                                         CLUTTER_ACTOR (item), page, position);

      if (*page == -1)
        *page = shell_icon_grid_get_current_page (shell_base_app_view_get_grid (view));

      return;
    }

  shell_page_manager_get_app_position (self->page_manager,
                                       shell_app_view_item_get_id (item), page, position);
}

static int
app_display_compare_items (ShellBaseAppView *view,
                           ShellAppViewItem *a,
                           ShellAppViewItem *b)
{
  int a_page, a_position, b_page, b_position;

  app_display_get_item_position (view, a, &a_page, &a_position);
  app_display_get_item_position (view, b, &b_page, &b_position);

  if (a_page == -1 && b_page == -1)
    return g_utf8_collate (shell_app_view_item_get_name (a), shell_app_view_item_get_name (b));
  else if (a_page == -1)
    return 1;
  else if (b_page == -1)
    return -1;

  if (a_page != b_page)
    return a_page - b_page;

  return a_position - b_position;
}

static void
app_display_folder_apps_changed_cb (ShellFolderIcon *icon,
                                    ShellAppDisplay *self)
{
  shell_base_app_view_redisplay (SHELL_BASE_APP_VIEW (self));
  app_display_save_pages (self);
}

static GPtrArray *
app_display_load_apps (ShellBaseAppView *view)
{
  ShellAppDisplay *self = SHELL_APP_DISPLAY (view);
  ShellWindowManager *window_manager = shell_base_app_view_get_window_manager (view);
  int drag_icon_size = shell_base_app_view_get_drag_icon_size (view);
  GPtrArray *app_icons = g_ptr_array_new_with_free_func (g_object_unref);
  ShellAppSystem *app_system = shell_app_system_get_default ();
  ShellAppFavorites *app_favorites = shell_app_favorites_get_default ();
  ShellParentalControlsManager *parental = shell_parental_controls_manager_get_default ();
  g_autoptr (GHashTable) apps_inside_folders = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (GPtrArray) apps = g_ptr_array_new ();
  g_auto (GStrv) folders = NULL;
  g_autofree char *settings_path = NULL;
  gboolean is_draggable;
  GList *installed, *l;
  char **id;
  guint i;

  g_list_free_full (self->app_info_list, g_object_unref);
  self->app_info_list = NULL;

  installed = shell_app_system_get_installed (app_system);
  for (l = installed; l != NULL; l = l->next)
    {
      GAppInfo *app_info = l->data;
      const char *app_id = g_app_info_get_id (app_info);

      /* catch invalid file encodings */
      if (app_id == NULL)
        continue;

      if (!shell_app_favorites_is_favorite (app_favorites, app_id) &&
          shell_parental_controls_manager_should_show_app (parental, app_info))
        self->app_info_list = g_list_prepend (self->app_info_list, g_object_ref (app_info));
    }
  self->app_info_list = g_list_reverse (self->app_info_list);

  for (l = self->app_info_list; l != NULL; l = l->next)
    g_ptr_array_add (apps, (gpointer) g_app_info_get_id (l->data));

  g_ptr_array_set_size (self->folder_icons, 0);

  g_object_get (self->folder_settings, "path", &settings_path, NULL);
  folders = g_settings_get_strv (self->folder_settings, "folder-children");
  for (id = folders; *id != NULL; id++)
    {
      g_autofree char *path = g_strdup_printf ("%sfolders/%s/", settings_path, *id);
      ShellAppViewItem *existing = base_app_view_lookup_item (view, *id);
      ShellFolderIcon *icon;
      g_auto (GStrv) app_ids = NULL;
      char **app_id;

      if (existing != NULL && SHELL_IS_FOLDER_ICON (existing))
        {
          icon = g_object_ref (SHELL_FOLDER_ICON (existing));
        }
      else
        {
          icon = g_object_ref_sink (shell_folder_icon_new (*id, path, self, window_manager));
          shell_app_view_item_set_drag_icon_size (SHELL_APP_VIEW_ITEM (icon), drag_icon_size);
          g_signal_connect_object (icon, "apps-changed",
                                   G_CALLBACK (app_display_folder_apps_changed_cb), self, 0);
        }

      /* Don't try to display empty folders */
      if (!clutter_actor_is_visible (CLUTTER_ACTOR (icon)))
        {
          if (existing == NULL)
            clutter_actor_destroy (CLUTTER_ACTOR (icon));
          g_object_unref (icon);
          continue;
        }

      g_ptr_array_add (app_icons, g_object_ref (icon));
      g_ptr_array_add (self->folder_icons, icon);

      app_ids = shell_folder_icon_get_app_ids (icon);
      for (app_id = app_ids; *app_id != NULL; app_id++)
        g_hash_table_add (apps_inside_folders, g_strdup (*app_id));
    }

  /* Allow dragging of the icon only if the Dash would accept a drop to
   * change favorite-apps. There are no other possible drop targets from
   * the app picker, so there's no other need for a drag to start,
   * at least on single-monitor setups.
   * This also disables drag-to-launch on multi-monitor setups,
   * but we hope that is not used much. */
  is_draggable = is_settings_writable ("favorite-apps") ||
                 is_settings_writable ("app-picker-layout");

  for (i = 0; i < apps->len; i++)
    {
      const char *app_id = g_ptr_array_index (apps, i);
      ShellAppViewItem *icon;
      ShellApp *app;

      if (g_hash_table_contains (apps_inside_folders, app_id))
        continue;

      icon = base_app_view_lookup_item (view, app_id);
      if (icon != NULL)
        {
          g_ptr_array_add (app_icons, g_object_ref (icon));
          continue;
        }

      app = shell_app_system_lookup_app (app_system, app_id);
      if (app == NULL)
        continue;

      icon = g_object_ref_sink (g_object_new (SHELL_TYPE_APP_ICON,
                                              "app", app,
                                              "is-draggable", is_draggable,
                                              "window-manager", window_manager,
                                              "drag-icon-size", drag_icon_size,
                                              NULL));
      g_ptr_array_add (app_icons, icon);
    }

  /* At last, if there's a placeholder available, add it */
  if (self->placeholder != NULL)
    g_ptr_array_add (app_icons, g_object_ref (self->placeholder));

  return app_icons;
}

static void
app_display_go_to_page (ShellBaseAppView *view,
                        int               page_number,
                        gboolean          animate)
{
  ShellAppDisplay *self = SHELL_APP_DISPLAY (view);
  ShellIconGrid *grid = shell_base_app_view_get_grid (view);

  page_number = CLAMP (page_number, 0, MAX (shell_icon_grid_get_n_pages (grid) - 1, 0));

  if (shell_icon_grid_get_current_page (grid) == page_number &&
      self->displaying_dialog && self->current_dialog != NULL)
    return;
  if (self->displaying_dialog && self->current_dialog != NULL)
    shell_app_folder_dialog_popdown (self->current_dialog, NULL, NULL, NULL);

  SHELL_BASE_APP_VIEW_CLASS (shell_app_display_parent_class)->go_to_page (view, page_number, animate);
}

static gboolean
app_display_can_scroll (ShellBaseAppView *view)
{
  ShellAppDisplay *self = SHELL_APP_DISPLAY (view);

  return !(self->displaying_dialog ||
           !clutter_actor_get_reactive (CLUTTER_ACTOR (shell_base_app_view_get_scroll_view (view))));
}

static void
app_display_dialog_destroyed_cb (ClutterActor    *dialog,
                                 ShellAppDisplay *self)
{
  if (self->current_dialog == (ShellAppFolderDialog *) dialog)
    g_clear_weak_pointer (&self->current_dialog);
}

static void
app_display_dialog_open_state_changed_cb (ShellAppFolderDialog *dialog,
                                          gboolean              is_open,
                                          ShellAppDisplay      *self)
{
  if (self->current_dialog != NULL)
    {
      g_signal_handlers_disconnect_by_func (self->current_dialog,
                                            app_display_dialog_destroyed_cb, self);
      g_clear_weak_pointer (&self->current_dialog);
    }

  if (is_open)
    {
      g_set_weak_pointer (&self->current_dialog, dialog);
      g_signal_connect_object (dialog, "destroy",
                               G_CALLBACK (app_display_dialog_destroyed_cb), self, 0);
    }

  self->displaying_dialog = is_open;
}

/**
 * shell_app_display_add_folder_dialog:
 * @self: a #ShellAppDisplay
 * @dialog: the dialog of a folder
 *
 * Adds the dialog of a folder to the overview, and keeps track of whether it
 * is open (`addFolderDialog ()`).
 */
void
shell_app_display_add_folder_dialog (ShellAppDisplay      *self,
                                     ShellAppFolderDialog *dialog)
{
  ShellLayoutManager *layout_manager = get_layout_manager ();

  g_return_if_fail (SHELL_IS_APP_DISPLAY (self));
  g_return_if_fail (SHELL_IS_APP_FOLDER_DIALOG (dialog));

  if (layout_manager != NULL)
    clutter_actor_add_child (CLUTTER_ACTOR (shell_layout_manager_get_overview_group (layout_manager)),
                             CLUTTER_ACTOR (dialog));

  g_signal_connect_object (dialog, "open-state-changed",
                           G_CALLBACK (app_display_dialog_open_state_changed_cb), self, 0);
}

static void
app_display_maybe_move_item (ShellBaseAppView *view,
                             ShellDragEvent   *event)
{
  ShellAppDisplay *self = SHELL_APP_DISPLAY (view);
  ShellAppViewItem *source;

  if (self->placeholder != NULL)
    source = SHELL_APP_VIEW_ITEM (self->placeholder);
  else if (SHELL_IS_APP_VIEW_ITEM (shell_drag_event_get_source (event)))
    source = SHELL_APP_VIEW_ITEM (shell_drag_event_get_source (event));
  else
    return;

  base_app_view_maybe_move_item_for (view, event, source);
}

static void
app_display_drag_begin (ShellBaseAppView *view,
                        GObject          *source)
{
  ShellAppDisplay *self = SHELL_APP_DISPLAY (view);
  ShellBaseAppView *source_view;

  SHELL_BASE_APP_VIEW_CLASS (shell_app_display_parent_class)->drag_begin (view, source);

  if (!SHELL_IS_APP_VIEW_ITEM (source))
    return;

  /* When dragging from a folder dialog, the dragged app icon doesn't
   * exist in AppDisplay. We work around that by adding a placeholder
   * icon that is either destroyed on cancel, or becomes the effective
   * new icon when dropped. */
  source_view = get_view_from_icon (CLUTTER_ACTOR (source));
  if (SHELL_IS_FOLDER_VIEW (source_view) ||
      shell_app_favorites_is_favorite (shell_app_favorites_get_default (),
                                       shell_app_view_item_get_id (SHELL_APP_VIEW_ITEM (source))))
    app_display_ensure_placeholder (self, SHELL_APP_VIEW_ITEM (source));
}

static ShellDragMotionResult
app_display_drag_motion (ShellBaseAppView *view,
                         ShellDragEvent   *event)
{
  ShellAppDisplay *self = SHELL_APP_DISPLAY (view);

  if (self->current_dialog != NULL)
    return SHELL_DRAG_MOTION_RESULT_CONTINUE;

  return SHELL_BASE_APP_VIEW_CLASS (shell_app_display_parent_class)->drag_motion (view, event);
}

static void
app_display_drag_end (ShellBaseAppView *view)
{
  ShellAppDisplay *self = SHELL_APP_DISPLAY (view);

  SHELL_BASE_APP_VIEW_CLASS (shell_app_display_parent_class)->drag_end (view);
  app_display_remove_placeholder (self);
  app_display_save_pages (self);
}

static void
app_display_drag_cancelled (ShellBaseAppView *view,
                            GObject          *source)
{
  ShellBaseAppView *source_view;

  if (CLUTTER_IS_ACTOR (source))
    {
      source_view = get_view_from_icon (CLUTTER_ACTOR (source));

      if (SHELL_IS_FOLDER_VIEW (source_view))
        return;
    }

  SHELL_BASE_APP_VIEW_CLASS (shell_app_display_parent_class)->drag_cancelled (view, source);
}

static gboolean
app_display_accept_drop (ShellBaseAppView *view,
                         GObject          *source)
{
  ShellAppDisplay *self = SHELL_APP_DISPLAY (view);
  ShellBaseAppView *source_view;
  ShellAppFavorites *favorites = shell_app_favorites_get_default ();
  const char *source_id;

  if (!SHELL_BASE_APP_VIEW_CLASS (shell_app_display_parent_class)->accept_drop (view, source))
    return FALSE;

  app_display_save_pages (self);

  source_view = get_view_from_icon (CLUTTER_ACTOR (source));
  if (SHELL_IS_FOLDER_VIEW (source_view))
    shell_folder_view_remove_app (SHELL_FOLDER_VIEW (source_view),
                                  item_get_app (SHELL_APP_VIEW_ITEM (source)));

  if (self->current_dialog != NULL)
    shell_app_folder_dialog_popdown (self->current_dialog, NULL, NULL, NULL);

  source_id = shell_app_view_item_get_id (SHELL_APP_VIEW_ITEM (source));
  if (shell_app_favorites_is_favorite (favorites, source_id))
    shell_app_favorites_remove_favorite (favorites, source_id);

  return TRUE;
}

/**
 * shell_app_display_create_folder:
 * @self: a #ShellAppDisplay
 * @apps: (array zero-terminated=1): the ids of the applications to put in the
 *   new folder; the first is the one the folder is created in place of
 *
 * Creates a folder with the applications (`createFolder ()`).
 *
 * Returns: %TRUE if the folder was created
 */
gboolean
shell_app_display_create_folder (ShellAppDisplay    *self,
                                 const char * const *apps)
{
  ShellBaseAppView *view;
  ShellIconGrid *grid;
  g_autofree char *new_folder_id = NULL;
  g_autofree char *settings_path = NULL;
  g_autofree char *new_folder_path = NULL;
  g_autofree char *folder_name = NULL;
  g_autoptr (GSettings) new_folder_settings = NULL;
  g_autoptr (GPtrArray) children = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GList) app_items = NULL;
  g_auto (GStrv) folders = NULL;
  ShellAppViewItem *first_item, *folder_item;
  int folder_page, folder_position, adjust = 0;
  GList *app_list = NULL;
  const char * const *id;
  char **folder_id;

  g_return_val_if_fail (SHELL_IS_APP_DISPLAY (self), FALSE);
  g_return_val_if_fail (apps != NULL && apps[0] != NULL, FALSE);

  view = SHELL_BASE_APP_VIEW (self);
  grid = shell_base_app_view_get_grid (view);

  /* The hovered AppIcon always passes its own id as the first
   * one, and this is where we want the folder to be created */
  first_item = base_app_view_lookup_item (view, apps[0]);
  if (first_item == NULL)
    return FALSE;

  for (id = apps; *id != NULL; id++)
    {
      if (base_app_view_lookup_item (view, *id) == NULL)
        return FALSE;
    }

  new_folder_id = g_uuid_string_random ();

  folders = g_settings_get_strv (self->folder_settings, "folder-children");
  for (folder_id = folders; *folder_id != NULL; folder_id++)
    g_ptr_array_add (children, g_strdup (*folder_id));
  g_ptr_array_add (children, g_strdup (new_folder_id));
  g_ptr_array_add (children, NULL);
  g_settings_set_strv (self->folder_settings, "folder-children",
                       (const char * const *) children->pdata);

  /* Create the new folder */
  g_object_get (self->folder_settings, "path", &settings_path, NULL);
  new_folder_path = g_strconcat (settings_path, "folders/", new_folder_id, "/", NULL);
  new_folder_settings = g_settings_new_with_path ("org.gnome.desktop.app-folders.folder",
                                                  new_folder_path);

  shell_icon_grid_get_item_position (grid, CLUTTER_ACTOR (first_item),
                                     &folder_page, &folder_position);

  /* Adjust the final position */
  for (id = apps; *id != NULL; id++)
    {
      int page, position;

      shell_icon_grid_get_item_position (grid,
                                         CLUTTER_ACTOR (base_app_view_lookup_item (view, *id)),
                                         &page, &position);
      if (page == folder_page && position < folder_position)
        adjust++;
    }
  folder_position -= adjust;

  for (id = apps; *id != NULL; id++)
    {
      ShellApp *app = item_get_app (base_app_view_lookup_item (view, *id));

      if (app != NULL)
        app_list = g_list_append (app_list, app);
    }

  folder_name = find_best_folder_name (app_list);
  g_list_free (app_list);
  if (folder_name == NULL)
    folder_name = g_strdup (_("Unnamed Folder"));

  g_settings_delay (new_folder_settings);
  g_settings_set_string (new_folder_settings, "name", folder_name);
  g_settings_set_strv (new_folder_settings, "apps", apps);
  g_settings_apply (new_folder_settings);

  shell_base_app_view_redisplay (view);

  /* Move the folder to where the icon target icon was */
  folder_item = base_app_view_lookup_item (view, new_folder_id);
  if (folder_item != NULL)
    base_app_view_move_item (view, folder_item, folder_page, folder_position);
  app_display_save_pages (self);

  return TRUE;
}

static void
app_display_overview_visible_changed_cb (GObject         *window_manager,
                                         GParamSpec      *pspec,
                                         ShellAppDisplay *self)
{
  /* The overview's `hidden` */
  if (!shell_window_manager_get_overview_visible (SHELL_WINDOW_MANAGER (window_manager)))
    shell_base_app_view_go_to_page (SHELL_BASE_APP_VIEW (self), 0, TRUE);
}

static void
app_display_redisplay_work_cb (gpointer user_data)
{
  ShellAppDisplay *self = user_data;
  ShellWindowManager *window_manager = shell_base_app_view_get_window_manager (SHELL_BASE_APP_VIEW (self));

  shell_base_app_view_redisplay (SHELL_BASE_APP_VIEW (self));

  if (!self->overview_hidden_connected && window_manager != NULL)
    {
      self->overview_hidden_connected = TRUE;
      g_signal_connect_object (window_manager, "notify::overview-visible",
                               G_CALLBACK (app_display_overview_visible_changed_cb), self, 0);
    }
}

static void
app_display_queue_redisplay (ShellAppDisplay *self)
{
  if (self->redisplay_work != NULL)
    shell_deferred_work_queue (self->redisplay_work);
}

static void
app_display_installed_changed_cb (ShellAppSystem  *app_system,
                                  ShellAppDisplay *self)
{
  app_display_queue_redisplay (self);
}

static void
app_display_folder_children_changed_cb (GSettings       *settings,
                                        const char      *key,
                                        ShellAppDisplay *self)
{
  app_display_queue_redisplay (self);
}

static void
app_display_page_manager_layout_changed_cb (ShellPageManager *manager,
                                            ShellAppDisplay  *self)
{
  shell_base_app_view_redisplay (SHELL_BASE_APP_VIEW (self));
}

static void
shell_app_display_constructed (GObject *object)
{
  ShellAppDisplay *self = SHELL_APP_DISPLAY (object);

  G_OBJECT_CLASS (shell_app_display_parent_class)->constructed (object);

  clutter_actor_set_reactive (CLUTTER_ACTOR (self), TRUE);

  self->page_manager = shell_page_manager_new (get_shell_settings ());
  g_signal_connect_object (self->page_manager, "layout-changed",
                           G_CALLBACK (app_display_page_manager_layout_changed_cb), self, 0);

  self->folder_icons = g_ptr_array_new_with_free_func (g_object_unref);

  self->redisplay_work = shell_deferred_work_new (CLUTTER_ACTOR (self),
                                                  app_display_redisplay_work_cb, self);

  g_signal_connect_object (shell_app_system_get_default (), "installed-changed",
                           G_CALLBACK (app_display_installed_changed_cb), self, 0);
  self->folder_settings = g_settings_new ("org.gnome.desktop.app-folders");
  app_display_ensure_default_folders (self);
  g_signal_connect_object (self->folder_settings, "changed::folder-children",
                           G_CALLBACK (app_display_folder_children_changed_cb), self, 0);
}

static void
shell_app_display_dispose (GObject *object)
{
  ShellAppDisplay *self = SHELL_APP_DISPLAY (object);

  g_clear_weak_pointer (&self->current_dialog);
  g_clear_pointer (&self->folder_icons, g_ptr_array_unref);
  g_clear_object (&self->placeholder);
  g_clear_object (&self->page_manager);
  g_clear_object (&self->folder_settings);

  G_OBJECT_CLASS (shell_app_display_parent_class)->dispose (object);
}

static void
shell_app_display_finalize (GObject *object)
{
  ShellAppDisplay *self = SHELL_APP_DISPLAY (object);

  g_list_free_full (self->app_info_list, g_object_unref);

  G_OBJECT_CLASS (shell_app_display_parent_class)->finalize (object);
}

static void
shell_app_display_class_init (ShellAppDisplayClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);
  ShellBaseAppViewClass *view_class = SHELL_BASE_APP_VIEW_CLASS (klass);
  ClutterBindingPool *binding_pool;

  object_class->constructed = shell_app_display_constructed;
  object_class->dispose = shell_app_display_dispose;
  object_class->finalize = shell_app_display_finalize;

  widget_class->navigate_focus = app_display_navigate_focus;

  view_class->redisplay = app_display_redisplay;
  view_class->get_item_position = app_display_get_item_position;
  view_class->compare_items = app_display_compare_items;
  view_class->load_apps = app_display_load_apps;
  view_class->go_to_page = app_display_go_to_page;
  view_class->can_scroll = app_display_can_scroll;
  view_class->maybe_move_item = app_display_maybe_move_item;
  view_class->drag_begin = app_display_drag_begin;
  view_class->drag_motion = app_display_drag_motion;
  view_class->drag_end = app_display_drag_end;
  view_class->drag_cancelled = app_display_drag_cancelled;
  view_class->accept_drop = app_display_accept_drop;

  binding_pool = clutter_binding_pool_get_for_class (klass);

  clutter_binding_pool_install_action (binding_pool, "next", CLUTTER_KEY_Page_Down, 0,
                                       G_CALLBACK (app_display_binding_next_cb), NULL, NULL);
  clutter_binding_pool_install_action (binding_pool, "prev", CLUTTER_KEY_Page_Up, 0,
                                       G_CALLBACK (app_display_binding_prev_cb), NULL, NULL);
  clutter_binding_pool_install_action (binding_pool, "first", CLUTTER_KEY_Home, 0,
                                       G_CALLBACK (app_display_binding_first_cb), NULL, NULL);
  clutter_binding_pool_install_action (binding_pool, "last", CLUTTER_KEY_End, 0,
                                       G_CALLBACK (app_display_binding_last_cb), NULL, NULL);
}

static void
shell_app_display_init (ShellAppDisplay *self)
{
}

/**
 * shell_app_display_new:
 * @window_manager: (nullable): the window manager the overview is reached
 *   through
 *
 * Creates the application grid of the overview (`new AppDisplay ()`).
 *
 * Returns: (transfer full): a new #ShellAppDisplay
 */
ShellAppDisplay *
shell_app_display_new (ShellWindowManager *window_manager)
{
  return g_object_new (SHELL_TYPE_APP_DISPLAY,
                       "window-manager", window_manager,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellSystemActionIcon (private, SystemActionIcon)                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SYSTEM_ACTION_ICON (shell_system_action_icon_get_type ())
G_DECLARE_FINAL_TYPE (ShellSystemActionIcon, shell_system_action_icon,
                      SHELL, SYSTEM_ACTION_ICON, ShellGridSearchResult)

struct _ShellSystemActionIcon
{
  ShellGridSearchResult parent;
};

G_DEFINE_FINAL_TYPE (ShellSystemActionIcon, shell_system_action_icon,
                     SHELL_TYPE_GRID_SEARCH_RESULT)

static void
system_action_icon_activate (ShellSearchResult *result)
{
  ShellSearchResultMeta *meta = shell_search_result_get_meta_info (result);
  ShellSearchProvider *provider = shell_search_result_get_provider (result);
  g_autoptr (GError) error = NULL;

  if (!shell_system_actions_activate_action (shell_system_actions_get_default (),
                                             shell_search_result_meta_get_id (meta), &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);

  if (SHELL_IS_APP_SEARCH_PROVIDER (provider))
    {
      ShellWindowManager *window_manager = NULL;

      g_object_get (provider, "window-manager", &window_manager, NULL);
      request_overview_hide (window_manager);
      g_clear_object (&window_manager);
    }
}

static void
shell_system_action_icon_class_init (ShellSystemActionIconClass *klass)
{
  SHELL_SEARCH_RESULT_CLASS (klass)->activate = system_action_icon_activate;
}

static void
shell_system_action_icon_init (ShellSystemActionIcon *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellAppSearchProvider                                                    */
/* ------------------------------------------------------------------------ */

enum
{
  PROVIDER_PROP_0,
  PROVIDER_PROP_WINDOW_MANAGER,
  PROVIDER_PROP_DRAG_ICON_SIZE,
  /* The properties of ShellSearchProvider */
  PROVIDER_PROP_ID,
  PROVIDER_PROP_APP_INFO,
  PROVIDER_PROP_CAN_LAUNCH_SEARCH,
  PROVIDER_PROP_IS_REMOTE_PROVIDER,
  PROVIDER_PROP_MAX_RESULTS,
  PROVIDER_N_PROPS
};

static GParamSpec *provider_props[PROVIDER_PROP_ID] = { NULL, };

struct _ShellAppSearchProvider
{
  GObject parent;

  ShellWindowManager *window_manager; /* weak */
  int drag_icon_size;

  ShellAppSystem *app_sys;
  ShellSystemActions *system_actions;
  ShellParentalControlsManager *parental_controls_manager;
};

static void shell_app_search_provider_iface_init (ShellSearchProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellAppSearchProvider, shell_app_search_provider, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (SHELL_TYPE_SEARCH_PROVIDER,
                                                      shell_app_search_provider_iface_init))

typedef struct
{
  double scale_factor;
  char *icon_name;
} SystemActionIconData;

static void
system_action_icon_data_free (gpointer data)
{
  SystemActionIconData *icon_data = data;

  g_free (icon_data->icon_name);
  g_free (icon_data);
}

static ClutterActor *
system_action_create_icon_cb (int      size,
                              gpointer user_data)
{
  SystemActionIconData *data = user_data;

  return g_object_new (ST_TYPE_ICON,
                       "icon-name", data->icon_name,
                       "width", (float) (size * data->scale_factor),
                       "height", (float) (size * data->scale_factor),
                       "style-class", "system-action-icon",
                       NULL);
}

static ClutterActor *
app_create_icon_cb (int      size,
                    gpointer user_data)
{
  return shell_app_create_icon_texture (SHELL_APP (user_data), size);
}

static void
app_search_provider_get_result_metas (ShellSearchProvider *provider,
                                      const char * const  *ids,
                                      GCancellable        *cancellable,
                                      ShellSearchRequest  *request)
{
  ShellAppSearchProvider *self = SHELL_APP_SEARCH_PROVIDER (provider);
  StThemeContext *context =
    st_theme_context_get_for_stage (shell_global_get_stage (shell_global_get ()));
  double scale_factor = st_theme_context_get_scale_factor (context);
  g_autoptr (GPtrArray) metas = g_ptr_array_new_with_free_func (g_object_unref);
  const char * const *id;

  for (id = ids; id != NULL && *id != NULL; id++)
    {
      ShellSearchResultMeta *meta;

      if (g_str_has_suffix (*id, ".desktop"))
        {
          ShellApp *app = shell_app_system_lookup_app (self->app_sys, *id);

          if (app == NULL)
            {
              /* The JS threw for an application that was uninstalled meanwhile */
              meta = shell_search_result_meta_new (*id, *id);
            }
          else
            {
              meta = shell_search_result_meta_new (shell_app_get_id (app), shell_app_get_name (app));
              shell_search_result_meta_set_create_icon_func (meta, app_create_icon_cb,
                                                             g_object_ref (app), g_object_unref);
            }
        }
      else
        {
          const char *name = shell_system_actions_get_name (self->system_actions, *id);
          const char *icon_name = shell_system_actions_get_icon_name (self->system_actions, *id);
          SystemActionIconData *data = g_new0 (SystemActionIconData, 1);

          data->scale_factor = scale_factor;
          data->icon_name = g_strdup (icon_name);

          meta = shell_search_result_meta_new (*id, name != NULL ? name : *id);
          shell_search_result_meta_set_create_icon_func (meta, system_action_create_icon_cb,
                                                         data, system_action_icon_data_free);
        }

      g_ptr_array_add (metas, meta);
    }

  shell_search_request_return_metas (request, (ShellSearchResultMeta **) metas->pdata, metas->len);
}

typedef struct
{
  GWeakRef provider;
  char **terms;
  ShellSearchRequest *request;
  gulong handler_id;
} PendingSearch;

static void
pending_search_free (gpointer data,
                     GClosure *closure)
{
  PendingSearch *pending = data;

  g_weak_ref_clear (&pending->provider);
  g_strfreev (pending->terms);
  g_clear_object (&pending->request);
  g_free (pending);
}

static void app_search_provider_search (ShellAppSearchProvider *self,
                                        const char * const     *terms,
                                        ShellSearchRequest     *request);

static void
parental_controls_initialized_cb (ShellParentalControlsManager *manager,
                                  PendingSearch                *pending)
{
  g_autoptr (ShellAppSearchProvider) self = g_weak_ref_get (&pending->provider);
  g_auto (GStrv) terms = NULL;
  g_autoptr (ShellSearchRequest) request = NULL;

  if (!shell_parental_controls_manager_get_initialized (manager))
    return;

  terms = g_steal_pointer (&pending->terms);
  request = g_steal_pointer (&pending->request);
  g_signal_handler_disconnect (manager, pending->handler_id);

  if (self != NULL)
    app_search_provider_search (self, (const char * const *) terms, request);
}

static int
usage_compare_cb (gconstpointer a,
                  gconstpointer b,
                  gpointer      user_data)
{
  const char *id_a = *(const char * const *) a;
  const char *id_b = *(const char * const *) b;

  return shell_app_usage_compare (user_data, id_a, id_b);
}

static void
app_search_provider_search (ShellAppSearchProvider *self,
                            const char * const     *terms,
                            ShellSearchRequest     *request)
{
  g_autofree char *query = NULL;
  g_autoptr (GStrvBuilder) results = g_strv_builder_new ();
  g_auto (GStrv) result_strv = NULL;
  g_auto (GStrv) action_matches = NULL;
  ShellAppUsage *usage;
  char ***groups, ***group;

  /* Defer until the parental controls manager is initialised, so the
   * results can be filtered correctly. */
  if (!shell_parental_controls_manager_get_initialized (self->parental_controls_manager))
    {
      PendingSearch *pending = g_new0 (PendingSearch, 1);

      g_weak_ref_init (&pending->provider, self);
      pending->terms = g_strdupv ((char **) terms);
      pending->request = g_object_ref (request);
      pending->handler_id = g_signal_connect_data (self->parental_controls_manager,
                                                   "app-filter-changed",
                                                   G_CALLBACK (parental_controls_initialized_cb),
                                                   pending, pending_search_free, 0);
      return;
    }

  query = g_strjoinv (" ", (char **) terms);
  groups = shell_app_system_search (query);
  usage = shell_app_usage_get_default ();

  for (group = groups; group != NULL && *group != NULL; group++)
    {
      g_autoptr (GPtrArray) filtered = g_ptr_array_new_with_free_func (g_free);
      char **app_id;
      guint i;

      for (app_id = *group; *app_id != NULL; app_id++)
        {
          ShellApp *app = shell_app_system_lookup_app (self->app_sys, *app_id);

          if (app != NULL &&
              shell_parental_controls_manager_should_show_app (self->parental_controls_manager,
                                                               app_info_of (app)))
            g_ptr_array_add (filtered, g_strdup (*app_id));
        }

      g_ptr_array_sort_values_with_data (filtered, usage_compare_cb, usage);
      for (i = 0; i < filtered->len; i++)
        g_strv_builder_add (results, g_ptr_array_index (filtered, i));
    }

  for (group = groups; group != NULL && *group != NULL; group++)
    g_strfreev (*group);
  g_free (groups);

  action_matches = shell_system_actions_get_matching_actions (self->system_actions, terms);
  if (action_matches != NULL)
    g_strv_builder_addv (results, (const char **) action_matches);

  result_strv = g_strv_builder_end (results);
  shell_search_request_return_results (request, (const char * const *) result_strv);
}

static void
app_search_provider_get_initial_result_set (ShellSearchProvider *provider,
                                            const char * const  *terms,
                                            GCancellable        *cancellable,
                                            ShellSearchRequest  *request)
{
  app_search_provider_search (SHELL_APP_SEARCH_PROVIDER (provider), terms, request);
}

static void
app_search_provider_get_subsearch_result_set (ShellSearchProvider *provider,
                                              const char * const  *previous_results,
                                              const char * const  *terms,
                                              GCancellable        *cancellable,
                                              ShellSearchRequest  *request)
{
  app_search_provider_search (SHELL_APP_SEARCH_PROVIDER (provider), terms, request);
}

static ClutterActor *
app_search_provider_create_result_object (ShellSearchProvider   *provider,
                                          ShellSearchResultMeta *meta)
{
  ShellAppSearchProvider *self = SHELL_APP_SEARCH_PROVIDER (provider);
  const char *id = shell_search_result_meta_get_id (meta);

  if (g_str_has_suffix (id, ".desktop"))
    {
      ShellApp *app = shell_app_system_lookup_app (self->app_sys, id);

      if (app == NULL)
        return NULL;

      return g_object_new (SHELL_TYPE_APP_ICON,
                           "app", app,
                           "expand-title-on-hover", FALSE,
                           "window-manager", self->window_manager,
                           "drag-icon-size", self->drag_icon_size,
                           NULL);
    }

  return g_object_new (SHELL_TYPE_SYSTEM_ACTION_ICON,
                       "provider", self,
                       "meta-info", meta,
                       NULL);
}

static void
shell_app_search_provider_iface_init (ShellSearchProviderInterface *iface)
{
  iface->get_result_metas = app_search_provider_get_result_metas;
  iface->get_initial_result_set = app_search_provider_get_initial_result_set;
  iface->get_subsearch_result_set = app_search_provider_get_subsearch_result_set;
  iface->create_result_object = app_search_provider_create_result_object;
}

static void
shell_app_search_provider_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellAppSearchProvider *self = SHELL_APP_SEARCH_PROVIDER (object);

  switch (prop_id)
    {
    case PROVIDER_PROP_WINDOW_MANAGER:
      g_clear_weak_pointer (&self->window_manager);
      g_set_weak_pointer (&self->window_manager, g_value_get_object (value));
      break;

    case PROVIDER_PROP_DRAG_ICON_SIZE:
      self->drag_icon_size = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_app_search_provider_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellAppSearchProvider *self = SHELL_APP_SEARCH_PROVIDER (object);

  switch (prop_id)
    {
    case PROVIDER_PROP_WINDOW_MANAGER:
      g_value_set_object (value, self->window_manager);
      break;

    case PROVIDER_PROP_DRAG_ICON_SIZE:
      g_value_set_int (value, self->drag_icon_size);
      break;

    case PROVIDER_PROP_ID:
      g_value_set_string (value, "applications");
      break;

    case PROVIDER_PROP_APP_INFO:
      g_value_set_object (value, NULL);
      break;

    case PROVIDER_PROP_CAN_LAUNCH_SEARCH:
      g_value_set_boolean (value, FALSE);
      break;

    case PROVIDER_PROP_IS_REMOTE_PROVIDER:
      g_value_set_boolean (value, FALSE);
      break;

    case PROVIDER_PROP_MAX_RESULTS:
      g_value_set_int (value, 6);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_app_search_provider_dispose (GObject *object)
{
  ShellAppSearchProvider *self = SHELL_APP_SEARCH_PROVIDER (object);

  g_clear_weak_pointer (&self->window_manager);

  G_OBJECT_CLASS (shell_app_search_provider_parent_class)->dispose (object);
}

static void
shell_app_search_provider_class_init (ShellAppSearchProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_app_search_provider_dispose;
  object_class->set_property = shell_app_search_provider_set_property;
  object_class->get_property = shell_app_search_provider_get_property;

  /**
   * ShellAppSearchProvider:window-manager:
   *
   * The window manager the overview is reached through, which the icons of
   * the results use.
   */
  provider_props[PROVIDER_PROP_WINDOW_MANAGER] =
    g_param_spec_object ("window-manager", NULL, NULL,
                         SHELL_TYPE_WINDOW_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellAppSearchProvider:drag-icon-size:
   *
   * The size of the icon that is dragged from a result, in logical pixels
   * (the icon size of the dash). It is given to the icons made from now on.
   */
  provider_props[PROVIDER_PROP_DRAG_ICON_SIZE] =
    g_param_spec_int ("drag-icon-size", NULL, NULL,
                      1, G_MAXINT, 64,
                      G_PARAM_READWRITE |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROVIDER_PROP_ID, provider_props);

  g_object_class_override_property (object_class, PROVIDER_PROP_ID, "id");
  g_object_class_override_property (object_class, PROVIDER_PROP_APP_INFO, "app-info");
  g_object_class_override_property (object_class, PROVIDER_PROP_CAN_LAUNCH_SEARCH, "can-launch-search");
  g_object_class_override_property (object_class, PROVIDER_PROP_IS_REMOTE_PROVIDER, "is-remote-provider");
  g_object_class_override_property (object_class, PROVIDER_PROP_MAX_RESULTS, "max-results");
}

static void
shell_app_search_provider_init (ShellAppSearchProvider *self)
{
  self->drag_icon_size = 64;
  self->app_sys = shell_app_system_get_default ();
  self->system_actions = shell_system_actions_get_default ();
  self->parental_controls_manager = shell_parental_controls_manager_get_default ();
}

/**
 * shell_app_search_provider_new:
 * @window_manager: (nullable): the window manager the overview is reached
 *   through
 *
 * Creates the provider of the installed applications and the system actions
 * (`new AppSearchProvider ()`).
 *
 * Returns: (transfer full): a new #ShellAppSearchProvider
 */
ShellAppSearchProvider *
shell_app_search_provider_new (ShellWindowManager *window_manager)
{
  return g_object_new (SHELL_TYPE_APP_SEARCH_PROVIDER,
                       "window-manager", window_manager,
                       NULL);
}
