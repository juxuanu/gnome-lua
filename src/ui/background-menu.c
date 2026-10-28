/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * background-menu.c: The menu which opens on the desktop background, with the
 * entries which open the background, display and general settings. Port of
 * js/ui/backgroundMenu.js.
 *
 * What the JS kept in the expando properties `_backgroundMenu` and
 * `_backgroundManager` of the background actor is attached to the actor as
 * object data. `Main.layoutManager` is the layout manager given to
 * shell_background_menu_add_to ().
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include "ui/background-menu.h"

#define BACKGROUND_MENU_DATA_KEY "shell-background-menu-data"

enum
{
  PROP_0,

  PROP_LAYOUT_MANAGER,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellBackgroundMenu
{
  ShellPopupMenu parent;

  /* Only valid until constructed () is done, not referenced */
  ShellLayoutManager *layout_manager;
};

G_DEFINE_FINAL_TYPE (ShellBackgroundMenu, shell_background_menu, SHELL_TYPE_POPUP_MENU)

typedef struct
{
  ShellBackgroundMenu *menu;
  ShellPopupMenuManager *manager;
  GWeakRef layout_manager;
} BackgroundMenuData;

static void
shell_background_menu_constructed (GObject *object)
{
  ShellBackgroundMenu *self = SHELL_BACKGROUND_MENU (object);
  ShellPopupMenuBase *base = SHELL_POPUP_MENU_BASE (self);
  ClutterActor *actor;
  ShellUiActor *ui_group;

  G_OBJECT_CLASS (shell_background_menu_parent_class)->constructed (object);

  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self->layout_manager));

  shell_popup_menu_base_add_settings_action (base, _("Change Background…"),
                                             "gnome-background-panel.desktop");
  shell_popup_menu_base_add_menu_item (base,
                                       g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM,
                                                     NULL));
  shell_popup_menu_base_add_settings_action (base, _("Display Settings"),
                                             "gnome-display-panel.desktop");
  shell_popup_menu_base_add_settings_action (base, _("Settings"),
                                             "org.gnome.Settings.desktop");

  actor = shell_popup_menu_base_get_actor (base);
  st_widget_add_style_class_name (ST_WIDGET (actor), "background-menu");

  ui_group = shell_layout_manager_get_ui_group (self->layout_manager);
  clutter_actor_add_child (CLUTTER_ACTOR (ui_group), actor);
  clutter_actor_hide (actor);

  self->layout_manager = NULL;
}

static void
shell_background_menu_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  ShellBackgroundMenu *self = SHELL_BACKGROUND_MENU (object);

  switch (prop_id)
    {
    case PROP_LAYOUT_MANAGER:
      self->layout_manager = g_value_get_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_background_menu_class_init (ShellBackgroundMenuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_background_menu_constructed;
  object_class->set_property = shell_background_menu_set_property;

  /**
   * ShellBackgroundMenu:layout-manager:
   *
   * The layout manager whose UI group the menu is added to. Only used while
   * the menu is constructed. #ShellPopupMenuBase:source-actor has to be its
   * dummy cursor, which shell_background_menu_new () takes care of.
   */
  props[PROP_LAYOUT_MANAGER] =
    g_param_spec_object ("layout-manager", NULL, NULL,
                         SHELL_TYPE_LAYOUT_MANAGER,
                         G_PARAM_WRITABLE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_background_menu_init (ShellBackgroundMenu *self)
{
}

/**
 * shell_background_menu_new:
 * @layout_manager: the layout manager of the shell
 *
 * Creates the background menu, pointing at the dummy cursor of
 * @layout_manager, and adds it to its UI group, hidden.
 *
 * Returns: (transfer full): a new #ShellBackgroundMenu
 */
ShellBackgroundMenu *
shell_background_menu_new (ShellLayoutManager *layout_manager)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (layout_manager), NULL);

  return g_object_new (SHELL_TYPE_BACKGROUND_MENU,
                       "source-actor", shell_layout_manager_get_dummy_cursor (layout_manager),
                       "arrow-alignment", 0.0,
                       "arrow-side", ST_SIDE_TOP,
                       "layout-manager", layout_manager,
                       NULL);
}

static void
background_menu_data_free (BackgroundMenuData *data)
{
  g_clear_object (&data->menu);
  g_clear_object (&data->manager);
  g_weak_ref_clear (&data->layout_manager);
  g_free (data);
}

static BackgroundMenuData *
get_data_for_gesture (ClutterGesture *gesture)
{
  ClutterActor *actor =
    clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (gesture));

  if (actor == NULL)
    return NULL;

  return g_object_get_data (G_OBJECT (actor), BACKGROUND_MENU_DATA_KEY);
}

static void
open_menu (BackgroundMenuData *data,
           ClutterPressGesture *gesture)
{
  g_autoptr (ShellLayoutManager) layout_manager = NULL;
  graphene_point_t coords;

  if (data->menu == NULL)
    return;

  layout_manager = g_weak_ref_get (&data->layout_manager);
  if (layout_manager == NULL)
    return;

  clutter_press_gesture_get_coords_abs (gesture, &coords);

  shell_layout_manager_set_dummy_cursor_geometry (layout_manager,
                                                  coords.x, coords.y, 0, 0);
  shell_popup_menu_base_open (SHELL_POPUP_MENU_BASE (data->menu),
                              SHELL_POPUP_ANIMATION_FULL);
}

static void
long_press_recognize_cb (ClutterPressGesture *gesture)
{
  BackgroundMenuData *data = get_data_for_gesture (CLUTTER_GESTURE (gesture));

  if (data == NULL || data->menu == NULL)
    return;

  if (shell_popup_menu_base_get_is_open (SHELL_POPUP_MENU_BASE (data->menu)))
    return;

  open_menu (data, gesture);
}

static void
click_recognize_cb (ClutterPressGesture *gesture)
{
  BackgroundMenuData *data = get_data_for_gesture (CLUTTER_GESTURE (gesture));

  if (data == NULL)
    return;

  open_menu (data, gesture);
}

static void
background_destroy_cb (ClutterActor *actor)
{
  BackgroundMenuData *data = g_object_get_data (G_OBJECT (actor),
                                                BACKGROUND_MENU_DATA_KEY);

  if (data == NULL)
    return;

  if (data->menu != NULL)
    shell_popup_menu_base_destroy (SHELL_POPUP_MENU_BASE (data->menu));

  g_clear_object (&data->menu);
  g_clear_object (&data->manager);
}

/**
 * shell_background_menu_add_to:
 * @actor: the background actor of a monitor
 * @layout_manager: the layout manager of the shell
 *
 * Makes @actor reactive and opens a #ShellBackgroundMenu at the pointer when
 * it is long pressed with the primary button, or pressed with the secondary
 * button. The menu is destroyed along with @actor. Adding the menu to an
 * actor which already has one does nothing.
 *
 * ShellMain calls this for every #ShellLayoutManager::background-added.
 */
void
shell_background_menu_add_to (ClutterActor       *actor,
                              ShellLayoutManager *layout_manager)
{
  BackgroundMenuData *data;
  ClutterAction *long_press_gesture, *click_gesture;

  g_return_if_fail (CLUTTER_IS_ACTOR (actor));
  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (layout_manager));

  if (g_object_get_data (G_OBJECT (actor), BACKGROUND_MENU_DATA_KEY) != NULL)
    return;

  clutter_actor_set_reactive (actor, TRUE);

  data = g_new0 (BackgroundMenuData, 1);
  g_weak_ref_init (&data->layout_manager, layout_manager);
  data->menu = shell_background_menu_new (layout_manager);
  data->manager = shell_popup_menu_manager_new (SHELL_ACTION_MODE_POPUP);
  shell_popup_menu_manager_add_menu (data->manager, G_OBJECT (data->menu));
  g_object_set_data_full (G_OBJECT (actor), BACKGROUND_MENU_DATA_KEY, data,
                          (GDestroyNotify) background_menu_data_free);

  long_press_gesture = clutter_long_press_gesture_new ();
  clutter_press_gesture_set_required_button (CLUTTER_PRESS_GESTURE (long_press_gesture),
                                             CLUTTER_BUTTON_PRIMARY);
  g_signal_connect (long_press_gesture, "recognize",
                    G_CALLBACK (long_press_recognize_cb), NULL);
  clutter_actor_add_action (actor, long_press_gesture);

  click_gesture = clutter_click_gesture_new ();
  clutter_press_gesture_set_required_button (CLUTTER_PRESS_GESTURE (click_gesture),
                                             CLUTTER_BUTTON_SECONDARY);
  clutter_click_gesture_set_recognize_on_press (CLUTTER_CLICK_GESTURE (click_gesture),
                                                TRUE);
  g_signal_connect (click_gesture, "recognize",
                    G_CALLBACK (click_recognize_cb), NULL);
  clutter_actor_add_action (actor, click_gesture);

  g_signal_connect (actor, "destroy",
                    G_CALLBACK (background_destroy_cb), NULL);
}
