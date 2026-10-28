/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * panel-menu.c: The widgets which sit in the top bar. Port of
 * js/ui/panelMenu.js (`ButtonBox` and `Button`) and of the `SystemIndicator`
 * of js/ui/quickSettings.js.
 *
 * Differences from the JS classes, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `PanelMenu.Button (menuAlignment, nameText, dontCreateMenu)` took three
 *    positional arguments. They are the construct properties `menu-alignment`
 *    and `dont-create-menu`, and the `accessible-name` that every actor has
 *    (the accessible name of the button is "" when none is given, as before).
 *  - `ButtonBox._delegate` (`this._delegate = this`, used by the drag and drop
 *    code to find `handleDragOver ()`) is gone: a subclass which wants to be
 *    a drop target implements #ShellDragTarget.
 *  - `menu` is a property of type GObject, because it is a
 *    #ShellPopupMenuBase (any subclass, such as the quick settings menu) or,
 *    for a button constructed with `dont-create-menu`, a
 *    #ShellPopupDummyMenu. Writing it is shell_panel_menu_button_set_menu ().
 *  - `Main.uiGroup` and `Main.layoutManager` are read from #ShellMain. A
 *    button created before the layout manager exists keeps its menu out of the
 *    stage (the JS would have thrown).
 *  - `quickSettingsItems`, the plain JS array of a system indicator, is
 *    shell_system_indicator_add_quick_settings_item () and
 *    shell_system_indicator_get_quick_settings_items (). The indicator holds a
 *    reference on its items until it is disposed.
 */

#include "config.h"

#include <math.h>

#include <atk/atk.h>
#include <clutter/clutter.h>

#include "shell-global.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/panel-menu.h"
#include "ui/popup-menu.h"

/* ------------------------------------------------------------------------ */
/* The two kinds of menu a button can have                                  */
/* ------------------------------------------------------------------------ */

static gboolean
is_panel_menu (GObject *menu)
{
  return SHELL_IS_POPUP_MENU_BASE (menu) || SHELL_IS_POPUP_DUMMY_MENU (menu);
}

static ClutterActor *
panel_menu_get_actor (GObject *menu)
{
  if (SHELL_IS_POPUP_MENU_BASE (menu))
    return shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (menu));
  if (SHELL_IS_POPUP_DUMMY_MENU (menu))
    return shell_popup_dummy_menu_get_actor (SHELL_POPUP_DUMMY_MENU (menu));

  return NULL;
}

static gboolean
panel_menu_get_is_open (GObject *menu)
{
  if (SHELL_IS_POPUP_MENU_BASE (menu))
    return shell_popup_menu_base_get_is_open (SHELL_POPUP_MENU_BASE (menu));
  if (SHELL_IS_POPUP_DUMMY_MENU (menu))
    return shell_popup_dummy_menu_get_is_open (SHELL_POPUP_DUMMY_MENU (menu));

  return FALSE;
}

static void
panel_menu_toggle (GObject *menu)
{
  if (SHELL_IS_POPUP_MENU_BASE (menu))
    shell_popup_menu_base_toggle (SHELL_POPUP_MENU_BASE (menu));
  else if (SHELL_IS_POPUP_DUMMY_MENU (menu))
    shell_popup_dummy_menu_toggle (SHELL_POPUP_DUMMY_MENU (menu));
}

static void
panel_menu_close (GObject             *menu,
                  ShellPopupAnimation  animate)
{
  if (SHELL_IS_POPUP_MENU_BASE (menu))
    shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (menu), animate);
  else if (SHELL_IS_POPUP_DUMMY_MENU (menu))
    shell_popup_dummy_menu_close (SHELL_POPUP_DUMMY_MENU (menu), animate);
}

static void
panel_menu_destroy (GObject *menu)
{
  if (SHELL_IS_POPUP_MENU_BASE (menu))
    shell_popup_menu_base_destroy (SHELL_POPUP_MENU_BASE (menu));
  else if (SHELL_IS_POPUP_DUMMY_MENU (menu))
    shell_popup_dummy_menu_destroy (SHELL_POPUP_DUMMY_MENU (menu));
}

/* ======================================================================== */
/* ShellPanelButtonBox                                                      */
/* ======================================================================== */

enum
{
  BOX_PROP_0,

  BOX_PROP_CONTAINER,

  N_BOX_PROPS
};

static GParamSpec *box_props[N_BOX_PROPS] = { NULL, };

typedef struct
{
  StBin *container; /* strong */

  float min_hpadding;
  float nat_hpadding;
} ShellPanelButtonBoxPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellPanelButtonBox, shell_panel_button_box, ST_TYPE_WIDGET)

/**
 * shell_panel_button_box_get_container:
 * @self: a #ShellPanelButtonBox
 *
 * Gets the bin which holds the button, and which is the actor that is added to
 * a box of the top bar (`container` in the old JS class).
 *
 * Returns: (transfer none): the container of the button
 */
StBin *
shell_panel_button_box_get_container (ShellPanelButtonBox *self)
{
  ShellPanelButtonBoxPrivate *priv;

  g_return_val_if_fail (SHELL_IS_PANEL_BUTTON_BOX (self), NULL);

  priv = shell_panel_button_box_get_instance_private (self);
  return priv->container;
}

static void
shell_panel_button_box_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  ShellPanelButtonBox *self = SHELL_PANEL_BUTTON_BOX (object);

  switch (prop_id)
    {
    case BOX_PROP_CONTAINER:
      g_value_set_object (value, shell_panel_button_box_get_container (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_panel_button_box_style_changed (StWidget *widget)
{
  ShellPanelButtonBox *self = SHELL_PANEL_BUTTON_BOX (widget);
  ShellPanelButtonBoxPrivate *priv = shell_panel_button_box_get_instance_private (self);
  StThemeNode *theme_node = st_widget_get_theme_node (widget);

  priv->min_hpadding = st_theme_node_get_length (theme_node, "-minimum-hpadding");
  priv->nat_hpadding = st_theme_node_get_length (theme_node, "-natural-hpadding");

  ST_WIDGET_CLASS (shell_panel_button_box_parent_class)->style_changed (widget);
}

static void
shell_panel_button_box_get_preferred_width (ClutterActor *actor,
                                            float         for_height,
                                            float        *min_width_p,
                                            float        *nat_width_p)
{
  ShellPanelButtonBox *self = SHELL_PANEL_BUTTON_BOX (actor);
  ShellPanelButtonBoxPrivate *priv = shell_panel_button_box_get_instance_private (self);
  ClutterActor *child = clutter_actor_get_first_child (actor);
  float minimum_size = 0.0f;
  float natural_size = 0.0f;

  if (child != NULL)
    clutter_actor_get_preferred_width (child, -1, &minimum_size, &natural_size);

  minimum_size += 2 * priv->min_hpadding;
  natural_size += 2 * priv->nat_hpadding;

  if (min_width_p != NULL)
    *min_width_p = minimum_size;
  if (nat_width_p != NULL)
    *nat_width_p = natural_size;
}

static void
shell_panel_button_box_get_preferred_height (ClutterActor *actor,
                                             float         for_width,
                                             float        *min_height_p,
                                             float        *nat_height_p)
{
  ClutterActor *child = clutter_actor_get_first_child (actor);
  float minimum_size = 0.0f;
  float natural_size = 0.0f;

  if (child != NULL)
    clutter_actor_get_preferred_height (child, -1, &minimum_size, &natural_size);

  if (min_height_p != NULL)
    *min_height_p = minimum_size;
  if (nat_height_p != NULL)
    *nat_height_p = natural_size;
}

static void
shell_panel_button_box_allocate (ClutterActor          *actor,
                                 const ClutterActorBox *box)
{
  ShellPanelButtonBox *self = SHELL_PANEL_BUTTON_BOX (actor);
  ShellPanelButtonBoxPrivate *priv = shell_panel_button_box_get_instance_private (self);
  ClutterActor *child;
  ClutterActorBox child_box;
  float nat_width;
  float avail_width, avail_height;

  /* Not chained up, like the vfunc_allocate () of the JS class */
  clutter_actor_set_allocation (actor, box);

  child = clutter_actor_get_first_child (actor);
  if (child == NULL)
    return;

  clutter_actor_get_preferred_width (child, -1, NULL, &nat_width);

  avail_width = box->x2 - box->x1;
  avail_height = box->y2 - box->y1;

  if (nat_width + 2 * priv->nat_hpadding <= avail_width)
    {
      child_box.x1 = priv->nat_hpadding;
      child_box.x2 = avail_width - priv->nat_hpadding;
    }
  else
    {
      child_box.x1 = priv->min_hpadding;
      child_box.x2 = avail_width - priv->min_hpadding;
    }

  child_box.y1 = 0;
  child_box.y2 = avail_height;

  clutter_actor_allocate (child, &child_box);
}

static void
shell_panel_button_box_destroy (ClutterActor *actor)
{
  ShellPanelButtonBox *self = SHELL_PANEL_BUTTON_BOX (actor);
  ShellPanelButtonBoxPrivate *priv = shell_panel_button_box_get_instance_private (self);

  /* _onDestroy (): this.container.child = null; this.container.destroy () */
  if (priv->container != NULL)
    {
      g_autoptr (StBin) container = g_object_ref (priv->container);

      st_bin_set_child (container, NULL);
      clutter_actor_destroy (CLUTTER_ACTOR (container));
    }

  CLUTTER_ACTOR_CLASS (shell_panel_button_box_parent_class)->destroy (actor);
}

static void
shell_panel_button_box_constructed (GObject *object)
{
  ShellPanelButtonBox *self = SHELL_PANEL_BUTTON_BOX (object);
  ShellPanelButtonBoxPrivate *priv = shell_panel_button_box_get_instance_private (self);

  G_OBJECT_CLASS (shell_panel_button_box_parent_class)->constructed (object);

  /* new St.Bin ({child: this}) */
  priv->container = ST_BIN (g_object_ref_sink (st_bin_new ()));
  st_bin_set_child (priv->container, CLUTTER_ACTOR (self));
}

static void
shell_panel_button_box_dispose (GObject *object)
{
  ShellPanelButtonBox *self = SHELL_PANEL_BUTTON_BOX (object);
  ShellPanelButtonBoxPrivate *priv = shell_panel_button_box_get_instance_private (self);

  g_clear_object (&priv->container);

  G_OBJECT_CLASS (shell_panel_button_box_parent_class)->dispose (object);
}

static void
shell_panel_button_box_class_init (ShellPanelButtonBoxClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);

  object_class->constructed = shell_panel_button_box_constructed;
  object_class->dispose = shell_panel_button_box_dispose;
  object_class->get_property = shell_panel_button_box_get_property;

  actor_class->get_preferred_width = shell_panel_button_box_get_preferred_width;
  actor_class->get_preferred_height = shell_panel_button_box_get_preferred_height;
  actor_class->allocate = shell_panel_button_box_allocate;
  actor_class->destroy = shell_panel_button_box_destroy;

  widget_class->style_changed = shell_panel_button_box_style_changed;

  /**
   * ShellPanelButtonBox:container:
   *
   * The #StBin which holds the button. It is what the panel adds to its boxes
   * and is destroyed with the button (`container`).
   */
  box_props[BOX_PROP_CONTAINER] =
    g_param_spec_object ("container", NULL, NULL,
                         ST_TYPE_BIN,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_BOX_PROPS, box_props);
}

static void
shell_panel_button_box_init (ShellPanelButtonBox *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  st_widget_set_style_class_name (ST_WIDGET (self), "panel-button");
  clutter_actor_set_x_expand (actor, TRUE);
  clutter_actor_set_y_expand (actor, TRUE);
}

/* ======================================================================== */
/* ShellPanelMenuButton                                                     */
/* ======================================================================== */

enum
{
  MENU_PROP_0,

  MENU_PROP_MENU_ALIGNMENT,
  MENU_PROP_DONT_CREATE_MENU,
  MENU_PROP_MENU,

  N_MENU_PROPS
};

static GParamSpec *menu_props[N_MENU_PROPS] = { NULL, };

enum
{
  MENU_SET,

  N_MENU_SIGNALS
};

static guint menu_signals[N_MENU_SIGNALS] = { 0, };

typedef struct
{
  /* construct properties */
  double menu_alignment;
  gboolean dont_create_menu;

  GObject *menu; /* strong; a ShellPopupMenuBase or a ShellPopupDummyMenu */

  ClutterClickGesture *click_gesture; /* strong */
  ClutterKeyController *key_controller; /* strong */
} ShellPanelMenuButtonPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellPanelMenuButton, shell_panel_menu_button,
                            SHELL_TYPE_PANEL_BUTTON_BOX)

/**
 * shell_panel_menu_button_get_menu:
 * @self: a #ShellPanelMenuButton
 *
 * Gets the menu of the button (`menu`): a #ShellPopupMenuBase, or a
 * #ShellPopupDummyMenu if the button was constructed with
 * #ShellPanelMenuButton:dont-create-menu and no menu was set since.
 *
 * Returns: (transfer none) (nullable): the menu
 */
GObject *
shell_panel_menu_button_get_menu (ShellPanelMenuButton *self)
{
  ShellPanelMenuButtonPrivate *priv;

  g_return_val_if_fail (SHELL_IS_PANEL_MENU_BUTTON (self), NULL);

  priv = shell_panel_menu_button_get_instance_private (self);
  return priv->menu;
}

/**
 * shell_panel_menu_button_set_sensitive:
 * @self: a #ShellPanelMenuButton
 * @sensitive: whether the button reacts to the pointer and the keyboard
 *
 * Makes the button reactive, focusable and hover tracking, or not
 * (`setSensitive ()`).
 */
void
shell_panel_menu_button_set_sensitive (ShellPanelMenuButton *self,
                                       gboolean              sensitive)
{
  g_return_if_fail (SHELL_IS_PANEL_MENU_BUTTON (self));

  clutter_actor_set_reactive (CLUTTER_ACTOR (self), sensitive);
  st_widget_set_can_focus (ST_WIDGET (self), sensitive);
  st_widget_set_track_hover (ST_WIDGET (self), sensitive);
}

/**
 * shell_panel_menu_button_get_menu_is_open:
 * @self: a #ShellPanelMenuButton
 *
 * Returns: whether the menu of the button is open
 */
gboolean
shell_panel_menu_button_get_menu_is_open (ShellPanelMenuButton *self)
{
  ShellPanelMenuButtonPrivate *priv;

  g_return_val_if_fail (SHELL_IS_PANEL_MENU_BUTTON (self), FALSE);

  priv = shell_panel_menu_button_get_instance_private (self);
  return priv->menu != NULL && panel_menu_get_is_open (priv->menu);
}

/**
 * shell_panel_menu_button_toggle_menu:
 * @self: a #ShellPanelMenuButton
 *
 * Toggles the menu of the button and, if that opened it, moves the key focus
 * into the menu. This is what the keybindings of the calendar and the quick
 * settings do (`Panel._toggleMenu ()`).
 */
void
shell_panel_menu_button_toggle_menu (ShellPanelMenuButton *self)
{
  ShellPanelMenuButtonPrivate *priv;

  g_return_if_fail (SHELL_IS_PANEL_MENU_BUTTON (self));

  priv = shell_panel_menu_button_get_instance_private (self);
  if (priv->menu == NULL)
    return;

  panel_menu_toggle (priv->menu);

  if (panel_menu_get_is_open (priv->menu))
    {
      ClutterActor *menu_actor = panel_menu_get_actor (priv->menu);

      if (ST_IS_WIDGET (menu_actor))
        st_widget_navigate_focus (ST_WIDGET (menu_actor), NULL,
                                  ST_DIR_TAB_FORWARD, FALSE);
    }
}

/**
 * shell_panel_menu_button_close_menu:
 * @self: a #ShellPanelMenuButton
 * @animate: how to animate the transition
 *
 * Closes the menu of the button, if it has one (`Panel._closeMenu ()`).
 */
void
shell_panel_menu_button_close_menu (ShellPanelMenuButton *self,
                                    ShellPopupAnimation   animate)
{
  ShellPanelMenuButtonPrivate *priv;

  g_return_if_fail (SHELL_IS_PANEL_MENU_BUTTON (self));

  priv = shell_panel_menu_button_get_instance_private (self);
  if (priv->menu != NULL)
    panel_menu_close (priv->menu, animate);
}

static void
click_gesture_recognize_cb (ClutterGesture       *gesture,
                            ShellPanelMenuButton *self)
{
  ShellPanelMenuButtonPrivate *priv = shell_panel_menu_button_get_instance_private (self);

  if (priv->menu != NULL)
    panel_menu_toggle (priv->menu);
}

static gboolean
key_controller_key_press_cb (ClutterKeyController *controller,
                             ShellPanelMenuButton *self)
{
  ShellPanelMenuButtonPrivate *priv = shell_panel_menu_button_get_instance_private (self);
  uint32_t symbol = 0;

  clutter_key_controller_get_key (controller, &symbol, NULL, NULL);

  if ((symbol == CLUTTER_KEY_Left || symbol == CLUTTER_KEY_Right) &&
      priv->menu != NULL)
    {
      ClutterStage *stage = CLUTTER_STAGE (clutter_actor_get_stage (CLUTTER_ACTOR (self)));
      ClutterActor *menu_actor = panel_menu_get_actor (priv->menu);
      ClutterActor *key_focus;
      ClutterActor *focus_actor;
      StWidget *group;

      if (stage == NULL || menu_actor == NULL)
        return CLUTTER_EVENT_PROPAGATE;

      key_focus = clutter_stage_get_key_focus (stage);
      focus_actor = clutter_actor_contains (menu_actor, key_focus) ?
                    CLUTTER_ACTOR (self) : key_focus;

      /* get_group () takes a widget, the stage itself can have the focus */
      if (!ST_IS_WIDGET (focus_actor))
        return CLUTTER_EVENT_PROPAGATE;

      group = st_focus_manager_get_group (shell_global_get_focus_manager (shell_global_get ()),
                                          ST_WIDGET (focus_actor));
      if (group != NULL)
        {
          StDirectionType direction =
            symbol == CLUTTER_KEY_Left ? ST_DIR_LEFT : ST_DIR_RIGHT;

          st_widget_navigate_focus (group, focus_actor, direction, FALSE);
          return CLUTTER_EVENT_STOP;
        }
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static void
menu_open_state_changed_cb (GObject              *menu,
                            gboolean              open,
                            ShellPanelMenuButton *self)
{
  ShellPanelMenuButtonPrivate *priv = shell_panel_menu_button_get_instance_private (self);
  ShellLayoutManager *layout_manager;
  ClutterActor *menu_actor;
  StThemeContext *theme_context;
  MtkRectangle work_area;
  int primary_index;
  int scale_factor;
  float vertical_margins;
  int max_height;
  g_autofree char *style = NULL;

  if (open)
    st_widget_add_style_pseudo_class (ST_WIDGET (self), "active");
  else
    st_widget_remove_style_pseudo_class (ST_WIDGET (self), "active");

  if (priv->menu == NULL)
    return;

  menu_actor = panel_menu_get_actor (priv->menu);
  layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  if (!ST_IS_WIDGET (menu_actor) || layout_manager == NULL)
    return;

  primary_index = shell_layout_manager_get_primary_index (layout_manager);
  if (primary_index < 0)
    return;

  /* Setting the max-height won't do any good if the minimum height of the
   * menu is higher then the screen; it's useful if part of the menu is
   * scrollable so the minimum height is smaller than the natural height */
  shell_layout_manager_get_work_area_for_monitor (layout_manager, primary_index, &work_area);
  theme_context = st_theme_context_get_for_stage (shell_global_get_stage (shell_global_get ()));
  scale_factor = st_theme_context_get_scale_factor (theme_context);
  vertical_margins = clutter_actor_get_margin_top (menu_actor) +
                     clutter_actor_get_margin_bottom (menu_actor);

  /* The workarea and margin dimensions are in physical pixels, but CSS
   * measures are in logical pixels, so make sure to consider the scale
   * factor when computing max-height */
  max_height = (int) floor ((work_area.height - vertical_margins) / scale_factor + 0.5);
  style = g_strdup_printf ("max-height: %dpx;", max_height);
  st_widget_set_style (ST_WIDGET (menu_actor), style);
}

static void
key_controller_detach (ShellPanelMenuButton *self)
{
  ShellPanelMenuButtonPrivate *priv = shell_panel_menu_button_get_instance_private (self);
  ClutterActor *owner;

  if (priv->key_controller == NULL)
    return;

  owner = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (priv->key_controller));
  if (owner != NULL)
    clutter_actor_remove_action (owner, CLUTTER_ACTION (priv->key_controller));
}

/**
 * shell_panel_menu_button_set_menu:
 * @self: a #ShellPanelMenuButton
 * @menu: (nullable) (type GObject.Object): a #ShellPopupMenuBase, a
 *   #ShellPopupDummyMenu or %NULL
 *
 * Replaces the menu of the button (`setMenu ()`). The previous menu is
 * destroyed. The new menu gets the `panel-menu` style class, is added to the
 * ui group, hidden, and the button toggles it when clicked. Emits
 * #ShellPanelMenuButton::menu-set.
 */
void
shell_panel_menu_button_set_menu (ShellPanelMenuButton *self,
                                  GObject              *menu)
{
  ShellPanelMenuButtonPrivate *priv;

  g_return_if_fail (SHELL_IS_PANEL_MENU_BUTTON (self));
  g_return_if_fail (menu == NULL || is_panel_menu (menu));

  priv = shell_panel_menu_button_get_instance_private (self);

  if (priv->menu == menu)
    return;

  if (priv->menu != NULL)
    {
      g_autoptr (GObject) old_menu = g_steal_pointer (&priv->menu);

      key_controller_detach (self);
      panel_menu_destroy (old_menu);
    }

  priv->menu = menu != NULL ? g_object_ref (menu) : NULL;

  if (priv->menu != NULL)
    {
      ClutterActor *menu_actor = panel_menu_get_actor (priv->menu);

      if (SHELL_IS_POPUP_MENU_BASE (priv->menu))
        {
          ShellLayoutManager *layout_manager =
            shell_main_get_layout_manager (shell_main_get_default ());

          if (ST_IS_WIDGET (menu_actor))
            st_widget_add_style_class_name (ST_WIDGET (menu_actor), "panel-menu");

          g_signal_connect_object (priv->menu, "open-state-changed",
                                   G_CALLBACK (menu_open_state_changed_cb),
                                   self, 0);

          clutter_actor_add_action (menu_actor, CLUTTER_ACTION (priv->key_controller));

          if (layout_manager != NULL)
            {
              clutter_actor_add_child (CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager)),
                                       menu_actor);
              clutter_actor_hide (menu_actor);
            }
        }
      else
        {
          /* The actor of a dummy menu is the button itself */
          clutter_actor_add_action (menu_actor, CLUTTER_ACTION (priv->key_controller));
        }
    }

  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (priv->click_gesture),
                                  priv->menu != NULL);

  g_object_notify_by_pspec (G_OBJECT (self), menu_props[MENU_PROP_MENU]);
  g_signal_emit (self, menu_signals[MENU_SET], 0);
}

static void
shell_panel_menu_button_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  ShellPanelMenuButton *self = SHELL_PANEL_MENU_BUTTON (object);
  ShellPanelMenuButtonPrivate *priv = shell_panel_menu_button_get_instance_private (self);

  switch (prop_id)
    {
    case MENU_PROP_MENU_ALIGNMENT:
      g_value_set_double (value, priv->menu_alignment);
      break;

    case MENU_PROP_DONT_CREATE_MENU:
      g_value_set_boolean (value, priv->dont_create_menu);
      break;

    case MENU_PROP_MENU:
      g_value_set_object (value, priv->menu);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_panel_menu_button_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  ShellPanelMenuButton *self = SHELL_PANEL_MENU_BUTTON (object);
  ShellPanelMenuButtonPrivate *priv = shell_panel_menu_button_get_instance_private (self);

  switch (prop_id)
    {
    case MENU_PROP_MENU_ALIGNMENT:
      priv->menu_alignment = g_value_get_double (value);
      break;

    case MENU_PROP_DONT_CREATE_MENU:
      priv->dont_create_menu = g_value_get_boolean (value);
      break;

    case MENU_PROP_MENU:
      shell_panel_menu_button_set_menu (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_panel_menu_button_constructed (GObject *object)
{
  ShellPanelMenuButton *self = SHELL_PANEL_MENU_BUTTON (object);
  ShellPanelMenuButtonPrivate *priv = shell_panel_menu_button_get_instance_private (self);

  G_OBJECT_CLASS (shell_panel_menu_button_parent_class)->constructed (object);

  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (priv->click_gesture),
                                  !priv->dont_create_menu);

  if (priv->dont_create_menu)
    {
      priv->menu = G_OBJECT (shell_popup_dummy_menu_new (CLUTTER_ACTOR (self)));
      clutter_actor_add_action (CLUTTER_ACTOR (self),
                                CLUTTER_ACTION (priv->key_controller));
    }
  else
    {
      g_autoptr (ShellPopupMenu) menu =
        shell_popup_menu_new (CLUTTER_ACTOR (self), priv->menu_alignment, ST_SIDE_TOP);

      shell_panel_menu_button_set_menu (self, G_OBJECT (menu));
    }
}

static void
shell_panel_menu_button_hide (ClutterActor *actor)
{
  ShellPanelMenuButton *self = SHELL_PANEL_MENU_BUTTON (actor);
  ShellPanelMenuButtonPrivate *priv = shell_panel_menu_button_get_instance_private (self);

  CLUTTER_ACTOR_CLASS (shell_panel_menu_button_parent_class)->hide (actor);

  if (priv->menu != NULL)
    panel_menu_close (priv->menu, SHELL_POPUP_ANIMATION_NONE);
}

static void
shell_panel_menu_button_destroy (ClutterActor *actor)
{
  ShellPanelMenuButton *self = SHELL_PANEL_MENU_BUTTON (actor);
  ShellPanelMenuButtonPrivate *priv = shell_panel_menu_button_get_instance_private (self);

  /* _onDestroy (): this.menu.destroy () before the ButtonBox cleanup. The
   * menu stays readable from the destroyed button, as before. */
  if (priv->menu != NULL)
    {
      g_autoptr (GObject) menu = g_object_ref (priv->menu);

      panel_menu_destroy (menu);
    }

  CLUTTER_ACTOR_CLASS (shell_panel_menu_button_parent_class)->destroy (actor);
}

static void
shell_panel_menu_button_dispose (GObject *object)
{
  ShellPanelMenuButton *self = SHELL_PANEL_MENU_BUTTON (object);
  ShellPanelMenuButtonPrivate *priv = shell_panel_menu_button_get_instance_private (self);

  if (priv->menu != NULL)
    g_signal_handlers_disconnect_by_data (priv->menu, self);

  g_clear_object (&priv->menu);
  g_clear_object (&priv->click_gesture);
  g_clear_object (&priv->key_controller);

  G_OBJECT_CLASS (shell_panel_menu_button_parent_class)->dispose (object);
}

static void
shell_panel_menu_button_class_init (ShellPanelMenuButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_panel_menu_button_constructed;
  object_class->dispose = shell_panel_menu_button_dispose;
  object_class->get_property = shell_panel_menu_button_get_property;
  object_class->set_property = shell_panel_menu_button_set_property;

  actor_class->hide = shell_panel_menu_button_hide;
  actor_class->destroy = shell_panel_menu_button_destroy;

  /**
   * ShellPanelMenuButton:menu-alignment:
   *
   * Where the arrow of the menu points to along the menu, from 0 to 1
   * (`menuAlignment`). Ignored with #ShellPanelMenuButton:dont-create-menu.
   */
  menu_props[MENU_PROP_MENU_ALIGNMENT] =
    g_param_spec_double ("menu-alignment", NULL, NULL,
                         0.0, 1.0, 0.0,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPanelMenuButton:dont-create-menu:
   *
   * Whether the button starts with a #ShellPopupDummyMenu and does not toggle
   * its menu when clicked until one is set with shell_panel_menu_button_set_menu ()
   * (`dontCreateMenu`).
   */
  menu_props[MENU_PROP_DONT_CREATE_MENU] =
    g_param_spec_boolean ("dont-create-menu", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellPanelMenuButton:menu:
   *
   * The menu of the button, a #ShellPopupMenuBase or a #ShellPopupDummyMenu
   * (`menu`). Writing it is shell_panel_menu_button_set_menu ().
   */
  menu_props[MENU_PROP_MENU] =
    g_param_spec_object ("menu", NULL, NULL,
                         G_TYPE_OBJECT,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_MENU_PROPS, menu_props);

  /**
   * ShellPanelMenuButton::menu-set:
   * @self: the #ShellPanelMenuButton
   *
   * Emitted after the menu of the button was replaced, including with %NULL.
   * Not emitted for the dummy menu a button starts with.
   */
  menu_signals[MENU_SET] =
    g_signal_new ("menu-set",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_panel_menu_button_init (ShellPanelMenuButton *self)
{
  ShellPanelMenuButtonPrivate *priv = shell_panel_menu_button_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);

  clutter_actor_set_reactive (actor, TRUE);
  st_widget_set_can_focus (ST_WIDGET (self), TRUE);
  st_widget_set_track_hover (ST_WIDGET (self), TRUE);
  clutter_actor_set_accessible_name (actor, "");
  clutter_actor_set_accessible_role (actor, ATK_ROLE_MENU);

  priv->click_gesture =
    CLUTTER_CLICK_GESTURE (g_object_ref_sink (clutter_click_gesture_new ()));
  clutter_click_gesture_set_recognize_on_press (priv->click_gesture, TRUE);
  g_signal_connect_object (priv->click_gesture, "recognize",
                           G_CALLBACK (click_gesture_recognize_cb), self, 0);
  clutter_actor_add_action (actor, CLUTTER_ACTION (priv->click_gesture));

  priv->key_controller =
    CLUTTER_KEY_CONTROLLER (g_object_ref_sink (clutter_key_controller_new (NULL)));
  g_signal_connect_object (priv->key_controller, "key-press",
                           G_CALLBACK (key_controller_key_press_cb), self, 0);
}

/* ======================================================================== */
/* ShellSystemIndicator                                                     */
/* ======================================================================== */

typedef struct
{
  GPtrArray *quick_settings_items; /* ClutterActor, strong */
} ShellSystemIndicatorPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellSystemIndicator, shell_system_indicator,
                            ST_TYPE_BOX_LAYOUT)

/**
 * shell_system_indicator_sync_indicators_visible:
 * @self: a #ShellSystemIndicator
 *
 * Shows the indicator if any of its children is visible and hides it
 * otherwise (`_syncIndicatorsVisible ()`). It is called whenever the
 * visibility of an icon added with shell_system_indicator_add_indicator ()
 * changes; call it after adding or removing other children.
 */
void
shell_system_indicator_sync_indicators_visible (ShellSystemIndicator *self)
{
  ClutterActor *actor;
  ClutterActor *child;
  gboolean any_visible = FALSE;

  g_return_if_fail (SHELL_IS_SYSTEM_INDICATOR (self));

  actor = CLUTTER_ACTOR (self);
  for (child = clutter_actor_get_first_child (actor);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      if (clutter_actor_is_visible (child))
        {
          any_visible = TRUE;
          break;
        }
    }

  if (any_visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

static void
icon_visible_changed_cb (ShellSystemIndicator *self)
{
  shell_system_indicator_sync_indicators_visible (self);
}

/**
 * shell_system_indicator_add_indicator:
 * @self: a #ShellSystemIndicator
 *
 * Adds a status icon (an #StIcon with the `system-status-icon` style class)
 * to the indicator and keeps the visibility of the indicator in sync with the
 * visibility of its icons (`_addIndicator ()`).
 *
 * Returns: (transfer none): the new icon
 */
StIcon *
shell_system_indicator_add_indicator (ShellSystemIndicator *self)
{
  ClutterActor *icon;

  g_return_val_if_fail (SHELL_IS_SYSTEM_INDICATOR (self), NULL);

  icon = st_icon_new ();
  st_widget_set_style_class_name (ST_WIDGET (icon), "system-status-icon");
  clutter_actor_add_child (CLUTTER_ACTOR (self), icon);
  g_signal_connect_object (icon, "notify::visible",
                           G_CALLBACK (icon_visible_changed_cb),
                           self, G_CONNECT_SWAPPED);
  shell_system_indicator_sync_indicators_visible (self);

  return ST_ICON (icon);
}

/**
 * shell_system_indicator_add_quick_settings_item:
 * @self: a #ShellSystemIndicator
 * @item: the quick settings item (a toggle, a menu toggle or a slider) which
 *   goes with this indicator
 *
 * Appends an item to the quick settings items of the indicator
 * (`quickSettingsItems.push (item)`). The quick settings menu of the panel
 * adds these items to its grid when the indicator is added to it. The
 * indicator keeps @item alive until it is disposed.
 */
void
shell_system_indicator_add_quick_settings_item (ShellSystemIndicator *self,
                                                ClutterActor         *item)
{
  ShellSystemIndicatorPrivate *priv;

  g_return_if_fail (SHELL_IS_SYSTEM_INDICATOR (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (item));

  priv = shell_system_indicator_get_instance_private (self);
  g_ptr_array_add (priv->quick_settings_items, g_object_ref_sink (item));
}

/**
 * shell_system_indicator_get_quick_settings_items:
 * @self: a #ShellSystemIndicator
 *
 * Gets the items added with shell_system_indicator_add_quick_settings_item (),
 * in the order they were added (`quickSettingsItems`). The array is owned by
 * the indicator and must not be modified.
 *
 * Returns: (transfer none) (element-type Clutter.Actor): the items
 */
GPtrArray *
shell_system_indicator_get_quick_settings_items (ShellSystemIndicator *self)
{
  ShellSystemIndicatorPrivate *priv;

  g_return_val_if_fail (SHELL_IS_SYSTEM_INDICATOR (self), NULL);

  priv = shell_system_indicator_get_instance_private (self);
  return priv->quick_settings_items;
}

static void
shell_system_indicator_dispose (GObject *object)
{
  ShellSystemIndicator *self = SHELL_SYSTEM_INDICATOR (object);
  ShellSystemIndicatorPrivate *priv = shell_system_indicator_get_instance_private (self);

  /* Dropped at once: an item can keep its indicator alive through a closure */
  g_ptr_array_set_size (priv->quick_settings_items, 0);

  G_OBJECT_CLASS (shell_system_indicator_parent_class)->dispose (object);
}

static void
shell_system_indicator_finalize (GObject *object)
{
  ShellSystemIndicator *self = SHELL_SYSTEM_INDICATOR (object);
  ShellSystemIndicatorPrivate *priv = shell_system_indicator_get_instance_private (self);

  g_clear_pointer (&priv->quick_settings_items, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_system_indicator_parent_class)->finalize (object);
}

static void
shell_system_indicator_class_init (ShellSystemIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_system_indicator_dispose;
  object_class->finalize = shell_system_indicator_finalize;
}

static void
shell_system_indicator_init (ShellSystemIndicator *self)
{
  ShellSystemIndicatorPrivate *priv = shell_system_indicator_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);

  priv->quick_settings_items = g_ptr_array_new_with_free_func (g_object_unref);

  st_widget_set_style_class_name (ST_WIDGET (self), "panel-status-indicators-box");
  clutter_actor_set_reactive (actor, TRUE);
  clutter_actor_hide (actor);
}
