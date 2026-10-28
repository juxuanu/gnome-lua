/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * quick-settings.c: The items of the quick settings menu of the top bar, the
 * menu which lays them out in a grid and the menu an item opens below itself.
 * Port of js/ui/quickSettings.js.
 *
 * Differences from the JS classes, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `QuickSettingsItem._init ({hasMenu})`, `QuickToggle._init (params)` and
 *    the others took a params object; they are construct properties now
 *    (`has-menu`, `title`, `icon-name`, ...). The defaults the JS applied
 *    before `...params` (`style_class`, `accessible_role`, `can_focus`) are set
 *    in constructed (), so a property passed to the constructor still wins.
 *    `can_focus: false, reactive: false, hasMenu: true` of the slider and
 *    `hasMenu: true` of the menu toggle were applied after `...params`; the
 *    latter is enforced, the other two are only defaults.
 *  - `menu` is a property (type #ShellQuickToggleMenu). It is created for
 *    `has-menu` items, and can be set to the menu of another item, as the
 *    system item of the quick settings does with the menu of its shutdown
 *    item. An item destroys the menu it created when it is destroyed.
 *  - `_box`, `_icon`, `_title`, `_subtitle` and `_menuButton` are the getters
 *    shell_quick_toggle_get_box () and its siblings. `QuickSlider.slider` is
 *    the read-only property `slider`.
 *  - The icon-name and gicon properties of the toggles are forwarded to the
 *    #StIcon (a menu toggle forwards them to its inner toggle) instead of being
 *    stored next to a bidirectional binding.
 *  - `QuickToggleMenu.setHeader (icon, title, subtitle)` takes an icon name;
 *    a #GIcon goes to shell_quick_toggle_menu_set_header_gicon ().
 *  - The `label` of a #ShellQuickToggle still warns and redirects to `title`.
 *  - `Main.layoutManager` "system-modal-opened" is handled by
 *    #ShellPopupMenu, which closes an open menu with the full animation, as
 *    the extra handler of the JS menu did.
 *  - The active item menu of the quick settings menu is only forgotten when
 *    that menu closes (the JS forgot it when any item menu closed).
 *  - A grid without rows has a spacing of zero (the JS preferred height was
 *    reduced by one row spacing).
 *  - The effect which dims the menu is enabled before it is eased, so an
 *    instant undim (animations off) leaves it disabled.
 */

#include "config.h"

#include <math.h>

#include <atk/atk.h>
#include <clutter/clutter.h>
#include <cogl/cogl.h>
#include <glib/gi18n-lib.h>
#include <graphene.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-later.h>

#include "shell-global.h"
#include "shell-hooks.h"
#include "ui/environment.h"
#include "ui/quick-settings.h"

#define DIM_BRIGHTNESS -0.4
#define POPUP_ANIMATION_TIME 400 /* milliseconds */

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static ClutterStage *
get_stage (void)
{
  return shell_global_get_stage (shell_global_get ());
}

static MetaLaters *
get_laters (void)
{
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  MetaCompositor *compositor;

  if (display == NULL)
    return NULL;

  compositor = meta_display_get_compositor (display);
  if (compositor == NULL)
    return NULL;

  return meta_compositor_get_laters (compositor);
}

static ShellEaseParams
make_ease_params (int                  duration,
                  ClutterAnimationMode mode)
{
  ShellEaseParams params = {
    .duration = duration,
    .delay = -1,
    .mode = mode,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  return params;
}

/* Keeps a weak reference to an object through an asynchronous completion */
typedef struct
{
  GWeakRef object;
  int duration;
  gboolean flag;
} EaseData;

static EaseData *
ease_data_new (gpointer object,
               int      duration,
               gboolean flag)
{
  EaseData *data = g_new0 (EaseData, 1);

  g_weak_ref_init (&data->object, object);
  data->duration = duration;
  data->flag = flag;
  return data;
}

static void
ease_data_free (gpointer user_data)
{
  EaseData *data = user_data;

  g_weak_ref_clear (&data->object);
  g_free (data);
}

/* `layout.hookup_style (container)` of environment.js for a grid layout */
static void
header_style_changed_cb (StWidget          *container,
                         ClutterGridLayout *layout)
{
  StThemeNode *node = st_widget_get_theme_node (container);
  double length;

  if (st_theme_node_lookup_length (node, "spacing-rows", FALSE, &length))
    clutter_grid_layout_set_row_spacing (layout, (guint) length);

  if (st_theme_node_lookup_length (node, "spacing-columns", FALSE, &length))
    clutter_grid_layout_set_column_spacing (layout, (guint) length);
}

static void
hookup_grid_style (ClutterGridLayout *layout,
                   StWidget          *container)
{
  g_signal_connect_object (container, "style-changed",
                           G_CALLBACK (header_style_changed_cb), layout, 0);
}

static void
set_actor_visible (ClutterActor *actor,
                   gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

static ClutterActor *
new_widget (const char *style_class)
{
  return g_object_new (ST_TYPE_WIDGET, "style-class", style_class, NULL);
}

/* ======================================================================== */
/* ShellQuickToggleMenu                                                     */
/* ======================================================================== */

typedef struct
{
  ClutterActor *header;          /* weak, a child of the box */
  ClutterGridLayout *header_layout; /* weak, the layout manager of the header */
  StIcon *header_icon;           /* weak */
  StLabel *header_title;         /* weak */
  StLabel *header_subtitle;      /* weak */
  ClutterActor *header_spacer;   /* weak */
} ShellQuickToggleMenuPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellQuickToggleMenu, shell_quick_toggle_menu,
                            SHELL_TYPE_POPUP_MENU_BASE)

static ClutterGridPosition
header_side (ShellQuickToggleMenu *self)
{
  ClutterActor *actor = shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (self));

  if (actor != NULL &&
      clutter_actor_get_text_direction (actor) == CLUTTER_TEXT_DIRECTION_RTL)
    return CLUTTER_GRID_POSITION_LEFT;

  return CLUTTER_GRID_POSITION_RIGHT;
}

static void
quick_toggle_menu_sync_checked (ShellQuickToggleMenu *self)
{
  ShellQuickToggleMenuPrivate *priv = shell_quick_toggle_menu_get_instance_private (self);
  ClutterActor *source = shell_popup_menu_base_get_source_actor (SHELL_POPUP_MENU_BASE (self));

  if (priv->header_icon == NULL)
    return;

  if (ST_IS_BUTTON (source) && st_button_get_checked (ST_BUTTON (source)))
    st_widget_add_style_class_name (ST_WIDGET (priv->header_icon), "active");
  else
    st_widget_remove_style_class_name (ST_WIDGET (priv->header_icon), "active");
}

static void
quick_toggle_menu_source_checked_cb (GObject              *source,
                                     GParamSpec           *pspec,
                                     ShellQuickToggleMenu *self)
{
  quick_toggle_menu_sync_checked (self);
}

static void
shell_quick_toggle_menu_constructed (GObject *object)
{
  ShellQuickToggleMenu *self = SHELL_QUICK_TOGGLE_MENU (object);
  ShellQuickToggleMenuPrivate *priv = shell_quick_toggle_menu_get_instance_private (self);
  ShellPopupMenuBase *base = SHELL_POPUP_MENU_BASE (self);
  ClutterActor *source_actor;
  ClutterActor *actor;
  ClutterActor *box;
  ClutterConstraint *constraint;
  ClutterLayoutManager *header_layout;
  ClutterGridPosition side;

  G_OBJECT_CLASS (shell_quick_toggle_menu_parent_class)->constructed (object);

  source_actor = shell_popup_menu_base_get_source_actor (base);
  box = CLUTTER_ACTOR (shell_popup_menu_base_get_box (base));

  constraint = clutter_bind_constraint_new (source_actor, CLUTTER_BIND_Y, 0);
  if (source_actor != NULL)
    g_object_bind_property (source_actor, "height",
                            constraint, "offset",
                            G_BINDING_DEFAULT);

  actor = g_object_new (ST_TYPE_WIDGET,
                        "layout-manager", clutter_bin_layout_new (),
                        "style-class", "quick-toggle-menu-container",
                        "reactive", TRUE,
                        "x-expand", TRUE,
                        "y-expand", FALSE,
                        "height", 0.f,
                        NULL);
  clutter_actor_add_constraint (actor, constraint);
  shell_popup_menu_base_set_actor (base, actor);
  clutter_actor_add_child (actor, box);
  st_widget_set_keynav_flags (ST_WIDGET (actor), ST_KEYNAV_FLAG_WRAP_VERTICALLY);

  st_focus_manager_add_group (st_focus_manager_get_for_stage (get_stage ()),
                              ST_WIDGET (actor));

  header_layout = clutter_grid_layout_new ();
  priv->header = g_object_new (ST_TYPE_WIDGET,
                               "style-class", "header",
                               "layout-manager", header_layout,
                               "visible", FALSE,
                               NULL);
  g_set_weak_pointer (&priv->header_layout, CLUTTER_GRID_LAYOUT (header_layout));
  hookup_grid_style (CLUTTER_GRID_LAYOUT (header_layout), ST_WIDGET (priv->header));
  clutter_actor_add_child (box, priv->header);
  /* The box owns the header now; ours is a weak pointer */
  g_object_add_weak_pointer (G_OBJECT (priv->header), (gpointer *) &priv->header);

  priv->header_icon = ST_ICON (st_icon_new ());
  st_widget_set_style_class_name (ST_WIDGET (priv->header_icon), "icon");
  clutter_actor_set_y_align (CLUTTER_ACTOR (priv->header_icon), CLUTTER_ACTOR_ALIGN_CENTER);
  g_object_add_weak_pointer (G_OBJECT (priv->header_icon), (gpointer *) &priv->header_icon);

  priv->header_title = ST_LABEL (st_label_new (NULL));
  st_widget_set_style_class_name (ST_WIDGET (priv->header_title), "title");
  clutter_actor_set_y_align (CLUTTER_ACTOR (priv->header_title), CLUTTER_ACTOR_ALIGN_CENTER);
  clutter_actor_set_y_expand (CLUTTER_ACTOR (priv->header_title), TRUE);
  g_object_add_weak_pointer (G_OBJECT (priv->header_title), (gpointer *) &priv->header_title);

  priv->header_subtitle = ST_LABEL (st_label_new (NULL));
  st_widget_set_style_class_name (ST_WIDGET (priv->header_subtitle), "subtitle");
  clutter_actor_set_y_align (CLUTTER_ACTOR (priv->header_subtitle), CLUTTER_ACTOR_ALIGN_CENTER);
  g_object_add_weak_pointer (G_OBJECT (priv->header_subtitle), (gpointer *) &priv->header_subtitle);

  priv->header_spacer = clutter_actor_new ();
  clutter_actor_set_x_expand (priv->header_spacer, TRUE);
  g_object_add_weak_pointer (G_OBJECT (priv->header_spacer), (gpointer *) &priv->header_spacer);

  side = header_side (self);

  clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (header_layout),
                              CLUTTER_ACTOR (priv->header_icon), 0, 0, 1, 2);
  clutter_grid_layout_attach_next_to (CLUTTER_GRID_LAYOUT (header_layout),
                                      CLUTTER_ACTOR (priv->header_title),
                                      CLUTTER_ACTOR (priv->header_icon),
                                      side, 1, 1);
  clutter_grid_layout_attach_next_to (CLUTTER_GRID_LAYOUT (header_layout),
                                      priv->header_spacer,
                                      CLUTTER_ACTOR (priv->header_title),
                                      side, 1, 1);
  clutter_grid_layout_attach_next_to (CLUTTER_GRID_LAYOUT (header_layout),
                                      CLUTTER_ACTOR (priv->header_subtitle),
                                      CLUTTER_ACTOR (priv->header_title),
                                      CLUTTER_GRID_POSITION_BOTTOM, 1, 1);

  if (source_actor != NULL)
    g_signal_connect_object (source_actor, "notify::checked",
                             G_CALLBACK (quick_toggle_menu_source_checked_cb),
                             self, 0);
  quick_toggle_menu_sync_checked (self);
}

static void
shell_quick_toggle_menu_dispose (GObject *object)
{
  ShellQuickToggleMenu *self = SHELL_QUICK_TOGGLE_MENU (object);
  ShellQuickToggleMenuPrivate *priv = shell_quick_toggle_menu_get_instance_private (self);

  g_clear_weak_pointer (&priv->header_layout);
  if (priv->header != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->header), (gpointer *) &priv->header);
  if (priv->header_icon != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->header_icon), (gpointer *) &priv->header_icon);
  if (priv->header_title != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->header_title), (gpointer *) &priv->header_title);
  if (priv->header_subtitle != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->header_subtitle), (gpointer *) &priv->header_subtitle);
  if (priv->header_spacer != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->header_spacer), (gpointer *) &priv->header_spacer);
  priv->header = NULL;
  priv->header_icon = NULL;
  priv->header_title = NULL;
  priv->header_subtitle = NULL;
  priv->header_spacer = NULL;

  G_OBJECT_CLASS (shell_quick_toggle_menu_parent_class)->dispose (object);
}

static void
quick_toggle_menu_open_done_cb (gboolean finished,
                                gpointer user_data)
{
  EaseData *data = user_data;
  g_autoptr (ShellQuickToggleMenu) self = g_weak_ref_get (&data->object);
  ShellEaseParams params;
  ClutterActor *actor;
  ClutterActor *box;

  if (!finished || self == NULL)
    return;

  actor = shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (self));
  box = CLUTTER_ACTOR (shell_popup_menu_base_get_box (SHELL_POPUP_MENU_BASE (self)));
  if (actor == NULL)
    return;

  params = make_ease_params (data->duration, 0);
  shell_actor_ease (box, &params, NULL, NULL, NULL, "opacity", 255, NULL);
  clutter_actor_set_height (actor, -1);
}

static gboolean
shell_quick_toggle_menu_real_open (ShellPopupMenuBase *base,
                                   ShellPopupAnimation animate)
{
  ShellPopupMenuBaseClass *parent_class =
    SHELL_POPUP_MENU_BASE_CLASS (shell_quick_toggle_menu_parent_class);
  ClutterActor *actor;
  ClutterActor *box;
  float previous_height;
  float target_height = 0;
  float distance;
  int duration;
  int actor_duration = 0;
  ShellEaseParams params;
  ClutterActor *source = shell_popup_menu_base_get_source_actor (base);

  /* The menu opens below its item: nothing to open while the item is not
   * on screen (the quick settings menu is closed or the item hidden), which
   * is what menu:open () of the extension API reports as false */
  if (source != NULL && !clutter_actor_is_mapped (source))
    return FALSE;

  if (!parent_class->open (base, animate))
    return FALSE;

  actor = shell_popup_menu_base_get_actor (base);
  box = CLUTTER_ACTOR (shell_popup_menu_base_get_box (base));

  previous_height = clutter_actor_get_height (actor);
  clutter_actor_set_height (actor, -1);
  clutter_actor_get_preferred_height (actor, -1, &target_height, NULL);
  clutter_actor_set_height (actor, previous_height);
  distance = fabsf (target_height - previous_height);

  duration = animate != SHELL_POPUP_ANIMATION_NONE ? POPUP_ANIMATION_TIME / 2 : 0;
  if (target_height > 0)
    actor_duration = (int) (duration * (distance / target_height));

  clutter_actor_set_opacity (box, 0);

  params = make_ease_params (actor_duration, 0);
  shell_actor_ease (actor, &params,
                    quick_toggle_menu_open_done_cb,
                    ease_data_new (base, duration, FALSE), ease_data_free,
                    "height", (double) target_height,
                    NULL);
  return TRUE;
}

static void
quick_toggle_menu_close_done_cb (gboolean finished,
                                 gpointer user_data)
{
  EaseData *data = user_data;
  g_autoptr (ShellQuickToggleMenu) self = g_weak_ref_get (&data->object);
  ClutterActor *actor;

  if (!finished || self == NULL)
    return;

  actor = shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (self));
  if (actor != NULL)
    clutter_actor_hide (actor);
  g_signal_emit_by_name (self, "menu-closed");
}

static void
quick_toggle_menu_box_hidden_cb (gboolean finished,
                                 gpointer user_data)
{
  EaseData *data = user_data;
  g_autoptr (ShellQuickToggleMenu) self = g_weak_ref_get (&data->object);
  ShellEaseParams params;
  ClutterActor *actor;

  if (!finished || self == NULL)
    return;

  actor = shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (self));
  if (actor == NULL)
    return;

  params = make_ease_params (data->duration, 0);
  shell_actor_ease (actor, &params,
                    quick_toggle_menu_close_done_cb,
                    ease_data_new (self, data->duration, FALSE), ease_data_free,
                    "height", 0.,
                    NULL);
}

static gboolean
shell_quick_toggle_menu_real_close (ShellPopupMenuBase *base,
                                    ShellPopupAnimation animate)
{
  ShellPopupMenuBaseClass *parent_class =
    SHELL_POPUP_MENU_BASE_CLASS (shell_quick_toggle_menu_parent_class);
  ClutterActor *box;
  ShellEaseParams params;
  int duration;
  int opacity;

  if (!parent_class->close (base, animate))
    return FALSE;

  box = CLUTTER_ACTOR (shell_popup_menu_base_get_box (base));
  opacity = clutter_actor_get_opacity (box);
  duration = animate != SHELL_POPUP_ANIMATION_NONE ? POPUP_ANIMATION_TIME / 2 : 0;

  params = make_ease_params ((int) (duration * (opacity / 255.0)), 0);
  shell_actor_ease (box, &params,
                    quick_toggle_menu_box_hidden_cb,
                    ease_data_new (base, duration, FALSE), ease_data_free,
                    "opacity", 0,
                    NULL);
  return TRUE;
}

static void
shell_quick_toggle_menu_class_init (ShellQuickToggleMenuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupMenuBaseClass *base_class = SHELL_POPUP_MENU_BASE_CLASS (klass);

  object_class->constructed = shell_quick_toggle_menu_constructed;
  object_class->dispose = shell_quick_toggle_menu_dispose;

  base_class->open = shell_quick_toggle_menu_real_open;
  base_class->close = shell_quick_toggle_menu_real_close;
}

static void
shell_quick_toggle_menu_init (ShellQuickToggleMenu *self)
{
}

/**
 * shell_quick_toggle_menu_new:
 * @source_actor: the item the menu belongs to
 *
 * Creates the menu which an item opens below itself (`new QuickToggleMenu
 * (sourceActor)`).
 *
 * Returns: (transfer full): a new #ShellQuickToggleMenu
 */
ShellQuickToggleMenu *
shell_quick_toggle_menu_new (ClutterActor *source_actor)
{
  g_return_val_if_fail (CLUTTER_IS_ACTOR (source_actor), NULL);

  return g_object_new (SHELL_TYPE_QUICK_TOGGLE_MENU,
                       "source-actor", source_actor,
                       "style-class", "quick-toggle-menu",
                       NULL);
}

static void
quick_toggle_menu_set_header_text (ShellQuickToggleMenu *self,
                                   const char           *title,
                                   const char           *subtitle)
{
  ShellQuickToggleMenuPrivate *priv = shell_quick_toggle_menu_get_instance_private (self);

  if (priv->header_title == NULL)
    return;

  st_label_set_text (priv->header_title, title != NULL ? title : "");

  st_label_set_text (priv->header_subtitle, subtitle != NULL ? subtitle : "");
  set_actor_visible (CLUTTER_ACTOR (priv->header_subtitle), subtitle != NULL && *subtitle != '\0');

  clutter_actor_show (priv->header);
}

/**
 * shell_quick_toggle_menu_set_header:
 * @self: a #ShellQuickToggleMenu
 * @icon_name: the name of the icon of the header
 * @title: the title of the header
 * @subtitle: (nullable): the subtitle of the header, hidden when empty
 *
 * Shows a header with an icon, a title and a subtitle above the items of the
 * menu (`setHeader ()`).
 */
void
shell_quick_toggle_menu_set_header (ShellQuickToggleMenu *self,
                                    const char           *icon_name,
                                    const char           *title,
                                    const char           *subtitle)
{
  ShellQuickToggleMenuPrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_TOGGLE_MENU (self));

  priv = shell_quick_toggle_menu_get_instance_private (self);
  if (priv->header_icon != NULL)
    st_icon_set_icon_name (priv->header_icon, icon_name);

  quick_toggle_menu_set_header_text (self, title, subtitle);
}

/**
 * shell_quick_toggle_menu_set_header_gicon:
 * @self: a #ShellQuickToggleMenu
 * @gicon: the icon of the header
 * @title: the title of the header
 * @subtitle: (nullable): the subtitle of the header, hidden when empty
 *
 * Like shell_quick_toggle_menu_set_header (), with a #GIcon (`setHeader ()`
 * with a `Gio.Icon`).
 */
void
shell_quick_toggle_menu_set_header_gicon (ShellQuickToggleMenu *self,
                                          GIcon                *gicon,
                                          const char           *title,
                                          const char           *subtitle)
{
  ShellQuickToggleMenuPrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_TOGGLE_MENU (self));
  g_return_if_fail (G_IS_ICON (gicon));

  priv = shell_quick_toggle_menu_get_instance_private (self);
  if (priv->header_icon != NULL)
    st_icon_set_gicon (priv->header_icon, gicon);

  quick_toggle_menu_set_header_text (self, title, subtitle);
}

/**
 * shell_quick_toggle_menu_add_header_suffix:
 * @self: a #ShellQuickToggleMenu
 * @actor: the actor to show at the end of the header, such as a spinner
 *
 * Adds an actor after the title of the header (`addHeaderSuffix ()`).
 */
void
shell_quick_toggle_menu_add_header_suffix (ShellQuickToggleMenu *self,
                                           ClutterActor         *actor)
{
  ShellQuickToggleMenuPrivate *priv;
  g_autoptr (ClutterActor) spacer = NULL;
  ClutterGridPosition side;

  g_return_if_fail (SHELL_IS_QUICK_TOGGLE_MENU (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (actor));

  priv = shell_quick_toggle_menu_get_instance_private (self);
  if (priv->header == NULL || priv->header_layout == NULL ||
      priv->header_spacer == NULL)
    return;

  side = header_side (self);

  spacer = g_object_ref (priv->header_spacer);
  clutter_actor_remove_child (priv->header, spacer);
  clutter_grid_layout_attach_next_to (priv->header_layout, actor,
                                      CLUTTER_ACTOR (priv->header_title),
                                      side, 1, 1);
  clutter_grid_layout_attach_next_to (priv->header_layout, spacer, actor,
                                      side, 1, 1);
}

/* ======================================================================== */
/* ShellQuickSettingsLayoutMeta and ShellQuickSettingsLayout                */
/* ======================================================================== */

enum
{
  META_PROP_0,

  META_PROP_COLUMN_SPAN,

  N_META_PROPS
};

static GParamSpec *meta_props[N_META_PROPS] = { NULL, };

struct _ShellQuickSettingsLayoutMeta
{
  ClutterLayoutMeta parent;

  int column_span;
};

G_DEFINE_FINAL_TYPE (ShellQuickSettingsLayoutMeta, shell_quick_settings_layout_meta,
                     CLUTTER_TYPE_LAYOUT_META)

static void
shell_quick_settings_layout_meta_get_property (GObject    *object,
                                               guint       prop_id,
                                               GValue     *value,
                                               GParamSpec *pspec)
{
  ShellQuickSettingsLayoutMeta *self = SHELL_QUICK_SETTINGS_LAYOUT_META (object);

  switch (prop_id)
    {
    case META_PROP_COLUMN_SPAN:
      g_value_set_int (value, self->column_span);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_quick_settings_layout_meta_set_property (GObject      *object,
                                               guint         prop_id,
                                               const GValue *value,
                                               GParamSpec   *pspec)
{
  ShellQuickSettingsLayoutMeta *self = SHELL_QUICK_SETTINGS_LAYOUT_META (object);

  switch (prop_id)
    {
    case META_PROP_COLUMN_SPAN:
      {
        int column_span = g_value_get_int (value);
        ClutterLayoutManager *manager;

        if (self->column_span == column_span)
          break;

        self->column_span = column_span;
        g_object_notify_by_pspec (object, meta_props[META_PROP_COLUMN_SPAN]);

        manager = clutter_layout_meta_get_manager (CLUTTER_LAYOUT_META (self));
        if (manager != NULL)
          clutter_layout_manager_layout_changed (manager);
      }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_quick_settings_layout_meta_class_init (ShellQuickSettingsLayoutMetaClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_quick_settings_layout_meta_get_property;
  object_class->set_property = shell_quick_settings_layout_meta_set_property;

  /**
   * ShellQuickSettingsLayoutMeta:column-span:
   *
   * The number of columns of the grid the child takes. Set it with
   * clutter_layout_manager_child_set_property () (`column-span`).
   */
  meta_props[META_PROP_COLUMN_SPAN] =
    g_param_spec_int ("column-span", NULL, NULL,
                      1, G_MAXINT32, 1,
                      G_PARAM_READWRITE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_META_PROPS, meta_props);
}

static void
shell_quick_settings_layout_meta_init (ShellQuickSettingsLayoutMeta *self)
{
  self->column_span = 1;
}

enum
{
  LAYOUT_PROP_0,

  LAYOUT_PROP_ROW_SPACING,
  LAYOUT_PROP_COLUMN_SPACING,
  LAYOUT_PROP_N_COLUMNS,
  LAYOUT_PROP_OVERLAY,

  N_LAYOUT_PROPS
};

static GParamSpec *layout_props[N_LAYOUT_PROPS] = { NULL, };

struct _ShellQuickSettingsLayout
{
  ClutterLayoutManager parent;

  ClutterActor *overlay;   /* weak, a child of the container */
  ClutterActor *container; /* weak */

  int row_spacing;
  int column_spacing;
  int n_columns;
};

G_DEFINE_FINAL_TYPE (ShellQuickSettingsLayout, shell_quick_settings_layout,
                     CLUTTER_TYPE_LAYOUT_MANAGER)

static void
layout_container_style_changed_cb (StWidget                 *container,
                                   ShellQuickSettingsLayout *self)
{
  StThemeNode *node = st_widget_get_theme_node (container);
  gboolean changed = FALSE;
  double length;

  if (st_theme_node_lookup_length (node, "spacing-rows", FALSE, &length))
    {
      changed = TRUE;
      g_object_set (self, "row-spacing", (int) length, NULL);
    }

  if (st_theme_node_lookup_length (node, "spacing-columns", FALSE, &length))
    {
      changed = TRUE;
      g_object_set (self, "column-spacing", (int) length, NULL);
    }

  if (changed)
    clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
}

static int
layout_get_col_span (ShellQuickSettingsLayout *self,
                     ClutterActor             *container,
                     ClutterActor             *child)
{
  ClutterLayoutMeta *meta =
    clutter_layout_manager_get_child_meta (CLUTTER_LAYOUT_MANAGER (self),
                                           container, child);
  int column_span = 1;

  if (SHELL_IS_QUICK_SETTINGS_LAYOUT_META (meta))
    column_span = SHELL_QUICK_SETTINGS_LAYOUT_META (meta)->column_span;

  return CLAMP (column_span, 1, self->n_columns);
}

static void
layout_get_max_child_width (ShellQuickSettingsLayout *self,
                            ClutterActor             *container,
                            float                    *min_width_p,
                            float                    *nat_width_p)
{
  float min_width = 0;
  float nat_width = 0;
  ClutterActor *child;

  for (child = clutter_actor_get_first_child (container);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      float child_min, child_nat;
      int col_span;

      if (child == self->overlay)
        continue;

      clutter_actor_get_preferred_width (child, -1, &child_min, &child_nat);
      col_span = layout_get_col_span (self, container, child);
      min_width = MAX (min_width, child_min / col_span);
      nat_width = MAX (nat_width, child_nat / col_span);
    }

  *min_width_p = min_width;
  *nat_width_p = nat_width;
}

/* An array of rows, each a GPtrArray of the (borrowed) children in it */
static GPtrArray *
layout_get_rows (ShellQuickSettingsLayout *self,
                 ClutterActor             *container)
{
  GPtrArray *rows = g_ptr_array_new_with_free_func ((GDestroyNotify) g_ptr_array_unref);
  GPtrArray *cur_row = NULL;
  int line_index = 0;
  ClutterActor *child;

  for (child = clutter_actor_get_first_child (container);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      int col_span;

      if (!clutter_actor_is_visible (child))
        continue;

      if (child == self->overlay)
        continue;

      if (line_index == 0)
        {
          cur_row = g_ptr_array_new ();
          g_ptr_array_add (rows, cur_row);
          line_index = 0;
        }

      col_span = layout_get_col_span (self, container, child);
      if (line_index + col_span > self->n_columns)
        {
          cur_row = g_ptr_array_new ();
          g_ptr_array_add (rows, cur_row);
          line_index = 0;
        }

      g_ptr_array_add (cur_row, child);
      line_index = (line_index + col_span) % self->n_columns;
    }

  return rows;
}

static void
layout_get_row_height (GPtrArray *row,
                       float     *min_height_p,
                       float     *nat_height_p)
{
  float min_height = 0;
  float nat_height = 0;
  guint i;

  for (i = 0; i < row->len; i++)
    {
      float child_min, child_nat;

      clutter_actor_get_preferred_height (g_ptr_array_index (row, i), -1,
                                          &child_min, &child_nat);
      min_height = MAX (min_height, child_min);
      nat_height = MAX (nat_height, child_nat);
    }

  *min_height_p = min_height;
  *nat_height_p = nat_height;
}

static GType
shell_quick_settings_layout_get_child_meta_type (ClutterLayoutManager *manager)
{
  return SHELL_TYPE_QUICK_SETTINGS_LAYOUT_META;
}

static void
shell_quick_settings_layout_set_container (ClutterLayoutManager *manager,
                                           ClutterActor         *container)
{
  ShellQuickSettingsLayout *self = SHELL_QUICK_SETTINGS_LAYOUT (manager);

  if (self->container != NULL)
    {
      g_signal_handlers_disconnect_by_func (self->container,
                                            layout_container_style_changed_cb,
                                            self);
      g_clear_weak_pointer (&self->container);
    }

  if (container != NULL)
    {
      g_set_weak_pointer (&self->container, container);

      if (ST_IS_WIDGET (container))
        g_signal_connect (container, "style-changed",
                          G_CALLBACK (layout_container_style_changed_cb), self);
    }
}

static void
shell_quick_settings_layout_get_preferred_width (ClutterLayoutManager *manager,
                                                 ClutterActor         *container,
                                                 float                 for_height,
                                                 float                *min_width_p,
                                                 float                *nat_width_p)
{
  ShellQuickSettingsLayout *self = SHELL_QUICK_SETTINGS_LAYOUT (manager);
  float child_min, child_nat;
  float spacing = (self->n_columns - 1) * self->column_spacing;

  layout_get_max_child_width (self, container, &child_min, &child_nat);

  if (min_width_p)
    *min_width_p = self->n_columns * child_min + spacing;
  if (nat_width_p)
    *nat_width_p = self->n_columns * child_nat + spacing;
}

static void
shell_quick_settings_layout_get_preferred_height (ClutterLayoutManager *manager,
                                                  ClutterActor         *container,
                                                  float                 for_width,
                                                  float                *min_height_p,
                                                  float                *nat_height_p)
{
  ShellQuickSettingsLayout *self = SHELL_QUICK_SETTINGS_LAYOUT (manager);
  g_autoptr (GPtrArray) rows = layout_get_rows (self, container);
  float min_height = 0;
  float nat_height = 0;
  float spacing;
  guint i;

  if (self->overlay != NULL)
    clutter_actor_get_preferred_height (self->overlay, for_width,
                                        &min_height, &nat_height);

  spacing = (rows->len > 0 ? rows->len - 1 : 0) * self->row_spacing;
  min_height += spacing;
  nat_height += spacing;

  for (i = 0; i < rows->len; i++)
    {
      float row_min, row_nat;

      layout_get_row_height (g_ptr_array_index (rows, i), &row_min, &row_nat);
      min_height += row_min;
      nat_height += row_nat;
    }

  if (min_height_p)
    *min_height_p = min_height;
  if (nat_height_p)
    *nat_height_p = nat_height;
}

static void
shell_quick_settings_layout_allocate (ClutterLayoutManager   *manager,
                                      ClutterActor           *container,
                                      const ClutterActorBox  *box)
{
  ShellQuickSettingsLayout *self = SHELL_QUICK_SETTINGS_LAYOUT (manager);
  g_autoptr (GPtrArray) rows = layout_get_rows (self, container);
  float box_width = clutter_actor_box_get_width (box);
  float overlay_height = 0;
  float avail_width;
  float child_width;
  gboolean is_rtl;
  float y = box->y1;
  guint i;

  if (self->overlay != NULL)
    {
      clutter_actor_get_preferred_height (self->overlay, box_width,
                                          NULL, &overlay_height);
      clutter_actor_allocate_available_size (self->overlay, 0, 0,
                                             box_width, overlay_height);
    }

  avail_width = box_width - (self->n_columns - 1) * self->column_spacing;
  child_width = floorf (avail_width / self->n_columns);

  is_rtl = clutter_actor_get_text_direction (container) == CLUTTER_TEXT_DIRECTION_RTL;

  for (i = 0; i < rows->len; i++)
    {
      GPtrArray *row = g_ptr_array_index (rows, i);
      gboolean has_open_menu = FALSE;
      float row_min, row_nat;
      int line_index = 0;
      guint j;

      layout_get_row_height (row, &row_min, &row_nat);

      for (j = 0; j < row->len; j++)
        {
          ClutterActor *child = g_ptr_array_index (row, j);
          ShellQuickToggleMenu *menu = NULL;
          ClutterActorBox child_box;
          int col_span = layout_get_col_span (self, container, child);
          float width = child_width * col_span +
                        self->column_spacing * (col_span - 1);
          float x = box->x1 + line_index * (child_width + self->column_spacing);

          if (is_rtl)
            x = box->x2 - width - x;

          child_box.x1 = x;
          child_box.y1 = y;
          child_box.x2 = x + width;
          child_box.y2 = y + row_nat;
          clutter_actor_allocate (child, &child_box);

          line_index = (line_index + col_span) % self->n_columns;

          if (SHELL_IS_QUICK_SETTINGS_ITEM (child))
            menu = shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (child));
          if (menu != NULL)
            {
              ClutterActor *menu_actor =
                shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (menu));

              if (menu_actor != NULL && clutter_actor_is_visible (menu_actor))
                has_open_menu = TRUE;
            }
        }

      y += row_nat + self->row_spacing;

      if (has_open_menu)
        y += overlay_height;
    }
}

static void
shell_quick_settings_layout_get_property (GObject    *object,
                                          guint       prop_id,
                                          GValue     *value,
                                          GParamSpec *pspec)
{
  ShellQuickSettingsLayout *self = SHELL_QUICK_SETTINGS_LAYOUT (object);

  switch (prop_id)
    {
    case LAYOUT_PROP_ROW_SPACING:
      g_value_set_int (value, self->row_spacing);
      break;

    case LAYOUT_PROP_COLUMN_SPACING:
      g_value_set_int (value, self->column_spacing);
      break;

    case LAYOUT_PROP_N_COLUMNS:
      g_value_set_int (value, self->n_columns);
      break;

    case LAYOUT_PROP_OVERLAY:
      g_value_set_object (value, self->overlay);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_quick_settings_layout_set_property (GObject      *object,
                                          guint         prop_id,
                                          const GValue *value,
                                          GParamSpec   *pspec)
{
  ShellQuickSettingsLayout *self = SHELL_QUICK_SETTINGS_LAYOUT (object);
  int int_value;

  switch (prop_id)
    {
    case LAYOUT_PROP_ROW_SPACING:
      int_value = g_value_get_int (value);
      if (self->row_spacing != int_value)
        {
          self->row_spacing = int_value;
          g_object_notify_by_pspec (object, layout_props[LAYOUT_PROP_ROW_SPACING]);
          clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
        }
      break;

    case LAYOUT_PROP_COLUMN_SPACING:
      int_value = g_value_get_int (value);
      if (self->column_spacing != int_value)
        {
          self->column_spacing = int_value;
          g_object_notify_by_pspec (object, layout_props[LAYOUT_PROP_COLUMN_SPACING]);
          clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
        }
      break;

    case LAYOUT_PROP_N_COLUMNS:
      int_value = g_value_get_int (value);
      if (self->n_columns != int_value)
        {
          self->n_columns = int_value;
          g_object_notify_by_pspec (object, layout_props[LAYOUT_PROP_N_COLUMNS]);
          clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
        }
      break;

    case LAYOUT_PROP_OVERLAY:
      g_set_weak_pointer (&self->overlay, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_quick_settings_layout_dispose (GObject *object)
{
  ShellQuickSettingsLayout *self = SHELL_QUICK_SETTINGS_LAYOUT (object);

  if (self->container != NULL)
    {
      g_signal_handlers_disconnect_by_func (self->container,
                                            layout_container_style_changed_cb,
                                            self);
      g_clear_weak_pointer (&self->container);
    }
  g_clear_weak_pointer (&self->overlay);

  G_OBJECT_CLASS (shell_quick_settings_layout_parent_class)->dispose (object);
}

static void
shell_quick_settings_layout_class_init (ShellQuickSettingsLayoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);

  object_class->get_property = shell_quick_settings_layout_get_property;
  object_class->set_property = shell_quick_settings_layout_set_property;
  object_class->dispose = shell_quick_settings_layout_dispose;

  layout_class->get_child_meta_type = shell_quick_settings_layout_get_child_meta_type;
  layout_class->set_container = shell_quick_settings_layout_set_container;
  layout_class->get_preferred_width = shell_quick_settings_layout_get_preferred_width;
  layout_class->get_preferred_height = shell_quick_settings_layout_get_preferred_height;
  layout_class->allocate = shell_quick_settings_layout_allocate;

  /**
   * ShellQuickSettingsLayout:row-spacing:
   *
   * The space between two rows, which the `spacing-rows` of the container
   * overrides.
   */
  layout_props[LAYOUT_PROP_ROW_SPACING] =
    g_param_spec_int ("row-spacing", NULL, NULL,
                      0, G_MAXINT32, 0,
                      G_PARAM_READWRITE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickSettingsLayout:column-spacing:
   *
   * The space between two columns, which the `spacing-columns` of the
   * container overrides.
   */
  layout_props[LAYOUT_PROP_COLUMN_SPACING] =
    g_param_spec_int ("column-spacing", NULL, NULL,
                      0, G_MAXINT32, 0,
                      G_PARAM_READWRITE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickSettingsLayout:n-columns:
   *
   * The number of columns of the grid.
   */
  layout_props[LAYOUT_PROP_N_COLUMNS] =
    g_param_spec_int ("n-columns", NULL, NULL,
                      1, G_MAXINT32, 1,
                      G_PARAM_READWRITE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickSettingsLayout:overlay:
   *
   * A child of the container which is not laid out as an item: it is as
   * high as the overlay of menus and is given the room above the rows.
   */
  layout_props[LAYOUT_PROP_OVERLAY] =
    g_param_spec_object ("overlay", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_LAYOUT_PROPS, layout_props);
}

static void
shell_quick_settings_layout_init (ShellQuickSettingsLayout *self)
{
  self->n_columns = 1;
}

/**
 * shell_quick_settings_layout_new:
 * @overlay: (nullable): the placeholder child of the container which is
 *   as high as the overlay of the menus of the items
 * @n_columns: the number of columns of the grid
 *
 * Creates the layout manager of the grid of the quick settings menu.
 *
 * Returns: (transfer full): a new #ShellQuickSettingsLayout
 */
ShellQuickSettingsLayout *
shell_quick_settings_layout_new (ClutterActor *overlay,
                                 int           n_columns)
{
  g_return_val_if_fail (overlay == NULL || CLUTTER_IS_ACTOR (overlay), NULL);
  g_return_val_if_fail (n_columns >= 1, NULL);

  return g_object_new (SHELL_TYPE_QUICK_SETTINGS_LAYOUT,
                       "overlay", overlay,
                       "n-columns", n_columns,
                       NULL);
}

/* ======================================================================== */
/* ShellQuickSettingsItem                                                   */
/* ======================================================================== */

enum
{
  ITEM_PROP_0,

  ITEM_PROP_HAS_MENU,
  ITEM_PROP_MENU,

  N_ITEM_PROPS
};

static GParamSpec *item_props[N_ITEM_PROPS] = { NULL, };

typedef struct
{
  gboolean has_menu;
  ShellQuickToggleMenu *menu; /* strong */
  gboolean menu_owned;
  ShellPopupMenuManager *menu_manager; /* strong */
} ShellQuickSettingsItemPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellQuickSettingsItem, shell_quick_settings_item,
                            ST_TYPE_BUTTON)

/**
 * shell_quick_settings_item_get_has_menu:
 * @self: a #ShellQuickSettingsItem
 *
 * Returns: whether the item was created with a menu (`hasMenu`)
 */
gboolean
shell_quick_settings_item_get_has_menu (ShellQuickSettingsItem *self)
{
  ShellQuickSettingsItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_SETTINGS_ITEM (self), FALSE);

  priv = shell_quick_settings_item_get_instance_private (self);
  return priv->has_menu;
}

/**
 * shell_quick_settings_item_get_menu:
 * @self: a #ShellQuickSettingsItem
 *
 * Returns: (transfer none) (nullable): the menu of the item, if it has one
 */
ShellQuickToggleMenu *
shell_quick_settings_item_get_menu (ShellQuickSettingsItem *self)
{
  ShellQuickSettingsItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_SETTINGS_ITEM (self), NULL);

  priv = shell_quick_settings_item_get_instance_private (self);
  return priv->menu;
}

/**
 * shell_quick_settings_item_set_menu:
 * @self: a #ShellQuickSettingsItem
 * @menu: (nullable): the new menu of the item
 *
 * Sets the menu which the quick settings menu adds to its overlay and which
 * makes room for itself below the item. An item with `has-menu` creates its
 * own; this is for an item which stands for the menu of one of its children
 * (`item.menu = child.menu`). The item does not destroy a menu set this way.
 */
void
shell_quick_settings_item_set_menu (ShellQuickSettingsItem *self,
                                    ShellQuickToggleMenu   *menu)
{
  ShellQuickSettingsItemPrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_SETTINGS_ITEM (self));
  g_return_if_fail (menu == NULL || SHELL_IS_QUICK_TOGGLE_MENU (menu));

  priv = shell_quick_settings_item_get_instance_private (self);
  if (priv->menu == menu)
    return;

  if (menu != NULL)
    g_object_ref (menu);
  g_clear_object (&priv->menu);
  priv->menu = menu;
  priv->menu_owned = FALSE;

  g_object_notify_by_pspec (G_OBJECT (self), item_props[ITEM_PROP_MENU]);
}

static void
shell_quick_settings_item_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellQuickSettingsItem *self = SHELL_QUICK_SETTINGS_ITEM (object);
  ShellQuickSettingsItemPrivate *priv = shell_quick_settings_item_get_instance_private (self);

  switch (prop_id)
    {
    case ITEM_PROP_HAS_MENU:
      g_value_set_boolean (value, priv->has_menu);
      break;

    case ITEM_PROP_MENU:
      g_value_set_object (value, priv->menu);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_quick_settings_item_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellQuickSettingsItem *self = SHELL_QUICK_SETTINGS_ITEM (object);
  ShellQuickSettingsItemPrivate *priv = shell_quick_settings_item_get_instance_private (self);

  switch (prop_id)
    {
    case ITEM_PROP_HAS_MENU:
      priv->has_menu = g_value_get_boolean (value);
      break;

    case ITEM_PROP_MENU:
      shell_quick_settings_item_set_menu (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_quick_settings_item_constructed (GObject *object)
{
  ShellQuickSettingsItem *self = SHELL_QUICK_SETTINGS_ITEM (object);
  ShellQuickSettingsItemPrivate *priv = shell_quick_settings_item_get_instance_private (self);

  G_OBJECT_CLASS (shell_quick_settings_item_parent_class)->constructed (object);

  if (priv->has_menu)
    {
      ClutterActor *menu_actor;

      priv->menu = shell_quick_toggle_menu_new (CLUTTER_ACTOR (self));
      priv->menu_owned = TRUE;

      menu_actor = shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (priv->menu));
      clutter_actor_hide (menu_actor);

      priv->menu_manager = shell_popup_menu_manager_new (SHELL_ACTION_MODE_POPUP);
      shell_popup_menu_manager_add_menu (priv->menu_manager, G_OBJECT (priv->menu));
    }
}

/* The menu lives in the overlay of the quick settings menu, not below the
 * item: it goes with the item, or it would stay behind for good. */
static void
shell_quick_settings_item_destroy_menu (ShellQuickSettingsItem *self)
{
  ShellQuickSettingsItemPrivate *priv = shell_quick_settings_item_get_instance_private (self);

  if (priv->menu != NULL && priv->menu_owned)
    {
      g_autoptr (ShellQuickToggleMenu) menu = g_steal_pointer (&priv->menu);

      priv->menu_owned = FALSE;
      shell_popup_menu_base_destroy (SHELL_POPUP_MENU_BASE (menu));
    }

  g_clear_object (&priv->menu);
}

static void
shell_quick_settings_item_destroy (ClutterActor *actor)
{
  shell_quick_settings_item_destroy_menu (SHELL_QUICK_SETTINGS_ITEM (actor));

  CLUTTER_ACTOR_CLASS (shell_quick_settings_item_parent_class)->destroy (actor);
}

static void
shell_quick_settings_item_dispose (GObject *object)
{
  ShellQuickSettingsItem *self = SHELL_QUICK_SETTINGS_ITEM (object);
  ShellQuickSettingsItemPrivate *priv = shell_quick_settings_item_get_instance_private (self);

  /* clutter_actor_destroy () runs g_object_run_dispose (), which calls this
   * (the most derived dispose) before ClutterActor's dispose emits "destroy"
   * and so before the destroy vfunc above: the menu must be destroyed here,
   * while the item still holds it, or it would outlive its item. */
  shell_quick_settings_item_destroy_menu (self);
  g_clear_object (&priv->menu_manager);

  G_OBJECT_CLASS (shell_quick_settings_item_parent_class)->dispose (object);
}

static void
shell_quick_settings_item_class_init (ShellQuickSettingsItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_quick_settings_item_constructed;
  object_class->dispose = shell_quick_settings_item_dispose;
  object_class->get_property = shell_quick_settings_item_get_property;
  object_class->set_property = shell_quick_settings_item_set_property;

  actor_class->destroy = shell_quick_settings_item_destroy;

  /**
   * ShellQuickSettingsItem:has-menu:
   *
   * Whether the item creates a #ShellQuickToggleMenu (`hasMenu`).
   */
  item_props[ITEM_PROP_HAS_MENU] =
    g_param_spec_boolean ("has-menu", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickSettingsItem:menu:
   *
   * The menu of the item (`menu`). See shell_quick_settings_item_set_menu ().
   */
  item_props[ITEM_PROP_MENU] =
    g_param_spec_object ("menu", NULL, NULL,
                         SHELL_TYPE_QUICK_TOGGLE_MENU,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_ITEM_PROPS, item_props);
}

static void
shell_quick_settings_item_init (ShellQuickSettingsItem *self)
{
}

/* Opens the menu of an item as the buttons of the items do */
static void
quick_settings_item_open_menu (ShellQuickSettingsItem *self)
{
  ShellQuickToggleMenu *menu = shell_quick_settings_item_get_menu (self);

  if (menu != NULL)
    shell_popup_menu_base_open (SHELL_POPUP_MENU_BASE (menu),
                                SHELL_POPUP_ANIMATION_FULL);
}

/* ======================================================================== */
/* ShellQuickToggle                                                         */
/* ======================================================================== */

enum
{
  TOGGLE_PROP_0,

  TOGGLE_PROP_TITLE,
  TOGGLE_PROP_SUBTITLE,
  TOGGLE_PROP_GICON,

  N_TOGGLE_PROPS,

  /* Overrides of the properties of StButton */
  TOGGLE_PROP_ICON_NAME,
  TOGGLE_PROP_LABEL
};

static GParamSpec *toggle_props[N_TOGGLE_PROPS] = { NULL, };

typedef struct
{
  StBoxLayout *box;       /* weak */
  StIcon *icon;           /* weak */
  StLabel *title_label;   /* weak */
  StLabel *subtitle_label; /* weak */

  char *title;
  char *subtitle;
} ShellQuickTogglePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellQuickToggle, shell_quick_toggle,
                            SHELL_TYPE_QUICK_SETTINGS_ITEM)

/**
 * shell_quick_toggle_get_title:
 * @self: a #ShellQuickToggle
 *
 * Returns: (nullable): the title of the toggle
 */
const char *
shell_quick_toggle_get_title (ShellQuickToggle *self)
{
  ShellQuickTogglePrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_TOGGLE (self), NULL);

  priv = shell_quick_toggle_get_instance_private (self);
  return priv->title;
}

/**
 * shell_quick_toggle_set_title:
 * @self: a #ShellQuickToggle
 * @title: (nullable): the new title
 *
 * Sets #ShellQuickToggle:title.
 */
void
shell_quick_toggle_set_title (ShellQuickToggle *self,
                              const char       *title)
{
  ShellQuickTogglePrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_TOGGLE (self));

  priv = shell_quick_toggle_get_instance_private (self);
  if (g_strcmp0 (priv->title, title) == 0)
    return;

  g_free (priv->title);
  priv->title = g_strdup (title);

  if (priv->title_label != NULL)
    st_label_set_text (priv->title_label, title != NULL ? title : "");

  g_object_notify_by_pspec (G_OBJECT (self), toggle_props[TOGGLE_PROP_TITLE]);
}

/**
 * shell_quick_toggle_get_subtitle:
 * @self: a #ShellQuickToggle
 *
 * Returns: (nullable): the subtitle of the toggle
 */
const char *
shell_quick_toggle_get_subtitle (ShellQuickToggle *self)
{
  ShellQuickTogglePrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_TOGGLE (self), NULL);

  priv = shell_quick_toggle_get_instance_private (self);
  return priv->subtitle;
}

/**
 * shell_quick_toggle_set_subtitle:
 * @self: a #ShellQuickToggle
 * @subtitle: (nullable): the new subtitle. The label is hidden for %NULL
 *
 * Sets #ShellQuickToggle:subtitle.
 */
void
shell_quick_toggle_set_subtitle (ShellQuickToggle *self,
                                 const char       *subtitle)
{
  ShellQuickTogglePrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_TOGGLE (self));

  priv = shell_quick_toggle_get_instance_private (self);
  if (g_strcmp0 (priv->subtitle, subtitle) == 0 &&
      (priv->subtitle == NULL) == (subtitle == NULL))
    return;

  g_free (priv->subtitle);
  priv->subtitle = g_strdup (subtitle);

  if (priv->subtitle_label != NULL)
    {
      st_label_set_text (priv->subtitle_label, subtitle != NULL ? subtitle : "");
      set_actor_visible (CLUTTER_ACTOR (priv->subtitle_label), subtitle != NULL);
    }

  g_object_notify_by_pspec (G_OBJECT (self), toggle_props[TOGGLE_PROP_SUBTITLE]);
}

/**
 * shell_quick_toggle_get_icon_name:
 * @self: a #ShellQuickToggle
 *
 * Returns: (nullable): the name of the icon of the toggle
 */
const char *
shell_quick_toggle_get_icon_name (ShellQuickToggle *self)
{
  ShellQuickTogglePrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_TOGGLE (self), NULL);

  priv = shell_quick_toggle_get_instance_private (self);
  return priv->icon != NULL ? st_icon_get_icon_name (priv->icon) : NULL;
}

/**
 * shell_quick_toggle_set_icon_name:
 * @self: a #ShellQuickToggle
 * @icon_name: (nullable): the name of the new icon
 *
 * Sets #ShellQuickToggle:icon-name.
 */
void
shell_quick_toggle_set_icon_name (ShellQuickToggle *self,
                                  const char       *icon_name)
{
  ShellQuickTogglePrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_TOGGLE (self));

  priv = shell_quick_toggle_get_instance_private (self);
  if (priv->icon != NULL)
    st_icon_set_icon_name (priv->icon, icon_name);
}

/**
 * shell_quick_toggle_get_gicon:
 * @self: a #ShellQuickToggle
 *
 * Returns: (transfer none) (nullable): the icon of the toggle
 */
GIcon *
shell_quick_toggle_get_gicon (ShellQuickToggle *self)
{
  ShellQuickTogglePrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_TOGGLE (self), NULL);

  priv = shell_quick_toggle_get_instance_private (self);
  return priv->icon != NULL ? st_icon_get_gicon (priv->icon) : NULL;
}

/**
 * shell_quick_toggle_set_gicon:
 * @self: a #ShellQuickToggle
 * @gicon: (nullable): the new icon
 *
 * Sets #ShellQuickToggle:gicon.
 */
void
shell_quick_toggle_set_gicon (ShellQuickToggle *self,
                              GIcon            *gicon)
{
  ShellQuickTogglePrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_TOGGLE (self));

  priv = shell_quick_toggle_get_instance_private (self);
  if (priv->icon != NULL)
    st_icon_set_gicon (priv->icon, gicon);
}

/**
 * shell_quick_toggle_get_box:
 * @self: a #ShellQuickToggle
 *
 * Returns: (transfer none): the box which holds the icon and the labels
 *   (`_box`)
 */
StBoxLayout *
shell_quick_toggle_get_box (ShellQuickToggle *self)
{
  ShellQuickTogglePrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_TOGGLE (self), NULL);

  priv = shell_quick_toggle_get_instance_private (self);
  return priv->box;
}

/**
 * shell_quick_toggle_get_icon:
 * @self: a #ShellQuickToggle
 *
 * Returns: (transfer none): the icon of the toggle (`_icon`)
 */
StIcon *
shell_quick_toggle_get_icon (ShellQuickToggle *self)
{
  ShellQuickTogglePrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_TOGGLE (self), NULL);

  priv = shell_quick_toggle_get_instance_private (self);
  return priv->icon;
}

/**
 * shell_quick_toggle_get_title_label:
 * @self: a #ShellQuickToggle
 *
 * Returns: (transfer none): the label of the title (`_title`)
 */
StLabel *
shell_quick_toggle_get_title_label (ShellQuickToggle *self)
{
  ShellQuickTogglePrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_TOGGLE (self), NULL);

  priv = shell_quick_toggle_get_instance_private (self);
  return priv->title_label;
}

/**
 * shell_quick_toggle_get_subtitle_label:
 * @self: a #ShellQuickToggle
 *
 * Returns: (transfer none): the label of the subtitle (`_subtitle`)
 */
StLabel *
shell_quick_toggle_get_subtitle_label (ShellQuickToggle *self)
{
  ShellQuickTogglePrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_TOGGLE (self), NULL);

  priv = shell_quick_toggle_get_instance_private (self);
  return priv->subtitle_label;
}

static void
quick_toggle_icon_notify_cb (GObject          *icon,
                             GParamSpec       *pspec,
                             ShellQuickToggle *self)
{
  g_object_notify (G_OBJECT (self), pspec->name);
}

static void
shell_quick_toggle_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellQuickToggle *self = SHELL_QUICK_TOGGLE (object);
  ShellQuickTogglePrivate *priv = shell_quick_toggle_get_instance_private (self);

  switch (prop_id)
    {
    case TOGGLE_PROP_TITLE:
      g_value_set_string (value, priv->title);
      break;

    case TOGGLE_PROP_SUBTITLE:
      g_value_set_string (value, priv->subtitle);
      break;

    case TOGGLE_PROP_GICON:
      g_value_set_object (value, shell_quick_toggle_get_gicon (self));
      break;

    case TOGGLE_PROP_ICON_NAME:
      g_value_set_string (value, shell_quick_toggle_get_icon_name (self));
      break;

    case TOGGLE_PROP_LABEL:
      g_debug ("Trying to get label from QuickToggle. Use title instead.");
      g_value_set_string (value, priv->title);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_quick_toggle_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellQuickToggle *self = SHELL_QUICK_TOGGLE (object);

  switch (prop_id)
    {
    case TOGGLE_PROP_TITLE:
      shell_quick_toggle_set_title (self, g_value_get_string (value));
      break;

    case TOGGLE_PROP_SUBTITLE:
      shell_quick_toggle_set_subtitle (self, g_value_get_string (value));
      break;

    case TOGGLE_PROP_GICON:
      shell_quick_toggle_set_gicon (self, g_value_get_object (value));
      break;

    case TOGGLE_PROP_ICON_NAME:
      shell_quick_toggle_set_icon_name (self, g_value_get_string (value));
      break;

    case TOGGLE_PROP_LABEL:
      g_debug ("Trying to set label on QuickToggle. Use title instead.");
      shell_quick_toggle_set_title (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_quick_toggle_constructed (GObject *object)
{
  ShellQuickToggle *self = SHELL_QUICK_TOGGLE (object);
  ShellQuickTogglePrivate *priv = shell_quick_toggle_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *title_box;

  G_OBJECT_CLASS (shell_quick_toggle_parent_class)->constructed (object);

  /* What the JS passed before `...params`. Properties given to the
   * constructor are applied after this and override it. */
  st_widget_set_style_class_name (ST_WIDGET (self), "quick-toggle button");
  clutter_actor_set_accessible_role (actor, ATK_ROLE_TOGGLE_BUTTON);
  st_widget_set_can_focus (ST_WIDGET (self), TRUE);

  priv->box = ST_BOX_LAYOUT (st_box_layout_new ());
  clutter_actor_set_x_expand (CLUTTER_ACTOR (priv->box), TRUE);
  st_bin_set_child (ST_BIN (self), CLUTTER_ACTOR (priv->box));
  g_object_add_weak_pointer (G_OBJECT (priv->box), (gpointer *) &priv->box);

  priv->icon = ST_ICON (st_icon_new ());
  st_widget_set_style_class_name (ST_WIDGET (priv->icon), "quick-toggle-icon");
  clutter_actor_set_x_expand (CLUTTER_ACTOR (priv->icon), FALSE);
  clutter_actor_add_child (CLUTTER_ACTOR (priv->box), CLUTTER_ACTOR (priv->icon));
  g_object_add_weak_pointer (G_OBJECT (priv->icon), (gpointer *) &priv->icon);

  /* StIcon links the two properties, so the toggle has the same behaviour */
  g_signal_connect_object (priv->icon, "notify::icon-name",
                           G_CALLBACK (quick_toggle_icon_notify_cb), self, 0);
  g_signal_connect_object (priv->icon, "notify::gicon",
                           G_CALLBACK (quick_toggle_icon_notify_cb), self, 0);

  priv->title_label = ST_LABEL (st_label_new (NULL));
  st_widget_set_style_class_name (ST_WIDGET (priv->title_label), "quick-toggle-title");
  clutter_actor_set_y_align (CLUTTER_ACTOR (priv->title_label), CLUTTER_ACTOR_ALIGN_CENTER);
  clutter_actor_set_x_align (CLUTTER_ACTOR (priv->title_label), CLUTTER_ACTOR_ALIGN_START);
  clutter_actor_set_x_expand (CLUTTER_ACTOR (priv->title_label), TRUE);
  g_object_add_weak_pointer (G_OBJECT (priv->title_label), (gpointer *) &priv->title_label);
  st_widget_set_label_actor (ST_WIDGET (self), CLUTTER_ACTOR (priv->title_label));

  priv->subtitle_label = ST_LABEL (st_label_new (NULL));
  st_widget_set_style_class_name (ST_WIDGET (priv->subtitle_label), "quick-toggle-subtitle");
  clutter_actor_set_y_align (CLUTTER_ACTOR (priv->subtitle_label), CLUTTER_ACTOR_ALIGN_CENTER);
  clutter_actor_set_x_align (CLUTTER_ACTOR (priv->subtitle_label), CLUTTER_ACTOR_ALIGN_START);
  clutter_actor_set_x_expand (CLUTTER_ACTOR (priv->subtitle_label), TRUE);
  g_object_add_weak_pointer (G_OBJECT (priv->subtitle_label), (gpointer *) &priv->subtitle_label);
  atk_object_add_relationship (clutter_actor_get_accessible (actor),
                               ATK_RELATION_DESCRIBED_BY,
                               clutter_actor_get_accessible (CLUTTER_ACTOR (priv->subtitle_label)));

  title_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                            "x-align", CLUTTER_ACTOR_ALIGN_START,
                            "x-expand", TRUE,
                            "orientation", CLUTTER_ORIENTATION_VERTICAL,
                            NULL);
  clutter_actor_add_child (title_box, CLUTTER_ACTOR (priv->title_label));
  clutter_actor_add_child (title_box, CLUTTER_ACTOR (priv->subtitle_label));
  clutter_actor_add_child (CLUTTER_ACTOR (priv->box), title_box);

  g_object_set (st_label_get_clutter_text (priv->title_label),
                "ellipsize", PANGO_ELLIPSIZE_END,
                NULL);

  /* title and subtitle are NULL: the title is empty, the subtitle hidden */
  st_label_set_text (priv->title_label, priv->title != NULL ? priv->title : "");
  st_label_set_text (priv->subtitle_label, priv->subtitle != NULL ? priv->subtitle : "");
  set_actor_visible (CLUTTER_ACTOR (priv->subtitle_label), priv->subtitle != NULL);
}

static void
shell_quick_toggle_dispose (GObject *object)
{
  ShellQuickToggle *self = SHELL_QUICK_TOGGLE (object);
  ShellQuickTogglePrivate *priv = shell_quick_toggle_get_instance_private (self);

  if (priv->box != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->box), (gpointer *) &priv->box);
  if (priv->icon != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->icon), (gpointer *) &priv->icon);
  if (priv->title_label != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->title_label), (gpointer *) &priv->title_label);
  if (priv->subtitle_label != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->subtitle_label), (gpointer *) &priv->subtitle_label);
  priv->box = NULL;
  priv->icon = NULL;
  priv->title_label = NULL;
  priv->subtitle_label = NULL;

  G_OBJECT_CLASS (shell_quick_toggle_parent_class)->dispose (object);
}

static void
shell_quick_toggle_finalize (GObject *object)
{
  ShellQuickToggle *self = SHELL_QUICK_TOGGLE (object);
  ShellQuickTogglePrivate *priv = shell_quick_toggle_get_instance_private (self);

  g_clear_pointer (&priv->title, g_free);
  g_clear_pointer (&priv->subtitle, g_free);

  G_OBJECT_CLASS (shell_quick_toggle_parent_class)->finalize (object);
}

static void
shell_quick_toggle_class_init (ShellQuickToggleClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_quick_toggle_constructed;
  object_class->dispose = shell_quick_toggle_dispose;
  object_class->finalize = shell_quick_toggle_finalize;
  object_class->get_property = shell_quick_toggle_get_property;
  object_class->set_property = shell_quick_toggle_set_property;

  /**
   * ShellQuickToggle:title:
   *
   * The title of the toggle.
   */
  toggle_props[TOGGLE_PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickToggle:subtitle:
   *
   * The subtitle of the toggle, which is hidden while it is %NULL.
   */
  toggle_props[TOGGLE_PROP_SUBTITLE] =
    g_param_spec_string ("subtitle", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickToggle:gicon:
   *
   * The icon of the toggle. Setting it unsets #ShellQuickToggle:icon-name.
   */
  toggle_props[TOGGLE_PROP_GICON] =
    g_param_spec_object ("gicon", NULL, NULL,
                         G_TYPE_ICON,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_TOGGLE_PROPS, toggle_props);

  /* The icon-name of the toggle is the one of its icon, not a new icon in
   * place of the child of the button. The label is the title. */
  g_object_class_override_property (object_class, TOGGLE_PROP_ICON_NAME, "icon-name");
  g_object_class_override_property (object_class, TOGGLE_PROP_LABEL, "label");
}

static void
shell_quick_toggle_init (ShellQuickToggle *self)
{
}

/* ======================================================================== */
/* ShellQuickMenuToggle                                                     */
/* ======================================================================== */

enum
{
  MENU_TOGGLE_PROP_0,

  MENU_TOGGLE_PROP_TITLE,
  MENU_TOGGLE_PROP_SUBTITLE,
  MENU_TOGGLE_PROP_GICON,
  MENU_TOGGLE_PROP_MENU_ENABLED,
  MENU_TOGGLE_PROP_MENU_BUTTON_ACCESSIBLE_NAME,

  N_MENU_TOGGLE_PROPS,

  MENU_TOGGLE_PROP_ICON_NAME
};

static GParamSpec *menu_toggle_props[N_MENU_TOGGLE_PROPS] = { NULL, };

typedef struct
{
  ShellQuickToggle *contents; /* weak, a child */
  ClutterActor *separator;    /* weak */
  StButton *menu_button;      /* weak */

  gboolean menu_enabled;
  char *menu_button_accessible_name;
} ShellQuickMenuTogglePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellQuickMenuToggle, shell_quick_menu_toggle,
                            SHELL_TYPE_QUICK_SETTINGS_ITEM)

/**
 * shell_quick_menu_toggle_get_title:
 * @self: a #ShellQuickMenuToggle
 *
 * Returns: (nullable): the title of the toggle
 */
const char *
shell_quick_menu_toggle_get_title (ShellQuickMenuToggle *self)
{
  ShellQuickMenuTogglePrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_MENU_TOGGLE (self), NULL);

  priv = shell_quick_menu_toggle_get_instance_private (self);
  return priv->contents != NULL ? shell_quick_toggle_get_title (priv->contents) : NULL;
}

/**
 * shell_quick_menu_toggle_set_title:
 * @self: a #ShellQuickMenuToggle
 * @title: (nullable): the new title
 *
 * Sets #ShellQuickMenuToggle:title.
 */
void
shell_quick_menu_toggle_set_title (ShellQuickMenuToggle *self,
                                   const char           *title)
{
  ShellQuickMenuTogglePrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_MENU_TOGGLE (self));

  priv = shell_quick_menu_toggle_get_instance_private (self);
  if (priv->contents != NULL)
    shell_quick_toggle_set_title (priv->contents, title);
}

/**
 * shell_quick_menu_toggle_get_subtitle:
 * @self: a #ShellQuickMenuToggle
 *
 * Returns: (nullable): the subtitle of the toggle
 */
const char *
shell_quick_menu_toggle_get_subtitle (ShellQuickMenuToggle *self)
{
  ShellQuickMenuTogglePrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_MENU_TOGGLE (self), NULL);

  priv = shell_quick_menu_toggle_get_instance_private (self);
  return priv->contents != NULL ? shell_quick_toggle_get_subtitle (priv->contents) : NULL;
}

/**
 * shell_quick_menu_toggle_set_subtitle:
 * @self: a #ShellQuickMenuToggle
 * @subtitle: (nullable): the new subtitle
 *
 * Sets #ShellQuickMenuToggle:subtitle.
 */
void
shell_quick_menu_toggle_set_subtitle (ShellQuickMenuToggle *self,
                                      const char           *subtitle)
{
  ShellQuickMenuTogglePrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_MENU_TOGGLE (self));

  priv = shell_quick_menu_toggle_get_instance_private (self);
  if (priv->contents != NULL)
    shell_quick_toggle_set_subtitle (priv->contents, subtitle);
}

/**
 * shell_quick_menu_toggle_get_icon_name:
 * @self: a #ShellQuickMenuToggle
 *
 * Returns: (nullable): the name of the icon of the toggle
 */
const char *
shell_quick_menu_toggle_get_icon_name (ShellQuickMenuToggle *self)
{
  ShellQuickMenuTogglePrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_MENU_TOGGLE (self), NULL);

  priv = shell_quick_menu_toggle_get_instance_private (self);
  return priv->contents != NULL ? shell_quick_toggle_get_icon_name (priv->contents) : NULL;
}

/**
 * shell_quick_menu_toggle_set_icon_name:
 * @self: a #ShellQuickMenuToggle
 * @icon_name: (nullable): the name of the new icon
 *
 * Sets #ShellQuickMenuToggle:icon-name.
 */
void
shell_quick_menu_toggle_set_icon_name (ShellQuickMenuToggle *self,
                                       const char           *icon_name)
{
  ShellQuickMenuTogglePrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_MENU_TOGGLE (self));

  priv = shell_quick_menu_toggle_get_instance_private (self);
  if (priv->contents != NULL)
    shell_quick_toggle_set_icon_name (priv->contents, icon_name);
}

/**
 * shell_quick_menu_toggle_get_gicon:
 * @self: a #ShellQuickMenuToggle
 *
 * Returns: (transfer none) (nullable): the icon of the toggle
 */
GIcon *
shell_quick_menu_toggle_get_gicon (ShellQuickMenuToggle *self)
{
  ShellQuickMenuTogglePrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_MENU_TOGGLE (self), NULL);

  priv = shell_quick_menu_toggle_get_instance_private (self);
  return priv->contents != NULL ? shell_quick_toggle_get_gicon (priv->contents) : NULL;
}

/**
 * shell_quick_menu_toggle_set_gicon:
 * @self: a #ShellQuickMenuToggle
 * @gicon: (nullable): the new icon
 *
 * Sets #ShellQuickMenuToggle:gicon.
 */
void
shell_quick_menu_toggle_set_gicon (ShellQuickMenuToggle *self,
                                   GIcon                *gicon)
{
  ShellQuickMenuTogglePrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_MENU_TOGGLE (self));

  priv = shell_quick_menu_toggle_get_instance_private (self);
  if (priv->contents != NULL)
    shell_quick_toggle_set_gicon (priv->contents, gicon);
}

/**
 * shell_quick_menu_toggle_get_menu_enabled:
 * @self: a #ShellQuickMenuToggle
 *
 * Returns: whether the button which opens the menu is shown
 */
gboolean
shell_quick_menu_toggle_get_menu_enabled (ShellQuickMenuToggle *self)
{
  ShellQuickMenuTogglePrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_MENU_TOGGLE (self), FALSE);

  priv = shell_quick_menu_toggle_get_instance_private (self);
  return priv->menu_enabled;
}

/**
 * shell_quick_menu_toggle_set_menu_enabled:
 * @self: a #ShellQuickMenuToggle
 * @enabled: whether the menu can be opened
 *
 * Sets #ShellQuickMenuToggle:menu-enabled.
 */
void
shell_quick_menu_toggle_set_menu_enabled (ShellQuickMenuToggle *self,
                                          gboolean              enabled)
{
  ShellQuickMenuTogglePrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_MENU_TOGGLE (self));

  priv = shell_quick_menu_toggle_get_instance_private (self);
  enabled = !!enabled;
  if (priv->menu_enabled == enabled)
    return;

  priv->menu_enabled = enabled;
  if (priv->menu_button != NULL)
    set_actor_visible (CLUTTER_ACTOR (priv->menu_button), enabled);

  g_object_notify_by_pspec (G_OBJECT (self),
                            menu_toggle_props[MENU_TOGGLE_PROP_MENU_ENABLED]);
}

/**
 * shell_quick_menu_toggle_get_menu_button:
 * @self: a #ShellQuickMenuToggle
 *
 * Returns: (transfer none): the button with the arrow which opens the menu
 *   (`_menuButton`)
 */
StButton *
shell_quick_menu_toggle_get_menu_button (ShellQuickMenuToggle *self)
{
  ShellQuickMenuTogglePrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_MENU_TOGGLE (self), NULL);

  priv = shell_quick_menu_toggle_get_instance_private (self);
  return priv->menu_button;
}

static void
menu_toggle_contents_notify_cb (GObject              *contents,
                                GParamSpec           *pspec,
                                ShellQuickMenuToggle *self)
{
  g_object_notify (G_OBJECT (self), pspec->name);
}

static void
menu_toggle_contents_clicked_cb (StButton             *contents,
                                 int                   clicked_button,
                                 ShellQuickMenuToggle *self)
{
  g_signal_emit_by_name (self, "clicked", clicked_button);
}

static void
menu_toggle_menu_button_clicked_cb (StButton             *button,
                                    int                   clicked_button,
                                    ShellQuickMenuToggle *self)
{
  quick_settings_item_open_menu (SHELL_QUICK_SETTINGS_ITEM (self));
}

static void
menu_toggle_popup_menu_cb (StWidget             *widget,
                           ShellQuickMenuToggle *self)
{
  st_widget_popup_menu (ST_WIDGET (self));
}

static void
menu_toggle_self_popup_menu_cb (StWidget             *widget,
                                ShellQuickMenuToggle *self)
{
  ShellQuickMenuTogglePrivate *priv = shell_quick_menu_toggle_get_instance_private (self);

  if (priv->menu_enabled)
    quick_settings_item_open_menu (SHELL_QUICK_SETTINGS_ITEM (self));
}

static void
shell_quick_menu_toggle_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  ShellQuickMenuToggle *self = SHELL_QUICK_MENU_TOGGLE (object);
  ShellQuickMenuTogglePrivate *priv = shell_quick_menu_toggle_get_instance_private (self);

  switch (prop_id)
    {
    case MENU_TOGGLE_PROP_TITLE:
      g_value_set_string (value, shell_quick_menu_toggle_get_title (self));
      break;

    case MENU_TOGGLE_PROP_SUBTITLE:
      g_value_set_string (value, shell_quick_menu_toggle_get_subtitle (self));
      break;

    case MENU_TOGGLE_PROP_GICON:
      g_value_set_object (value, shell_quick_menu_toggle_get_gicon (self));
      break;

    case MENU_TOGGLE_PROP_ICON_NAME:
      g_value_set_string (value, shell_quick_menu_toggle_get_icon_name (self));
      break;

    case MENU_TOGGLE_PROP_MENU_ENABLED:
      g_value_set_boolean (value, priv->menu_enabled);
      break;

    case MENU_TOGGLE_PROP_MENU_BUTTON_ACCESSIBLE_NAME:
      g_value_set_string (value, priv->menu_button_accessible_name);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_quick_menu_toggle_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  ShellQuickMenuToggle *self = SHELL_QUICK_MENU_TOGGLE (object);
  ShellQuickMenuTogglePrivate *priv = shell_quick_menu_toggle_get_instance_private (self);

  switch (prop_id)
    {
    case MENU_TOGGLE_PROP_TITLE:
      shell_quick_menu_toggle_set_title (self, g_value_get_string (value));
      break;

    case MENU_TOGGLE_PROP_SUBTITLE:
      shell_quick_menu_toggle_set_subtitle (self, g_value_get_string (value));
      break;

    case MENU_TOGGLE_PROP_GICON:
      shell_quick_menu_toggle_set_gicon (self, g_value_get_object (value));
      break;

    case MENU_TOGGLE_PROP_ICON_NAME:
      shell_quick_menu_toggle_set_icon_name (self, g_value_get_string (value));
      break;

    case MENU_TOGGLE_PROP_MENU_ENABLED:
      shell_quick_menu_toggle_set_menu_enabled (self, g_value_get_boolean (value));
      break;

    case MENU_TOGGLE_PROP_MENU_BUTTON_ACCESSIBLE_NAME:
      if (g_strcmp0 (priv->menu_button_accessible_name, g_value_get_string (value)) != 0)
        {
          g_free (priv->menu_button_accessible_name);
          priv->menu_button_accessible_name = g_value_dup_string (value);
          if (priv->menu_button != NULL)
            clutter_actor_set_accessible_name (CLUTTER_ACTOR (priv->menu_button),
                                               priv->menu_button_accessible_name);
          g_object_notify_by_pspec (object, pspec);
        }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_quick_menu_toggle_constructed (GObject *object)
{
  ShellQuickMenuToggle *self = SHELL_QUICK_MENU_TOGGLE (object);
  ShellQuickMenuTogglePrivate *priv = shell_quick_menu_toggle_get_instance_private (self);
  ShellQuickSettingsItemPrivate *item_priv =
    shell_quick_settings_item_get_instance_private (SHELL_QUICK_SETTINGS_ITEM (self));
  ClutterActor *box;
  ClutterActor *icon;

  /* The menu toggle always has a menu */
  item_priv->has_menu = TRUE;

  G_OBJECT_CLASS (shell_quick_menu_toggle_parent_class)->constructed (object);

  st_widget_add_style_class_name (ST_WIDGET (self), "quick-toggle-has-menu");

  box = g_object_new (ST_TYPE_BOX_LAYOUT, "x-expand", TRUE, NULL);
  st_bin_set_child (ST_BIN (self), box);

  priv->contents = g_object_new (SHELL_TYPE_QUICK_TOGGLE,
                                 "x-expand", TRUE,
                                 NULL);
  g_object_add_weak_pointer (G_OBJECT (priv->contents), (gpointer *) &priv->contents);
  clutter_actor_add_child (box, CLUTTER_ACTOR (priv->contents));

  priv->separator = new_widget ("quick-toggle-separator");
  g_object_add_weak_pointer (G_OBJECT (priv->separator), (gpointer *) &priv->separator);
  clutter_actor_add_child (box, priv->separator);

  icon = st_icon_new ();
  st_icon_set_icon_name (ST_ICON (icon), "go-next-symbolic");
  priv->menu_button = g_object_new (ST_TYPE_BUTTON,
                                    "style-class", "quick-toggle-menu-button icon-button",
                                    "child", icon,
                                    "can-focus", TRUE,
                                    "x-expand", FALSE,
                                    "y-expand", TRUE,
                                    NULL);
  g_object_add_weak_pointer (G_OBJECT (priv->menu_button), (gpointer *) &priv->menu_button);
  clutter_actor_add_child (box, CLUTTER_ACTOR (priv->menu_button));

  g_object_bind_property (priv->menu_button, "visible",
                          priv->separator, "visible",
                          G_BINDING_SYNC_CREATE);

  g_object_bind_property (self, "toggle-mode",
                          priv->contents, "toggle-mode",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self, "checked",
                          priv->contents, "checked",
                          G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);

  g_object_bind_property (self, "reactive",
                          priv->menu_button, "reactive",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self, "reactive",
                          priv->contents, "reactive",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self, "checked",
                          priv->menu_button, "checked",
                          G_BINDING_SYNC_CREATE);

  g_signal_connect_object (priv->contents, "notify::title",
                           G_CALLBACK (menu_toggle_contents_notify_cb), self, 0);
  g_signal_connect_object (priv->contents, "notify::subtitle",
                           G_CALLBACK (menu_toggle_contents_notify_cb), self, 0);
  g_signal_connect_object (priv->contents, "notify::gicon",
                           G_CALLBACK (menu_toggle_contents_notify_cb), self, 0);
  g_signal_connect_object (priv->contents, "notify::icon-name",
                           G_CALLBACK (menu_toggle_contents_notify_cb), self, 0);

  g_signal_connect_object (priv->contents, "clicked",
                           G_CALLBACK (menu_toggle_contents_clicked_cb), self, 0);
  g_signal_connect_object (priv->menu_button, "clicked",
                           G_CALLBACK (menu_toggle_menu_button_clicked_cb), self, 0);
  g_signal_connect_object (priv->menu_button, "popup-menu",
                           G_CALLBACK (menu_toggle_popup_menu_cb), self, 0);
  g_signal_connect_object (priv->contents, "popup-menu",
                           G_CALLBACK (menu_toggle_popup_menu_cb), self, 0);
  g_signal_connect_object (self, "popup-menu",
                           G_CALLBACK (menu_toggle_self_popup_menu_cb), self, 0);

  /* What the bindings of the JS copied when they were created */
  set_actor_visible (CLUTTER_ACTOR (priv->menu_button), priv->menu_enabled);
  if (priv->menu_button_accessible_name != NULL)
    clutter_actor_set_accessible_name (CLUTTER_ACTOR (priv->menu_button),
                                       priv->menu_button_accessible_name);
}

static void
shell_quick_menu_toggle_dispose (GObject *object)
{
  ShellQuickMenuToggle *self = SHELL_QUICK_MENU_TOGGLE (object);
  ShellQuickMenuTogglePrivate *priv = shell_quick_menu_toggle_get_instance_private (self);

  if (priv->contents != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->contents), (gpointer *) &priv->contents);
  if (priv->separator != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->separator), (gpointer *) &priv->separator);
  if (priv->menu_button != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->menu_button), (gpointer *) &priv->menu_button);
  priv->contents = NULL;
  priv->separator = NULL;
  priv->menu_button = NULL;

  G_OBJECT_CLASS (shell_quick_menu_toggle_parent_class)->dispose (object);
}

static void
shell_quick_menu_toggle_finalize (GObject *object)
{
  ShellQuickMenuToggle *self = SHELL_QUICK_MENU_TOGGLE (object);
  ShellQuickMenuTogglePrivate *priv = shell_quick_menu_toggle_get_instance_private (self);

  g_clear_pointer (&priv->menu_button_accessible_name, g_free);

  G_OBJECT_CLASS (shell_quick_menu_toggle_parent_class)->finalize (object);
}

static void
shell_quick_menu_toggle_class_init (ShellQuickMenuToggleClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_quick_menu_toggle_constructed;
  object_class->dispose = shell_quick_menu_toggle_dispose;
  object_class->finalize = shell_quick_menu_toggle_finalize;
  object_class->get_property = shell_quick_menu_toggle_get_property;
  object_class->set_property = shell_quick_menu_toggle_set_property;

  /**
   * ShellQuickMenuToggle:title:
   *
   * The title of the toggle.
   */
  menu_toggle_props[MENU_TOGGLE_PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickMenuToggle:subtitle:
   *
   * The subtitle of the toggle, which is hidden while it is %NULL.
   */
  menu_toggle_props[MENU_TOGGLE_PROP_SUBTITLE] =
    g_param_spec_string ("subtitle", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickMenuToggle:gicon:
   *
   * The icon of the toggle.
   */
  menu_toggle_props[MENU_TOGGLE_PROP_GICON] =
    g_param_spec_object ("gicon", NULL, NULL,
                         G_TYPE_ICON,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickMenuToggle:menu-enabled:
   *
   * Whether the button which opens the menu is shown (`menuEnabled`).
   */
  menu_toggle_props[MENU_TOGGLE_PROP_MENU_ENABLED] =
    g_param_spec_boolean ("menu-enabled", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickMenuToggle:menu-button-accessible-name:
   *
   * The accessible name of the button which opens the menu
   * (`menuButtonAccessibleName`).
   */
  menu_toggle_props[MENU_TOGGLE_PROP_MENU_BUTTON_ACCESSIBLE_NAME] =
    g_param_spec_string ("menu-button-accessible-name", NULL, NULL,
                         _("Open menu"),
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_MENU_TOGGLE_PROPS,
                                     menu_toggle_props);
  g_object_class_override_property (object_class, MENU_TOGGLE_PROP_ICON_NAME,
                                    "icon-name");
}

static void
shell_quick_menu_toggle_init (ShellQuickMenuToggle *self)
{
  ShellQuickMenuTogglePrivate *priv = shell_quick_menu_toggle_get_instance_private (self);

  priv->menu_enabled = TRUE;
  priv->menu_button_accessible_name = g_strdup (_("Open menu"));
}

/* ======================================================================== */
/* ShellQuickSlider                                                         */
/* ======================================================================== */

enum
{
  SLIDER_PROP_0,

  SLIDER_PROP_SLIDER,
  SLIDER_PROP_GICON,
  SLIDER_PROP_ICON_REACTIVE,
  SLIDER_PROP_ICON_LABEL,
  SLIDER_PROP_MENU_ENABLED,
  SLIDER_PROP_MENU_BUTTON_ACCESSIBLE_NAME,

  N_SLIDER_PROPS,

  SLIDER_PROP_ICON_NAME
};

static GParamSpec *slider_props[N_SLIDER_PROPS] = { NULL, };

enum
{
  SLIDER_ICON_CLICKED,
  N_SLIDER_SIGNALS
};

static guint slider_signals[N_SLIDER_SIGNALS] = { 0, };

typedef struct
{
  StIcon *icon;            /* weak */
  StButton *icon_button;   /* weak */
  ShellSlider *slider;     /* weak */
  ClutterActor *slider_bin; /* weak */
  StButton *menu_button;   /* weak */

  gboolean icon_reactive;
  gboolean sensitive;
  char *icon_label;
  gboolean menu_enabled;
  char *menu_button_accessible_name;
} ShellQuickSliderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellQuickSlider, shell_quick_slider,
                            SHELL_TYPE_QUICK_SETTINGS_ITEM)

/**
 * shell_quick_slider_get_slider:
 * @self: a #ShellQuickSlider
 *
 * Returns: (transfer none): the slider (`slider`)
 */
ShellSlider *
shell_quick_slider_get_slider (ShellQuickSlider *self)
{
  ShellQuickSliderPrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_SLIDER (self), NULL);

  priv = shell_quick_slider_get_instance_private (self);
  return priv->slider;
}

/**
 * shell_quick_slider_get_icon_name:
 * @self: a #ShellQuickSlider
 *
 * Returns: (nullable): the name of the icon of the slider
 */
const char *
shell_quick_slider_get_icon_name (ShellQuickSlider *self)
{
  ShellQuickSliderPrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_SLIDER (self), NULL);

  priv = shell_quick_slider_get_instance_private (self);
  return priv->icon != NULL ? st_icon_get_icon_name (priv->icon) : NULL;
}

/**
 * shell_quick_slider_set_icon_name:
 * @self: a #ShellQuickSlider
 * @icon_name: (nullable): the name of the new icon
 *
 * Sets #ShellQuickSlider:icon-name.
 */
void
shell_quick_slider_set_icon_name (ShellQuickSlider *self,
                                  const char       *icon_name)
{
  ShellQuickSliderPrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_SLIDER (self));

  priv = shell_quick_slider_get_instance_private (self);
  if (priv->icon != NULL)
    st_icon_set_icon_name (priv->icon, icon_name);
}

/**
 * shell_quick_slider_get_gicon:
 * @self: a #ShellQuickSlider
 *
 * Returns: (transfer none) (nullable): the icon of the slider
 */
GIcon *
shell_quick_slider_get_gicon (ShellQuickSlider *self)
{
  ShellQuickSliderPrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_SLIDER (self), NULL);

  priv = shell_quick_slider_get_instance_private (self);
  return priv->icon != NULL ? st_icon_get_gicon (priv->icon) : NULL;
}

/**
 * shell_quick_slider_set_gicon:
 * @self: a #ShellQuickSlider
 * @gicon: (nullable): the new icon
 *
 * Sets #ShellQuickSlider:gicon.
 */
void
shell_quick_slider_set_gicon (ShellQuickSlider *self,
                              GIcon            *gicon)
{
  ShellQuickSliderPrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_SLIDER (self));

  priv = shell_quick_slider_get_instance_private (self);
  if (priv->icon != NULL)
    st_icon_set_gicon (priv->icon, gicon);
}

/**
 * shell_quick_slider_get_icon_reactive:
 * @self: a #ShellQuickSlider
 *
 * Returns: whether the icon is a button
 */
gboolean
shell_quick_slider_get_icon_reactive (ShellQuickSlider *self)
{
  ShellQuickSliderPrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_SLIDER (self), FALSE);

  priv = shell_quick_slider_get_instance_private (self);
  return priv->icon_reactive;
}

/**
 * shell_quick_slider_set_icon_reactive:
 * @self: a #ShellQuickSlider
 * @reactive: whether the icon is a button
 *
 * Sets #ShellQuickSlider:icon-reactive.
 */
void
shell_quick_slider_set_icon_reactive (ShellQuickSlider *self,
                                      gboolean          reactive)
{
  ShellQuickSliderPrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_SLIDER (self));

  priv = shell_quick_slider_get_instance_private (self);
  reactive = !!reactive;
  if (priv->icon_reactive == reactive)
    return;

  priv->icon_reactive = reactive;
  if (priv->icon_button != NULL)
    {
      clutter_actor_set_reactive (CLUTTER_ACTOR (priv->icon_button),
                                  reactive && priv->sensitive);
      st_widget_set_can_focus (ST_WIDGET (priv->icon_button), reactive);
    }

  g_object_notify_by_pspec (G_OBJECT (self), slider_props[SLIDER_PROP_ICON_REACTIVE]);
}

/**
 * shell_quick_slider_get_sensitive:
 * @self: a #ShellQuickSlider
 *
 * Returns: whether the parts of the slider react to the user
 */
gboolean
shell_quick_slider_get_sensitive (ShellQuickSlider *self)
{
  ShellQuickSliderPrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_SLIDER (self), FALSE);

  priv = shell_quick_slider_get_instance_private (self);
  return priv->sensitive;
}

/**
 * shell_quick_slider_set_sensitive:
 * @self: a #ShellQuickSlider
 * @sensitive: whether the slider, its icon button and its menu button react
 *   to the user
 *
 * The slider itself is never reactive (its parts are), so this is what
 * `reactive` is for the other items. The icon button also needs
 * #ShellQuickSlider:icon-reactive.
 */
void
shell_quick_slider_set_sensitive (ShellQuickSlider *self,
                                  gboolean          sensitive)
{
  ShellQuickSliderPrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_SLIDER (self));

  priv = shell_quick_slider_get_instance_private (self);
  sensitive = !!sensitive;
  if (priv->sensitive == sensitive)
    return;

  priv->sensitive = sensitive;
  if (priv->slider != NULL)
    clutter_actor_set_reactive (CLUTTER_ACTOR (priv->slider), sensitive);
  if (priv->icon_button != NULL)
    clutter_actor_set_reactive (CLUTTER_ACTOR (priv->icon_button),
                                sensitive && priv->icon_reactive);
  if (priv->menu_button != NULL)
    clutter_actor_set_reactive (CLUTTER_ACTOR (priv->menu_button), sensitive);
}

/**
 * shell_quick_slider_get_icon_label:
 * @self: a #ShellQuickSlider
 *
 * Returns: the accessible name of the icon button
 */
const char *
shell_quick_slider_get_icon_label (ShellQuickSlider *self)
{
  ShellQuickSliderPrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_SLIDER (self), NULL);

  priv = shell_quick_slider_get_instance_private (self);
  return priv->icon_label;
}

/**
 * shell_quick_slider_set_icon_label:
 * @self: a #ShellQuickSlider
 * @label: the accessible name of the icon button
 *
 * Sets #ShellQuickSlider:icon-label.
 */
void
shell_quick_slider_set_icon_label (ShellQuickSlider *self,
                                   const char       *label)
{
  ShellQuickSliderPrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_SLIDER (self));

  priv = shell_quick_slider_get_instance_private (self);
  if (g_strcmp0 (priv->icon_label, label) == 0)
    return;

  g_free (priv->icon_label);
  priv->icon_label = g_strdup (label);
  if (priv->icon_button != NULL)
    clutter_actor_set_accessible_name (CLUTTER_ACTOR (priv->icon_button),
                                       priv->icon_label);

  g_object_notify_by_pspec (G_OBJECT (self), slider_props[SLIDER_PROP_ICON_LABEL]);
}

/**
 * shell_quick_slider_get_menu_enabled:
 * @self: a #ShellQuickSlider
 *
 * Returns: whether the button which opens the menu is shown
 */
gboolean
shell_quick_slider_get_menu_enabled (ShellQuickSlider *self)
{
  ShellQuickSliderPrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_SLIDER (self), FALSE);

  priv = shell_quick_slider_get_instance_private (self);
  return priv->menu_enabled;
}

/**
 * shell_quick_slider_set_menu_enabled:
 * @self: a #ShellQuickSlider
 * @enabled: whether the menu can be opened
 *
 * Sets #ShellQuickSlider:menu-enabled.
 */
void
shell_quick_slider_set_menu_enabled (ShellQuickSlider *self,
                                     gboolean          enabled)
{
  ShellQuickSliderPrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_SLIDER (self));

  priv = shell_quick_slider_get_instance_private (self);
  enabled = !!enabled;
  if (priv->menu_enabled == enabled)
    return;

  priv->menu_enabled = enabled;
  if (priv->menu_button != NULL)
    set_actor_visible (CLUTTER_ACTOR (priv->menu_button), enabled);

  g_object_notify_by_pspec (G_OBJECT (self), slider_props[SLIDER_PROP_MENU_ENABLED]);
}

static void
slider_icon_notify_cb (GObject          *icon,
                       GParamSpec       *pspec,
                       ShellQuickSlider *self)
{
  g_object_notify (G_OBJECT (self), pspec->name);
}

static void
slider_icon_button_clicked_cb (StButton         *button,
                               int               clicked_button,
                               ShellQuickSlider *self)
{
  g_signal_emit (self, slider_signals[SLIDER_ICON_CLICKED], 0);
}

/* Show as regular icon when non-interactive */
static void
slider_icon_button_notify_reactive_cb (GObject          *object,
                                       GParamSpec       *pspec,
                                       ShellQuickSlider *self)
{
  st_widget_remove_style_pseudo_class (ST_WIDGET (object), "insensitive");
}

static void
slider_key_focus_in_cb (ClutterActor     *slider,
                        ShellQuickSlider *self)
{
  ShellQuickSliderPrivate *priv = shell_quick_slider_get_instance_private (self);

  if (priv->slider_bin != NULL)
    st_widget_add_style_pseudo_class (ST_WIDGET (priv->slider_bin), "focus");
}

static void
slider_key_focus_out_cb (ClutterActor     *slider,
                         ShellQuickSlider *self)
{
  ShellQuickSliderPrivate *priv = shell_quick_slider_get_instance_private (self);

  if (priv->slider_bin != NULL)
    st_widget_remove_style_pseudo_class (ST_WIDGET (priv->slider_bin), "focus");
}

static void
slider_menu_button_clicked_cb (StButton         *button,
                               int               clicked_button,
                               ShellQuickSlider *self)
{
  quick_settings_item_open_menu (SHELL_QUICK_SETTINGS_ITEM (self));
}

static void
slider_popup_menu_cb (StWidget         *widget,
                      ShellQuickSlider *self)
{
  ShellQuickSliderPrivate *priv = shell_quick_slider_get_instance_private (self);

  if (priv->menu_enabled)
    quick_settings_item_open_menu (SHELL_QUICK_SETTINGS_ITEM (self));
}

static void
shell_quick_slider_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellQuickSlider *self = SHELL_QUICK_SLIDER (object);
  ShellQuickSliderPrivate *priv = shell_quick_slider_get_instance_private (self);

  switch (prop_id)
    {
    case SLIDER_PROP_SLIDER:
      g_value_set_object (value, priv->slider);
      break;

    case SLIDER_PROP_GICON:
      g_value_set_object (value, shell_quick_slider_get_gicon (self));
      break;

    case SLIDER_PROP_ICON_NAME:
      g_value_set_string (value, shell_quick_slider_get_icon_name (self));
      break;

    case SLIDER_PROP_ICON_REACTIVE:
      g_value_set_boolean (value, priv->icon_reactive);
      break;

    case SLIDER_PROP_ICON_LABEL:
      g_value_set_string (value, priv->icon_label);
      break;

    case SLIDER_PROP_MENU_ENABLED:
      g_value_set_boolean (value, priv->menu_enabled);
      break;

    case SLIDER_PROP_MENU_BUTTON_ACCESSIBLE_NAME:
      g_value_set_string (value, priv->menu_button_accessible_name);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_quick_slider_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellQuickSlider *self = SHELL_QUICK_SLIDER (object);
  ShellQuickSliderPrivate *priv = shell_quick_slider_get_instance_private (self);

  switch (prop_id)
    {
    case SLIDER_PROP_GICON:
      shell_quick_slider_set_gicon (self, g_value_get_object (value));
      break;

    case SLIDER_PROP_ICON_NAME:
      shell_quick_slider_set_icon_name (self, g_value_get_string (value));
      break;

    case SLIDER_PROP_ICON_REACTIVE:
      shell_quick_slider_set_icon_reactive (self, g_value_get_boolean (value));
      break;

    case SLIDER_PROP_ICON_LABEL:
      shell_quick_slider_set_icon_label (self, g_value_get_string (value));
      break;

    case SLIDER_PROP_MENU_ENABLED:
      shell_quick_slider_set_menu_enabled (self, g_value_get_boolean (value));
      break;

    case SLIDER_PROP_MENU_BUTTON_ACCESSIBLE_NAME:
      if (g_strcmp0 (priv->menu_button_accessible_name, g_value_get_string (value)) != 0)
        {
          g_free (priv->menu_button_accessible_name);
          priv->menu_button_accessible_name = g_value_dup_string (value);
          if (priv->menu_button != NULL)
            clutter_actor_set_accessible_name (CLUTTER_ACTOR (priv->menu_button),
                                               priv->menu_button_accessible_name);
          g_object_notify_by_pspec (object, pspec);
        }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_quick_slider_constructed (GObject *object)
{
  ShellQuickSlider *self = SHELL_QUICK_SLIDER (object);
  ShellQuickSliderPrivate *priv = shell_quick_slider_get_instance_private (self);
  ShellQuickSettingsItemPrivate *item_priv =
    shell_quick_settings_item_get_instance_private (SHELL_QUICK_SETTINGS_ITEM (self));
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *box;
  ClutterActor *icon;

  /* The slider always has a menu */
  item_priv->has_menu = TRUE;

  G_OBJECT_CLASS (shell_quick_slider_parent_class)->constructed (object);

  st_widget_set_style_class_name (ST_WIDGET (self), "quick-slider");
  st_widget_set_can_focus (ST_WIDGET (self), FALSE);
  clutter_actor_set_reactive (actor, FALSE);

  box = g_object_new (ST_TYPE_BOX_LAYOUT, "x-expand", TRUE, NULL);
  st_bin_set_child (ST_BIN (self), box);

  priv->icon = ST_ICON (st_icon_new ());
  g_object_add_weak_pointer (G_OBJECT (priv->icon), (gpointer *) &priv->icon);
  g_signal_connect_object (priv->icon, "notify::icon-name",
                           G_CALLBACK (slider_icon_notify_cb), self, 0);
  g_signal_connect_object (priv->icon, "notify::gicon",
                           G_CALLBACK (slider_icon_notify_cb), self, 0);

  priv->icon_button = g_object_new (ST_TYPE_BUTTON,
                                    "child", priv->icon,
                                    "style-class", "icon-button flat",
                                    "can-focus", priv->icon_reactive,
                                    "x-expand", FALSE,
                                    "y-expand", TRUE,
                                    NULL);
  g_object_add_weak_pointer (G_OBJECT (priv->icon_button), (gpointer *) &priv->icon_button);
  g_signal_connect_object (priv->icon_button, "clicked",
                           G_CALLBACK (slider_icon_button_clicked_cb), self, 0);
  g_signal_connect_object (priv->icon_button, "notify::reactive",
                           G_CALLBACK (slider_icon_button_notify_reactive_cb), self, 0);
  clutter_actor_set_reactive (CLUTTER_ACTOR (priv->icon_button), priv->icon_reactive);
  clutter_actor_set_accessible_name (CLUTTER_ACTOR (priv->icon_button),
                                     priv->icon_label != NULL ? priv->icon_label : "");
  clutter_actor_add_child (box, CLUTTER_ACTOR (priv->icon_button));

  priv->slider = shell_slider_new (0);
  g_object_add_weak_pointer (G_OBJECT (priv->slider), (gpointer *) &priv->slider);

  /* for focus indication */
  priv->slider_bin = g_object_new (ST_TYPE_BIN,
                                   "style-class", "slider-bin",
                                   "child", priv->slider,
                                   "reactive", TRUE,
                                   "x-expand", TRUE,
                                   "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                   NULL);
  g_object_add_weak_pointer (G_OBJECT (priv->slider_bin), (gpointer *) &priv->slider_bin);
  clutter_actor_add_child (box, priv->slider_bin);

  g_signal_connect_object (priv->slider, "key-focus-in",
                           G_CALLBACK (slider_key_focus_in_cb), self, 0);
  g_signal_connect_object (priv->slider, "key-focus-out",
                           G_CALLBACK (slider_key_focus_out_cb), self, 0);

  icon = st_icon_new ();
  st_icon_set_icon_name (ST_ICON (icon), "go-next-symbolic");
  priv->menu_button = g_object_new (ST_TYPE_BUTTON,
                                    "child", icon,
                                    "style-class", "icon-button flat",
                                    "can-focus", TRUE,
                                    "x-expand", FALSE,
                                    "y-expand", TRUE,
                                    NULL);
  g_object_add_weak_pointer (G_OBJECT (priv->menu_button), (gpointer *) &priv->menu_button);
  clutter_actor_add_child (box, CLUTTER_ACTOR (priv->menu_button));

  if (priv->menu_button_accessible_name != NULL)
    clutter_actor_set_accessible_name (CLUTTER_ACTOR (priv->menu_button),
                                       priv->menu_button_accessible_name);
  set_actor_visible (CLUTTER_ACTOR (priv->menu_button), priv->menu_enabled);
  g_signal_connect_object (priv->menu_button, "clicked",
                           G_CALLBACK (slider_menu_button_clicked_cb), self, 0);
  g_signal_connect_object (priv->slider, "popup-menu",
                           G_CALLBACK (slider_popup_menu_cb), self, 0);
}

static void
shell_quick_slider_dispose (GObject *object)
{
  ShellQuickSlider *self = SHELL_QUICK_SLIDER (object);
  ShellQuickSliderPrivate *priv = shell_quick_slider_get_instance_private (self);

  if (priv->icon != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->icon), (gpointer *) &priv->icon);
  if (priv->icon_button != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->icon_button), (gpointer *) &priv->icon_button);
  if (priv->slider != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->slider), (gpointer *) &priv->slider);
  if (priv->slider_bin != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->slider_bin), (gpointer *) &priv->slider_bin);
  if (priv->menu_button != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->menu_button), (gpointer *) &priv->menu_button);
  priv->icon = NULL;
  priv->icon_button = NULL;
  priv->slider = NULL;
  priv->slider_bin = NULL;
  priv->menu_button = NULL;

  G_OBJECT_CLASS (shell_quick_slider_parent_class)->dispose (object);
}

static void
shell_quick_slider_finalize (GObject *object)
{
  ShellQuickSlider *self = SHELL_QUICK_SLIDER (object);
  ShellQuickSliderPrivate *priv = shell_quick_slider_get_instance_private (self);

  g_clear_pointer (&priv->icon_label, g_free);
  g_clear_pointer (&priv->menu_button_accessible_name, g_free);

  G_OBJECT_CLASS (shell_quick_slider_parent_class)->finalize (object);
}

static void
shell_quick_slider_class_init (ShellQuickSliderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_quick_slider_constructed;
  object_class->dispose = shell_quick_slider_dispose;
  object_class->finalize = shell_quick_slider_finalize;
  object_class->get_property = shell_quick_slider_get_property;
  object_class->set_property = shell_quick_slider_set_property;

  /**
   * ShellQuickSlider:slider:
   *
   * The slider (`slider`).
   */
  slider_props[SLIDER_PROP_SLIDER] =
    g_param_spec_object ("slider", NULL, NULL,
                         SHELL_TYPE_SLIDER,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickSlider:gicon:
   *
   * The icon of the slider. Setting it unsets #ShellQuickSlider:icon-name.
   */
  slider_props[SLIDER_PROP_GICON] =
    g_param_spec_object ("gicon", NULL, NULL,
                         G_TYPE_ICON,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickSlider:icon-reactive:
   *
   * Whether the icon is a button, which emits #ShellQuickSlider::icon-clicked
   * (`iconReactive`).
   */
  slider_props[SLIDER_PROP_ICON_REACTIVE] =
    g_param_spec_boolean ("icon-reactive", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickSlider:icon-label:
   *
   * The accessible name of the icon button (`iconLabel`).
   */
  slider_props[SLIDER_PROP_ICON_LABEL] =
    g_param_spec_string ("icon-label", NULL, NULL,
                         "",
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickSlider:menu-enabled:
   *
   * Whether the button which opens the menu is shown (`menuEnabled`).
   */
  slider_props[SLIDER_PROP_MENU_ENABLED] =
    g_param_spec_boolean ("menu-enabled", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellQuickSlider:menu-button-accessible-name:
   *
   * The accessible name of the button which opens the menu
   * (`menuButtonAccessibleName`).
   */
  slider_props[SLIDER_PROP_MENU_BUTTON_ACCESSIBLE_NAME] =
    g_param_spec_string ("menu-button-accessible-name", NULL, NULL,
                         _("Open menu"),
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_SLIDER_PROPS, slider_props);
  g_object_class_override_property (object_class, SLIDER_PROP_ICON_NAME, "icon-name");

  /**
   * ShellQuickSlider::icon-clicked:
   * @self: the #ShellQuickSlider
   *
   * Emitted when the icon button is clicked (`icon-clicked`).
   */
  slider_signals[SLIDER_ICON_CLICKED] =
    g_signal_new ("icon-clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_quick_slider_init (ShellQuickSlider *self)
{
  ShellQuickSliderPrivate *priv = shell_quick_slider_get_instance_private (self);

  priv->sensitive = TRUE;
  priv->icon_label = g_strdup ("");
  priv->menu_button_accessible_name = g_strdup (_("Open menu"));
}

/* ======================================================================== */
/* ShellQuickSettingsMenu                                                   */
/* ======================================================================== */

enum
{
  QSM_PROP_0,

  QSM_PROP_N_COLUMNS,

  N_QSM_PROPS
};

static GParamSpec *qsm_props[N_QSM_PROPS] = { NULL, };

typedef struct
{
  int n_columns;

  ClutterActor *overlay; /* weak, a child of the actor of the menu */
  ClutterActor *grid;    /* weak */
  ClutterConstraint *x_constraint; /* weak, owned by the overlay */
  ClutterConstraint *y_constraint;
  ClutterEffect *dim_effect;       /* weak, owned by the box pointer */
  ShellQuickToggleMenu *active_menu; /* weak */

  MetaLaters *laters; /* weak */
  guint x_later;
  guint y_later;
} ShellQuickSettingsMenuPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellQuickSettingsMenu, shell_quick_settings_menu,
                            SHELL_TYPE_POPUP_MENU)

static void
quick_settings_menu_clear_later (ShellQuickSettingsMenuPrivate *priv,
                                 guint                         *id)
{
  if (*id != 0 && priv->laters != NULL)
    meta_laters_remove (priv->laters, *id);

  *id = 0;
}

/* Picks up additional spacing from any intermediate actors */
static void
quick_settings_menu_update_offset (ShellQuickSettingsMenu *self,
                                   gboolean                is_x)
{
  ShellQuickSettingsMenuPrivate *priv = shell_quick_settings_menu_get_instance_private (self);
  ShellBoxPointer *box_pointer = shell_popup_menu_get_box_pointer (SHELL_POPUP_MENU (self));
  ClutterConstraint *constraint = is_x ? priv->x_constraint : priv->y_constraint;
  graphene_point3d_t zero = GRAPHENE_POINT3D_INIT (0.f, 0.f, 0.f);
  graphene_point3d_t offset;

  if (priv->grid == NULL || box_pointer == NULL || constraint == NULL)
    return;

  clutter_actor_apply_relative_transform_to_point (priv->grid,
                                                   CLUTTER_ACTOR (box_pointer),
                                                   &zero, &offset);
  clutter_bind_constraint_set_offset (CLUTTER_BIND_CONSTRAINT (constraint),
                                      is_x ? offset.x : offset.y);
}

static gboolean
quick_settings_menu_x_later_cb (gpointer user_data)
{
  ShellQuickSettingsMenu *self = user_data;
  ShellQuickSettingsMenuPrivate *priv = shell_quick_settings_menu_get_instance_private (self);

  priv->x_later = 0;
  quick_settings_menu_update_offset (self, TRUE);
  return G_SOURCE_REMOVE;
}

static gboolean
quick_settings_menu_y_later_cb (gpointer user_data)
{
  ShellQuickSettingsMenu *self = user_data;
  ShellQuickSettingsMenuPrivate *priv = shell_quick_settings_menu_get_instance_private (self);

  priv->y_later = 0;
  quick_settings_menu_update_offset (self, FALSE);
  return G_SOURCE_REMOVE;
}

static void
quick_settings_menu_queue_offset (ShellQuickSettingsMenu *self,
                                  gboolean                is_x)
{
  ShellQuickSettingsMenuPrivate *priv = shell_quick_settings_menu_get_instance_private (self);
  guint *id = is_x ? &priv->x_later : &priv->y_later;
  MetaLaters *laters;

  if (*id != 0 || priv->grid == NULL)
    return;

  laters = get_laters ();
  if (laters == NULL)
    return;

  g_set_weak_pointer (&priv->laters, laters);
  *id = meta_laters_add (laters, META_LATER_BEFORE_REDRAW,
                         is_x ? quick_settings_menu_x_later_cb
                              : quick_settings_menu_y_later_cb,
                         self, NULL);
}

static void
quick_settings_menu_x_notify_cb (GObject                *object,
                                 GParamSpec             *pspec,
                                 ShellQuickSettingsMenu *self)
{
  quick_settings_menu_queue_offset (self, TRUE);
}

static void
quick_settings_menu_y_notify_cb (GObject                *object,
                                 GParamSpec             *pspec,
                                 ShellQuickSettingsMenu *self)
{
  quick_settings_menu_queue_offset (self, FALSE);
}

/* Hook "quick-settings.opened" (docs/api/hooks.md) */
static void
quick_settings_menu_open_state_changed_cb (ShellQuickSettingsMenu *self,
                                           gboolean                is_open)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();

  if (is_open && shell_hook_registry_is_declared (registry, "quick-settings.opened"))
    shell_hook_registry_emit_after (registry, "quick-settings.opened", NULL);
}

static void
quick_settings_menu_closed_cb (ShellQuickSettingsMenu *self)
{
  ClutterActor *actor = shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (self));

  if (actor != NULL)
    clutter_actor_hide (actor);
}

static void
quick_settings_menu_dim_done_cb (gboolean finished,
                                 gpointer user_data)
{
  EaseData *data = user_data;
  g_autoptr (ShellQuickSettingsMenu) self = g_weak_ref_get (&data->object);
  ShellQuickSettingsMenuPrivate *priv;

  if (self == NULL)
    return;

  priv = shell_quick_settings_menu_get_instance_private (self);
  if (priv->dim_effect != NULL)
    clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (priv->dim_effect), data->flag);
}

static void
quick_settings_menu_set_dimmed (ShellQuickSettingsMenu *self,
                                gboolean                dim)
{
  ShellQuickSettingsMenuPrivate *priv = shell_quick_settings_menu_get_instance_private (self);
  ShellBoxPointer *box_pointer = shell_popup_menu_get_box_pointer (SHELL_POPUP_MENU (self));
  ShellEaseParams params = make_ease_params (POPUP_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
  g_auto (GValue) target = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;
  CoglColor color;
  uint8_t val;

  if (box_pointer == NULL || priv->dim_effect == NULL)
    return;

  val = (uint8_t) (127 * (1 + (dim ? 1 : 0) * DIM_BRIGHTNESS));
  color.red = val;
  color.green = val;
  color.blue = val;
  color.alpha = 255;

  g_value_init (&target, COGL_TYPE_COLOR);
  g_value_set_boxed (&target, &color);

  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (priv->dim_effect), TRUE);

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (box_pointer),
                                       "@effects.dim.brightness",
                                       &target, &params,
                                       quick_settings_menu_dim_done_cb,
                                       ease_data_new (self, 0, dim),
                                       ease_data_free,
                                       &error))
    g_debug ("%s: %s", G_STRFUNC, error->message);
}

static void
quick_settings_menu_item_menu_state_cb (ShellQuickToggleMenu   *menu,
                                        gboolean                is_open,
                                        ShellQuickSettingsMenu *self)
{
  ShellQuickSettingsMenuPrivate *priv = shell_quick_settings_menu_get_instance_private (self);

  quick_settings_menu_set_dimmed (self, is_open);

  if (is_open)
    g_set_weak_pointer (&priv->active_menu, menu);
  else if (priv->active_menu == menu)
    g_clear_weak_pointer (&priv->active_menu);
}

static void
quick_settings_menu_complete_add_item (ShellQuickSettingsMenu *self,
                                       ClutterActor           *item,
                                       int                     col_span)
{
  ShellQuickSettingsMenuPrivate *priv = shell_quick_settings_menu_get_instance_private (self);
  g_auto (GValue) value = G_VALUE_INIT;
  ShellQuickToggleMenu *menu = NULL;

  g_value_init (&value, G_TYPE_INT);
  g_value_set_int (&value, MAX (col_span, 1));
  clutter_layout_manager_child_set_property (clutter_actor_get_layout_manager (priv->grid),
                                             priv->grid, item,
                                             "column-span", &value);

  if (SHELL_IS_QUICK_SETTINGS_ITEM (item))
    menu = shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (item));

  if (menu != NULL)
    {
      ClutterActor *menu_actor = shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (menu));

      clutter_actor_add_child (priv->overlay, menu_actor);
      g_signal_connect_object (menu, "open-state-changed",
                               G_CALLBACK (quick_settings_menu_item_menu_state_cb),
                               self, 0);
    }
}

static gboolean
shell_quick_settings_menu_real_close (ShellPopupMenuBase *base,
                                      ShellPopupAnimation animate)
{
  ShellQuickSettingsMenu *self = SHELL_QUICK_SETTINGS_MENU (base);
  ShellQuickSettingsMenuPrivate *priv = shell_quick_settings_menu_get_instance_private (self);
  ShellPopupMenuBaseClass *parent_class =
    SHELL_POPUP_MENU_BASE_CLASS (shell_quick_settings_menu_parent_class);

  if (!parent_class->close (base, animate))
    return FALSE;

  if (priv->active_menu != NULL)
    shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (priv->active_menu), animate);

  return TRUE;
}

static void
shell_quick_settings_menu_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellQuickSettingsMenu *self = SHELL_QUICK_SETTINGS_MENU (object);
  ShellQuickSettingsMenuPrivate *priv = shell_quick_settings_menu_get_instance_private (self);

  switch (prop_id)
    {
    case QSM_PROP_N_COLUMNS:
      g_value_set_int (value, priv->n_columns);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_quick_settings_menu_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellQuickSettingsMenu *self = SHELL_QUICK_SETTINGS_MENU (object);
  ShellQuickSettingsMenuPrivate *priv = shell_quick_settings_menu_get_instance_private (self);

  switch (prop_id)
    {
    case QSM_PROP_N_COLUMNS:
      priv->n_columns = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_quick_settings_menu_constructed (GObject *object)
{
  ShellQuickSettingsMenu *self = SHELL_QUICK_SETTINGS_MENU (object);
  ShellQuickSettingsMenuPrivate *priv = shell_quick_settings_menu_get_instance_private (self);
  ShellPopupMenuBase *base = SHELL_POPUP_MENU_BASE (self);
  ShellBoxPointer *box_pointer;
  ClutterActor *actor;
  ClutterActor *box;
  ClutterActor *placeholder;
  ClutterEffect *dim_effect;
  ClutterConstraint *constraint;

  G_OBJECT_CLASS (shell_quick_settings_menu_parent_class)->constructed (object);

  box_pointer = shell_popup_menu_get_box_pointer (SHELL_POPUP_MENU (self));
  box = CLUTTER_ACTOR (shell_popup_menu_base_get_box (base));

  actor = g_object_new (ST_TYPE_WIDGET,
                        "reactive", TRUE,
                        "width", 0.f,
                        "height", 0.f,
                        NULL);
  shell_popup_menu_base_set_actor (base, actor);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (box_pointer));
  /* Undo superclass defaults */
  st_widget_set_keynav_flags (ST_WIDGET (box_pointer), ST_KEYNAV_FLAG_NONE);

  st_focus_manager_add_group (st_focus_manager_get_for_stage (get_stage ()),
                              ST_WIDGET (actor));

  g_signal_connect (self, "menu-closed",
                    G_CALLBACK (quick_settings_menu_closed_cb), NULL);
  g_signal_connect (self, "open-state-changed",
                    G_CALLBACK (quick_settings_menu_open_state_changed_cb), NULL);

  dim_effect = clutter_brightness_contrast_effect_new ();
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (dim_effect), FALSE);
  clutter_actor_add_effect_with_name (CLUTTER_ACTOR (box_pointer), "dim", dim_effect);
  g_set_weak_pointer (&priv->dim_effect, dim_effect);
  st_widget_add_style_class_name (ST_WIDGET (box), "quick-settings");

  /* Overlay layer for menus */
  g_set_weak_pointer (&priv->overlay,
                      g_object_new (CLUTTER_TYPE_ACTOR,
                                    "layout-manager", clutter_bin_layout_new (),
                                    NULL));

  /* "clone" */
  placeholder = clutter_actor_new ();
  clutter_actor_add_constraint (placeholder,
                                clutter_bind_constraint_new (priv->overlay,
                                                             CLUTTER_BIND_HEIGHT, 0));

  g_set_weak_pointer (&priv->grid,
                      g_object_new (ST_TYPE_WIDGET,
                                    "style-class", "quick-settings-grid",
                                    "layout-manager",
                                    shell_quick_settings_layout_new (placeholder,
                                                                     priv->n_columns),
                                    NULL));
  clutter_actor_add_child (box, priv->grid);
  clutter_actor_add_child (priv->grid, placeholder);

  priv->x_constraint = clutter_bind_constraint_new (CLUTTER_ACTOR (box_pointer),
                                                    CLUTTER_BIND_X, 0);
  priv->y_constraint = clutter_bind_constraint_new (CLUTTER_ACTOR (box_pointer),
                                                    CLUTTER_BIND_Y, 0);

  g_signal_connect_object (priv->grid, "notify::x",
                           G_CALLBACK (quick_settings_menu_x_notify_cb), self, 0);
  g_signal_connect_object (box, "notify::x",
                           G_CALLBACK (quick_settings_menu_x_notify_cb), self, 0);
  g_signal_connect_object (shell_box_pointer_get_bin (box_pointer), "notify::x",
                           G_CALLBACK (quick_settings_menu_x_notify_cb), self, 0);

  g_signal_connect_object (priv->grid, "notify::y",
                           G_CALLBACK (quick_settings_menu_y_notify_cb), self, 0);
  g_signal_connect_object (box, "notify::y",
                           G_CALLBACK (quick_settings_menu_y_notify_cb), self, 0);
  g_signal_connect_object (shell_box_pointer_get_bin (box_pointer), "notify::y",
                           G_CALLBACK (quick_settings_menu_y_notify_cb), self, 0);

  clutter_actor_add_constraint (priv->overlay, priv->x_constraint);
  clutter_actor_add_constraint (priv->overlay, priv->y_constraint);
  constraint = clutter_bind_constraint_new (priv->grid, CLUTTER_BIND_WIDTH, 0);
  clutter_actor_add_constraint (priv->overlay, constraint);

  clutter_actor_add_child (actor, priv->overlay);

  /* The overlay owns the constraints; ours are weak pointers */
  g_object_add_weak_pointer (G_OBJECT (priv->x_constraint), (gpointer *) &priv->x_constraint);
  g_object_add_weak_pointer (G_OBJECT (priv->y_constraint), (gpointer *) &priv->y_constraint);
}

static void
shell_quick_settings_menu_dispose (GObject *object)
{
  ShellQuickSettingsMenu *self = SHELL_QUICK_SETTINGS_MENU (object);
  ShellQuickSettingsMenuPrivate *priv = shell_quick_settings_menu_get_instance_private (self);

  quick_settings_menu_clear_later (priv, &priv->x_later);
  quick_settings_menu_clear_later (priv, &priv->y_later);
  g_clear_weak_pointer (&priv->laters);

  if (priv->x_constraint != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->x_constraint), (gpointer *) &priv->x_constraint);
  if (priv->y_constraint != NULL)
    g_object_remove_weak_pointer (G_OBJECT (priv->y_constraint), (gpointer *) &priv->y_constraint);
  priv->x_constraint = NULL;
  priv->y_constraint = NULL;

  g_clear_weak_pointer (&priv->overlay);
  g_clear_weak_pointer (&priv->grid);
  g_clear_weak_pointer (&priv->dim_effect);
  g_clear_weak_pointer (&priv->active_menu);

  G_OBJECT_CLASS (shell_quick_settings_menu_parent_class)->dispose (object);
}

static void
shell_quick_settings_menu_class_init (ShellQuickSettingsMenuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupMenuBaseClass *base_class = SHELL_POPUP_MENU_BASE_CLASS (klass);

  object_class->constructed = shell_quick_settings_menu_constructed;
  object_class->dispose = shell_quick_settings_menu_dispose;
  object_class->get_property = shell_quick_settings_menu_get_property;
  object_class->set_property = shell_quick_settings_menu_set_property;

  base_class->close = shell_quick_settings_menu_real_close;

  /**
   * ShellQuickSettingsMenu:n-columns:
   *
   * The number of columns of the grid of items (`nColumns`).
   */
  qsm_props[QSM_PROP_N_COLUMNS] =
    g_param_spec_int ("n-columns", NULL, NULL,
                      1, G_MAXINT32, 1,
                      G_PARAM_READWRITE |
                      G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_QSM_PROPS, qsm_props);

  /* Lua extensions (gnome-lua): the hook point of the menu */
  shell_hook_registry_declare (shell_hook_registry_get_default (),
                               "quick-settings.opened", "", FALSE,
                               "The quick settings menu opened");
}

static void
shell_quick_settings_menu_init (ShellQuickSettingsMenu *self)
{
  ShellQuickSettingsMenuPrivate *priv = shell_quick_settings_menu_get_instance_private (self);

  priv->n_columns = 1;
}

/**
 * shell_quick_settings_menu_new:
 * @source_actor: the actor the menu is attached to
 * @n_columns: the number of columns of the grid of items
 *
 * Creates the quick settings menu (`new QuickSettingsMenu (sourceActor,
 * nColumns)`).
 *
 * Returns: (transfer full): a new #ShellQuickSettingsMenu
 */
ShellQuickSettingsMenu *
shell_quick_settings_menu_new (ClutterActor *source_actor,
                               int           n_columns)
{
  g_return_val_if_fail (CLUTTER_IS_ACTOR (source_actor), NULL);
  g_return_val_if_fail (n_columns >= 1, NULL);

  return g_object_new (SHELL_TYPE_QUICK_SETTINGS_MENU,
                       "source-actor", source_actor,
                       "n-columns", n_columns,
                       NULL);
}

/**
 * shell_quick_settings_menu_add_item:
 * @self: a #ShellQuickSettingsMenu
 * @item: the item to add to the end of the grid
 * @col_span: the number of columns the item takes
 *
 * Adds an item to the grid (`addItem ()`). The menu of the item, if it has
 * one, is added to the overlay of the menu.
 */
void
shell_quick_settings_menu_add_item (ShellQuickSettingsMenu *self,
                                    ClutterActor           *item,
                                    int                     col_span)
{
  ShellQuickSettingsMenuPrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_SETTINGS_MENU (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (item));

  priv = shell_quick_settings_menu_get_instance_private (self);
  if (priv->grid == NULL)
    return;

  clutter_actor_add_child (priv->grid, item);
  quick_settings_menu_complete_add_item (self, item, col_span);
}

/**
 * shell_quick_settings_menu_insert_item_before:
 * @self: a #ShellQuickSettingsMenu
 * @item: the item to add to the grid
 * @sibling: (nullable): the child of the grid to add @item before. For
 *   %NULL, @item goes first
 * @col_span: the number of columns the item takes
 *
 * Adds an item to the grid before another child (`insertItemBefore ()`).
 */
void
shell_quick_settings_menu_insert_item_before (ShellQuickSettingsMenu *self,
                                              ClutterActor           *item,
                                              ClutterActor           *sibling,
                                              int                     col_span)
{
  ShellQuickSettingsMenuPrivate *priv;

  g_return_if_fail (SHELL_IS_QUICK_SETTINGS_MENU (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (item));
  g_return_if_fail (sibling == NULL || CLUTTER_IS_ACTOR (sibling));

  priv = shell_quick_settings_menu_get_instance_private (self);
  if (priv->grid == NULL)
    return;

  clutter_actor_insert_child_below (priv->grid, item, sibling);
  quick_settings_menu_complete_add_item (self, item, col_span);
}

/**
 * shell_quick_settings_menu_get_item_col_span:
 * @self: a #ShellQuickSettingsMenu
 * @item: a child of the grid
 *
 * Returns: the number of columns @item spans, 0 if it is not in the grid
 */
int
shell_quick_settings_menu_get_item_col_span (ShellQuickSettingsMenu *self,
                                             ClutterActor           *item)
{
  ShellQuickSettingsMenuPrivate *priv;
  g_auto (GValue) value = G_VALUE_INIT;

  g_return_val_if_fail (SHELL_IS_QUICK_SETTINGS_MENU (self), 0);
  g_return_val_if_fail (CLUTTER_IS_ACTOR (item), 0);

  priv = shell_quick_settings_menu_get_instance_private (self);
  if (priv->grid == NULL || clutter_actor_get_parent (item) != priv->grid)
    return 0;

  g_value_init (&value, G_TYPE_INT);
  clutter_layout_manager_child_get_property (clutter_actor_get_layout_manager (priv->grid),
                                             priv->grid, item,
                                             "column-span", &value);
  return g_value_get_int (&value);
}

/**
 * shell_quick_settings_menu_set_item_col_span:
 * @self: a #ShellQuickSettingsMenu
 * @item: a child of the grid
 * @col_span: the number of columns @item takes
 *
 * Changes the number of columns of an item of the grid.
 */
void
shell_quick_settings_menu_set_item_col_span (ShellQuickSettingsMenu *self,
                                             ClutterActor           *item,
                                             int                     col_span)
{
  ShellQuickSettingsMenuPrivate *priv;
  g_auto (GValue) value = G_VALUE_INIT;

  g_return_if_fail (SHELL_IS_QUICK_SETTINGS_MENU (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (item));

  priv = shell_quick_settings_menu_get_instance_private (self);
  if (priv->grid == NULL || clutter_actor_get_parent (item) != priv->grid)
    return;

  g_value_init (&value, G_TYPE_INT);
  g_value_set_int (&value, MAX (col_span, 1));
  clutter_layout_manager_child_set_property (clutter_actor_get_layout_manager (priv->grid),
                                             priv->grid, item,
                                             "column-span", &value);
}

/**
 * shell_quick_settings_menu_get_first_item:
 * @self: a #ShellQuickSettingsMenu
 *
 * Returns: (transfer none) (nullable): the first child of the grid
 *   (`getFirstItem ()`). The grid holds an invisible placeholder as its last
 *   child, so for an empty menu this is the placeholder
 */
ClutterActor *
shell_quick_settings_menu_get_first_item (ShellQuickSettingsMenu *self)
{
  ShellQuickSettingsMenuPrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_SETTINGS_MENU (self), NULL);

  priv = shell_quick_settings_menu_get_instance_private (self);
  return priv->grid != NULL ? clutter_actor_get_first_child (priv->grid) : NULL;
}

/**
 * shell_quick_settings_menu_get_grid:
 * @self: a #ShellQuickSettingsMenu
 *
 * Returns: (transfer none) (nullable): the widget which holds the items
 *   (`_grid`)
 */
ClutterActor *
shell_quick_settings_menu_get_grid (ShellQuickSettingsMenu *self)
{
  ShellQuickSettingsMenuPrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_SETTINGS_MENU (self), NULL);

  priv = shell_quick_settings_menu_get_instance_private (self);
  return priv->grid;
}

/**
 * shell_quick_settings_menu_get_overlay:
 * @self: a #ShellQuickSettingsMenu
 *
 * Returns: (transfer none) (nullable): the layer which holds the menus of
 *   the items (`_overlay`)
 */
ClutterActor *
shell_quick_settings_menu_get_overlay (ShellQuickSettingsMenu *self)
{
  ShellQuickSettingsMenuPrivate *priv;

  g_return_val_if_fail (SHELL_IS_QUICK_SETTINGS_MENU (self), NULL);

  priv = shell_quick_settings_menu_get_instance_private (self);
  return priv->overlay;
}
