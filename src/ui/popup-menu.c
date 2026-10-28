/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * popup-menu.c: The shell's menu library: menu items, the menu classes
 * (popup menu with a box pointer, submenu, section, dummy) and the menu
 * manager that grabs input while a menu is open. Port of js/ui/popupMenu.js.
 *
 * Differences from the JS that every reader of this file should know about:
 *
 *  - #ShellPopupMenuBase was a `Signals.EventEmitter` with an `actor`; it is
 *    a plain #GObject with an #ShellPopupMenuBase:actor property now. The
 *    `actor._delegate = menu` back pointer is object data on the actor, read
 *    with shell_popup_menu_base_get_from_actor (). The actor keeps a strong
 *    reference to the menu until the menu is destroyed, like the closure of
 *    its "destroy" handler did in JS.
 *  - open () and close () took a params object (`animate`, `fadeOnly`,
 *    `triggerEvent`); they take a #ShellPopupAnimation now. `{animate: false}`
 *    is NONE, `{fadeOnly: true}` is FADE, the default is FULL. `triggerEvent`
 *    is gone (no caller ever passed it).
 *  - The constructor parameters are construct properties (`source-actor`,
 *    `arrow-alignment`, `arrow-side`, `style-class`, `activatable`, ...).
 *  - Main.sessionMode, Main.layoutManager's "system-modal-opened" and the
 *    `Main.overview.hide ()` / `Main.panel.closeQuickSettings ()` pair of
 *    addSettingsAction () are reached through #ShellMain (its `session-mode`
 *    property and its "system-modal-opened" and "settings-app-activating"
 *    signals), which ShellMain feeds.
 */

#include "config.h"

#include <atk/atk.h>
#include <clutter/clutter.h>
/* clutter.h does not include the motion controller (mutter 51) */
#define __CLUTTER_H_INSIDE__
#include <clutter/clutter-motion-controller.h>
#undef __CLUTTER_H_INSIDE__
#include <meta/meta-backend.h>

#include "shell-app.h"
#include "shell-app-system.h"
/* The generated St enum types (ST_TYPE_SIDE) are not part of st.h */
#define ST_H_INSIDE 1
#include "st-enum-types.h"
#undef ST_H_INSIDE
#include "shell-enum-types.h"
#include "shell-global.h"
#include "ui/environment.h"
#include "ui/main.h"
#include "ui/popup-menu.h"

#define MENU_DATA_KEY "shell-popup-menu-base"

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static ClutterStage *
get_stage (void)
{
  return shell_global_get_stage (shell_global_get ());
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

static gpointer
ref_copy_func (gconstpointer src,
               gpointer      data)
{
  return g_object_ref ((gpointer) src);
}

static void
unref_data (gpointer data)
{
  g_object_unref (data);
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

/* The actor of a menu item or a menu: `menuItem.actor` */
static ClutterActor *
menu_item_actor (GObject *menu_item)
{
  if (CLUTTER_IS_ACTOR (menu_item))
    return CLUTTER_ACTOR (menu_item);

  if (SHELL_IS_POPUP_MENU_BASE (menu_item))
    return shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (menu_item));

  return NULL;
}

static void
destroy_menu_item (GObject *menu_item)
{
  if (CLUTTER_IS_ACTOR (menu_item))
    clutter_actor_destroy (CLUTTER_ACTOR (menu_item));
  else if (SHELL_IS_POPUP_MENU_BASE (menu_item))
    shell_popup_menu_base_destroy (SHELL_POPUP_MENU_BASE (menu_item));
}

/* isPopupMenuItemVisible () */
static gboolean
is_popup_menu_item_visible (ClutterActor *child)
{
  ShellPopupMenuBase *menu = shell_popup_menu_base_get_from_actor (child);

  if (menu != NULL && SHELL_IS_POPUP_MENU_SECTION (menu) &&
      shell_popup_menu_base_is_empty (menu))
    return FALSE;

  return clutter_actor_is_visible (child);
}

static gboolean
child_is_separator (ClutterActor *child)
{
  return SHELL_IS_POPUP_SEPARATOR_MENU_ITEM (child);
}

/* ======================================================================== */
/* ShellSwitch                                                              */
/* ======================================================================== */

enum
{
  SWITCH_PROP_0,

  SWITCH_PROP_STATE,

  N_SWITCH_PROPS
};

static GParamSpec *switch_props[N_SWITCH_PROPS] = { NULL, };

struct _ShellSwitch
{
  StWidget parent;

  ClutterActor *on_icon;  /* weak */
  ClutterActor *off_icon; /* weak */
  ClutterActor *handle;   /* weak */
  ClutterAlignConstraint *align_constraint; /* weak */
  ClutterPanGesture *pan_gesture; /* weak */

  GSettings *a11y_settings;
  ClutterGrab *grab;
  float initial_grab_x;

  gboolean state;
};

G_DEFINE_FINAL_TYPE (ShellSwitch, shell_switch, ST_TYPE_WIDGET)

static void
switch_update_icon_opacity (ShellSwitch *self)
{
  uint8_t opacity =
    g_settings_get_boolean (self->a11y_settings, "show-status-shapes") ? 255 : 0;

  clutter_actor_set_opacity (self->on_icon, opacity);
  clutter_actor_set_opacity (self->off_icon, opacity);
}

static void
switch_a11y_changed_cb (GSettings   *settings,
                        const char  *key,
                        ShellSwitch *self)
{
  switch_update_icon_opacity (self);
}

static void
switch_dismiss_grab (ShellSwitch *self)
{
  if (self->grab == NULL)
    return;

  clutter_grab_dismiss (self->grab);
  g_clear_object (&self->grab);
}

static void
switch_start_dragging_cb (ClutterPanGesture *gesture,
                          ShellSwitch       *self)
{
  ClutterActor *stage = clutter_actor_get_stage (CLUTTER_ACTOR (self));
  graphene_point_t coords;

  clutter_pan_gesture_get_centroid_abs (gesture, &coords);
  self->initial_grab_x = coords.x;

  switch_dismiss_grab (self);
  if (stage != NULL)
    self->grab = clutter_stage_grab (CLUTTER_STAGE (stage), CLUTTER_ACTOR (self));
}

static void
switch_end_dragging_cb (ClutterPanGesture *gesture,
                        ShellSwitch       *self)
{
  graphene_vec2_t delta;

  switch_dismiss_grab (self);

  clutter_pan_gesture_get_accumulated_delta (gesture, &delta);
  if (graphene_vec2_get_x (&delta) > 0)
    shell_switch_set_state (self,
                            clutter_align_constraint_get_factor (self->align_constraint) > 0.5);
  else
    shell_switch_toggle (self);
}

static void
switch_drag_update_cb (ClutterPanGesture *gesture,
                       ShellSwitch       *self)
{
  graphene_point_t coords;
  float factor_diff, factor;

  clutter_pan_gesture_get_centroid_abs (gesture, &coords);
  factor_diff = (coords.x - self->initial_grab_x) /
                (clutter_actor_get_width (CLUTTER_ACTOR (self)) -
                 clutter_actor_get_width (self->handle));
  factor = factor_diff + (self->state ? 1.0 : 0.0);

  clutter_align_constraint_set_factor (self->align_constraint,
                                       CLAMP (factor, 0.0, 1.0));
}

static void
switch_apply_state (ShellSwitch *self,
                    gboolean     state)
{
  StWidget *widget = ST_WIDGET (self);
  g_auto (GValue) target = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;
  ShellEaseParams params;
  int duration = 0;

  if (self->handle == NULL)
    return;

  /* Calling get_theme_node () while unmapped is an error, avoid that */
  if (clutter_actor_is_mapped (self->handle))
    {
      StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (self->handle));

      duration = st_theme_node_get_transition_duration (theme_node);
    }

  if (state)
    st_widget_add_style_pseudo_class (widget, "checked");
  else
    st_widget_remove_style_pseudo_class (widget, "checked");

  params = make_ease_params (duration, 0);

  g_value_init (&target, G_TYPE_DOUBLE);
  g_value_set_double (&target, state ? 1.0 : 0.0);

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (self->handle),
                                       "@constraints.align.factor",
                                       &target, &params,
                                       NULL, NULL, NULL, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

static void
shell_switch_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  ShellSwitch *self = SHELL_SWITCH (object);

  switch (prop_id)
    {
    case SWITCH_PROP_STATE:
      g_value_set_boolean (value, self->state);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_switch_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  ShellSwitch *self = SHELL_SWITCH (object);

  switch (prop_id)
    {
    case SWITCH_PROP_STATE:
      shell_switch_set_state (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_switch_constructed (GObject *object)
{
  ShellSwitch *self = SHELL_SWITCH (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *box;

  G_OBJECT_CLASS (shell_switch_parent_class)->constructed (object);

  box = g_object_new (ST_TYPE_BOX_LAYOUT,
                      "x-expand", TRUE,
                      "y-expand", TRUE,
                      NULL);
  clutter_actor_add_constraint (box,
                                clutter_bind_constraint_new (actor,
                                                             CLUTTER_BIND_SIZE,
                                                             0));
  clutter_actor_add_child (actor, box);

  self->on_icon = g_object_new (ST_TYPE_ICON,
                                "icon-name", "switch-on-symbolic",
                                "x-expand", TRUE,
                                "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                NULL);
  g_object_add_weak_pointer (G_OBJECT (self->on_icon), (gpointer *) &self->on_icon);
  clutter_actor_add_child (box, self->on_icon);

  self->off_icon = g_object_new (ST_TYPE_ICON,
                                 "icon-name", "switch-off-symbolic",
                                 "x-expand", TRUE,
                                 "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                 "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                 NULL);
  g_object_add_weak_pointer (G_OBJECT (self->off_icon), (gpointer *) &self->off_icon);
  clutter_actor_add_child (box, self->off_icon);

  self->handle = g_object_new (ST_TYPE_WIDGET,
                               "style-class", "handle",
                               "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                               NULL);
  g_object_add_weak_pointer (G_OBJECT (self->handle), (gpointer *) &self->handle);
  clutter_actor_add_constraint (self->handle,
                                clutter_bind_constraint_new (actor,
                                                             CLUTTER_BIND_HEIGHT,
                                                             0));
  g_object_bind_property (self, "reactive", self->handle, "reactive",
                          G_BINDING_SYNC_CREATE);

  self->align_constraint =
    CLUTTER_ALIGN_CONSTRAINT (clutter_align_constraint_new (actor,
                                                            CLUTTER_ALIGN_X_AXIS,
                                                            0.0));
  clutter_actor_meta_set_name (CLUTTER_ACTOR_META (self->align_constraint),
                               "align");
  g_object_add_weak_pointer (G_OBJECT (self->align_constraint),
                             (gpointer *) &self->align_constraint);
  clutter_actor_add_constraint (self->handle,
                                CLUTTER_CONSTRAINT (self->align_constraint));
  clutter_actor_add_child (actor, self->handle);

  self->pan_gesture = CLUTTER_PAN_GESTURE (clutter_pan_gesture_new ());
  clutter_pan_gesture_set_begin_threshold (self->pan_gesture, 0);
  g_object_add_weak_pointer (G_OBJECT (self->pan_gesture),
                             (gpointer *) &self->pan_gesture);
  g_signal_connect (self->pan_gesture, "recognize",
                    G_CALLBACK (switch_start_dragging_cb), self);
  g_signal_connect (self->pan_gesture, "pan-update",
                    G_CALLBACK (switch_drag_update_cb), self);
  g_signal_connect (self->pan_gesture, "end",
                    G_CALLBACK (switch_end_dragging_cb), self);
  clutter_actor_add_action (self->handle, CLUTTER_ACTION (self->pan_gesture));

  switch_apply_state (self, FALSE);

  self->a11y_settings = g_settings_new ("org.gnome.desktop.a11y.interface");
  g_signal_connect_object (self->a11y_settings, "changed::show-status-shapes",
                           G_CALLBACK (switch_a11y_changed_cb), self, 0);
  switch_update_icon_opacity (self);
}

static void
shell_switch_dispose (GObject *object)
{
  ShellSwitch *self = SHELL_SWITCH (object);

  switch_dismiss_grab (self);
  g_clear_object (&self->a11y_settings);

  g_clear_weak_pointer (&self->on_icon);
  g_clear_weak_pointer (&self->off_icon);
  g_clear_weak_pointer (&self->handle);
  g_clear_weak_pointer (&self->align_constraint);
  g_clear_weak_pointer (&self->pan_gesture);

  G_OBJECT_CLASS (shell_switch_parent_class)->dispose (object);
}

static void
shell_switch_class_init (ShellSwitchClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_switch_constructed;
  object_class->dispose = shell_switch_dispose;
  object_class->get_property = shell_switch_get_property;
  object_class->set_property = shell_switch_set_property;

  /**
   * ShellSwitch:state:
   *
   * Whether the switch is on. The handle slides to its new position.
   */
  switch_props[SWITCH_PROP_STATE] =
    g_param_spec_boolean ("state", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_SWITCH_PROPS, switch_props);
}

static void
shell_switch_init (ShellSwitch *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  StWidget *widget = ST_WIDGET (self);

  st_widget_set_style_class_name (widget, "toggle-switch");
  clutter_actor_set_accessible_role (actor, ATK_ROLE_CHECK_BOX);
  clutter_actor_set_y_align (actor, CLUTTER_ACTOR_ALIGN_CENTER);
  st_widget_set_track_hover (widget, TRUE);
  clutter_actor_set_reactive (actor, TRUE);
}

/**
 * shell_switch_new:
 * @state: whether the switch starts turned on
 *
 * Creates a new #ShellSwitch (the `PopupMenu.Switch` of old).
 *
 * Returns: (transfer none): a new #ShellSwitch
 */
ShellSwitch *
shell_switch_new (gboolean state)
{
  return g_object_new (SHELL_TYPE_SWITCH, "state", state, NULL);
}

/**
 * shell_switch_get_state:
 * @self: a #ShellSwitch
 *
 * Returns: whether the switch is on
 */
gboolean
shell_switch_get_state (ShellSwitch *self)
{
  g_return_val_if_fail (SHELL_IS_SWITCH (self), FALSE);

  return self->state;
}

/**
 * shell_switch_set_state:
 * @self: a #ShellSwitch
 * @state: whether the switch is on
 *
 * Turns the switch on or off.
 */
void
shell_switch_set_state (ShellSwitch *self,
                        gboolean     state)
{
  g_return_if_fail (SHELL_IS_SWITCH (self));

  state = !!state;
  switch_apply_state (self, state);

  if (self->state != state)
    {
      self->state = state;
      g_object_notify_by_pspec (G_OBJECT (self), switch_props[SWITCH_PROP_STATE]);
    }
}

/**
 * shell_switch_toggle:
 * @self: a #ShellSwitch
 *
 * Flips the switch.
 */
void
shell_switch_toggle (ShellSwitch *self)
{
  g_return_if_fail (SHELL_IS_SWITCH (self));

  shell_switch_set_state (self, !self->state);
}

/* ======================================================================== */
/* ShellPopupMenuBase                                                       */
/* ======================================================================== */

enum
{
  BASE_PROP_0,

  BASE_PROP_SOURCE_ACTOR,
  BASE_PROP_FOCUS_ACTOR,
  BASE_PROP_ACTOR,
  BASE_PROP_BOX,
  BASE_PROP_STYLE_CLASS,
  BASE_PROP_IS_OPEN,
  BASE_PROP_LENGTH,
  BASE_PROP_SENSITIVE,

  N_BASE_PROPS
};

static GParamSpec *base_props[N_BASE_PROPS] = { NULL, };

enum
{
  OPEN_STATE_CHANGED,
  ACTIVATE,
  ACTIVE_CHANGED,
  MENU_CLOSED,
  DESTROY,

  N_BASE_SIGNALS
};

static guint base_signals[N_BASE_SIGNALS] = { 0, };

typedef struct
{
  ClutterActor *source_actor; /* weak */
  ClutterActor *focus_actor; /* weak */

  ClutterActor *actor; /* strong */
  gulong actor_destroy_id;

  StBoxLayout *box; /* strong */
  char *style_class;

  guint length;
  gboolean is_open;
  gboolean destroyed;
  gboolean sensitive;

  ShellPopupBaseMenuItem *active_menu_item; /* weak */
  ShellPopupMenuBase *parent; /* weak */
  ShellPopupMenuBase *opened_submenu; /* weak */

  GHashTable *settings_actions; /* desktop file -> GWeakRef * (of the item) */

  ShellSessionMode *session_mode; /* weak */
  gulong session_updated_id;
} ShellPopupMenuBasePrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (ShellPopupMenuBase, shell_popup_menu_base,
                                     G_TYPE_OBJECT)

static void
weak_ref_free (gpointer data)
{
  GWeakRef *ref = data;

  g_weak_ref_clear (ref);
  g_free (ref);
}

/**
 * shell_popup_menu_base_get_from_actor:
 * @actor: a #ClutterActor
 *
 * Gets the menu which owns @actor as its actor (`actor._delegate` of old).
 * Menu items are their own actors and have no menu; use a type check
 * (`instanceof Shell.PopupBaseMenuItem`) for those.
 *
 * Returns: (transfer none) (nullable): the #ShellPopupMenuBase, or %NULL if
 *   @actor is not the actor of a menu
 */
ShellPopupMenuBase *
shell_popup_menu_base_get_from_actor (ClutterActor *actor)
{
  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), NULL);

  return g_object_get_data (G_OBJECT (actor), MENU_DATA_KEY);
}

static void
menu_base_set_is_open (ShellPopupMenuBase *self,
                       gboolean            is_open)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);

  if (priv->is_open == is_open)
    return;

  priv->is_open = is_open;
  g_object_notify_by_pspec (G_OBJECT (self), base_props[BASE_PROP_IS_OPEN]);
}

static gboolean
session_allows_settings (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);
  gboolean allow_settings = TRUE;

  if (priv->session_mode != NULL)
    g_object_get (priv->session_mode, "allow-settings", &allow_settings, NULL);

  return allow_settings;
}

static void
session_updated_cb (ShellSessionMode   *session_mode,
                    ShellPopupMenuBase *self)
{
  shell_popup_menu_base_set_settings_visibility (self, session_allows_settings (self));
  shell_popup_menu_base_close (self, SHELL_POPUP_ANIMATION_FULL);
}

static void
actor_destroyed_cb (ClutterActor       *actor,
                    ShellPopupMenuBase *self)
{
  shell_popup_menu_base_destroy (self);
}

static void
menu_base_disconnect_actor (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);

  if (priv->actor == NULL)
    return;

  g_clear_signal_handler (&priv->actor_destroy_id, priv->actor);
}

/**
 * shell_popup_menu_base_set_actor:
 * @self: a #ShellPopupMenuBase
 * @actor: the new actor of the menu
 *
 * Sets the actor of the menu. The menu is destroyed when the actor is, and
 * the actor holds a reference to the menu until then. Subclasses which
 * replace the actor of their parent class use this.
 */
void
shell_popup_menu_base_set_actor (ShellPopupMenuBase *self,
                                 ClutterActor       *actor)
{
  ShellPopupMenuBasePrivate *priv;
  g_autoptr (ShellPopupMenuBase) keep_alive = NULL;

  g_return_if_fail (SHELL_IS_POPUP_MENU_BASE (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (actor));

  priv = shell_popup_menu_base_get_instance_private (self);
  if (priv->actor == actor)
    return;

  keep_alive = g_object_ref (self);

  if (priv->actor != NULL)
    {
      menu_base_disconnect_actor (self);

      if (shell_popup_menu_base_get_from_actor (priv->actor) == self)
        g_object_set_data (G_OBJECT (priv->actor), MENU_DATA_KEY, NULL);
    }

  /* Actors are floating until they have a parent */
  g_object_ref_sink (actor);
  g_clear_object (&priv->actor);
  priv->actor = actor;
  priv->actor_destroy_id = g_signal_connect (actor, "destroy",
                                             G_CALLBACK (actor_destroyed_cb),
                                             self);
  g_object_set_data_full (G_OBJECT (actor), MENU_DATA_KEY,
                          g_object_ref (self), unref_data);

  g_object_notify_by_pspec (G_OBJECT (self), base_props[BASE_PROP_ACTOR]);
}

/**
 * shell_popup_menu_base_get_actor:
 * @self: a #ShellPopupMenuBase
 *
 * Returns: (transfer none): the actor of the menu
 */
ClutterActor *
shell_popup_menu_base_get_actor (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), NULL);

  priv = shell_popup_menu_base_get_instance_private (self);
  return priv->actor;
}

/**
 * shell_popup_menu_base_get_box:
 * @self: a #ShellPopupMenuBase
 *
 * Returns: (transfer none): the box layout which holds the items of the menu
 */
StBoxLayout *
shell_popup_menu_base_get_box (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), NULL);

  priv = shell_popup_menu_base_get_instance_private (self);
  return priv->box;
}

/**
 * shell_popup_menu_base_get_source_actor:
 * @self: a #ShellPopupMenuBase
 *
 * Returns: (transfer none) (nullable): the actor the menu is attached to
 */
ClutterActor *
shell_popup_menu_base_get_source_actor (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), NULL);

  priv = shell_popup_menu_base_get_instance_private (self);
  return priv->source_actor;
}

/**
 * shell_popup_menu_base_get_focus_actor:
 * @self: a #ShellPopupMenuBase
 *
 * Returns: (transfer none) (nullable): the actor which gets the key focus
 *   when the menu is dismissed; the source actor by default
 */
ClutterActor *
shell_popup_menu_base_get_focus_actor (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), NULL);

  priv = shell_popup_menu_base_get_instance_private (self);
  return priv->focus_actor;
}

/**
 * shell_popup_menu_base_set_focus_actor:
 * @self: a #ShellPopupMenuBase
 * @actor: (nullable): the new focus actor
 *
 * Sets #ShellPopupMenuBase:focus-actor.
 */
void
shell_popup_menu_base_set_focus_actor (ShellPopupMenuBase *self,
                                       ClutterActor       *actor)
{
  ShellPopupMenuBasePrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_MENU_BASE (self));

  priv = shell_popup_menu_base_get_instance_private (self);
  if (g_set_weak_pointer (&priv->focus_actor, actor))
    g_object_notify_by_pspec (G_OBJECT (self), base_props[BASE_PROP_FOCUS_ACTOR]);
}

/**
 * shell_popup_menu_base_get_is_open:
 * @self: a #ShellPopupMenuBase
 *
 * Returns: whether the menu is open
 */
gboolean
shell_popup_menu_base_get_is_open (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), FALSE);

  priv = shell_popup_menu_base_get_instance_private (self);
  return priv->is_open;
}

/**
 * shell_popup_menu_base_get_length:
 * @self: a #ShellPopupMenuBase
 *
 * Returns: the number of items and sections which were added to the menu
 *   and not destroyed
 */
guint
shell_popup_menu_base_get_length (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), 0);

  priv = shell_popup_menu_base_get_instance_private (self);
  return priv->length;
}

/**
 * shell_popup_menu_base_get_sensitive:
 * @self: a #ShellPopupMenuBase
 *
 * Returns: whether the items of the menu may be activated; a menu is
 *   insensitive if it, or the menu it was added to, is
 */
gboolean
shell_popup_menu_base_get_sensitive (ShellPopupMenuBase *self)
{
  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), FALSE);

  return SHELL_POPUP_MENU_BASE_GET_CLASS (self)->get_sensitive (self);
}

/**
 * shell_popup_menu_base_set_sensitive:
 * @self: a #ShellPopupMenuBase
 * @sensitive: whether the menu is sensitive
 *
 * Sets the sensitivity of the menu and notifies #ShellPopupMenuBase:sensitive,
 * even if it did not change, so that the items resynchronize.
 */
void
shell_popup_menu_base_set_sensitive (ShellPopupMenuBase *self,
                                     gboolean            sensitive)
{
  ShellPopupMenuBasePrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_MENU_BASE (self));

  priv = shell_popup_menu_base_get_instance_private (self);
  priv->sensitive = !!sensitive;
  g_object_notify_by_pspec (G_OBJECT (self), base_props[BASE_PROP_SENSITIVE]);
}

/**
 * shell_popup_menu_base_get_parent_menu:
 * @self: a #ShellPopupMenuBase
 *
 * Returns: (transfer none) (nullable): the menu @self was added to
 */
ShellPopupMenuBase *
shell_popup_menu_base_get_parent_menu (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), NULL);

  priv = shell_popup_menu_base_get_instance_private (self);
  return priv->parent;
}

/**
 * shell_popup_menu_base_set_parent_menu:
 * @self: a #ShellPopupMenuBase
 * @parent: (nullable): the menu @self is added to
 *
 * Called by shell_popup_menu_base_add_menu_item (); subclasses which wrap a
 * section in an item forward it to the section.
 */
void
shell_popup_menu_base_set_parent_menu (ShellPopupMenuBase *self,
                                       ShellPopupMenuBase *parent)
{
  ShellPopupMenuBasePrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_MENU_BASE (self));
  g_return_if_fail (parent == NULL || SHELL_IS_POPUP_MENU_BASE (parent));

  priv = shell_popup_menu_base_get_instance_private (self);
  g_set_weak_pointer (&priv->parent, parent);
}

/**
 * shell_popup_menu_base_get_top_menu:
 * @self: a #ShellPopupMenuBase
 *
 * Returns: (transfer none): the outermost menu @self is part of
 */
ShellPopupMenuBase *
shell_popup_menu_base_get_top_menu (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), NULL);

  priv = shell_popup_menu_base_get_instance_private (self);
  if (priv->parent != NULL)
    return shell_popup_menu_base_get_top_menu (priv->parent);

  return self;
}

static gboolean
shell_popup_menu_base_real_get_sensitive (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);
  gboolean parent_sensitive = TRUE;

  if (priv->parent != NULL)
    parent_sensitive = shell_popup_menu_base_get_sensitive (priv->parent);

  return priv->sensitive && parent_sensitive;
}

static void
maybe_start_keynav (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);
  const ClutterEvent *event = clutter_get_current_event ();
  MetaBackend *backend = shell_global_get_backend (shell_global_get ());

  if (event == NULL)
    return;

  if (meta_backend_get_last_input_device (backend) !=
      clutter_event_get_source_device (event))
    return;

  switch (clutter_event_type (event))
    {
    case CLUTTER_KEY_PRESS:
    case CLUTTER_KEY_RELEASE:
      if (ST_IS_WIDGET (priv->actor))
        st_widget_navigate_focus (ST_WIDGET (priv->actor), NULL,
                                  ST_DIR_TAB_FORWARD, FALSE);
      break;

    default:
      break;
    }
}

static gboolean
shell_popup_menu_base_real_open (ShellPopupMenuBase *self,
                                 ShellPopupAnimation animate)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);

  if (priv->is_open)
    return FALSE;

  if (shell_popup_menu_base_is_empty (self))
    return FALSE;

  menu_base_set_is_open (self, TRUE);
  g_signal_emit (self, base_signals[OPEN_STATE_CHANGED], 0, TRUE);
  clutter_actor_show (priv->actor);
  maybe_start_keynav (self);
  return TRUE;
}

static gboolean
shell_popup_menu_base_real_close (ShellPopupMenuBase *self,
                                  ShellPopupAnimation animate)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);

  if (!priv->is_open)
    return FALSE;

  menu_base_set_is_open (self, FALSE);
  g_signal_emit (self, base_signals[OPEN_STATE_CHANGED], 0, FALSE);
  return TRUE;
}

static void
shell_popup_menu_base_real_destroy (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);

  shell_popup_menu_base_close (self, SHELL_POPUP_ANIMATION_NONE);
  shell_popup_menu_base_remove_all (self);

  if (priv->actor != NULL)
    {
      g_autoptr (ClutterActor) actor = g_object_ref (priv->actor);

      menu_base_disconnect_actor (self);
      clutter_actor_destroy (actor);
    }

  g_signal_emit (self, base_signals[DESTROY], 0);

  if (priv->session_mode != NULL)
    g_clear_signal_handler (&priv->session_updated_id, priv->session_mode);
}

static gboolean
shell_popup_menu_base_real_is_empty (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);
  g_autoptr (GList) children = clutter_actor_get_children (CLUTTER_ACTOR (priv->box));
  GList *l;

  for (l = children; l != NULL; l = l->next)
    {
      ClutterActor *child = l->data;

      if (child_is_separator (child))
        continue;

      if (is_popup_menu_item_visible (child))
        return FALSE;
    }

  return TRUE;
}

/**
 * shell_popup_menu_base_open:
 * @self: a #ShellPopupMenuBase
 * @animate: how to animate the transition
 *
 * Opens the menu. This is the `open` virtual function, which JS subclasses
 * override with `vfunc_open ()`.
 *
 * Returns: whether the open state changed. It does not if the menu is
 *   already open or has nothing to show
 */
gboolean
shell_popup_menu_base_open (ShellPopupMenuBase *self,
                            ShellPopupAnimation animate)
{
  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), FALSE);

  return SHELL_POPUP_MENU_BASE_GET_CLASS (self)->open (self, animate);
}

/**
 * shell_popup_menu_base_close:
 * @self: a #ShellPopupMenuBase
 * @animate: how to animate the transition
 *
 * Closes the menu. This is the `close` virtual function, which JS subclasses
 * override with `vfunc_close ()`.
 *
 * Returns: whether the open state changed
 */
gboolean
shell_popup_menu_base_close (ShellPopupMenuBase *self,
                             ShellPopupAnimation animate)
{
  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), FALSE);

  return SHELL_POPUP_MENU_BASE_GET_CLASS (self)->close (self, animate);
}

/**
 * shell_popup_menu_base_toggle:
 * @self: a #ShellPopupMenuBase
 *
 * Opens the menu if it is closed and closes it if it is open, animating
 * the transition.
 */
void
shell_popup_menu_base_toggle (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_MENU_BASE (self));

  priv = shell_popup_menu_base_get_instance_private (self);
  if (priv->is_open)
    shell_popup_menu_base_close (self, SHELL_POPUP_ANIMATION_FULL);
  else
    shell_popup_menu_base_open (self, SHELL_POPUP_ANIMATION_FULL);
}

/**
 * shell_popup_menu_base_destroy:
 * @self: a #ShellPopupMenuBase
 *
 * Closes the menu, destroys its items and its actor and emits
 * #ShellPopupMenuBase::destroy. This is the `destroy` virtual function,
 * which JS subclasses override with `vfunc_destroy ()`. Destroying a menu
 * twice does nothing.
 */
void
shell_popup_menu_base_destroy (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv;
  g_autoptr (ShellPopupMenuBase) keep_alive = NULL;

  g_return_if_fail (SHELL_IS_POPUP_MENU_BASE (self));

  priv = shell_popup_menu_base_get_instance_private (self);
  if (priv->destroyed)
    return;

  keep_alive = g_object_ref (self);
  priv->destroyed = TRUE;

  SHELL_POPUP_MENU_BASE_GET_CLASS (self)->destroy (self);

  /* Drop the reference the actor held on the menu */
  if (priv->actor != NULL &&
      shell_popup_menu_base_get_from_actor (priv->actor) == self)
    g_object_set_data (G_OBJECT (priv->actor), MENU_DATA_KEY, NULL);
}

/**
 * shell_popup_menu_base_is_empty:
 * @self: a #ShellPopupMenuBase
 *
 * Whether the menu has no visible item. Separators do not count, sections
 * count if they are not empty. This is the `is_empty` virtual function.
 *
 * Returns: %TRUE if the menu has nothing to show
 */
gboolean
shell_popup_menu_base_is_empty (ShellPopupMenuBase *self)
{
  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), TRUE);

  return SHELL_POPUP_MENU_BASE_GET_CLASS (self)->is_empty (self);
}

/**
 * shell_popup_menu_base_item_activated:
 * @self: a #ShellPopupMenuBase
 *
 * Closes the topmost menu @self is part of. Called after an item of the menu
 * was activated.
 */
void
shell_popup_menu_base_item_activated (ShellPopupMenuBase *self)
{
  g_return_if_fail (SHELL_IS_POPUP_MENU_BASE (self));

  shell_popup_menu_base_close (shell_popup_menu_base_get_top_menu (self),
                               SHELL_POPUP_ANIMATION_FULL);
}

/**
 * shell_popup_menu_base_set_opened_submenu:
 * @self: a #ShellPopupMenuBase
 * @submenu: (nullable): the submenu which was opened, or %NULL if it closed
 *
 * Called on the topmost menu when a submenu opens or closes: the submenu
 * which was open until then is closed.
 */
void
shell_popup_menu_base_set_opened_submenu (ShellPopupMenuBase *self,
                                          ShellPopupMenuBase *submenu)
{
  ShellPopupMenuBasePrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_MENU_BASE (self));
  g_return_if_fail (submenu == NULL || SHELL_IS_POPUP_MENU_BASE (submenu));

  priv = shell_popup_menu_base_get_instance_private (self);

  if (priv->opened_submenu != NULL)
    {
      g_autoptr (ShellPopupMenuBase) previous = g_object_ref (priv->opened_submenu);

      shell_popup_menu_base_close (previous, SHELL_POPUP_ANIMATION_FULL);
    }

  g_set_weak_pointer (&priv->opened_submenu, submenu);
}

/* ------------------------------------------------------------------------ */
/* Items                                                                    */
/* ------------------------------------------------------------------------ */

/* The items and sections of the menu, in order (`_getMenuItems ()`). Owned
 * references are taken so that the list survives the destruction of items. */
static GList *
menu_base_list_menu_items (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);
  g_autoptr (GList) children = clutter_actor_get_children (CLUTTER_ACTOR (priv->box));
  GList *items = NULL;
  GList *l;

  for (l = children; l != NULL; l = l->next)
    {
      ClutterActor *child = l->data;

      if (SHELL_IS_POPUP_BASE_MENU_ITEM (child))
        {
          items = g_list_prepend (items, child);
        }
      else
        {
          ShellPopupMenuBase *menu = shell_popup_menu_base_get_from_actor (child);

          if (menu != NULL && SHELL_IS_POPUP_MENU_SECTION (menu))
            items = g_list_prepend (items, menu);
        }
    }

  return g_list_reverse (items);
}

/**
 * shell_popup_menu_base_get_menu_items:
 * @self: a #ShellPopupMenuBase
 *
 * Gets the items and sections of the menu, in order. Submenus are not
 * listed on their own, but their #ShellPopupSubMenuMenuItem is.
 *
 * Returns: (transfer container) (element-type GObject): a list of
 *   #ShellPopupBaseMenuItem and #ShellPopupMenuSection
 */
GList *
shell_popup_menu_base_get_menu_items (ShellPopupMenuBase *self)
{
  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), NULL);

  return menu_base_list_menu_items (self);
}

/**
 * shell_popup_menu_base_get_first_menu_item:
 * @self: a #ShellPopupMenuBase
 *
 * Returns: (transfer none) (nullable): the first item or section of the menu
 */
GObject *
shell_popup_menu_base_get_first_menu_item (ShellPopupMenuBase *self)
{
  g_autoptr (GList) items = NULL;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), NULL);

  items = menu_base_list_menu_items (self);
  return items != NULL ? items->data : NULL;
}

/**
 * shell_popup_menu_base_get_n_menu_items:
 * @self: a #ShellPopupMenuBase
 *
 * Returns: the number of items and sections in the menu
 */
guint
shell_popup_menu_base_get_n_menu_items (ShellPopupMenuBase *self)
{
  g_autoptr (GList) items = NULL;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), 0);

  items = menu_base_list_menu_items (self);
  return g_list_length (items);
}

/**
 * shell_popup_menu_base_remove_all:
 * @self: a #ShellPopupMenuBase
 *
 * Destroys all the items and sections of the menu.
 */
void
shell_popup_menu_base_remove_all (ShellPopupMenuBase *self)
{
  g_autolist (GObject) items = NULL;
  GList *list;
  GList *l;

  g_return_if_fail (SHELL_IS_POPUP_MENU_BASE (self));

  list = menu_base_list_menu_items (self);
  items = g_list_copy_deep (list, ref_copy_func, NULL);
  g_list_free (list);

  for (l = items; l != NULL; l = l->next)
    destroy_menu_item (l->data);
}

static void
update_separator_visibility (ShellPopupMenuBase          *self,
                             ShellPopupSeparatorMenuItem *item)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (item);
  g_autoptr (GList) list = NULL;
  g_autoptr (GPtrArray) children = g_ptr_array_new ();
  StLabel *label = shell_popup_separator_menu_item_get_label (item);
  const char *text = st_label_get_text (label);
  guint index = 0;
  gboolean found = FALSE;
  GList *l;
  int before, after;

  if (text != NULL && *text != '\0')
    return;

  list = clutter_actor_get_children (CLUTTER_ACTOR (priv->box));
  for (l = list; l != NULL; l = l->next)
    {
      if (l->data == actor)
        {
          index = children->len;
          found = TRUE;
        }

      g_ptr_array_add (children, l->data);
    }

  if (!found)
    return;

  before = (int) index - 1;
  while (before >= 0 &&
         !is_popup_menu_item_visible (g_ptr_array_index (children, before)))
    before--;

  if (before < 0 || child_is_separator (g_ptr_array_index (children, before)))
    {
      clutter_actor_hide (actor);
      return;
    }

  after = index + 1;
  while (after < (int) children->len &&
         !is_popup_menu_item_visible (g_ptr_array_index (children, after)))
    after++;

  if (after >= (int) children->len ||
      child_is_separator (g_ptr_array_index (children, after)))
    {
      clutter_actor_hide (actor);
      return;
    }

  clutter_actor_show (actor);
}

static void
item_notify_active_cb (GObject            *object,
                       GParamSpec         *pspec,
                       ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);
  ShellPopupBaseMenuItem *item = SHELL_POPUP_BASE_MENU_ITEM (object);
  gboolean active = shell_popup_base_menu_item_get_active (item);

  if (active && priv->active_menu_item != item)
    {
      if (priv->active_menu_item != NULL)
        shell_popup_base_menu_item_set_active (priv->active_menu_item, FALSE);

      g_set_weak_pointer (&priv->active_menu_item, item);
      g_signal_emit (self, base_signals[ACTIVE_CHANGED], 0, item);
    }
  else if (!active && priv->active_menu_item == item)
    {
      g_clear_weak_pointer (&priv->active_menu_item);
      g_signal_emit (self, base_signals[ACTIVE_CHANGED], 0, NULL);
    }
}

static void
item_notify_sensitive_cb (GObject            *object,
                          GParamSpec         *pspec,
                          ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);
  ShellPopupBaseMenuItem *item = SHELL_POPUP_BASE_MENU_ITEM (object);
  gboolean sensitive = shell_popup_base_menu_item_get_sensitive (item);

  if (!sensitive && priv->active_menu_item == item)
    {
      if (!ST_IS_WIDGET (priv->actor) ||
          !st_widget_navigate_focus (ST_WIDGET (priv->actor),
                                     CLUTTER_ACTOR (item),
                                     ST_DIR_TAB_FORWARD, TRUE))
        clutter_actor_grab_key_focus (priv->actor);
    }
  else if (sensitive && priv->active_menu_item == NULL)
    {
      ClutterActor *stage_actor = clutter_actor_get_stage (priv->actor);

      if (stage_actor != NULL &&
          clutter_stage_get_key_focus (CLUTTER_STAGE (stage_actor)) == priv->actor)
        clutter_actor_grab_key_focus (CLUTTER_ACTOR (item));
    }
}

static void
item_activate_after_cb (ShellPopupBaseMenuItem *item,
                        const ClutterEvent     *event,
                        ShellPopupMenuBase     *self)
{
  g_signal_emit (self, base_signals[ACTIVATE], 0, item);
  shell_popup_menu_base_item_activated (self);
}

static void
item_destroyed_cb (ClutterActor       *item,
                   ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);

  if (priv->active_menu_item == (ShellPopupBaseMenuItem *) item)
    g_clear_weak_pointer (&priv->active_menu_item);

  if (priv->length > 0)
    priv->length--;

  g_signal_handlers_disconnect_by_data (item, self);
  g_signal_handlers_disconnect_by_data (self, item);
}

static void
menu_sync_item_sensitive_cb (ShellPopupMenuBase     *self,
                             GParamSpec             *pspec,
                             ShellPopupBaseMenuItem *item)
{
  shell_popup_base_menu_item_sync_sensitive (item);
}

/* _connectItemSignals () */
static void
connect_item_signals (ShellPopupMenuBase     *self,
                      ShellPopupBaseMenuItem *item)
{
  g_signal_connect_object (item, "notify::active",
                           G_CALLBACK (item_notify_active_cb), self, 0);
  g_signal_connect_object (item, "notify::sensitive",
                           G_CALLBACK (item_notify_sensitive_cb), self, 0);
  g_signal_connect_object (item, "activate",
                           G_CALLBACK (item_activate_after_cb), self,
                           G_CONNECT_AFTER);
  g_signal_connect_object (item, "destroy",
                           G_CALLBACK (item_destroyed_cb), self, 0);

  g_signal_connect_object (self, "notify::sensitive",
                           G_CALLBACK (menu_sync_item_sensitive_cb), item, 0);
}

/* _subMenuActiveChanged () */
static void
active_item_destroyed_cb (ClutterActor       *item,
                          ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);

  if (priv->active_menu_item == (ShellPopupBaseMenuItem *) item)
    g_clear_weak_pointer (&priv->active_menu_item);
}

static void
sub_menu_active_changed_cb (ShellPopupMenuBase     *submenu,
                            ShellPopupBaseMenuItem *submenu_item,
                            ShellPopupMenuBase     *self)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);

  if (priv->active_menu_item == submenu_item)
    return;

  if (priv->active_menu_item != NULL)
    shell_popup_base_menu_item_set_active (priv->active_menu_item, FALSE);

  g_set_weak_pointer (&priv->active_menu_item, submenu_item);

  if (submenu_item != NULL)
    {
      g_signal_handlers_disconnect_by_func (submenu_item,
                                            active_item_destroyed_cb, self);
      g_signal_connect_object (submenu_item, "destroy",
                               G_CALLBACK (active_item_destroyed_cb), self, 0);
    }

  g_signal_emit (self, base_signals[ACTIVE_CHANGED], 0, submenu_item);
}

static void
section_destroyed_cb (ShellPopupMenuBase *section,
                      ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);

  if (priv->length > 0)
    priv->length--;

  g_signal_handlers_disconnect_by_data (self, section);
  g_signal_handlers_disconnect_by_data (section, self);
}

static void
section_relay_open_state_cb (ShellPopupMenuBase *self,
                             gboolean            open,
                             ShellPopupMenuBase *section)
{
  if (open)
    shell_popup_menu_base_open (section, SHELL_POPUP_ANIMATION_FULL);
  else
    shell_popup_menu_base_close (section, SHELL_POPUP_ANIMATION_FULL);
}

static void
section_relay_menu_closed_cb (ShellPopupMenuBase *self,
                              ShellPopupMenuBase *section)
{
  g_signal_emit (section, base_signals[MENU_CLOSED], 0);
}

static void
section_relay_sensitive_cb (ShellPopupMenuBase *self,
                            GParamSpec         *pspec,
                            ShellPopupMenuBase *section)
{
  g_object_notify_by_pspec (G_OBJECT (section), base_props[BASE_PROP_SENSITIVE]);
}

static void
submenu_destroyed_cb (ShellPopupMenuBase *submenu,
                      ShellPopupMenuBase *self)
{
  g_signal_handlers_disconnect_by_data (submenu, self);
}

static void
submenu_item_parent_closed_cb (ShellPopupMenuBase *self,
                               ShellPopupBaseMenuItem *item)
{
  ShellPopupSubMenu *submenu =
    shell_popup_sub_menu_menu_item_get_menu (SHELL_POPUP_SUB_MENU_MENU_ITEM (item));

  shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (submenu),
                               SHELL_POPUP_ANIMATION_NONE);
}

static void
separator_relay_open_state_cb (ShellPopupMenuBase          *self,
                               gboolean                     open,
                               ShellPopupSeparatorMenuItem *item)
{
  update_separator_visibility (self, item);
}

static void
menu_base_insert_menu_item (ShellPopupMenuBase *self,
                            GObject            *menu_item,
                            int                 position)
{
  ShellPopupMenuBasePrivate *priv;
  ClutterActor *actor;
  ClutterActor *before_item = NULL;

  priv = shell_popup_menu_base_get_instance_private (self);
  actor = menu_item_actor (menu_item);

  if (position < 0)
    {
      clutter_actor_add_child (CLUTTER_ACTOR (priv->box), actor);
    }
  else
    {
      g_autolist (GObject) items = NULL;
      GList *list = menu_base_list_menu_items (self);
      GList *nth;

      items = g_list_copy_deep (list, ref_copy_func, NULL);
      g_list_free (list);

      nth = g_list_nth (items, position);
      if (nth != NULL)
        {
          before_item = menu_item_actor (nth->data);
          clutter_actor_insert_child_below (CLUTTER_ACTOR (priv->box), actor,
                                            before_item);
        }
      else
        {
          clutter_actor_add_child (CLUTTER_ACTOR (priv->box), actor);
        }
    }

  if (SHELL_IS_POPUP_MENU_SECTION (menu_item))
    {
      ShellPopupMenuBase *section = SHELL_POPUP_MENU_BASE (menu_item);

      g_signal_connect_object (section, "active-changed",
                               G_CALLBACK (sub_menu_active_changed_cb), self, 0);
      g_signal_connect_object (section, "destroy",
                               G_CALLBACK (section_destroyed_cb), self, 0);

      g_signal_connect_object (self, "open-state-changed",
                               G_CALLBACK (section_relay_open_state_cb), section, 0);
      g_signal_connect_object (self, "menu-closed",
                               G_CALLBACK (section_relay_menu_closed_cb), section, 0);
      g_signal_connect_object (self, "notify::sensitive",
                               G_CALLBACK (section_relay_sensitive_cb), section, 0);
    }
  else if (SHELL_IS_POPUP_SUB_MENU_MENU_ITEM (menu_item))
    {
      ShellPopupBaseMenuItem *item = SHELL_POPUP_BASE_MENU_ITEM (menu_item);
      ShellPopupMenuBase *submenu =
        SHELL_POPUP_MENU_BASE (shell_popup_sub_menu_menu_item_get_menu (SHELL_POPUP_SUB_MENU_MENU_ITEM (item)));
      ClutterActor *submenu_actor = shell_popup_menu_base_get_actor (submenu);

      if (before_item == NULL)
        clutter_actor_add_child (CLUTTER_ACTOR (priv->box), submenu_actor);
      else
        clutter_actor_insert_child_below (CLUTTER_ACTOR (priv->box),
                                          submenu_actor, before_item);

      connect_item_signals (self, item);
      g_signal_connect_object (submenu, "active-changed",
                               G_CALLBACK (sub_menu_active_changed_cb), self, 0);
      g_signal_connect_object (submenu, "destroy",
                               G_CALLBACK (submenu_destroyed_cb), self, 0);
      g_signal_connect_object (self, "menu-closed",
                               G_CALLBACK (submenu_item_parent_closed_cb), item, 0);
    }
  else if (SHELL_IS_POPUP_SEPARATOR_MENU_ITEM (menu_item))
    {
      ShellPopupBaseMenuItem *item = SHELL_POPUP_BASE_MENU_ITEM (menu_item);

      connect_item_signals (self, item);

      /* updateSeparatorVisibility needs to get called any time the
       * separator's adjacent siblings change visibility or position.
       * open-state-changed isn't exactly that, but doing it in more
       * precise ways would require a lot more bookkeeping.
       */
      g_signal_connect_object (self, "open-state-changed",
                               G_CALLBACK (separator_relay_open_state_cb), item, 0);
    }
  else
    {
      connect_item_signals (self, SHELL_POPUP_BASE_MENU_ITEM (menu_item));
    }

  if (SHELL_IS_POPUP_BASE_MENU_ITEM (menu_item))
    shell_popup_base_menu_item_set_parent_menu (SHELL_POPUP_BASE_MENU_ITEM (menu_item), self);
  else
    shell_popup_menu_base_set_parent_menu (SHELL_POPUP_MENU_BASE (menu_item), self);

  priv->length++;
}

/**
 * shell_popup_menu_base_add_menu_item:
 * @self: a #ShellPopupMenuBase
 * @menu_item: a #ShellPopupBaseMenuItem or a #ShellPopupMenuSection
 *
 * Adds an item or a section at the end of the menu. Items are actors and are
 * added to the box of the menu; the actor of a section is, too, and so is the
 * actor of the submenu of a #ShellPopupSubMenuMenuItem, right after the item.
 */
void
shell_popup_menu_base_add_menu_item (ShellPopupMenuBase *self,
                                     GObject            *menu_item)
{
  g_return_if_fail (SHELL_IS_POPUP_MENU_BASE (self));
  g_return_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (menu_item) ||
                    SHELL_IS_POPUP_MENU_SECTION (menu_item));

  menu_base_insert_menu_item (self, menu_item, -1);
}

/**
 * shell_popup_menu_base_insert_menu_item:
 * @self: a #ShellPopupMenuBase
 * @menu_item: a #ShellPopupBaseMenuItem or a #ShellPopupMenuSection
 * @position: the index to insert @menu_item at, among the items and sections
 *   of the menu. Appended if it is out of range
 *
 * Like shell_popup_menu_base_add_menu_item (), but at a given position
 * (`addMenuItem (menuItem, position)`).
 */
void
shell_popup_menu_base_insert_menu_item (ShellPopupMenuBase *self,
                                        GObject            *menu_item,
                                        int                 position)
{
  g_return_if_fail (SHELL_IS_POPUP_MENU_BASE (self));
  g_return_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (menu_item) ||
                    SHELL_IS_POPUP_MENU_SECTION (menu_item));
  g_return_if_fail (position >= 0);

  menu_base_insert_menu_item (self, menu_item, position);
}

/**
 * shell_popup_menu_base_move_menu_item:
 * @self: a #ShellPopupMenuBase
 * @menu_item: an item or section of the menu
 * @position: the new index of @menu_item
 *
 * Moves an item or section to another position in the menu.
 */
void
shell_popup_menu_base_move_menu_item (ShellPopupMenuBase *self,
                                      GObject            *menu_item,
                                      int                 position)
{
  ShellPopupMenuBasePrivate *priv;
  g_autolist (GObject) items = NULL;
  GList *list;
  GPtrArray *array;
  GList *l;
  guint i = 0;

  g_return_if_fail (SHELL_IS_POPUP_MENU_BASE (self));
  g_return_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (menu_item) ||
                    SHELL_IS_POPUP_MENU_SECTION (menu_item));

  priv = shell_popup_menu_base_get_instance_private (self);

  list = menu_base_list_menu_items (self);
  items = g_list_copy_deep (list, ref_copy_func, NULL);
  g_list_free (list);

  array = g_ptr_array_new ();
  for (l = items; l != NULL; l = l->next)
    g_ptr_array_add (array, l->data);

  while (i < array->len && position > 0)
    {
      if (g_ptr_array_index (array, i) != menu_item)
        position--;
      i++;
    }

  if (i < array->len)
    {
      if (g_ptr_array_index (array, i) != menu_item)
        clutter_actor_set_child_below_sibling (CLUTTER_ACTOR (priv->box),
                                               menu_item_actor (menu_item),
                                               menu_item_actor (g_ptr_array_index (array, i)));
    }
  else
    {
      clutter_actor_set_child_above_sibling (CLUTTER_ACTOR (priv->box),
                                             menu_item_actor (menu_item), NULL);
    }

  g_ptr_array_free (array, TRUE);
}

typedef struct
{
  ShellPopupMenuActionFunc callback;
  gpointer user_data;
  GDestroyNotify destroy;
} ActionData;

static void
action_data_free (gpointer data,
                  GClosure *closure)
{
  ActionData *action = data;

  if (action->destroy != NULL)
    action->destroy (action->user_data);
  g_free (action);
}

static void
action_item_activate_cb (ShellPopupBaseMenuItem *item,
                         const ClutterEvent     *event,
                         ActionData             *action)
{
  if (action->callback != NULL)
    action->callback (event, action->user_data);
}

static ShellPopupBaseMenuItem *
add_action_item (ShellPopupMenuBase       *self,
                 ShellPopupBaseMenuItem   *item,
                 ShellPopupMenuActionFunc  callback,
                 gpointer                  user_data,
                 GDestroyNotify            destroy)
{
  ActionData *action = g_new0 (ActionData, 1);

  action->callback = callback;
  action->user_data = user_data;
  action->destroy = destroy;

  menu_base_insert_menu_item (self, G_OBJECT (item), -1);
  g_signal_connect_data (item, "activate",
                         G_CALLBACK (action_item_activate_cb),
                         action, action_data_free, 0);

  return item;
}

/**
 * shell_popup_menu_base_add_action:
 * @self: a #ShellPopupMenuBase
 * @title: the label of the new item
 * @callback: (scope notified) (closure user_data) (destroy destroy) (nullable):
 *   called when the item is activated
 * @user_data: data for @callback
 * @destroy: called on @user_data when the item is destroyed
 *
 * Adds a #ShellPopupMenuItem which calls @callback when activated.
 *
 * Returns: (transfer none): the new item
 */
ShellPopupBaseMenuItem *
shell_popup_menu_base_add_action (ShellPopupMenuBase       *self,
                                  const char               *title,
                                  ShellPopupMenuActionFunc  callback,
                                  gpointer                  user_data,
                                  GDestroyNotify            destroy)
{
  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), NULL);

  return add_action_item (self,
                          g_object_new (SHELL_TYPE_POPUP_MENU_ITEM,
                                        "text", title,
                                        NULL),
                          callback, user_data, destroy);
}

/**
 * shell_popup_menu_base_add_action_with_icon:
 * @self: a #ShellPopupMenuBase
 * @title: the label of the new item
 * @gicon: (nullable): the icon of the item
 * @icon_name: (nullable): the name of the icon of the item, used if @gicon
 *   is %NULL
 * @callback: (scope notified) (closure user_data) (destroy destroy) (nullable):
 *   called when the item is activated
 * @user_data: data for @callback
 * @destroy: called on @user_data when the item is destroyed
 *
 * Adds a #ShellPopupImageMenuItem which calls @callback when activated.
 *
 * Returns: (transfer none): the new item
 */
ShellPopupBaseMenuItem *
shell_popup_menu_base_add_action_with_icon (ShellPopupMenuBase       *self,
                                            const char               *title,
                                            GIcon                    *gicon,
                                            const char               *icon_name,
                                            ShellPopupMenuActionFunc  callback,
                                            gpointer                  user_data,
                                            GDestroyNotify            destroy)
{
  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), NULL);

  return add_action_item (self,
                          g_object_new (SHELL_TYPE_POPUP_IMAGE_MENU_ITEM,
                                        "text", title,
                                        "gicon", gicon,
                                        "icon-name", gicon != NULL ? NULL : icon_name,
                                        NULL),
                          callback, user_data, destroy);
}

static void
settings_action_cb (const ClutterEvent *event,
                    gpointer            user_data)
{
  const char *desktop_file = user_data;
  ShellApp *app = shell_app_system_lookup_app (shell_app_system_get_default (),
                                               desktop_file);

  if (app == NULL)
    {
      g_message ("Settings panel for desktop file %s could not be loaded!",
                 desktop_file);
      return;
    }

  /* Main.overview.hide (); Main.panel.closeQuickSettings () */
  shell_main_emit_settings_app_activating (shell_main_get_default ());
  shell_app_activate (app);
}

/**
 * shell_popup_menu_base_add_settings_action:
 * @self: a #ShellPopupMenuBase
 * @title: the label of the new item
 * @desktop_file: the desktop file ID of the settings panel
 *
 * Adds an item which activates a settings panel. The item is only visible
 * if the session mode allows settings. Activating it emits
 * #ShellMain::settings-app-activating first, which ShellMain uses to hide the
 * overview and close the quick settings.
 *
 * Returns: (transfer none): the new item
 */
ShellPopupBaseMenuItem *
shell_popup_menu_base_add_settings_action (ShellPopupMenuBase *self,
                                           const char         *title,
                                           const char         *desktop_file)
{
  ShellPopupMenuBasePrivate *priv;
  ShellPopupBaseMenuItem *item;
  GWeakRef *ref;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU_BASE (self), NULL);
  g_return_val_if_fail (desktop_file != NULL, NULL);

  priv = shell_popup_menu_base_get_instance_private (self);

  item = shell_popup_menu_base_add_action (self, title, settings_action_cb,
                                           g_strdup (desktop_file), g_free);
  set_actor_visible (CLUTTER_ACTOR (item), session_allows_settings (self));

  ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, item);
  g_hash_table_replace (priv->settings_actions, g_strdup (desktop_file), ref);

  return item;
}

/**
 * shell_popup_menu_base_set_settings_visibility:
 * @self: a #ShellPopupMenuBase
 * @visible: whether the items added by shell_popup_menu_base_add_settings_action ()
 *   are visible
 *
 * Shows or hides the settings items of the menu. The menu does it by itself
 * when the session mode changes.
 */
void
shell_popup_menu_base_set_settings_visibility (ShellPopupMenuBase *self,
                                               gboolean            visible)
{
  ShellPopupMenuBasePrivate *priv;
  GHashTableIter iter;
  gpointer value;

  g_return_if_fail (SHELL_IS_POPUP_MENU_BASE (self));

  priv = shell_popup_menu_base_get_instance_private (self);

  g_hash_table_iter_init (&iter, priv->settings_actions);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      g_autoptr (ClutterActor) item = g_weak_ref_get (value);

      if (item != NULL)
        set_actor_visible (item, visible);
    }
}

/* ------------------------------------------------------------------------ */
/* Class                                                                    */
/* ------------------------------------------------------------------------ */

static void
shell_popup_menu_base_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  ShellPopupMenuBase *self = SHELL_POPUP_MENU_BASE (object);
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);

  switch (prop_id)
    {
    case BASE_PROP_SOURCE_ACTOR:
      g_value_set_object (value, priv->source_actor);
      break;

    case BASE_PROP_FOCUS_ACTOR:
      g_value_set_object (value, priv->focus_actor);
      break;

    case BASE_PROP_ACTOR:
      g_value_set_object (value, priv->actor);
      break;

    case BASE_PROP_BOX:
      g_value_set_object (value, priv->box);
      break;

    case BASE_PROP_STYLE_CLASS:
      g_value_set_string (value, priv->style_class);
      break;

    case BASE_PROP_IS_OPEN:
      g_value_set_boolean (value, priv->is_open);
      break;

    case BASE_PROP_LENGTH:
      g_value_set_uint (value, priv->length);
      break;

    case BASE_PROP_SENSITIVE:
      g_value_set_boolean (value, shell_popup_menu_base_get_sensitive (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_menu_base_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  ShellPopupMenuBase *self = SHELL_POPUP_MENU_BASE (object);
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);

  switch (prop_id)
    {
    case BASE_PROP_SOURCE_ACTOR:
      g_set_weak_pointer (&priv->source_actor, g_value_get_object (value));
      break;

    case BASE_PROP_FOCUS_ACTOR:
      shell_popup_menu_base_set_focus_actor (self, g_value_get_object (value));
      break;

    case BASE_PROP_ACTOR:
      if (g_value_get_object (value) != NULL)
        shell_popup_menu_base_set_actor (self, g_value_get_object (value));
      break;

    case BASE_PROP_STYLE_CLASS:
      g_free (priv->style_class);
      priv->style_class = g_value_dup_string (value);
      break;

    case BASE_PROP_SENSITIVE:
      shell_popup_menu_base_set_sensitive (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_menu_base_constructed (GObject *object)
{
  ShellPopupMenuBase *self = SHELL_POPUP_MENU_BASE (object);
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);
  ShellSessionMode *session_mode;

  G_OBJECT_CLASS (shell_popup_menu_base_parent_class)->constructed (object);

  if (priv->style_class != NULL)
    st_widget_set_style_class_name (ST_WIDGET (priv->box), priv->style_class);

  if (priv->focus_actor == NULL)
    g_set_weak_pointer (&priv->focus_actor, priv->source_actor);

  session_mode = shell_main_get_session_mode (shell_main_get_default ());
  if (session_mode != NULL)
    {
      g_set_weak_pointer (&priv->session_mode, session_mode);
      priv->session_updated_id =
        g_signal_connect_object (session_mode, "updated",
                                 G_CALLBACK (session_updated_cb), self, 0);
    }
}

static void
shell_popup_menu_base_dispose (GObject *object)
{
  ShellPopupMenuBase *self = SHELL_POPUP_MENU_BASE (object);
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);

  if (priv->session_mode != NULL)
    g_clear_signal_handler (&priv->session_updated_id, priv->session_mode);
  g_clear_weak_pointer (&priv->session_mode);

  if (priv->actor != NULL)
    menu_base_disconnect_actor (self);

  g_clear_weak_pointer (&priv->source_actor);
  g_clear_weak_pointer (&priv->focus_actor);
  g_clear_weak_pointer (&priv->active_menu_item);
  g_clear_weak_pointer (&priv->parent);
  g_clear_weak_pointer (&priv->opened_submenu);
  g_clear_pointer (&priv->settings_actions, g_hash_table_unref);
  g_clear_object (&priv->box);
  g_clear_object (&priv->actor);

  G_OBJECT_CLASS (shell_popup_menu_base_parent_class)->dispose (object);
}

static void
shell_popup_menu_base_finalize (GObject *object)
{
  ShellPopupMenuBase *self = SHELL_POPUP_MENU_BASE (object);
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);

  g_free (priv->style_class);

  G_OBJECT_CLASS (shell_popup_menu_base_parent_class)->finalize (object);
}

static void
shell_popup_menu_base_class_init (ShellPopupMenuBaseClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_popup_menu_base_constructed;
  object_class->dispose = shell_popup_menu_base_dispose;
  object_class->finalize = shell_popup_menu_base_finalize;
  object_class->get_property = shell_popup_menu_base_get_property;
  object_class->set_property = shell_popup_menu_base_set_property;

  klass->open = shell_popup_menu_base_real_open;
  klass->close = shell_popup_menu_base_real_close;
  klass->destroy = shell_popup_menu_base_real_destroy;
  klass->is_empty = shell_popup_menu_base_real_is_empty;
  klass->get_sensitive = shell_popup_menu_base_real_get_sensitive;

  /**
   * ShellPopupMenuBase:source-actor:
   *
   * The actor the menu is attached to (`sourceActor`).
   */
  base_props[BASE_PROP_SOURCE_ACTOR] =
    g_param_spec_object ("source-actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupMenuBase:focus-actor:
   *
   * The actor which gets the key focus when the menu is dismissed
   * (`focusActor`). The source actor by default.
   */
  base_props[BASE_PROP_FOCUS_ACTOR] =
    g_param_spec_object ("focus-actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupMenuBase:actor:
   *
   * The actor which shows the menu. The menu is destroyed with it. Menus
   * which wrap their parent's actor in another one set this after
   * construction.
   */
  base_props[BASE_PROP_ACTOR] =
    g_param_spec_object ("actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupMenuBase:box:
   *
   * The vertical box which holds the items of the menu.
   */
  base_props[BASE_PROP_BOX] =
    g_param_spec_object ("box", NULL, NULL,
                         ST_TYPE_BOX_LAYOUT,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupMenuBase:style-class:
   *
   * The CSS style class of #ShellPopupMenuBase:box (the `styleClass`
   * constructor argument).
   */
  base_props[BASE_PROP_STYLE_CLASS] =
    g_param_spec_string ("style-class", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupMenuBase:is-open:
   *
   * Whether the menu is open (`isOpen`).
   */
  base_props[BASE_PROP_IS_OPEN] =
    g_param_spec_boolean ("is-open", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupMenuBase:length:
   *
   * The number of items and sections added to the menu.
   */
  base_props[BASE_PROP_LENGTH] =
    g_param_spec_uint ("length", NULL, NULL,
                       0, G_MAXUINT, 0,
                       G_PARAM_READABLE |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupMenuBase:sensitive:
   *
   * Whether the items of the menu may be activated. Reading it takes the
   * menu this one was added to into account. Writing it always notifies.
   */
  base_props[BASE_PROP_SENSITIVE] =
    g_param_spec_boolean ("sensitive", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_BASE_PROPS, base_props);

  /**
   * ShellPopupMenuBase::open-state-changed:
   * @self: the #ShellPopupMenuBase
   * @open: whether the menu is open now
   *
   * Emitted when the menu opens or closes. A section emits it when the menu
   * it was added to does.
   */
  base_signals[OPEN_STATE_CHANGED] =
    g_signal_new ("open-state-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  /**
   * ShellPopupMenuBase::activate:
   * @self: the #ShellPopupMenuBase
   * @item: the #ShellPopupBaseMenuItem which was activated
   *
   * Emitted after an item of the menu was activated, before the menu closes.
   */
  base_signals[ACTIVATE] =
    g_signal_new ("activate",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, SHELL_TYPE_POPUP_BASE_MENU_ITEM);

  /**
   * ShellPopupMenuBase::active-changed:
   * @self: the #ShellPopupMenuBase
   * @item: (nullable): the item which is active now, if any
   *
   * Emitted when the highlighted item of the menu changes.
   */
  base_signals[ACTIVE_CHANGED] =
    g_signal_new ("active-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, SHELL_TYPE_POPUP_BASE_MENU_ITEM);

  /**
   * ShellPopupMenuBase::menu-closed:
   * @self: the #ShellPopupMenuBase
   *
   * Emitted when the menu has finished closing, after its animation.
   */
  base_signals[MENU_CLOSED] =
    g_signal_new ("menu-closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellPopupMenuBase::destroy:
   * @self: the #ShellPopupMenuBase
   *
   * Emitted when the menu was destroyed.
   */
  base_signals[DESTROY] =
    g_signal_new ("destroy",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_popup_menu_base_init (ShellPopupMenuBase *self)
{
  ShellPopupMenuBasePrivate *priv = shell_popup_menu_base_get_instance_private (self);

  priv->sensitive = TRUE;
  priv->settings_actions = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, weak_ref_free);

  priv->box = g_object_ref_sink (ST_BOX_LAYOUT (st_box_layout_new ()));
  st_box_layout_set_orientation (priv->box, CLUTTER_ORIENTATION_VERTICAL);
  clutter_actor_set_x_expand (CLUTTER_ACTOR (priv->box), TRUE);
  clutter_actor_set_y_expand (CLUTTER_ACTOR (priv->box), TRUE);
}

/* ======================================================================== */
/* ShellPopupBaseMenuItem                                                   */
/* ======================================================================== */

enum
{
  ITEM_PROP_0,

  ITEM_PROP_ACTIVE,
  ITEM_PROP_SENSITIVE,
  ITEM_PROP_ACTIVATABLE,
  ITEM_PROP_ACTIVE_ON_HOVER,
  ITEM_PROP_EXTRA_STYLE_CLASS,
  ITEM_PROP_ORNAMENT,
  ITEM_PROP_ORNAMENT_ICON,
  ITEM_PROP_ACTOR,

  N_ITEM_PROPS
};

static GParamSpec *item_props[N_ITEM_PROPS] = { NULL, };

enum
{
  ITEM_ACTIVATE,

  N_ITEM_SIGNALS
};

static guint item_signals[N_ITEM_SIGNALS] = { 0, };

typedef struct
{
  StIcon *ornament_icon; /* weak, a child */
  ShellPopupMenuBase *parent; /* weak */
  ClutterClickGesture *click_gesture; /* weak, an action */

  gboolean active;
  gboolean sensitive;
  int ornament;

  /*
   * What the constructor of the JS class took from its `params`. `reactive`
   * is a regular (non construct) property of ClutterActor, which g_object_new ()
   * applies after constructed (), so the state the JS derived from it is
   * given by the construct-only properties below instead: an item which is
   * not reactive has both of them unset.
   */
  gboolean activatable; /* the `activate` param, if reactive */
  gboolean active_on_hover; /* the `hover` param, if reactive */
  char *extra_style_class; /* the `style_class` param */
  gboolean inert; /* set by classes which are never reactive */
} ShellPopupBaseMenuItemPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellPopupBaseMenuItem, shell_popup_base_menu_item,
                            ST_TYPE_BOX_LAYOUT)

static void
item_click_recognize_cb (ClutterClickGesture    *gesture,
                         ShellPopupBaseMenuItem *self)
{
  shell_popup_base_menu_item_activate (self, clutter_get_current_event ());
}

static void
item_click_pressed_cb (GObject                *gobject,
                       GParamSpec             *pspec,
                       ShellPopupBaseMenuItem *self)
{
  StWidget *widget = ST_WIDGET (self);

  if (clutter_press_gesture_get_pressed (CLUTTER_PRESS_GESTURE (gobject)))
    st_widget_add_style_pseudo_class (widget, "active");
  else
    st_widget_remove_style_pseudo_class (widget, "active");
}

static gboolean
item_activate_binding_cb (GObject             *object,
                          const char          *action_name,
                          guint                key_val,
                          ClutterModifierType  modifiers,
                          gpointer             user_data)
{
  ShellPopupBaseMenuItem *self = SHELL_POPUP_BASE_MENU_ITEM (object);
  ShellPopupBaseMenuItemPrivate *priv =
    shell_popup_base_menu_item_get_instance_private (self);

  if (priv->activatable)
    {
      shell_popup_base_menu_item_activate (self, clutter_get_current_event ());
      return CLUTTER_EVENT_STOP;
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static void
shell_popup_base_menu_item_real_activate (ShellPopupBaseMenuItem *self,
                                          const ClutterEvent     *event)
{
  g_signal_emit (self, item_signals[ITEM_ACTIVATE], 0, event);
}

static gboolean
shell_popup_base_menu_item_real_sync_sensitive (ShellPopupBaseMenuItem *self)
{
  gboolean sensitive = shell_popup_base_menu_item_get_sensitive (self);

  clutter_actor_set_reactive (CLUTTER_ACTOR (self), sensitive);
  st_widget_set_can_focus (ST_WIDGET (self), sensitive);
  g_object_notify_by_pspec (G_OBJECT (self), item_props[ITEM_PROP_SENSITIVE]);

  return sensitive;
}

static void
shell_popup_base_menu_item_real_set_parent_menu (ShellPopupBaseMenuItem *self,
                                                 ShellPopupMenuBase     *parent)
{
  ShellPopupBaseMenuItemPrivate *priv =
    shell_popup_base_menu_item_get_instance_private (self);

  g_set_weak_pointer (&priv->parent, parent);
}

static void
shell_popup_base_menu_item_real_update_ornament_style (ShellPopupBaseMenuItem *self)
{
  ShellPopupBaseMenuItemPrivate *priv =
    shell_popup_base_menu_item_get_instance_private (self);
  StWidget *widget = ST_WIDGET (self);

  if (priv->ornament == SHELL_ORNAMENT_CHECK ||
      priv->ornament == SHELL_ORNAMENT_NONE)
    st_widget_add_style_class_name (widget, "popup-ornamented-menu-item");
  else
    st_widget_remove_style_class_name (widget, "popup-ornamented-menu-item");
}

static void
shell_popup_base_menu_item_key_focus_in (ClutterActor *actor)
{
  ClutterActorClass *parent_class =
    CLUTTER_ACTOR_CLASS (shell_popup_base_menu_item_parent_class);

  if (parent_class->key_focus_in != NULL)
    parent_class->key_focus_in (actor);

  shell_popup_base_menu_item_set_active (SHELL_POPUP_BASE_MENU_ITEM (actor), TRUE);
}

static void
shell_popup_base_menu_item_key_focus_out (ClutterActor *actor)
{
  ClutterActorClass *parent_class =
    CLUTTER_ACTOR_CLASS (shell_popup_base_menu_item_parent_class);

  if (parent_class->key_focus_out != NULL)
    parent_class->key_focus_out (actor);

  shell_popup_base_menu_item_set_active (SHELL_POPUP_BASE_MENU_ITEM (actor), FALSE);
}

static void
shell_popup_base_menu_item_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  ShellPopupBaseMenuItem *self = SHELL_POPUP_BASE_MENU_ITEM (object);
  ShellPopupBaseMenuItemPrivate *priv =
    shell_popup_base_menu_item_get_instance_private (self);

  switch (prop_id)
    {
    case ITEM_PROP_ACTIVE:
      g_value_set_boolean (value, priv->active);
      break;

    case ITEM_PROP_SENSITIVE:
      g_value_set_boolean (value, shell_popup_base_menu_item_get_sensitive (self));
      break;

    case ITEM_PROP_ACTIVATABLE:
      g_value_set_boolean (value, priv->activatable);
      break;

    case ITEM_PROP_ACTIVE_ON_HOVER:
      g_value_set_boolean (value, priv->active_on_hover);
      break;

    case ITEM_PROP_EXTRA_STYLE_CLASS:
      g_value_set_string (value, priv->extra_style_class);
      break;

    case ITEM_PROP_ORNAMENT:
      g_value_set_enum (value, priv->ornament);
      break;

    case ITEM_PROP_ORNAMENT_ICON:
      g_value_set_object (value, priv->ornament_icon);
      break;

    case ITEM_PROP_ACTOR:
      g_value_set_object (value, self);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_base_menu_item_set_property (GObject      *object,
                                         guint         prop_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
  ShellPopupBaseMenuItem *self = SHELL_POPUP_BASE_MENU_ITEM (object);
  ShellPopupBaseMenuItemPrivate *priv =
    shell_popup_base_menu_item_get_instance_private (self);

  switch (prop_id)
    {
    case ITEM_PROP_ACTIVE:
      shell_popup_base_menu_item_set_active (self, g_value_get_boolean (value));
      break;

    case ITEM_PROP_SENSITIVE:
      shell_popup_base_menu_item_set_sensitive (self, g_value_get_boolean (value));
      break;

    case ITEM_PROP_ACTIVATABLE:
      priv->activatable = g_value_get_boolean (value);
      break;

    case ITEM_PROP_ACTIVE_ON_HOVER:
      priv->active_on_hover = g_value_get_boolean (value);
      break;

    case ITEM_PROP_EXTRA_STYLE_CLASS:
      g_free (priv->extra_style_class);
      priv->extra_style_class = g_value_dup_string (value);
      break;

    case ITEM_PROP_ORNAMENT:
      shell_popup_base_menu_item_set_ornament (self, g_value_get_enum (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_base_menu_item_constructed (GObject *object)
{
  ShellPopupBaseMenuItem *self = SHELL_POPUP_BASE_MENU_ITEM (object);
  ShellPopupBaseMenuItemPrivate *priv =
    shell_popup_base_menu_item_get_instance_private (self);

  G_OBJECT_CLASS (shell_popup_base_menu_item_parent_class)->constructed (object);

  priv->ornament_icon = g_object_new (ST_TYPE_ICON,
                                      "style-class", "popup-menu-ornament",
                                      NULL);
  g_object_add_weak_pointer (G_OBJECT (priv->ornament_icon),
                             (gpointer *) &priv->ornament_icon);
  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (priv->ornament_icon));

  shell_popup_base_menu_item_set_ornament (self, SHELL_ORNAMENT_HIDDEN);

  if (priv->inert)
    {
      priv->activatable = FALSE;
      priv->active_on_hover = FALSE;
    }

  st_widget_set_track_hover (ST_WIDGET (self),
                             priv->activatable || priv->active_on_hover);

  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (priv->click_gesture),
                                  priv->activatable);

  if (!priv->activatable)
    st_widget_add_style_class_name (ST_WIDGET (self), "popup-inactive-menu-item");

  if (priv->extra_style_class != NULL && *priv->extra_style_class != '\0')
    st_widget_add_style_class_name (ST_WIDGET (self), priv->extra_style_class);

  if (priv->active_on_hover)
    g_object_bind_property (self, "hover", self, "active", G_BINDING_SYNC_CREATE);
}

static void
shell_popup_base_menu_item_finalize (GObject *object)
{
  ShellPopupBaseMenuItem *self = SHELL_POPUP_BASE_MENU_ITEM (object);
  ShellPopupBaseMenuItemPrivate *priv =
    shell_popup_base_menu_item_get_instance_private (self);

  g_free (priv->extra_style_class);

  G_OBJECT_CLASS (shell_popup_base_menu_item_parent_class)->finalize (object);
}

static void
shell_popup_base_menu_item_dispose (GObject *object)
{
  ShellPopupBaseMenuItem *self = SHELL_POPUP_BASE_MENU_ITEM (object);
  ShellPopupBaseMenuItemPrivate *priv =
    shell_popup_base_menu_item_get_instance_private (self);

  g_clear_weak_pointer (&priv->ornament_icon);
  g_clear_weak_pointer (&priv->parent);
  g_clear_weak_pointer (&priv->click_gesture);

  G_OBJECT_CLASS (shell_popup_base_menu_item_parent_class)->dispose (object);
}

static void
shell_popup_base_menu_item_class_init (ShellPopupBaseMenuItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  ClutterBindingPool *binding_pool;

  object_class->constructed = shell_popup_base_menu_item_constructed;
  object_class->dispose = shell_popup_base_menu_item_dispose;
  object_class->finalize = shell_popup_base_menu_item_finalize;
  object_class->get_property = shell_popup_base_menu_item_get_property;
  object_class->set_property = shell_popup_base_menu_item_set_property;

  actor_class->key_focus_in = shell_popup_base_menu_item_key_focus_in;
  actor_class->key_focus_out = shell_popup_base_menu_item_key_focus_out;

  klass->activate = shell_popup_base_menu_item_real_activate;
  klass->sync_sensitive = shell_popup_base_menu_item_real_sync_sensitive;
  klass->set_parent_menu = shell_popup_base_menu_item_real_set_parent_menu;
  klass->update_ornament_style = shell_popup_base_menu_item_real_update_ornament_style;

  /**
   * ShellPopupBaseMenuItem:active:
   *
   * Whether the item is highlighted: it has the key focus or is hovered.
   */
  item_props[ITEM_PROP_ACTIVE] =
    g_param_spec_boolean ("active", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupBaseMenuItem:sensitive:
   *
   * Whether the item may be activated. It is only if the item can be
   * activated at all, the sensitivity of the item has not been set to
   * %FALSE and the menu it was added to is sensitive. Setting it updates
   * #ClutterActor:reactive and #StWidget:can-focus.
   */
  item_props[ITEM_PROP_SENSITIVE] =
    g_param_spec_boolean ("sensitive", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupBaseMenuItem:activatable:
   *
   * Whether the item can be activated (the `activate` constructor
   * parameter). An item which is not #ClutterActor:reactive must unset this
   * and #ShellPopupBaseMenuItem:active-on-hover as well, since
   * #ClutterActor:reactive is not a construct property: the JS derived
   * these from it, when the item was created.
   */
  item_props[ITEM_PROP_ACTIVATABLE] =
    g_param_spec_boolean ("activatable", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupBaseMenuItem:active-on-hover:
   *
   * Whether hovering the item makes it active (the `hover` constructor
   * parameter).
   */
  item_props[ITEM_PROP_ACTIVE_ON_HOVER] =
    g_param_spec_boolean ("active-on-hover", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupBaseMenuItem:extra-style-class:
   *
   * A style class added to the ones of the item (the `style_class`
   * constructor parameter). Setting #StWidget:style-class replaces all of
   * them, including `popup-menu-item`.
   */
  item_props[ITEM_PROP_EXTRA_STYLE_CLASS] =
    g_param_spec_string ("extra-style-class", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupBaseMenuItem:ornament:
   *
   * The #ShellOrnament shown at the start of the item.
   */
  item_props[ITEM_PROP_ORNAMENT] =
    g_param_spec_enum ("ornament", NULL, NULL,
                       SHELL_TYPE_ORNAMENT,
                       SHELL_ORNAMENT_HIDDEN,
                       G_PARAM_READWRITE |
                       G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupBaseMenuItem:ornament-icon:
   *
   * The icon which shows the ornament (`_ornamentIcon`).
   */
  item_props[ITEM_PROP_ORNAMENT_ICON] =
    g_param_spec_object ("ornament-icon", NULL, NULL,
                         ST_TYPE_ICON,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupBaseMenuItem:actor:
   *
   * The item itself. Kept for compatibility with the code which used
   * `menuItem.actor` when the items were not actors.
   */
  item_props[ITEM_PROP_ACTOR] =
    g_param_spec_object ("actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_ITEM_PROPS, item_props);

  /**
   * ShellPopupBaseMenuItem::activate:
   * @self: the #ShellPopupBaseMenuItem
   * @event: (nullable): the event which activated the item
   *
   * Emitted when the item is activated, by a click, the space bar or the
   * Return key. Subclasses which override the `activate` virtual function
   * decide whether to emit it by chaining up.
   */
  item_signals[ITEM_ACTIVATE] =
    g_signal_new ("activate",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, CLUTTER_TYPE_EVENT);

  binding_pool = clutter_binding_pool_get_for_class (klass);
  clutter_binding_pool_install_action (binding_pool, "activate",
                                       CLUTTER_KEY_space, 0,
                                       G_CALLBACK (item_activate_binding_cb),
                                       NULL, NULL);
  clutter_binding_pool_install_action (binding_pool, "activate",
                                       CLUTTER_KEY_Return, 0,
                                       G_CALLBACK (item_activate_binding_cb),
                                       NULL, NULL);
}

static void
shell_popup_base_menu_item_init (ShellPopupBaseMenuItem *self)
{
  ShellPopupBaseMenuItemPrivate *priv =
    shell_popup_base_menu_item_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  StWidget *widget = ST_WIDGET (self);

  priv->sensitive = TRUE;
  priv->ornament = -1;

  st_widget_set_style_class_name (widget, "popup-menu-item");
  clutter_actor_set_reactive (actor, TRUE);
  st_widget_set_can_focus (widget, TRUE);
  clutter_actor_set_accessible_role (actor, ATK_ROLE_MENU_ITEM);

  priv->click_gesture = CLUTTER_CLICK_GESTURE (clutter_click_gesture_new ());
  g_object_add_weak_pointer (G_OBJECT (priv->click_gesture),
                             (gpointer *) &priv->click_gesture);
  g_signal_connect (priv->click_gesture, "recognize",
                    G_CALLBACK (item_click_recognize_cb), self);
  g_signal_connect (priv->click_gesture, "notify::pressed",
                    G_CALLBACK (item_click_pressed_cb), self);
  clutter_actor_add_action (actor, CLUTTER_ACTION (priv->click_gesture));
}

/**
 * shell_popup_base_menu_item_get_active:
 * @self: a #ShellPopupBaseMenuItem
 *
 * Returns: whether the item is highlighted
 */
gboolean
shell_popup_base_menu_item_get_active (ShellPopupBaseMenuItem *self)
{
  ShellPopupBaseMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (self), FALSE);

  priv = shell_popup_base_menu_item_get_instance_private (self);
  return priv->active;
}

/**
 * shell_popup_base_menu_item_set_active:
 * @self: a #ShellPopupBaseMenuItem
 * @active: whether the item is highlighted
 *
 * Highlights the item, and gives it the key focus if it can have it.
 */
void
shell_popup_base_menu_item_set_active (ShellPopupBaseMenuItem *self,
                                       gboolean                active)
{
  ShellPopupBaseMenuItemPrivate *priv;
  StWidget *widget;

  g_return_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (self));

  priv = shell_popup_base_menu_item_get_instance_private (self);
  active = !!active;
  if (priv->active == active)
    return;

  priv->active = active;
  widget = ST_WIDGET (self);

  if (active)
    {
      st_widget_add_style_pseudo_class (widget, "selected");
      if (st_widget_get_can_focus (widget))
        clutter_actor_grab_key_focus (CLUTTER_ACTOR (self));
    }
  else
    {
      st_widget_remove_style_pseudo_class (widget, "selected");
      /* Remove the CSS active state if the user press the button and
       * while holding moves to another menu item, so we don't paint all items.
       * The correct behaviour would be to set the new item with the CSS
       * active state as well, but button-press-event is not triggered,
       * so we should track it in our own, which would involve some work
       * in the container
       */
      st_widget_remove_style_pseudo_class (widget, "active");
    }

  g_object_notify_by_pspec (G_OBJECT (self), item_props[ITEM_PROP_ACTIVE]);
}

/**
 * shell_popup_base_menu_item_get_sensitive:
 * @self: a #ShellPopupBaseMenuItem
 *
 * Returns: whether the item may be activated
 */
gboolean
shell_popup_base_menu_item_get_sensitive (ShellPopupBaseMenuItem *self)
{
  ShellPopupBaseMenuItemPrivate *priv;
  gboolean parent_sensitive = TRUE;

  g_return_val_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (self), FALSE);

  priv = shell_popup_base_menu_item_get_instance_private (self);

  if (priv->parent != NULL)
    parent_sensitive = shell_popup_menu_base_get_sensitive (priv->parent);

  return priv->activatable && priv->sensitive && parent_sensitive;
}

/**
 * shell_popup_base_menu_item_set_sensitive:
 * @self: a #ShellPopupBaseMenuItem
 * @sensitive: whether the item may be activated
 *
 * Sets the sensitivity of the item and synchronizes the actor.
 */
void
shell_popup_base_menu_item_set_sensitive (ShellPopupBaseMenuItem *self,
                                          gboolean                sensitive)
{
  ShellPopupBaseMenuItemPrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (self));

  priv = shell_popup_base_menu_item_get_instance_private (self);

  sensitive = !!sensitive;
  if (priv->sensitive == sensitive)
    return;

  priv->sensitive = sensitive;
  shell_popup_base_menu_item_sync_sensitive (self);
}

/**
 * shell_popup_base_menu_item_sync_sensitive:
 * @self: a #ShellPopupBaseMenuItem
 *
 * Makes the actor follow the sensitivity of the item. Menus call it when
 * their own sensitivity changes. This is the `sync_sensitive` virtual
 * function.
 *
 * Returns: the sensitivity of the item
 */
gboolean
shell_popup_base_menu_item_sync_sensitive (ShellPopupBaseMenuItem *self)
{
  g_return_val_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (self), FALSE);

  return SHELL_POPUP_BASE_MENU_ITEM_GET_CLASS (self)->sync_sensitive (self);
}

/**
 * shell_popup_base_menu_item_activate:
 * @self: a #ShellPopupBaseMenuItem
 * @event: (nullable): the event which activates the item
 *
 * Activates the item. This is the `activate` virtual function, which JS
 * subclasses override with `vfunc_activate ()`; the default emits
 * #ShellPopupBaseMenuItem::activate.
 */
void
shell_popup_base_menu_item_activate (ShellPopupBaseMenuItem *self,
                                     const ClutterEvent     *event)
{
  g_return_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (self));

  SHELL_POPUP_BASE_MENU_ITEM_GET_CLASS (self)->activate (self, event);
}

/**
 * shell_popup_base_menu_item_get_ornament:
 * @self: a #ShellPopupBaseMenuItem
 *
 * Returns: the #ShellOrnament of the item
 */
ShellOrnament
shell_popup_base_menu_item_get_ornament (ShellPopupBaseMenuItem *self)
{
  ShellPopupBaseMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (self), SHELL_ORNAMENT_HIDDEN);

  priv = shell_popup_base_menu_item_get_instance_private (self);
  return priv->ornament;
}

/**
 * shell_popup_base_menu_item_set_ornament:
 * @self: a #ShellPopupBaseMenuItem
 * @ornament: the new ornament
 *
 * Sets the ornament of the item (`setOrnament ()`).
 */
void
shell_popup_base_menu_item_set_ornament (ShellPopupBaseMenuItem *self,
                                         ShellOrnament           ornament)
{
  ShellPopupBaseMenuItemPrivate *priv;
  ClutterActor *actor;

  g_return_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (self));

  priv = shell_popup_base_menu_item_get_instance_private (self);
  if ((int) ornament == priv->ornament)
    return;

  priv->ornament = ornament;
  actor = CLUTTER_ACTOR (self);

  if (priv->ornament_icon != NULL)
    {
      if (ornament == SHELL_ORNAMENT_DOT)
        {
          st_icon_set_icon_name (priv->ornament_icon, "ornament-dot-checked-symbolic");
          clutter_actor_add_accessible_state (actor, ATK_STATE_CHECKED);
        }
      else if (ornament == SHELL_ORNAMENT_NO_DOT)
        {
          st_icon_set_icon_name (priv->ornament_icon, "ornament-dot-unchecked-symbolic");
          clutter_actor_remove_accessible_state (actor, ATK_STATE_CHECKED);
        }
      else if (ornament == SHELL_ORNAMENT_CHECK)
        {
          st_icon_set_icon_name (priv->ornament_icon, "ornament-check-symbolic");
          clutter_actor_add_accessible_state (actor, ATK_STATE_CHECKED);
        }
      else if (ornament == SHELL_ORNAMENT_NONE || ornament == SHELL_ORNAMENT_HIDDEN)
        {
          st_icon_set_icon_name (priv->ornament_icon, "");
          clutter_actor_remove_accessible_state (actor, ATK_STATE_CHECKED);
        }

      set_actor_visible (CLUTTER_ACTOR (priv->ornament_icon),
                                 ornament != SHELL_ORNAMENT_HIDDEN);
    }

  SHELL_POPUP_BASE_MENU_ITEM_GET_CLASS (self)->update_ornament_style (self);

  g_object_notify_by_pspec (G_OBJECT (self), item_props[ITEM_PROP_ORNAMENT]);
}

/**
 * shell_popup_base_menu_item_get_ornament_icon:
 * @self: a #ShellPopupBaseMenuItem
 *
 * Returns: (transfer none): the icon which shows the ornament
 */
StIcon *
shell_popup_base_menu_item_get_ornament_icon (ShellPopupBaseMenuItem *self)
{
  ShellPopupBaseMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (self), NULL);

  priv = shell_popup_base_menu_item_get_instance_private (self);
  return priv->ornament_icon;
}

/**
 * shell_popup_base_menu_item_get_parent_menu:
 * @self: a #ShellPopupBaseMenuItem
 *
 * Returns: (transfer none) (nullable): the menu the item was added to
 */
ShellPopupMenuBase *
shell_popup_base_menu_item_get_parent_menu (ShellPopupBaseMenuItem *self)
{
  ShellPopupBaseMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (self), NULL);

  priv = shell_popup_base_menu_item_get_instance_private (self);
  return priv->parent;
}

/**
 * shell_popup_base_menu_item_set_parent_menu:
 * @self: a #ShellPopupBaseMenuItem
 * @parent: (nullable): the menu the item is added to
 *
 * Called when the item is added to a menu. This is the `set_parent_menu`
 * virtual function (`_setParent ()` of old), which subclasses override to
 * pass the parent on to the menus they contain.
 */
void
shell_popup_base_menu_item_set_parent_menu (ShellPopupBaseMenuItem *self,
                                            ShellPopupMenuBase     *parent)
{
  g_return_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (self));
  g_return_if_fail (parent == NULL || SHELL_IS_POPUP_MENU_BASE (parent));

  SHELL_POPUP_BASE_MENU_ITEM_GET_CLASS (self)->set_parent_menu (self, parent);
}

/**
 * shell_popup_base_menu_item_get_top_menu:
 * @self: a #ShellPopupBaseMenuItem
 *
 * Returns: (transfer none) (nullable): the outermost menu the item is part of
 */
ShellPopupMenuBase *
shell_popup_base_menu_item_get_top_menu (ShellPopupBaseMenuItem *self)
{
  ShellPopupBaseMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_BASE_MENU_ITEM (self), NULL);

  priv = shell_popup_base_menu_item_get_instance_private (self);
  if (priv->parent != NULL)
    return shell_popup_menu_base_get_top_menu (priv->parent);

  return NULL;
}

/**
 * shell_popup_menu_arrow_icon:
 * @side: the side to which the arrow points
 *
 * Creates an arrow icon (`PopupMenu.arrowIcon ()`).
 *
 * Returns: (transfer none): a new #StIcon
 */
StIcon *
shell_popup_menu_arrow_icon (StSide side)
{
  const char *icon_name = NULL;

  switch (side)
    {
    case ST_SIDE_TOP:
      icon_name = "pan-up-symbolic";
      break;

    case ST_SIDE_RIGHT:
      icon_name = "pan-end-symbolic";
      break;

    case ST_SIDE_BOTTOM:
      icon_name = "pan-down-symbolic";
      break;

    case ST_SIDE_LEFT:
      icon_name = "pan-start-symbolic";
      break;

    default:
      g_assert_not_reached ();
    }

  return g_object_new (ST_TYPE_ICON,
                       "style-class", "popup-menu-arrow",
                       "icon-name", icon_name,
                       "accessible-role", ATK_ROLE_ARROW,
                       "y-expand", TRUE,
                       "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       NULL);
}

/* ======================================================================== */
/* ShellPopupMenuItem                                                       */
/* ======================================================================== */

enum
{
  MENU_ITEM_PROP_0,

  MENU_ITEM_PROP_TEXT,
  MENU_ITEM_PROP_LABEL,

  N_MENU_ITEM_PROPS
};

static GParamSpec *menu_item_props[N_MENU_ITEM_PROPS] = { NULL, };

typedef struct
{
  StLabel *label; /* weak, a child */
} ShellPopupMenuItemPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellPopupMenuItem, shell_popup_menu_item,
                            SHELL_TYPE_POPUP_BASE_MENU_ITEM)

/* The label every kind of item shows: a label which expands vertically and
 * is centered in the item */
static StLabel *
item_label_new (void)
{
  return g_object_new (ST_TYPE_LABEL,
                       "y-expand", TRUE,
                       "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       NULL);
}

static void
shell_popup_menu_item_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  ShellPopupMenuItem *self = SHELL_POPUP_MENU_ITEM (object);
  ShellPopupMenuItemPrivate *priv = shell_popup_menu_item_get_instance_private (self);

  switch (prop_id)
    {
    case MENU_ITEM_PROP_TEXT:
      g_value_set_string (value, st_label_get_text (priv->label));
      break;

    case MENU_ITEM_PROP_LABEL:
      g_value_set_object (value, priv->label);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_menu_item_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  ShellPopupMenuItem *self = SHELL_POPUP_MENU_ITEM (object);

  switch (prop_id)
    {
    case MENU_ITEM_PROP_TEXT:
      shell_popup_menu_item_set_text (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_menu_item_constructed (GObject *object)
{
  ShellPopupMenuItem *self = SHELL_POPUP_MENU_ITEM (object);
  ShellPopupMenuItemPrivate *priv = shell_popup_menu_item_get_instance_private (self);

  G_OBJECT_CLASS (shell_popup_menu_item_parent_class)->constructed (object);

  priv->label = item_label_new ();
  g_object_add_weak_pointer (G_OBJECT (priv->label), (gpointer *) &priv->label);
  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (priv->label));
  st_widget_set_label_actor (ST_WIDGET (self), CLUTTER_ACTOR (priv->label));
}

static void
shell_popup_menu_item_dispose (GObject *object)
{
  ShellPopupMenuItem *self = SHELL_POPUP_MENU_ITEM (object);
  ShellPopupMenuItemPrivate *priv = shell_popup_menu_item_get_instance_private (self);

  g_clear_weak_pointer (&priv->label);

  G_OBJECT_CLASS (shell_popup_menu_item_parent_class)->dispose (object);
}

static void
shell_popup_menu_item_class_init (ShellPopupMenuItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_popup_menu_item_constructed;
  object_class->dispose = shell_popup_menu_item_dispose;
  object_class->get_property = shell_popup_menu_item_get_property;
  object_class->set_property = shell_popup_menu_item_set_property;

  /**
   * ShellPopupMenuItem:text:
   *
   * The text of the label of the item.
   */
  menu_item_props[MENU_ITEM_PROP_TEXT] =
    g_param_spec_string ("text", NULL, NULL,
                         "",
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupMenuItem:label:
   *
   * The label of the item (`label`).
   */
  menu_item_props[MENU_ITEM_PROP_LABEL] =
    g_param_spec_object ("label", NULL, NULL,
                         ST_TYPE_LABEL,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_MENU_ITEM_PROPS, menu_item_props);
}

static void
shell_popup_menu_item_init (ShellPopupMenuItem *self)
{
}

/**
 * shell_popup_menu_item_get_label:
 * @self: a #ShellPopupMenuItem
 *
 * Returns: (transfer none): the label of the item
 */
StLabel *
shell_popup_menu_item_get_label (ShellPopupMenuItem *self)
{
  ShellPopupMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU_ITEM (self), NULL);

  priv = shell_popup_menu_item_get_instance_private (self);
  return priv->label;
}

/**
 * shell_popup_menu_item_get_text:
 * @self: a #ShellPopupMenuItem
 *
 * Returns: (transfer none): the text of the label of the item
 */
const char *
shell_popup_menu_item_get_text (ShellPopupMenuItem *self)
{
  ShellPopupMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU_ITEM (self), NULL);

  priv = shell_popup_menu_item_get_instance_private (self);
  return priv->label != NULL ? st_label_get_text (priv->label) : "";
}

/**
 * shell_popup_menu_item_set_text:
 * @self: a #ShellPopupMenuItem
 * @text: the new text of the label of the item
 *
 * Sets #ShellPopupMenuItem:text.
 */
void
shell_popup_menu_item_set_text (ShellPopupMenuItem *self,
                                const char         *text)
{
  ShellPopupMenuItemPrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_MENU_ITEM (self));

  priv = shell_popup_menu_item_get_instance_private (self);
  if (priv->label == NULL)
    return;

  if (g_strcmp0 (st_label_get_text (priv->label), text ? text : "") == 0)
    return;

  st_label_set_text (priv->label, text ? text : "");
  g_object_notify_by_pspec (G_OBJECT (self), menu_item_props[MENU_ITEM_PROP_TEXT]);
}

/* ======================================================================== */
/* ShellPopupSeparatorMenuItem                                              */
/* ======================================================================== */

enum
{
  SEPARATOR_PROP_0,

  SEPARATOR_PROP_TEXT,
  SEPARATOR_PROP_LABEL,

  N_SEPARATOR_PROPS
};

static GParamSpec *separator_props[N_SEPARATOR_PROPS] = { NULL, };

typedef struct
{
  StLabel *label; /* weak, a child */
} ShellPopupSeparatorMenuItemPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellPopupSeparatorMenuItem,
                            shell_popup_separator_menu_item,
                            SHELL_TYPE_POPUP_BASE_MENU_ITEM)

/* _syncVisibility () */
static void
separator_sync_visibility (ShellPopupSeparatorMenuItem *self)
{
  ShellPopupSeparatorMenuItemPrivate *priv =
    shell_popup_separator_menu_item_get_instance_private (self);
  const char *text = st_label_get_text (priv->label);

  set_actor_visible (CLUTTER_ACTOR (priv->label), text != NULL && *text != '\0');
}

static void
separator_label_text_changed_cb (GObject                     *object,
                                 GParamSpec                  *pspec,
                                 ShellPopupSeparatorMenuItem *self)
{
  separator_sync_visibility (self);
}

static void
shell_popup_separator_menu_item_get_property (GObject    *object,
                                              guint       prop_id,
                                              GValue     *value,
                                              GParamSpec *pspec)
{
  ShellPopupSeparatorMenuItem *self = SHELL_POPUP_SEPARATOR_MENU_ITEM (object);
  ShellPopupSeparatorMenuItemPrivate *priv =
    shell_popup_separator_menu_item_get_instance_private (self);

  switch (prop_id)
    {
    case SEPARATOR_PROP_TEXT:
      g_value_set_string (value, st_label_get_text (priv->label));
      break;

    case SEPARATOR_PROP_LABEL:
      g_value_set_object (value, priv->label);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_separator_menu_item_set_property (GObject      *object,
                                              guint         prop_id,
                                              const GValue *value,
                                              GParamSpec   *pspec)
{
  ShellPopupSeparatorMenuItem *self = SHELL_POPUP_SEPARATOR_MENU_ITEM (object);

  switch (prop_id)
    {
    case SEPARATOR_PROP_TEXT:
      shell_popup_separator_menu_item_set_text (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_separator_menu_item_constructed (GObject *object)
{
  ShellPopupSeparatorMenuItem *self = SHELL_POPUP_SEPARATOR_MENU_ITEM (object);
  ShellPopupSeparatorMenuItemPrivate *priv =
    shell_popup_separator_menu_item_get_instance_private (self);
  ClutterActor *separator;

  G_OBJECT_CLASS (shell_popup_separator_menu_item_parent_class)->constructed (object);

  priv->label = g_object_new (ST_TYPE_LABEL, "text", "", NULL);
  g_object_add_weak_pointer (G_OBJECT (priv->label), (gpointer *) &priv->label);
  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (priv->label));
  st_widget_set_label_actor (ST_WIDGET (self), CLUTTER_ACTOR (priv->label));

  g_signal_connect_object (priv->label, "notify::text",
                           G_CALLBACK (separator_label_text_changed_cb), self, 0);
  separator_sync_visibility (self);

  separator = g_object_new (ST_TYPE_WIDGET,
                            "style-class", "popup-separator-menu-item-separator",
                            "x-expand", TRUE,
                            "y-expand", TRUE,
                            "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                            NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), separator);
}

static void
shell_popup_separator_menu_item_dispose (GObject *object)
{
  ShellPopupSeparatorMenuItem *self = SHELL_POPUP_SEPARATOR_MENU_ITEM (object);
  ShellPopupSeparatorMenuItemPrivate *priv =
    shell_popup_separator_menu_item_get_instance_private (self);

  g_clear_weak_pointer (&priv->label);

  G_OBJECT_CLASS (shell_popup_separator_menu_item_parent_class)->dispose (object);
}

static void
shell_popup_separator_menu_item_class_init (ShellPopupSeparatorMenuItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_popup_separator_menu_item_constructed;
  object_class->dispose = shell_popup_separator_menu_item_dispose;
  object_class->get_property = shell_popup_separator_menu_item_get_property;
  object_class->set_property = shell_popup_separator_menu_item_set_property;

  /**
   * ShellPopupSeparatorMenuItem:text:
   *
   * The text of the separator, empty for a plain line. The label is only
   * visible if there is a text.
   */
  separator_props[SEPARATOR_PROP_TEXT] =
    g_param_spec_string ("text", NULL, NULL,
                         "",
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupSeparatorMenuItem:label:
   *
   * The label of the separator (`label`).
   */
  separator_props[SEPARATOR_PROP_LABEL] =
    g_param_spec_object ("label", NULL, NULL,
                         ST_TYPE_LABEL,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_SEPARATOR_PROPS, separator_props);
}

static void
shell_popup_separator_menu_item_init (ShellPopupSeparatorMenuItem *self)
{
  ShellPopupBaseMenuItemPrivate *base_priv =
    shell_popup_base_menu_item_get_instance_private (SHELL_POPUP_BASE_MENU_ITEM (self));
  ClutterActor *actor = CLUTTER_ACTOR (self);
  StWidget *widget = ST_WIDGET (self);

  /* reactive: false, can_focus: false */
  base_priv->inert = TRUE;

  st_widget_add_style_class_name (widget, "popup-separator-menu-item");
  clutter_actor_set_reactive (actor, FALSE);
  st_widget_set_can_focus (widget, FALSE);
  clutter_actor_set_accessible_role (actor, ATK_ROLE_SEPARATOR);
}

/**
 * shell_popup_separator_menu_item_get_label:
 * @self: a #ShellPopupSeparatorMenuItem
 *
 * Returns: (transfer none): the label of the separator
 */
StLabel *
shell_popup_separator_menu_item_get_label (ShellPopupSeparatorMenuItem *self)
{
  ShellPopupSeparatorMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_SEPARATOR_MENU_ITEM (self), NULL);

  priv = shell_popup_separator_menu_item_get_instance_private (self);
  return priv->label;
}

/**
 * shell_popup_separator_menu_item_get_text:
 * @self: a #ShellPopupSeparatorMenuItem
 *
 * Returns: (transfer none): the text of the separator
 */
const char *
shell_popup_separator_menu_item_get_text (ShellPopupSeparatorMenuItem *self)
{
  ShellPopupSeparatorMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_SEPARATOR_MENU_ITEM (self), NULL);

  priv = shell_popup_separator_menu_item_get_instance_private (self);
  return priv->label != NULL ? st_label_get_text (priv->label) : "";
}

/**
 * shell_popup_separator_menu_item_set_text:
 * @self: a #ShellPopupSeparatorMenuItem
 * @text: (nullable): the new text of the separator
 *
 * Sets #ShellPopupSeparatorMenuItem:text.
 */
void
shell_popup_separator_menu_item_set_text (ShellPopupSeparatorMenuItem *self,
                                          const char                  *text)
{
  ShellPopupSeparatorMenuItemPrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_SEPARATOR_MENU_ITEM (self));

  priv = shell_popup_separator_menu_item_get_instance_private (self);
  if (priv->label == NULL)
    return;

  if (g_strcmp0 (st_label_get_text (priv->label), text ? text : "") == 0)
    return;

  st_label_set_text (priv->label, text ? text : "");
  g_object_notify_by_pspec (G_OBJECT (self), separator_props[SEPARATOR_PROP_TEXT]);
}

/* ======================================================================== */
/* ShellPopupSwitchMenuItem                                                 */
/* ======================================================================== */

enum
{
  SWITCH_ITEM_PROP_0,

  SWITCH_ITEM_PROP_TEXT,
  SWITCH_ITEM_PROP_LABEL,
  SWITCH_ITEM_PROP_STATE,

  N_SWITCH_ITEM_PROPS
};

static GParamSpec *switch_item_props[N_SWITCH_ITEM_PROPS] = { NULL, };

enum
{
  SWITCH_ITEM_TOGGLED,

  N_SWITCH_ITEM_SIGNALS
};

static guint switch_item_signals[N_SWITCH_ITEM_SIGNALS] = { 0, };

typedef struct
{
  StLabel *label; /* weak, a child */
  ShellSwitch *switch_actor; /* strong, parented to the status bin or not */
  StBin *status_bin; /* weak, a child */
  StLabel *status_label; /* strong, parented to the status bin or not */
} ShellPopupSwitchMenuItemPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellPopupSwitchMenuItem,
                            shell_popup_switch_menu_item,
                            SHELL_TYPE_POPUP_BASE_MENU_ITEM)

/* _checkAccessibleState () */
static void
switch_item_check_accessible_state (ShellPopupSwitchMenuItem *self)
{
  ShellPopupSwitchMenuItemPrivate *priv =
    shell_popup_switch_menu_item_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);

  if (clutter_actor_get_accessible_role (actor) == ATK_ROLE_CHECK_MENU_ITEM &&
      shell_switch_get_state (priv->switch_actor))
    clutter_actor_add_accessible_state (actor, ATK_STATE_CHECKED);
  else
    clutter_actor_remove_accessible_state (actor, ATK_STATE_CHECKED);
}

/* _onToggled () */
static void
switch_item_switch_state_changed_cb (GObject                  *object,
                                     GParamSpec               *pspec,
                                     ShellPopupSwitchMenuItem *self)
{
  ShellPopupSwitchMenuItemPrivate *priv =
    shell_popup_switch_menu_item_get_instance_private (self);

  g_signal_emit (self, switch_item_signals[SWITCH_ITEM_TOGGLED], 0,
                 shell_switch_get_state (priv->switch_actor));
  g_object_notify_by_pspec (G_OBJECT (self), switch_item_props[SWITCH_ITEM_PROP_STATE]);
  switch_item_check_accessible_state (self);
}

static void
shell_popup_switch_menu_item_real_activate (ShellPopupBaseMenuItem *item,
                                            const ClutterEvent     *event)
{
  ShellPopupSwitchMenuItem *self = SHELL_POPUP_SWITCH_MENU_ITEM (item);
  ShellPopupSwitchMenuItemPrivate *priv =
    shell_popup_switch_menu_item_get_instance_private (self);
  ShellPopupBaseMenuItemClass *parent_class =
    SHELL_POPUP_BASE_MENU_ITEM_CLASS (shell_popup_switch_menu_item_parent_class);

  if (clutter_actor_is_mapped (CLUTTER_ACTOR (priv->switch_actor)))
    shell_popup_switch_menu_item_toggle (self);

  /* we allow pressing space to toggle the switch
   * without closing the menu */
  if (event != NULL &&
      clutter_event_type (event) == CLUTTER_KEY_PRESS &&
      clutter_event_get_key_symbol (event) == CLUTTER_KEY_space)
    return;

  parent_class->activate (item, event);
}

static void
shell_popup_switch_menu_item_get_property (GObject    *object,
                                           guint       prop_id,
                                           GValue     *value,
                                           GParamSpec *pspec)
{
  ShellPopupSwitchMenuItem *self = SHELL_POPUP_SWITCH_MENU_ITEM (object);
  ShellPopupSwitchMenuItemPrivate *priv =
    shell_popup_switch_menu_item_get_instance_private (self);

  switch (prop_id)
    {
    case SWITCH_ITEM_PROP_TEXT:
      g_value_set_string (value, st_label_get_text (priv->label));
      break;

    case SWITCH_ITEM_PROP_LABEL:
      g_value_set_object (value, priv->label);
      break;

    case SWITCH_ITEM_PROP_STATE:
      g_value_set_boolean (value, shell_switch_get_state (priv->switch_actor));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_switch_menu_item_set_property (GObject      *object,
                                           guint         prop_id,
                                           const GValue *value,
                                           GParamSpec   *pspec)
{
  ShellPopupSwitchMenuItem *self = SHELL_POPUP_SWITCH_MENU_ITEM (object);

  switch (prop_id)
    {
    case SWITCH_ITEM_PROP_TEXT:
      shell_popup_switch_menu_item_set_text (self, g_value_get_string (value));
      break;

    case SWITCH_ITEM_PROP_STATE:
      shell_popup_switch_menu_item_set_state (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_switch_menu_item_constructed (GObject *object)
{
  ShellPopupSwitchMenuItem *self = SHELL_POPUP_SWITCH_MENU_ITEM (object);
  ShellPopupSwitchMenuItemPrivate *priv =
    shell_popup_switch_menu_item_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);

  G_OBJECT_CLASS (shell_popup_switch_menu_item_parent_class)->constructed (object);

  priv->label = item_label_new ();
  g_object_add_weak_pointer (G_OBJECT (priv->label), (gpointer *) &priv->label);

  priv->switch_actor = g_object_ref_sink (shell_switch_new (FALSE));
  g_signal_connect_object (priv->switch_actor, "notify::state",
                           G_CALLBACK (switch_item_switch_state_changed_cb),
                           self, 0);
  g_object_bind_property (self, "reactive", priv->switch_actor, "reactive",
                          G_BINDING_SYNC_CREATE);

  clutter_actor_set_accessible_role (actor, ATK_ROLE_CHECK_MENU_ITEM);
  switch_item_check_accessible_state (self);
  st_widget_set_label_actor (ST_WIDGET (self), CLUTTER_ACTOR (priv->label));

  clutter_actor_add_child (actor, CLUTTER_ACTOR (priv->label));

  priv->status_bin = g_object_new (ST_TYPE_BIN,
                                   "x-align", CLUTTER_ACTOR_ALIGN_END,
                                   "x-expand", TRUE,
                                   NULL);
  g_object_add_weak_pointer (G_OBJECT (priv->status_bin),
                             (gpointer *) &priv->status_bin);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (priv->status_bin));

  priv->status_label = g_object_ref_sink (g_object_new (ST_TYPE_LABEL,
                                                        "text", "",
                                                        "style-class", "popup-status-menu-item",
                                                        "y-expand", TRUE,
                                                        "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                                        NULL));
  st_bin_set_child (priv->status_bin, CLUTTER_ACTOR (priv->switch_actor));
}

static void
shell_popup_switch_menu_item_dispose (GObject *object)
{
  ShellPopupSwitchMenuItem *self = SHELL_POPUP_SWITCH_MENU_ITEM (object);
  ShellPopupSwitchMenuItemPrivate *priv =
    shell_popup_switch_menu_item_get_instance_private (self);

  g_clear_weak_pointer (&priv->label);
  g_clear_weak_pointer (&priv->status_bin);
  g_clear_object (&priv->switch_actor);
  g_clear_object (&priv->status_label);

  G_OBJECT_CLASS (shell_popup_switch_menu_item_parent_class)->dispose (object);
}

static void
shell_popup_switch_menu_item_class_init (ShellPopupSwitchMenuItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupBaseMenuItemClass *item_class = SHELL_POPUP_BASE_MENU_ITEM_CLASS (klass);

  object_class->constructed = shell_popup_switch_menu_item_constructed;
  object_class->dispose = shell_popup_switch_menu_item_dispose;
  object_class->get_property = shell_popup_switch_menu_item_get_property;
  object_class->set_property = shell_popup_switch_menu_item_set_property;

  item_class->activate = shell_popup_switch_menu_item_real_activate;

  /**
   * ShellPopupSwitchMenuItem:text:
   *
   * The text of the label of the item.
   */
  switch_item_props[SWITCH_ITEM_PROP_TEXT] =
    g_param_spec_string ("text", NULL, NULL,
                         "",
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupSwitchMenuItem:label:
   *
   * The label of the item (`label`).
   */
  switch_item_props[SWITCH_ITEM_PROP_LABEL] =
    g_param_spec_object ("label", NULL, NULL,
                         ST_TYPE_LABEL,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupSwitchMenuItem:state:
   *
   * Whether the switch is on.
   */
  switch_item_props[SWITCH_ITEM_PROP_STATE] =
    g_param_spec_boolean ("state", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_SWITCH_ITEM_PROPS, switch_item_props);

  /**
   * ShellPopupSwitchMenuItem::toggled:
   * @self: the #ShellPopupSwitchMenuItem
   * @state: whether the switch is on now
   *
   * Emitted when the state of the switch changes.
   */
  switch_item_signals[SWITCH_ITEM_TOGGLED] =
    g_signal_new ("toggled",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void
shell_popup_switch_menu_item_init (ShellPopupSwitchMenuItem *self)
{
}

/**
 * shell_popup_switch_menu_item_get_label:
 * @self: a #ShellPopupSwitchMenuItem
 *
 * Returns: (transfer none): the label of the item
 */
StLabel *
shell_popup_switch_menu_item_get_label (ShellPopupSwitchMenuItem *self)
{
  ShellPopupSwitchMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_SWITCH_MENU_ITEM (self), NULL);

  priv = shell_popup_switch_menu_item_get_instance_private (self);
  return priv->label;
}

/**
 * shell_popup_switch_menu_item_get_text:
 * @self: a #ShellPopupSwitchMenuItem
 *
 * Returns: (transfer none): the text of the label of the item
 */
const char *
shell_popup_switch_menu_item_get_text (ShellPopupSwitchMenuItem *self)
{
  ShellPopupSwitchMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_SWITCH_MENU_ITEM (self), NULL);

  priv = shell_popup_switch_menu_item_get_instance_private (self);
  return priv->label != NULL ? st_label_get_text (priv->label) : "";
}

/**
 * shell_popup_switch_menu_item_set_text:
 * @self: a #ShellPopupSwitchMenuItem
 * @text: (nullable): the new text of the label of the item
 *
 * Sets #ShellPopupSwitchMenuItem:text.
 */
void
shell_popup_switch_menu_item_set_text (ShellPopupSwitchMenuItem *self,
                                       const char               *text)
{
  ShellPopupSwitchMenuItemPrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_SWITCH_MENU_ITEM (self));

  priv = shell_popup_switch_menu_item_get_instance_private (self);
  if (priv->label == NULL)
    return;

  if (g_strcmp0 (st_label_get_text (priv->label), text ? text : "") == 0)
    return;

  st_label_set_text (priv->label, text ? text : "");
  g_object_notify_by_pspec (G_OBJECT (self), switch_item_props[SWITCH_ITEM_PROP_TEXT]);
}

/**
 * shell_popup_switch_menu_item_get_state:
 * @self: a #ShellPopupSwitchMenuItem
 *
 * Returns: whether the switch is on
 */
gboolean
shell_popup_switch_menu_item_get_state (ShellPopupSwitchMenuItem *self)
{
  ShellPopupSwitchMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_SWITCH_MENU_ITEM (self), FALSE);

  priv = shell_popup_switch_menu_item_get_instance_private (self);
  return shell_switch_get_state (priv->switch_actor);
}

/**
 * shell_popup_switch_menu_item_set_state:
 * @self: a #ShellPopupSwitchMenuItem
 * @state: whether the switch is on
 *
 * Sets #ShellPopupSwitchMenuItem:state. Emits
 * #ShellPopupSwitchMenuItem::toggled if it changes.
 */
void
shell_popup_switch_menu_item_set_state (ShellPopupSwitchMenuItem *self,
                                        gboolean                  state)
{
  ShellPopupSwitchMenuItemPrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_SWITCH_MENU_ITEM (self));

  priv = shell_popup_switch_menu_item_get_instance_private (self);
  if (shell_switch_get_state (priv->switch_actor) == !!state)
    return;

  shell_switch_set_state (priv->switch_actor, state);
}

/**
 * shell_popup_switch_menu_item_set_toggle_state:
 * @self: a #ShellPopupSwitchMenuItem
 * @state: whether the switch is on
 *
 * Same as shell_popup_switch_menu_item_set_state () (`setToggleState ()`).
 */
void
shell_popup_switch_menu_item_set_toggle_state (ShellPopupSwitchMenuItem *self,
                                               gboolean                  state)
{
  g_return_if_fail (SHELL_IS_POPUP_SWITCH_MENU_ITEM (self));

  shell_popup_switch_menu_item_set_state (self, state);
}

/**
 * shell_popup_switch_menu_item_set_status:
 * @self: a #ShellPopupSwitchMenuItem
 * @text: (nullable): a text to show instead of the switch, or %NULL to show
 *   the switch again
 *
 * Replaces the switch by a status text (`setStatus ()`). The item cannot be
 * activated while it shows a status.
 */
void
shell_popup_switch_menu_item_set_status (ShellPopupSwitchMenuItem *self,
                                         const char               *text)
{
  ShellPopupSwitchMenuItemPrivate *priv;
  ClutterActor *actor;

  g_return_if_fail (SHELL_IS_POPUP_SWITCH_MENU_ITEM (self));

  priv = shell_popup_switch_menu_item_get_instance_private (self);
  actor = CLUTTER_ACTOR (self);

  if (text != NULL)
    {
      st_label_set_text (priv->status_label, text);
      st_bin_set_child (priv->status_bin, CLUTTER_ACTOR (priv->status_label));
      clutter_actor_set_reactive (actor, FALSE);
      clutter_actor_set_accessible_role (actor, ATK_ROLE_MENU_ITEM);
    }
  else
    {
      st_bin_set_child (priv->status_bin, CLUTTER_ACTOR (priv->switch_actor));
      clutter_actor_set_reactive (actor, TRUE);
      clutter_actor_set_accessible_role (actor, ATK_ROLE_CHECK_MENU_ITEM);
    }

  switch_item_check_accessible_state (self);
}

/**
 * shell_popup_switch_menu_item_toggle:
 * @self: a #ShellPopupSwitchMenuItem
 *
 * Flips the switch.
 */
void
shell_popup_switch_menu_item_toggle (ShellPopupSwitchMenuItem *self)
{
  ShellPopupSwitchMenuItemPrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_SWITCH_MENU_ITEM (self));

  priv = shell_popup_switch_menu_item_get_instance_private (self);
  shell_switch_toggle (priv->switch_actor);
}

/* ======================================================================== */
/* ShellPopupImageMenuItem                                                  */
/* ======================================================================== */

enum
{
  IMAGE_ITEM_PROP_0,

  IMAGE_ITEM_PROP_TEXT,
  IMAGE_ITEM_PROP_LABEL,
  IMAGE_ITEM_PROP_ICON,
  IMAGE_ITEM_PROP_GICON,
  IMAGE_ITEM_PROP_ICON_NAME,

  N_IMAGE_ITEM_PROPS
};

static GParamSpec *image_item_props[N_IMAGE_ITEM_PROPS] = { NULL, };

typedef struct
{
  StLabel *label; /* weak, a child */
  StIcon *icon; /* weak, a child */
} ShellPopupImageMenuItemPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellPopupImageMenuItem,
                            shell_popup_image_menu_item,
                            SHELL_TYPE_POPUP_BASE_MENU_ITEM)

static void
shell_popup_image_menu_item_real_update_ornament_style (ShellPopupBaseMenuItem *item)
{
  /* we move the ornament after the label, so we don't need
   * additional padding regardless of ornament visibility */
}

static void
image_item_icon_changed_cb (GObject                 *object,
                            GParamSpec              *pspec,
                            ShellPopupImageMenuItem *self)
{
  if (g_strcmp0 (pspec->name, "gicon") == 0)
    g_object_notify_by_pspec (G_OBJECT (self), image_item_props[IMAGE_ITEM_PROP_GICON]);
  else
    g_object_notify_by_pspec (G_OBJECT (self), image_item_props[IMAGE_ITEM_PROP_ICON_NAME]);
}

static void
shell_popup_image_menu_item_get_property (GObject    *object,
                                          guint       prop_id,
                                          GValue     *value,
                                          GParamSpec *pspec)
{
  ShellPopupImageMenuItem *self = SHELL_POPUP_IMAGE_MENU_ITEM (object);
  ShellPopupImageMenuItemPrivate *priv =
    shell_popup_image_menu_item_get_instance_private (self);

  switch (prop_id)
    {
    case IMAGE_ITEM_PROP_TEXT:
      g_value_set_string (value, st_label_get_text (priv->label));
      break;

    case IMAGE_ITEM_PROP_LABEL:
      g_value_set_object (value, priv->label);
      break;

    case IMAGE_ITEM_PROP_ICON:
      g_value_set_object (value, priv->icon);
      break;

    case IMAGE_ITEM_PROP_GICON:
      g_value_set_object (value, st_icon_get_gicon (priv->icon));
      break;

    case IMAGE_ITEM_PROP_ICON_NAME:
      g_value_set_string (value, st_icon_get_icon_name (priv->icon));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_image_menu_item_set_property (GObject      *object,
                                          guint         prop_id,
                                          const GValue *value,
                                          GParamSpec   *pspec)
{
  ShellPopupImageMenuItem *self = SHELL_POPUP_IMAGE_MENU_ITEM (object);

  switch (prop_id)
    {
    case IMAGE_ITEM_PROP_TEXT:
      shell_popup_image_menu_item_set_text (self, g_value_get_string (value));
      break;

    case IMAGE_ITEM_PROP_GICON:
      shell_popup_image_menu_item_set_gicon (self, g_value_get_object (value));
      break;

    case IMAGE_ITEM_PROP_ICON_NAME:
      shell_popup_image_menu_item_set_icon_name (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_image_menu_item_constructed (GObject *object)
{
  ShellPopupImageMenuItem *self = SHELL_POPUP_IMAGE_MENU_ITEM (object);
  ShellPopupImageMenuItemPrivate *priv =
    shell_popup_image_menu_item_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  StIcon *ornament_icon;

  G_OBJECT_CLASS (shell_popup_image_menu_item_parent_class)->constructed (object);

  priv->icon = g_object_new (ST_TYPE_ICON,
                             "style-class", "popup-menu-icon",
                             "x-align", CLUTTER_ACTOR_ALIGN_END,
                             NULL);
  g_object_add_weak_pointer (G_OBJECT (priv->icon), (gpointer *) &priv->icon);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (priv->icon));
  g_signal_connect_object (priv->icon, "notify::gicon",
                           G_CALLBACK (image_item_icon_changed_cb), self, 0);
  g_signal_connect_object (priv->icon, "notify::icon-name",
                           G_CALLBACK (image_item_icon_changed_cb), self, 0);

  priv->label = item_label_new ();
  g_object_add_weak_pointer (G_OBJECT (priv->label), (gpointer *) &priv->label);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (priv->label));
  st_widget_set_label_actor (ST_WIDGET (self), CLUTTER_ACTOR (priv->label));

  ornament_icon = shell_popup_base_menu_item_get_ornament_icon (SHELL_POPUP_BASE_MENU_ITEM (self));
  clutter_actor_set_child_above_sibling (actor, CLUTTER_ACTOR (ornament_icon),
                                         CLUTTER_ACTOR (priv->label));
}

static void
shell_popup_image_menu_item_dispose (GObject *object)
{
  ShellPopupImageMenuItem *self = SHELL_POPUP_IMAGE_MENU_ITEM (object);
  ShellPopupImageMenuItemPrivate *priv =
    shell_popup_image_menu_item_get_instance_private (self);

  g_clear_weak_pointer (&priv->label);
  g_clear_weak_pointer (&priv->icon);

  G_OBJECT_CLASS (shell_popup_image_menu_item_parent_class)->dispose (object);
}

static void
shell_popup_image_menu_item_class_init (ShellPopupImageMenuItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupBaseMenuItemClass *item_class = SHELL_POPUP_BASE_MENU_ITEM_CLASS (klass);

  object_class->constructed = shell_popup_image_menu_item_constructed;
  object_class->dispose = shell_popup_image_menu_item_dispose;
  object_class->get_property = shell_popup_image_menu_item_get_property;
  object_class->set_property = shell_popup_image_menu_item_set_property;

  item_class->update_ornament_style = shell_popup_image_menu_item_real_update_ornament_style;

  /**
   * ShellPopupImageMenuItem:text:
   *
   * The text of the label of the item.
   */
  image_item_props[IMAGE_ITEM_PROP_TEXT] =
    g_param_spec_string ("text", NULL, NULL,
                         "",
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupImageMenuItem:label:
   *
   * The label of the item (`label`).
   */
  image_item_props[IMAGE_ITEM_PROP_LABEL] =
    g_param_spec_object ("label", NULL, NULL,
                         ST_TYPE_LABEL,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupImageMenuItem:icon:
   *
   * The icon of the item.
   */
  image_item_props[IMAGE_ITEM_PROP_ICON] =
    g_param_spec_object ("icon", NULL, NULL,
                         ST_TYPE_ICON,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupImageMenuItem:gicon:
   *
   * The #GIcon of the item. `setIcon ()` took either a #GIcon or an icon
   * name: set this or #ShellPopupImageMenuItem:icon-name.
   */
  image_item_props[IMAGE_ITEM_PROP_GICON] =
    g_param_spec_object ("gicon", NULL, NULL,
                         G_TYPE_ICON,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupImageMenuItem:icon-name:
   *
   * The name of the themed icon of the item.
   */
  image_item_props[IMAGE_ITEM_PROP_ICON_NAME] =
    g_param_spec_string ("icon-name", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_IMAGE_ITEM_PROPS, image_item_props);
}

static void
shell_popup_image_menu_item_init (ShellPopupImageMenuItem *self)
{
}

/**
 * shell_popup_image_menu_item_get_label:
 * @self: a #ShellPopupImageMenuItem
 *
 * Returns: (transfer none): the label of the item
 */
StLabel *
shell_popup_image_menu_item_get_label (ShellPopupImageMenuItem *self)
{
  ShellPopupImageMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_IMAGE_MENU_ITEM (self), NULL);

  priv = shell_popup_image_menu_item_get_instance_private (self);
  return priv->label;
}

/**
 * shell_popup_image_menu_item_get_icon:
 * @self: a #ShellPopupImageMenuItem
 *
 * Returns: (transfer none): the icon of the item
 */
StIcon *
shell_popup_image_menu_item_get_icon (ShellPopupImageMenuItem *self)
{
  ShellPopupImageMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_IMAGE_MENU_ITEM (self), NULL);

  priv = shell_popup_image_menu_item_get_instance_private (self);
  return priv->icon;
}

/**
 * shell_popup_image_menu_item_get_text:
 * @self: a #ShellPopupImageMenuItem
 *
 * Returns: (transfer none): the text of the label of the item
 */
const char *
shell_popup_image_menu_item_get_text (ShellPopupImageMenuItem *self)
{
  ShellPopupImageMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_IMAGE_MENU_ITEM (self), NULL);

  priv = shell_popup_image_menu_item_get_instance_private (self);
  return priv->label != NULL ? st_label_get_text (priv->label) : "";
}

/**
 * shell_popup_image_menu_item_set_text:
 * @self: a #ShellPopupImageMenuItem
 * @text: (nullable): the new text of the label of the item
 *
 * Sets #ShellPopupImageMenuItem:text.
 */
void
shell_popup_image_menu_item_set_text (ShellPopupImageMenuItem *self,
                                      const char              *text)
{
  ShellPopupImageMenuItemPrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_IMAGE_MENU_ITEM (self));

  priv = shell_popup_image_menu_item_get_instance_private (self);
  if (priv->label == NULL)
    return;

  if (g_strcmp0 (st_label_get_text (priv->label), text ? text : "") == 0)
    return;

  st_label_set_text (priv->label, text ? text : "");
  g_object_notify_by_pspec (G_OBJECT (self), image_item_props[IMAGE_ITEM_PROP_TEXT]);
}

/**
 * shell_popup_image_menu_item_get_gicon:
 * @self: a #ShellPopupImageMenuItem
 *
 * Returns: (transfer none) (nullable): the #GIcon of the item
 */
GIcon *
shell_popup_image_menu_item_get_gicon (ShellPopupImageMenuItem *self)
{
  ShellPopupImageMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_IMAGE_MENU_ITEM (self), NULL);

  priv = shell_popup_image_menu_item_get_instance_private (self);
  return st_icon_get_gicon (priv->icon);
}

/**
 * shell_popup_image_menu_item_set_gicon:
 * @self: a #ShellPopupImageMenuItem
 * @gicon: (nullable): the new icon of the item
 *
 * Sets #ShellPopupImageMenuItem:gicon.
 */
void
shell_popup_image_menu_item_set_gicon (ShellPopupImageMenuItem *self,
                                       GIcon                   *gicon)
{
  ShellPopupImageMenuItemPrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_IMAGE_MENU_ITEM (self));
  g_return_if_fail (gicon == NULL || G_IS_ICON (gicon));

  priv = shell_popup_image_menu_item_get_instance_private (self);
  if (priv->icon != NULL)
    st_icon_set_gicon (priv->icon, gicon);
}

/**
 * shell_popup_image_menu_item_get_icon_name:
 * @self: a #ShellPopupImageMenuItem
 *
 * Returns: (transfer none) (nullable): the name of the icon of the item
 */
const char *
shell_popup_image_menu_item_get_icon_name (ShellPopupImageMenuItem *self)
{
  ShellPopupImageMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_IMAGE_MENU_ITEM (self), NULL);

  priv = shell_popup_image_menu_item_get_instance_private (self);
  return st_icon_get_icon_name (priv->icon);
}

/**
 * shell_popup_image_menu_item_set_icon_name:
 * @self: a #ShellPopupImageMenuItem
 * @icon_name: (nullable): the name of the new icon of the item
 *
 * Sets #ShellPopupImageMenuItem:icon-name.
 */
void
shell_popup_image_menu_item_set_icon_name (ShellPopupImageMenuItem *self,
                                           const char              *icon_name)
{
  ShellPopupImageMenuItemPrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_IMAGE_MENU_ITEM (self));

  priv = shell_popup_image_menu_item_get_instance_private (self);
  if (priv->icon != NULL)
    st_icon_set_icon_name (priv->icon, icon_name);
}

/* ======================================================================== */
/* ShellPopupMenu                                                           */
/* ======================================================================== */

enum
{
  MENU_PROP_0,

  MENU_PROP_ARROW_ALIGNMENT,
  MENU_PROP_ARROW_SIDE,
  MENU_PROP_BOX_POINTER,

  N_MENU_PROPS
};

static GParamSpec *menu_props[N_MENU_PROPS] = { NULL, };

typedef struct
{
  ShellBoxPointer *box_pointer; /* strong, the actor unless a subclass replaced it */
  double arrow_alignment;
  StSide arrow_side;

  ClutterKeyController *key_controller; /* strong, an action of the source actor */
  gboolean source_actor_destroyed;

  gulong system_modal_opened_id;
} ShellPopupMenuPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellPopupMenu, shell_popup_menu,
                            SHELL_TYPE_POPUP_MENU_BASE)

static GWeakRef *
weak_ref_new (gpointer object)
{
  GWeakRef *ref = g_new0 (GWeakRef, 1);

  g_weak_ref_init (ref, object);
  return ref;
}

static void
menu_close_done_cb (gpointer user_data)
{
  g_autoptr (ShellPopupMenu) self = g_weak_ref_get (user_data);

  if (self != NULL)
    g_signal_emit (self, base_signals[MENU_CLOSED], 0);
}

static void
menu_system_modal_opened_cb (ShellMain      *main,
                             ShellPopupMenu *self)
{
  shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (self),
                               SHELL_POPUP_ANIMATION_FULL);
}

static void
menu_source_mapped_cb (GObject        *object,
                       GParamSpec     *pspec,
                       ShellPopupMenu *self)
{
  if (!clutter_actor_is_mapped (CLUTTER_ACTOR (object)))
    shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (self),
                                 SHELL_POPUP_ANIMATION_FULL);
}

static void
menu_source_destroyed_cb (ClutterActor   *source,
                          ShellPopupMenu *self)
{
  ShellPopupMenuPrivate *priv = shell_popup_menu_get_instance_private (self);

  priv->source_actor_destroyed = TRUE;
}

static gboolean
menu_key_press_cb (ClutterKeyController *controller,
                   ShellPopupMenu       *self)
{
  ShellPopupMenuPrivate *priv = shell_popup_menu_get_instance_private (self);
  ShellPopupMenuBase *base = SHELL_POPUP_MENU_BASE (self);
  ClutterActor *source_actor = shell_popup_menu_base_get_source_actor (base);
  ClutterActor *actor = shell_popup_menu_base_get_actor (base);
  ClutterModifierType pressed, latched, locked;
  uint32_t nav_key = 0;
  uint32_t symbol = 0;

  /* Disable toggling the menu by keyboard
   * when it cannot be toggled by pointer */
  if (source_actor == NULL || !clutter_actor_get_reactive (source_actor))
    return CLUTTER_EVENT_PROPAGATE;

  switch (shell_box_pointer_get_arrow_side (priv->box_pointer))
    {
    case ST_SIDE_TOP:
      nav_key = CLUTTER_KEY_Down;
      break;

    case ST_SIDE_BOTTOM:
      nav_key = CLUTTER_KEY_Up;
      break;

    case ST_SIDE_LEFT:
      nav_key = CLUTTER_KEY_Right;
      break;

    case ST_SIDE_RIGHT:
      nav_key = CLUTTER_KEY_Left;
      break;

    default:
      g_assert_not_reached ();
    }

  clutter_key_controller_get_key (controller, &symbol, NULL, NULL);
  clutter_key_controller_get_state (controller, &pressed, &latched, &locked);

  /* If user has a modifier down then don't handle the key presses */
  if (pressed || latched)
    return CLUTTER_EVENT_PROPAGATE;

  if (symbol == CLUTTER_KEY_space || symbol == CLUTTER_KEY_Return)
    {
      shell_popup_menu_base_toggle (base);
      return CLUTTER_EVENT_STOP;
    }
  else if (symbol == nav_key)
    {
      if (!shell_popup_menu_base_get_is_open (base))
        shell_popup_menu_base_toggle (base);

      if (ST_IS_WIDGET (actor))
        st_widget_navigate_focus (ST_WIDGET (actor), NULL,
                                  ST_DIR_TAB_FORWARD, FALSE);
      return CLUTTER_EVENT_STOP;
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static gboolean
shell_popup_menu_real_open (ShellPopupMenuBase  *base,
                            ShellPopupAnimation  animate)
{
  ShellPopupMenu *self = SHELL_POPUP_MENU (base);
  ShellPopupMenuPrivate *priv = shell_popup_menu_get_instance_private (self);
  ShellPopupMenuBaseClass *parent_class =
    SHELL_POPUP_MENU_BASE_CLASS (shell_popup_menu_parent_class);
  ClutterActor *source_actor;
  ClutterActor *actor;
  ClutterActor *parent;

  if (!parent_class->open (base, animate))
    return FALSE;

  if (priv->system_modal_opened_id == 0)
    priv->system_modal_opened_id =
      g_signal_connect_object (shell_main_get_default (), "system-modal-opened",
                               G_CALLBACK (menu_system_modal_opened_cb), self, 0);

  source_actor = shell_popup_menu_base_get_source_actor (base);
  shell_box_pointer_set_position (priv->box_pointer, source_actor,
                                  priv->arrow_alignment);
  shell_box_pointer_open (priv->box_pointer, animate, NULL, NULL, NULL);

  actor = shell_popup_menu_base_get_actor (base);
  parent = clutter_actor_get_parent (actor);
  if (parent != NULL)
    clutter_actor_set_child_above_sibling (parent, actor, NULL);

  /* The position calculation expects the source actor allocation to be
   * up to date, so queue relayout on parent to ensure it gets allocated
   * first.
   */
  if (source_actor != NULL && !clutter_actor_has_allocation (source_actor))
    {
      ClutterActor *source_parent = clutter_actor_get_parent (source_actor);

      if (source_parent != NULL)
        clutter_actor_queue_relayout (source_parent);
    }

  return TRUE;
}

static gboolean
shell_popup_menu_real_close (ShellPopupMenuBase  *base,
                             ShellPopupAnimation  animate)
{
  ShellPopupMenu *self = SHELL_POPUP_MENU (base);
  ShellPopupMenuPrivate *priv = shell_popup_menu_get_instance_private (self);
  ShellPopupMenuBasePrivate *base_priv = shell_popup_menu_base_get_instance_private (base);
  ShellPopupMenuBaseClass *parent_class =
    SHELL_POPUP_MENU_BASE_CLASS (shell_popup_menu_parent_class);

  if (base_priv->active_menu_item != NULL)
    shell_popup_base_menu_item_set_active (base_priv->active_menu_item, FALSE);

  if (priv->box_pointer != NULL &&
      clutter_actor_is_visible (CLUTTER_ACTOR (priv->box_pointer)))
    shell_box_pointer_close (priv->box_pointer, animate,
                             menu_close_done_cb, weak_ref_new (self),
                             weak_ref_free);

  return parent_class->close (base, animate);
}

static void
shell_popup_menu_real_destroy (ShellPopupMenuBase *base)
{
  ShellPopupMenu *self = SHELL_POPUP_MENU (base);
  ShellPopupMenuPrivate *priv = shell_popup_menu_get_instance_private (self);
  ShellPopupMenuBaseClass *parent_class =
    SHELL_POPUP_MENU_BASE_CLASS (shell_popup_menu_parent_class);
  ClutterActor *source_actor = shell_popup_menu_base_get_source_actor (base);

  if (source_actor != NULL)
    {
      g_signal_handlers_disconnect_by_data (source_actor, self);

      if (!priv->source_actor_destroyed)
        clutter_actor_remove_action (source_actor,
                                     CLUTTER_ACTION (priv->key_controller));
    }

  g_clear_signal_handler (&priv->system_modal_opened_id, shell_main_get_default ());

  parent_class->destroy (base);
}

static void
shell_popup_menu_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  ShellPopupMenu *self = SHELL_POPUP_MENU (object);
  ShellPopupMenuPrivate *priv = shell_popup_menu_get_instance_private (self);

  switch (prop_id)
    {
    case MENU_PROP_ARROW_ALIGNMENT:
      g_value_set_double (value, priv->arrow_alignment);
      break;

    case MENU_PROP_ARROW_SIDE:
      g_value_set_enum (value, priv->arrow_side);
      break;

    case MENU_PROP_BOX_POINTER:
      g_value_set_object (value, priv->box_pointer);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_menu_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  ShellPopupMenu *self = SHELL_POPUP_MENU (object);
  ShellPopupMenuPrivate *priv = shell_popup_menu_get_instance_private (self);

  switch (prop_id)
    {
    case MENU_PROP_ARROW_ALIGNMENT:
      priv->arrow_alignment = g_value_get_double (value);
      break;

    case MENU_PROP_ARROW_SIDE:
      priv->arrow_side = g_value_get_enum (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_menu_constructed (GObject *object)
{
  ShellPopupMenu *self = SHELL_POPUP_MENU (object);
  ShellPopupMenuPrivate *priv = shell_popup_menu_get_instance_private (self);
  ShellPopupMenuBase *base = SHELL_POPUP_MENU_BASE (self);
  StBoxLayout *box;
  ClutterActor *source_actor;
  ClutterActor *actor;

  G_OBJECT_CLASS (shell_popup_menu_parent_class)->constructed (object);

  box = shell_popup_menu_base_get_box (base);
  source_actor = shell_popup_menu_base_get_source_actor (base);

  st_widget_set_style_class_name (ST_WIDGET (box), "popup-menu-content");

  /* A subclass can replace the actor of the menu by one which contains the
   * box pointer, so the menu keeps its own reference to it */
  priv->box_pointer = g_object_ref_sink (g_object_new (SHELL_TYPE_BOX_POINTER,
                                                       "arrow-side", priv->arrow_side,
                                                       NULL));
  actor = CLUTTER_ACTOR (priv->box_pointer);

  shell_popup_menu_base_set_actor (base, actor);
  st_widget_set_style_class_name (ST_WIDGET (actor), "popup-menu-boxpointer");

  st_bin_set_child (shell_box_pointer_get_bin (priv->box_pointer),
                    CLUTTER_ACTOR (box));
  st_widget_add_style_class_name (ST_WIDGET (actor), "popup-menu");
  st_widget_set_keynav_flags (ST_WIDGET (actor), ST_KEYNAV_FLAG_WRAP_VERTICALLY);

  st_focus_manager_add_group (st_focus_manager_get_for_stage (get_stage ()),
                              ST_WIDGET (actor));
  clutter_actor_set_reactive (actor, TRUE);

  priv->key_controller =
    CLUTTER_KEY_CONTROLLER (g_object_ref_sink (clutter_key_controller_new (NULL)));
  g_signal_connect_object (priv->key_controller, "key-press",
                           G_CALLBACK (menu_key_press_cb), self, 0);

  if (source_actor != NULL)
    {
      g_signal_connect_object (source_actor, "notify::mapped",
                               G_CALLBACK (menu_source_mapped_cb), self, 0);
      g_signal_connect_object (source_actor, "destroy",
                               G_CALLBACK (menu_source_destroyed_cb), self, 0);
      clutter_actor_add_action (source_actor,
                                CLUTTER_ACTION (priv->key_controller));
    }
}

static void
shell_popup_menu_dispose (GObject *object)
{
  ShellPopupMenu *self = SHELL_POPUP_MENU (object);
  ShellPopupMenuPrivate *priv = shell_popup_menu_get_instance_private (self);

  g_clear_signal_handler (&priv->system_modal_opened_id, shell_main_get_default ());
  g_clear_object (&priv->box_pointer);
  g_clear_object (&priv->key_controller);

  G_OBJECT_CLASS (shell_popup_menu_parent_class)->dispose (object);
}

static void
shell_popup_menu_class_init (ShellPopupMenuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupMenuBaseClass *base_class = SHELL_POPUP_MENU_BASE_CLASS (klass);

  object_class->constructed = shell_popup_menu_constructed;
  object_class->dispose = shell_popup_menu_dispose;
  object_class->get_property = shell_popup_menu_get_property;
  object_class->set_property = shell_popup_menu_set_property;

  base_class->open = shell_popup_menu_real_open;
  base_class->close = shell_popup_menu_real_close;
  base_class->destroy = shell_popup_menu_real_destroy;

  /**
   * ShellPopupMenu:arrow-alignment:
   *
   * Where the arrow of the box pointer points to along the menu, from 0 to 1
   * (`arrowAlignment`).
   */
  menu_props[MENU_PROP_ARROW_ALIGNMENT] =
    g_param_spec_double ("arrow-alignment", NULL, NULL,
                         0.0, 1.0, 0.0,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupMenu:arrow-side:
   *
   * The side of the menu the arrow points from (`arrowSide`).
   */
  menu_props[MENU_PROP_ARROW_SIDE] =
    g_param_spec_enum ("arrow-side", NULL, NULL,
                       ST_TYPE_SIDE,
                       ST_SIDE_TOP,
                       G_PARAM_READWRITE |
                       G_PARAM_CONSTRUCT_ONLY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupMenu:box-pointer:
   *
   * The #ShellBoxPointer the menu is shown in (`_boxPointer`). It is the
   * #ShellPopupMenuBase:actor of the menu unless a subclass wraps it in
   * another actor.
   */
  menu_props[MENU_PROP_BOX_POINTER] =
    g_param_spec_object ("box-pointer", NULL, NULL,
                         SHELL_TYPE_BOX_POINTER,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_MENU_PROPS, menu_props);
}

static void
shell_popup_menu_init (ShellPopupMenu *self)
{
  ShellPopupMenuPrivate *priv = shell_popup_menu_get_instance_private (self);

  priv->arrow_side = ST_SIDE_TOP;
}

/**
 * shell_popup_menu_new:
 * @source_actor: (nullable): the actor the menu is attached to
 * @arrow_alignment: where the arrow points to along the menu, from 0 to 1
 * @arrow_side: the side of the menu the arrow points from
 *
 * Creates a new #ShellPopupMenu (`new PopupMenu (sourceActor, arrowAlignment,
 * arrowSide)`).
 *
 * Returns: (transfer full): a new #ShellPopupMenu
 */
ShellPopupMenu *
shell_popup_menu_new (ClutterActor *source_actor,
                      double        arrow_alignment,
                      StSide        arrow_side)
{
  return g_object_new (SHELL_TYPE_POPUP_MENU,
                       "source-actor", source_actor,
                       "arrow-alignment", arrow_alignment,
                       "arrow-side", arrow_side,
                       NULL);
}

/**
 * shell_popup_menu_get_box_pointer:
 * @self: a #ShellPopupMenu
 *
 * Returns: (transfer none): the box pointer the menu is shown in
 */
ShellBoxPointer *
shell_popup_menu_get_box_pointer (ShellPopupMenu *self)
{
  ShellPopupMenuPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU (self), NULL);

  priv = shell_popup_menu_get_instance_private (self);
  return priv->box_pointer;
}

/**
 * shell_popup_menu_get_arrow_side:
 * @self: a #ShellPopupMenu
 *
 * Returns: the side of the menu the arrow points from, as the box pointer has
 *   it now (it may flip it to fit the menu on the screen)
 */
StSide
shell_popup_menu_get_arrow_side (ShellPopupMenu *self)
{
  ShellPopupMenuPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU (self), ST_SIDE_TOP);

  priv = shell_popup_menu_get_instance_private (self);
  if (priv->box_pointer != NULL)
    return shell_box_pointer_get_arrow_side (priv->box_pointer);

  return priv->arrow_side;
}

/**
 * shell_popup_menu_get_arrow_alignment:
 * @self: a #ShellPopupMenu
 *
 * Returns: where the arrow points to along the menu, from 0 to 1
 */
double
shell_popup_menu_get_arrow_alignment (ShellPopupMenu *self)
{
  ShellPopupMenuPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_MENU (self), 0.0);

  priv = shell_popup_menu_get_instance_private (self);
  return priv->arrow_alignment;
}

/**
 * shell_popup_menu_set_arrow_origin:
 * @self: a #ShellPopupMenu
 * @origin: the position of the arrow along the box pointer, in pixels
 *
 * Moves the arrow of the menu (`setArrowOrigin ()`).
 */
void
shell_popup_menu_set_arrow_origin (ShellPopupMenu *self,
                                   double          origin)
{
  ShellPopupMenuPrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_MENU (self));

  priv = shell_popup_menu_get_instance_private (self);
  if (priv->box_pointer != NULL)
    shell_box_pointer_set_arrow_origin (priv->box_pointer, origin);
}

/**
 * shell_popup_menu_set_source_alignment:
 * @self: a #ShellPopupMenu
 * @alignment: where along the source actor the menu points at, from 0 to 1
 *
 * Sets the alignment of the menu along its source actor
 * (`setSourceAlignment ()`).
 */
void
shell_popup_menu_set_source_alignment (ShellPopupMenu *self,
                                       double          alignment)
{
  ShellPopupMenuPrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_MENU (self));

  priv = shell_popup_menu_get_instance_private (self);
  if (priv->box_pointer != NULL)
    shell_box_pointer_set_source_alignment (priv->box_pointer, alignment);
}

/* ======================================================================== */
/* ShellPopupDummyMenu                                                      */
/* ======================================================================== */

enum
{
  DUMMY_PROP_0,

  DUMMY_PROP_SOURCE_ACTOR,
  DUMMY_PROP_ACTOR,
  DUMMY_PROP_IS_OPEN,
  DUMMY_PROP_SENSITIVE,

  N_DUMMY_PROPS
};

static GParamSpec *dummy_props[N_DUMMY_PROPS] = { NULL, };

enum
{
  DUMMY_OPEN_STATE_CHANGED,
  DUMMY_DESTROY,

  N_DUMMY_SIGNALS
};

static guint dummy_signals[N_DUMMY_SIGNALS] = { 0, };

struct _ShellPopupDummyMenu
{
  GObject parent;

  ClutterActor *source_actor; /* weak */
  gboolean is_open;
};

G_DEFINE_FINAL_TYPE (ShellPopupDummyMenu, shell_popup_dummy_menu, G_TYPE_OBJECT)

static void
shell_popup_dummy_menu_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  ShellPopupDummyMenu *self = SHELL_POPUP_DUMMY_MENU (object);

  switch (prop_id)
    {
    case DUMMY_PROP_SOURCE_ACTOR:
    case DUMMY_PROP_ACTOR:
      g_value_set_object (value, self->source_actor);
      break;

    case DUMMY_PROP_IS_OPEN:
      g_value_set_boolean (value, self->is_open);
      break;

    case DUMMY_PROP_SENSITIVE:
      g_value_set_boolean (value, TRUE);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_dummy_menu_set_property (GObject      *object,
                                     guint         prop_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  ShellPopupDummyMenu *self = SHELL_POPUP_DUMMY_MENU (object);

  switch (prop_id)
    {
    case DUMMY_PROP_SOURCE_ACTOR:
      g_set_weak_pointer (&self->source_actor, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_dummy_menu_dispose (GObject *object)
{
  ShellPopupDummyMenu *self = SHELL_POPUP_DUMMY_MENU (object);

  g_clear_weak_pointer (&self->source_actor);

  G_OBJECT_CLASS (shell_popup_dummy_menu_parent_class)->dispose (object);
}

static void
shell_popup_dummy_menu_class_init (ShellPopupDummyMenuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_popup_dummy_menu_dispose;
  object_class->get_property = shell_popup_dummy_menu_get_property;
  object_class->set_property = shell_popup_dummy_menu_set_property;

  /**
   * ShellPopupDummyMenu:source-actor:
   *
   * The actor the menu stands in for (`sourceActor`).
   */
  dummy_props[DUMMY_PROP_SOURCE_ACTOR] =
    g_param_spec_object ("source-actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupDummyMenu:actor:
   *
   * The actor of the menu, which is its source actor.
   */
  dummy_props[DUMMY_PROP_ACTOR] =
    g_param_spec_object ("actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupDummyMenu:is-open:
   *
   * Whether the menu is open (`isOpen`).
   */
  dummy_props[DUMMY_PROP_IS_OPEN] =
    g_param_spec_boolean ("is-open", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupDummyMenu:sensitive:
   *
   * Always %TRUE.
   */
  dummy_props[DUMMY_PROP_SENSITIVE] =
    g_param_spec_boolean ("sensitive", NULL, NULL,
                          TRUE,
                          G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_DUMMY_PROPS, dummy_props);

  /**
   * ShellPopupDummyMenu::open-state-changed:
   * @self: the #ShellPopupDummyMenu
   * @open: whether the menu is open now
   *
   * Emitted when the menu opens or closes.
   */
  dummy_signals[DUMMY_OPEN_STATE_CHANGED] =
    g_signal_new ("open-state-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  /**
   * ShellPopupDummyMenu::destroy:
   * @self: the #ShellPopupDummyMenu
   *
   * Emitted when the menu was destroyed.
   */
  dummy_signals[DUMMY_DESTROY] =
    g_signal_new ("destroy",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_popup_dummy_menu_init (ShellPopupDummyMenu *self)
{
}

/**
 * shell_popup_dummy_menu_new:
 * @source_actor: the actor the menu stands in for
 *
 * Creates a menu which never shows anything, for the actors which are
 * managed by a #ShellPopupMenuManager but have no menu to show.
 *
 * Returns: (transfer full): a new #ShellPopupDummyMenu
 */
ShellPopupDummyMenu *
shell_popup_dummy_menu_new (ClutterActor *source_actor)
{
  return g_object_new (SHELL_TYPE_POPUP_DUMMY_MENU,
                       "source-actor", source_actor,
                       NULL);
}

/**
 * shell_popup_dummy_menu_get_actor:
 * @self: a #ShellPopupDummyMenu
 *
 * Returns: (transfer none) (nullable): the actor of the menu, which is the
 *   source actor
 */
ClutterActor *
shell_popup_dummy_menu_get_actor (ShellPopupDummyMenu *self)
{
  g_return_val_if_fail (SHELL_IS_POPUP_DUMMY_MENU (self), NULL);

  return self->source_actor;
}

/**
 * shell_popup_dummy_menu_get_is_open:
 * @self: a #ShellPopupDummyMenu
 *
 * Returns: whether the menu is "open"
 */
gboolean
shell_popup_dummy_menu_get_is_open (ShellPopupDummyMenu *self)
{
  g_return_val_if_fail (SHELL_IS_POPUP_DUMMY_MENU (self), FALSE);

  return self->is_open;
}

/**
 * shell_popup_dummy_menu_get_sensitive:
 * @self: a #ShellPopupDummyMenu
 *
 * Returns: %TRUE
 */
gboolean
shell_popup_dummy_menu_get_sensitive (ShellPopupDummyMenu *self)
{
  g_return_val_if_fail (SHELL_IS_POPUP_DUMMY_MENU (self), FALSE);

  return TRUE;
}

/**
 * shell_popup_dummy_menu_open:
 * @self: a #ShellPopupDummyMenu
 * @animate: ignored
 *
 * Marks the menu as open and emits #ShellPopupDummyMenu::open-state-changed.
 */
void
shell_popup_dummy_menu_open (ShellPopupDummyMenu *self,
                             ShellPopupAnimation  animate)
{
  g_return_if_fail (SHELL_IS_POPUP_DUMMY_MENU (self));

  if (self->is_open)
    return;

  self->is_open = TRUE;
  g_object_notify_by_pspec (G_OBJECT (self), dummy_props[DUMMY_PROP_IS_OPEN]);
  g_signal_emit (self, dummy_signals[DUMMY_OPEN_STATE_CHANGED], 0, TRUE);
}

/**
 * shell_popup_dummy_menu_close:
 * @self: a #ShellPopupDummyMenu
 * @animate: ignored
 *
 * Marks the menu as closed and emits #ShellPopupDummyMenu::open-state-changed.
 */
void
shell_popup_dummy_menu_close (ShellPopupDummyMenu *self,
                              ShellPopupAnimation  animate)
{
  g_return_if_fail (SHELL_IS_POPUP_DUMMY_MENU (self));

  if (!self->is_open)
    return;

  self->is_open = FALSE;
  g_object_notify_by_pspec (G_OBJECT (self), dummy_props[DUMMY_PROP_IS_OPEN]);
  g_signal_emit (self, dummy_signals[DUMMY_OPEN_STATE_CHANGED], 0, FALSE);
}

/**
 * shell_popup_dummy_menu_toggle:
 * @self: a #ShellPopupDummyMenu
 *
 * Does nothing.
 */
void
shell_popup_dummy_menu_toggle (ShellPopupDummyMenu *self)
{
  g_return_if_fail (SHELL_IS_POPUP_DUMMY_MENU (self));
}

/**
 * shell_popup_dummy_menu_destroy:
 * @self: a #ShellPopupDummyMenu
 *
 * Emits #ShellPopupDummyMenu::destroy.
 */
void
shell_popup_dummy_menu_destroy (ShellPopupDummyMenu *self)
{
  g_autoptr (ShellPopupDummyMenu) keep_alive = NULL;

  g_return_if_fail (SHELL_IS_POPUP_DUMMY_MENU (self));

  keep_alive = g_object_ref (self);
  g_signal_emit (self, dummy_signals[DUMMY_DESTROY], 0);
}

/* ======================================================================== */
/* ShellPopupSubMenu                                                        */
/* ======================================================================== */

enum
{
  SUB_MENU_PROP_0,

  SUB_MENU_PROP_SOURCE_ARROW,

  N_SUB_MENU_PROPS
};

static GParamSpec *sub_menu_props[N_SUB_MENU_PROPS] = { NULL, };

#define SUB_MENU_ANIMATION_TIME 250

typedef struct
{
  ClutterActor *source_arrow; /* weak */
  ClutterKeyController *key_controller; /* strong, an action of the actor */
} ShellPopupSubMenuPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellPopupSubMenu, shell_popup_sub_menu,
                            SHELL_TYPE_POPUP_MENU_BASE)

/* _needsScrollbar () */
static gboolean
sub_menu_needs_scrollbar (ShellPopupSubMenu *self)
{
  ShellPopupMenuBase *top_menu = shell_popup_menu_base_get_top_menu (SHELL_POPUP_MENU_BASE (self));
  ClutterActor *top_actor = shell_popup_menu_base_get_actor (top_menu);
  StThemeNode *theme_node;
  float natural_height = 0;
  int max_height;

  if (!ST_IS_WIDGET (top_actor))
    return FALSE;

  clutter_actor_get_preferred_height (top_actor, -1, NULL, &natural_height);
  theme_node = st_widget_get_theme_node (ST_WIDGET (top_actor));

  max_height = st_theme_node_get_max_height (theme_node);
  return max_height >= 0 && natural_height >= max_height;
}

static void
sub_menu_open_done_cb (gboolean finished,
                       gpointer user_data)
{
  g_autoptr (ClutterActor) actor = g_weak_ref_get (user_data);

  if (finished && actor != NULL)
    clutter_actor_set_height (actor, -1);
}

static void
sub_menu_close_done_cb (gboolean finished,
                        gpointer user_data)
{
  g_autoptr (ClutterActor) actor = g_weak_ref_get (user_data);

  if (finished && actor != NULL)
    {
      clutter_actor_hide (actor);
      clutter_actor_set_height (actor, -1);
    }
}

static void
sub_menu_ease_arrow (ShellPopupSubMenu *self,
                     double             angle,
                     int                duration)
{
  ShellPopupSubMenuPrivate *priv = shell_popup_sub_menu_get_instance_private (self);
  ShellEaseParams params = make_ease_params (duration, CLUTTER_EASE_OUT_EXPO);

  if (priv->source_arrow == NULL)
    return;

  shell_actor_ease (priv->source_arrow, &params, NULL, NULL, NULL,
                    "rotation-angle-z", angle,
                    NULL);
}

static gboolean
shell_popup_sub_menu_real_open (ShellPopupMenuBase  *base,
                                ShellPopupAnimation  animate)
{
  ShellPopupSubMenu *self = SHELL_POPUP_SUB_MENU (base);
  ShellPopupMenuBaseClass *parent_class =
    SHELL_POPUP_MENU_BASE_CLASS (shell_popup_sub_menu_parent_class);
  ClutterActor *actor;
  ShellEaseParams params;
  gboolean needs_scrollbar;
  gboolean do_animate = animate != SHELL_POPUP_ANIMATION_NONE;
  double target_angle;
  float natural_height = 0;
  int duration;

  if (!parent_class->open (base, animate))
    return FALSE;

  actor = shell_popup_menu_base_get_actor (base);
  needs_scrollbar = sub_menu_needs_scrollbar (self);

  /* St.ScrollView always requests space horizontally for a possible vertical
   * scrollbar if in AUTOMATIC mode. Doing better would require implementation
   * of width-for-height in St.BoxLayout and St.ScrollView. This looks bad
   * when we *don't* need it, so turn off the scrollbar when that's true.
   * Dynamic changes in whether we need it aren't handled properly.
   */
  if (ST_IS_SCROLL_VIEW (actor))
    g_object_set (actor,
                  "vscrollbar-policy",
                  needs_scrollbar ? ST_POLICY_AUTOMATIC : ST_POLICY_NEVER,
                  NULL);

  if (needs_scrollbar)
    st_widget_add_style_pseudo_class (ST_WIDGET (actor), "scrolled");
  else
    st_widget_remove_style_pseudo_class (ST_WIDGET (actor), "scrolled");

  /* It looks funny if we animate with a scrollbar (at what point is
   * the scrollbar added?) so just skip that case */
  if (do_animate && needs_scrollbar)
    do_animate = FALSE;

  target_angle =
    clutter_actor_get_text_direction (actor) == CLUTTER_TEXT_DIRECTION_RTL ? -90 : 90;

  duration = do_animate ? SUB_MENU_ANIMATION_TIME : 0;
  clutter_actor_get_preferred_height (actor, -1, NULL, &natural_height);
  clutter_actor_set_height (actor, 0);

  params = make_ease_params (duration, CLUTTER_EASE_OUT_EXPO);
  shell_actor_ease (actor, &params,
                    sub_menu_open_done_cb, weak_ref_new (actor), weak_ref_free,
                    "height", (double) natural_height,
                    NULL);
  sub_menu_ease_arrow (self, target_angle, duration);

  return TRUE;
}

static gboolean
shell_popup_sub_menu_real_close (ShellPopupMenuBase  *base,
                                 ShellPopupAnimation  animate)
{
  ShellPopupSubMenu *self = SHELL_POPUP_SUB_MENU (base);
  ShellPopupMenuBasePrivate *base_priv = shell_popup_menu_base_get_instance_private (base);
  ShellPopupMenuBaseClass *parent_class =
    SHELL_POPUP_MENU_BASE_CLASS (shell_popup_sub_menu_parent_class);
  ClutterActor *actor;
  ShellEaseParams params;
  gboolean do_animate = animate != SHELL_POPUP_ANIMATION_NONE;
  int duration;

  if (!parent_class->close (base, animate))
    return FALSE;

  if (base_priv->active_menu_item != NULL)
    shell_popup_base_menu_item_set_active (base_priv->active_menu_item, FALSE);

  if (do_animate && sub_menu_needs_scrollbar (self))
    do_animate = FALSE;

  actor = shell_popup_menu_base_get_actor (base);
  duration = do_animate ? SUB_MENU_ANIMATION_TIME : 0;

  params = make_ease_params (duration, CLUTTER_EASE_OUT_EXPO);
  shell_actor_ease (actor, &params,
                    sub_menu_close_done_cb, weak_ref_new (actor), weak_ref_free,
                    "height", 0.,
                    NULL);
  sub_menu_ease_arrow (self, 0, duration);

  return TRUE;
}

static gboolean
shell_popup_sub_menu_real_get_sensitive (ShellPopupMenuBase *base)
{
  ShellPopupMenuBasePrivate *base_priv = shell_popup_menu_base_get_instance_private (base);
  ClutterActor *source_actor = base_priv->source_actor;
  gboolean source_sensitive = TRUE;

  if (SHELL_IS_POPUP_BASE_MENU_ITEM (source_actor))
    source_sensitive =
      shell_popup_base_menu_item_get_sensitive (SHELL_POPUP_BASE_MENU_ITEM (source_actor));

  return base_priv->sensitive && source_sensitive;
}

static gboolean
sub_menu_key_press_cb (ClutterKeyController *controller,
                       ShellPopupSubMenu    *self)
{
  ShellPopupMenuBase *base = SHELL_POPUP_MENU_BASE (self);
  ClutterActor *source_actor = shell_popup_menu_base_get_source_actor (base);
  uint32_t symbol = 0;

  /* Move focus back to parent menu if the user types Left. */
  clutter_key_controller_get_key (controller, &symbol, NULL, NULL);

  if (shell_popup_menu_base_get_is_open (base) && symbol == CLUTTER_KEY_Left)
    {
      shell_popup_menu_base_close (base, SHELL_POPUP_ANIMATION_FULL);
      if (source_actor != NULL)
        clutter_actor_grab_key_focus (source_actor);
      return CLUTTER_EVENT_STOP;
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static void
shell_popup_sub_menu_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  ShellPopupSubMenu *self = SHELL_POPUP_SUB_MENU (object);
  ShellPopupSubMenuPrivate *priv = shell_popup_sub_menu_get_instance_private (self);

  switch (prop_id)
    {
    case SUB_MENU_PROP_SOURCE_ARROW:
      g_value_set_object (value, priv->source_arrow);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_sub_menu_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  ShellPopupSubMenu *self = SHELL_POPUP_SUB_MENU (object);
  ShellPopupSubMenuPrivate *priv = shell_popup_sub_menu_get_instance_private (self);

  switch (prop_id)
    {
    case SUB_MENU_PROP_SOURCE_ARROW:
      g_set_weak_pointer (&priv->source_arrow, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_sub_menu_constructed (GObject *object)
{
  ShellPopupSubMenu *self = SHELL_POPUP_SUB_MENU (object);
  ShellPopupSubMenuPrivate *priv = shell_popup_sub_menu_get_instance_private (self);
  ShellPopupMenuBase *base = SHELL_POPUP_MENU_BASE (self);
  ClutterActor *actor;

  G_OBJECT_CLASS (shell_popup_sub_menu_parent_class)->constructed (object);

  /* Since a function of a submenu might be to provide a "More.." expander
   * with long content, we make it scrollable - the scrollbar will only take
   * effect if a CSS max-height is set on the top menu.
   */
  actor = g_object_new (ST_TYPE_SCROLL_VIEW,
                        "style-class", "popup-sub-menu",
                        "vscrollbar-policy", ST_POLICY_NEVER,
                        NULL);
  st_scroll_view_set_child (ST_SCROLL_VIEW (actor),
                            ST_SCROLLABLE (shell_popup_menu_base_get_box (base)));

  shell_popup_menu_base_set_actor (base, actor);
  clutter_actor_set_clip_to_allocation (actor, TRUE);
  clutter_actor_hide (actor);

  priv->key_controller =
    CLUTTER_KEY_CONTROLLER (g_object_ref_sink (clutter_key_controller_new (NULL)));
  g_signal_connect_object (priv->key_controller, "key-press",
                           G_CALLBACK (sub_menu_key_press_cb), self, 0);
  clutter_actor_add_action (actor, CLUTTER_ACTION (priv->key_controller));
}

static void
shell_popup_sub_menu_dispose (GObject *object)
{
  ShellPopupSubMenu *self = SHELL_POPUP_SUB_MENU (object);
  ShellPopupSubMenuPrivate *priv = shell_popup_sub_menu_get_instance_private (self);

  g_clear_weak_pointer (&priv->source_arrow);
  g_clear_object (&priv->key_controller);

  G_OBJECT_CLASS (shell_popup_sub_menu_parent_class)->dispose (object);
}

static void
shell_popup_sub_menu_class_init (ShellPopupSubMenuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupMenuBaseClass *base_class = SHELL_POPUP_MENU_BASE_CLASS (klass);

  object_class->constructed = shell_popup_sub_menu_constructed;
  object_class->dispose = shell_popup_sub_menu_dispose;
  object_class->get_property = shell_popup_sub_menu_get_property;
  object_class->set_property = shell_popup_sub_menu_set_property;

  base_class->open = shell_popup_sub_menu_real_open;
  base_class->close = shell_popup_sub_menu_real_close;
  base_class->get_sensitive = shell_popup_sub_menu_real_get_sensitive;

  /**
   * ShellPopupSubMenu:source-arrow:
   *
   * The arrow which is rotated when the menu opens or closes (`sourceArrow`).
   */
  sub_menu_props[SUB_MENU_PROP_SOURCE_ARROW] =
    g_param_spec_object ("source-arrow", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_SUB_MENU_PROPS, sub_menu_props);
}

static void
shell_popup_sub_menu_init (ShellPopupSubMenu *self)
{
}

/**
 * shell_popup_sub_menu_get_source_arrow:
 * @self: a #ShellPopupSubMenu
 *
 * Returns: (transfer none) (nullable): the arrow of the item of the submenu
 */
ClutterActor *
shell_popup_sub_menu_get_source_arrow (ShellPopupSubMenu *self)
{
  ShellPopupSubMenuPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_SUB_MENU (self), NULL);

  priv = shell_popup_sub_menu_get_instance_private (self);
  return priv->source_arrow;
}

/* ======================================================================== */
/* ShellPopupMenuSection                                                    */
/* ======================================================================== */

G_DEFINE_TYPE (ShellPopupMenuSection, shell_popup_menu_section,
               SHELL_TYPE_POPUP_MENU_BASE)

/* deliberately ignore any attempt to open () or close (), but emit the
 * corresponding signal so children can still pick it up */
static gboolean
shell_popup_menu_section_real_open (ShellPopupMenuBase  *base,
                                    ShellPopupAnimation  animate)
{
  g_signal_emit (base, base_signals[OPEN_STATE_CHANGED], 0, TRUE);

  return FALSE;
}

static gboolean
shell_popup_menu_section_real_close (ShellPopupMenuBase  *base,
                                     ShellPopupAnimation  animate)
{
  g_signal_emit (base, base_signals[OPEN_STATE_CHANGED], 0, FALSE);

  return FALSE;
}

static void
shell_popup_menu_section_constructed (GObject *object)
{
  ShellPopupMenuBase *base = SHELL_POPUP_MENU_BASE (object);
  ClutterActor *box;

  G_OBJECT_CLASS (shell_popup_menu_section_parent_class)->constructed (object);

  box = CLUTTER_ACTOR (shell_popup_menu_base_get_box (base));
  shell_popup_menu_base_set_actor (base, box);
  menu_base_set_is_open (base, TRUE);

  st_widget_add_style_class_name (ST_WIDGET (box), "popup-menu-section");
}

static void
shell_popup_menu_section_class_init (ShellPopupMenuSectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupMenuBaseClass *base_class = SHELL_POPUP_MENU_BASE_CLASS (klass);

  object_class->constructed = shell_popup_menu_section_constructed;

  base_class->open = shell_popup_menu_section_real_open;
  base_class->close = shell_popup_menu_section_real_close;
}

static void
shell_popup_menu_section_init (ShellPopupMenuSection *self)
{
}

/**
 * shell_popup_menu_section_new:
 *
 * Creates a section of a #ShellPopupMenu: a list of items which is handled
 * like a submenu (items can be added to and removed from it, it can be
 * destroyed and added to another menu), but is completely transparent to the
 * user.
 *
 * Returns: (transfer full): a new #ShellPopupMenuSection
 */
ShellPopupMenuSection *
shell_popup_menu_section_new (void)
{
  return g_object_new (SHELL_TYPE_POPUP_MENU_SECTION, NULL);
}

/* ======================================================================== */
/* ShellPopupSubMenuMenuItem                                                */
/* ======================================================================== */

enum
{
  SUB_ITEM_PROP_0,

  SUB_ITEM_PROP_TEXT,
  SUB_ITEM_PROP_WANT_ICON,
  SUB_ITEM_PROP_MENU,
  SUB_ITEM_PROP_LABEL,
  SUB_ITEM_PROP_ICON,

  N_SUB_ITEM_PROPS
};

static GParamSpec *sub_item_props[N_SUB_ITEM_PROPS] = { NULL, };

typedef struct
{
  ShellPopupSubMenu *menu; /* strong */
  StLabel *label; /* weak, a child */
  StIcon *icon; /* weak, a child, if wanted */
  ClutterActor *triangle; /* weak, a descendant */
  gboolean want_icon;
} ShellPopupSubMenuMenuItemPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellPopupSubMenuMenuItem,
                            shell_popup_sub_menu_menu_item,
                            SHELL_TYPE_POPUP_BASE_MENU_ITEM)

static gboolean
sub_item_open_binding_cb (GObject             *object,
                          const char          *action_name,
                          guint                key_val,
                          ClutterModifierType  modifiers,
                          gpointer             user_data)
{
  ShellPopupSubMenuMenuItem *self = SHELL_POPUP_SUB_MENU_MENU_ITEM (object);
  ShellPopupSubMenuMenuItemPrivate *priv =
    shell_popup_sub_menu_menu_item_get_instance_private (self);
  ClutterActor *menu_actor;

  shell_popup_sub_menu_menu_item_set_submenu_shown (self, TRUE);

  menu_actor = shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (priv->menu));
  if (ST_IS_WIDGET (menu_actor))
    st_widget_navigate_focus (ST_WIDGET (menu_actor), NULL, ST_DIR_DOWN, FALSE);

  return CLUTTER_EVENT_STOP;
}

/* _subMenuOpenStateChanged () */
static void
sub_item_open_state_changed_cb (ShellPopupMenuBase       *menu,
                                gboolean                  open,
                                ShellPopupSubMenuMenuItem *self)
{
  ShellPopupBaseMenuItem *item = SHELL_POPUP_BASE_MENU_ITEM (self);
  StWidget *widget = ST_WIDGET (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellPopupMenuBase *top_menu = shell_popup_base_menu_item_get_top_menu (item);

  if (open)
    {
      st_widget_add_style_pseudo_class (widget, "open");
      if (top_menu != NULL)
        shell_popup_menu_base_set_opened_submenu (top_menu, menu);
      clutter_actor_add_accessible_state (actor, ATK_STATE_EXPANDED);
      st_widget_add_style_pseudo_class (widget, "checked");
    }
  else
    {
      st_widget_remove_style_pseudo_class (widget, "open");
      if (top_menu != NULL)
        shell_popup_menu_base_set_opened_submenu (top_menu, NULL);
      clutter_actor_remove_accessible_state (actor, ATK_STATE_EXPANDED);
      st_widget_remove_style_pseudo_class (widget, "checked");
    }
}

static void
sub_item_destroyed_cb (ClutterActor              *actor,
                       ShellPopupSubMenuMenuItem *self)
{
  ShellPopupSubMenuMenuItemPrivate *priv =
    shell_popup_sub_menu_menu_item_get_instance_private (self);

  if (priv->menu != NULL)
    shell_popup_menu_base_destroy (SHELL_POPUP_MENU_BASE (priv->menu));
}

static void
shell_popup_sub_menu_menu_item_real_set_parent_menu (ShellPopupBaseMenuItem *item,
                                                     ShellPopupMenuBase     *parent)
{
  ShellPopupSubMenuMenuItem *self = SHELL_POPUP_SUB_MENU_MENU_ITEM (item);
  ShellPopupSubMenuMenuItemPrivate *priv =
    shell_popup_sub_menu_menu_item_get_instance_private (self);
  ShellPopupBaseMenuItemClass *parent_class =
    SHELL_POPUP_BASE_MENU_ITEM_CLASS (shell_popup_sub_menu_menu_item_parent_class);

  parent_class->set_parent_menu (item, parent);

  if (priv->menu != NULL)
    shell_popup_menu_base_set_parent_menu (SHELL_POPUP_MENU_BASE (priv->menu), parent);
}

static gboolean
shell_popup_sub_menu_menu_item_real_sync_sensitive (ShellPopupBaseMenuItem *item)
{
  ShellPopupSubMenuMenuItem *self = SHELL_POPUP_SUB_MENU_MENU_ITEM (item);
  ShellPopupSubMenuMenuItemPrivate *priv =
    shell_popup_sub_menu_menu_item_get_instance_private (self);
  ShellPopupBaseMenuItemClass *parent_class =
    SHELL_POPUP_BASE_MENU_ITEM_CLASS (shell_popup_sub_menu_menu_item_parent_class);
  gboolean sensitive = parent_class->sync_sensitive (item);

  if (priv->triangle != NULL)
    set_actor_visible (priv->triangle, sensitive);

  /* The JS passed `false` as the parameters of close (), which is not
   * `{animate: false}`: the submenu is animated. */
  if (!sensitive && priv->menu != NULL)
    shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (priv->menu),
                                 SHELL_POPUP_ANIMATION_FULL);

  return sensitive;
}

static void
shell_popup_sub_menu_menu_item_real_activate (ShellPopupBaseMenuItem *item,
                                              const ClutterEvent     *event)
{
  ShellPopupSubMenuMenuItem *self = SHELL_POPUP_SUB_MENU_MENU_ITEM (item);
  ShellPopupSubMenuMenuItemPrivate *priv =
    shell_popup_sub_menu_menu_item_get_instance_private (self);

  shell_popup_sub_menu_menu_item_set_submenu_shown (
    self,
    !shell_popup_menu_base_get_is_open (SHELL_POPUP_MENU_BASE (priv->menu)));
}

static void
shell_popup_sub_menu_menu_item_get_property (GObject    *object,
                                             guint       prop_id,
                                             GValue     *value,
                                             GParamSpec *pspec)
{
  ShellPopupSubMenuMenuItem *self = SHELL_POPUP_SUB_MENU_MENU_ITEM (object);
  ShellPopupSubMenuMenuItemPrivate *priv =
    shell_popup_sub_menu_menu_item_get_instance_private (self);

  switch (prop_id)
    {
    case SUB_ITEM_PROP_TEXT:
      g_value_set_string (value, st_label_get_text (priv->label));
      break;

    case SUB_ITEM_PROP_WANT_ICON:
      g_value_set_boolean (value, priv->want_icon);
      break;

    case SUB_ITEM_PROP_MENU:
      g_value_set_object (value, priv->menu);
      break;

    case SUB_ITEM_PROP_LABEL:
      g_value_set_object (value, priv->label);
      break;

    case SUB_ITEM_PROP_ICON:
      g_value_set_object (value, priv->icon);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_sub_menu_menu_item_set_property (GObject      *object,
                                             guint         prop_id,
                                             const GValue *value,
                                             GParamSpec   *pspec)
{
  ShellPopupSubMenuMenuItem *self = SHELL_POPUP_SUB_MENU_MENU_ITEM (object);
  ShellPopupSubMenuMenuItemPrivate *priv =
    shell_popup_sub_menu_menu_item_get_instance_private (self);

  switch (prop_id)
    {
    case SUB_ITEM_PROP_TEXT:
      shell_popup_sub_menu_menu_item_set_text (self, g_value_get_string (value));
      break;

    case SUB_ITEM_PROP_WANT_ICON:
      priv->want_icon = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_sub_menu_menu_item_constructed (GObject *object)
{
  ShellPopupSubMenuMenuItem *self = SHELL_POPUP_SUB_MENU_MENU_ITEM (object);
  ShellPopupSubMenuMenuItemPrivate *priv =
    shell_popup_sub_menu_menu_item_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *expander;
  ClutterActor *triangle_bin;

  G_OBJECT_CLASS (shell_popup_sub_menu_menu_item_parent_class)->constructed (object);

  st_widget_add_style_class_name (ST_WIDGET (self), "popup-submenu-menu-item");

  if (priv->want_icon)
    {
      priv->icon = g_object_new (ST_TYPE_ICON,
                                 "style-class", "popup-menu-icon",
                                 NULL);
      g_object_add_weak_pointer (G_OBJECT (priv->icon), (gpointer *) &priv->icon);
      clutter_actor_add_child (actor, CLUTTER_ACTOR (priv->icon));
    }

  priv->label = item_label_new ();
  g_object_add_weak_pointer (G_OBJECT (priv->label), (gpointer *) &priv->label);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (priv->label));
  st_widget_set_label_actor (ST_WIDGET (self), CLUTTER_ACTOR (priv->label));

  expander = g_object_new (ST_TYPE_BIN,
                           "style-class", "popup-menu-item-expander",
                           "x-expand", TRUE,
                           NULL);
  clutter_actor_add_child (actor, expander);

  priv->triangle = CLUTTER_ACTOR (shell_popup_menu_arrow_icon (ST_SIDE_RIGHT));
  g_object_add_weak_pointer (G_OBJECT (priv->triangle), (gpointer *) &priv->triangle);
  clutter_actor_set_pivot_point (priv->triangle, 0.5, 0.6);

  triangle_bin = g_object_new (ST_TYPE_WIDGET,
                               "y-expand", TRUE,
                               "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                               NULL);
  clutter_actor_add_child (triangle_bin, priv->triangle);

  clutter_actor_add_child (actor, triangle_bin);
  clutter_actor_add_accessible_state (actor, ATK_STATE_EXPANDABLE);

  priv->menu = g_object_new (SHELL_TYPE_POPUP_SUB_MENU,
                             "source-actor", self,
                             "source-arrow", priv->triangle,
                             NULL);
  g_signal_connect_object (priv->menu, "open-state-changed",
                           G_CALLBACK (sub_item_open_state_changed_cb), self, 0);
  g_signal_connect (self, "destroy",
                    G_CALLBACK (sub_item_destroyed_cb), self);
}

static void
shell_popup_sub_menu_menu_item_dispose (GObject *object)
{
  ShellPopupSubMenuMenuItem *self = SHELL_POPUP_SUB_MENU_MENU_ITEM (object);
  ShellPopupSubMenuMenuItemPrivate *priv =
    shell_popup_sub_menu_menu_item_get_instance_private (self);

  g_clear_weak_pointer (&priv->label);
  g_clear_weak_pointer (&priv->icon);
  g_clear_weak_pointer (&priv->triangle);

  /* The parent's dispose emits ::destroy, whose handler (sub_item_destroyed_cb)
   * destroys the submenu and so its items: the menu must still be there. */
  G_OBJECT_CLASS (shell_popup_sub_menu_menu_item_parent_class)->dispose (object);

  g_clear_object (&priv->menu);
}

static void
shell_popup_sub_menu_menu_item_class_init (ShellPopupSubMenuMenuItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupBaseMenuItemClass *item_class = SHELL_POPUP_BASE_MENU_ITEM_CLASS (klass);
  ClutterBindingPool *binding_pool;

  object_class->constructed = shell_popup_sub_menu_menu_item_constructed;
  object_class->dispose = shell_popup_sub_menu_menu_item_dispose;
  object_class->get_property = shell_popup_sub_menu_menu_item_get_property;
  object_class->set_property = shell_popup_sub_menu_menu_item_set_property;

  item_class->activate = shell_popup_sub_menu_menu_item_real_activate;
  item_class->sync_sensitive = shell_popup_sub_menu_menu_item_real_sync_sensitive;
  item_class->set_parent_menu = shell_popup_sub_menu_menu_item_real_set_parent_menu;

  /**
   * ShellPopupSubMenuMenuItem:text:
   *
   * The text of the label of the item.
   */
  sub_item_props[SUB_ITEM_PROP_TEXT] =
    g_param_spec_string ("text", NULL, NULL,
                         "",
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupSubMenuMenuItem:want-icon:
   *
   * Whether the item has an icon before its label (the `wantIcon`
   * constructor argument).
   */
  sub_item_props[SUB_ITEM_PROP_WANT_ICON] =
    g_param_spec_boolean ("want-icon", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupSubMenuMenuItem:menu:
   *
   * The submenu which the item opens.
   */
  sub_item_props[SUB_ITEM_PROP_MENU] =
    g_param_spec_object ("menu", NULL, NULL,
                         SHELL_TYPE_POPUP_SUB_MENU,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupSubMenuMenuItem:label:
   *
   * The label of the item (`label`).
   */
  sub_item_props[SUB_ITEM_PROP_LABEL] =
    g_param_spec_object ("label", NULL, NULL,
                         ST_TYPE_LABEL,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupSubMenuMenuItem:icon:
   *
   * The icon of the item, if #ShellPopupSubMenuMenuItem:want-icon is set
   * (`icon`).
   */
  sub_item_props[SUB_ITEM_PROP_ICON] =
    g_param_spec_object ("icon", NULL, NULL,
                         ST_TYPE_ICON,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_SUB_ITEM_PROPS, sub_item_props);

  binding_pool = clutter_binding_pool_get_for_class (klass);
  clutter_binding_pool_install_action (binding_pool, "open",
                                       CLUTTER_KEY_Right, 0,
                                       G_CALLBACK (sub_item_open_binding_cb),
                                       NULL, NULL);
}

static void
shell_popup_sub_menu_menu_item_init (ShellPopupSubMenuMenuItem *self)
{
}

/**
 * shell_popup_sub_menu_menu_item_get_menu:
 * @self: a #ShellPopupSubMenuMenuItem
 *
 * Returns: (transfer none): the submenu of the item
 */
ShellPopupSubMenu *
shell_popup_sub_menu_menu_item_get_menu (ShellPopupSubMenuMenuItem *self)
{
  ShellPopupSubMenuMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_SUB_MENU_MENU_ITEM (self), NULL);

  priv = shell_popup_sub_menu_menu_item_get_instance_private (self);
  return priv->menu;
}

/**
 * shell_popup_sub_menu_menu_item_get_label:
 * @self: a #ShellPopupSubMenuMenuItem
 *
 * Returns: (transfer none): the label of the item
 */
StLabel *
shell_popup_sub_menu_menu_item_get_label (ShellPopupSubMenuMenuItem *self)
{
  ShellPopupSubMenuMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_SUB_MENU_MENU_ITEM (self), NULL);

  priv = shell_popup_sub_menu_menu_item_get_instance_private (self);
  return priv->label;
}

/**
 * shell_popup_sub_menu_menu_item_get_icon:
 * @self: a #ShellPopupSubMenuMenuItem
 *
 * Returns: (transfer none) (nullable): the icon of the item, if it has one
 */
StIcon *
shell_popup_sub_menu_menu_item_get_icon (ShellPopupSubMenuMenuItem *self)
{
  ShellPopupSubMenuMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_SUB_MENU_MENU_ITEM (self), NULL);

  priv = shell_popup_sub_menu_menu_item_get_instance_private (self);
  return priv->icon;
}

/**
 * shell_popup_sub_menu_menu_item_get_text:
 * @self: a #ShellPopupSubMenuMenuItem
 *
 * Returns: (transfer none): the text of the label of the item
 */
const char *
shell_popup_sub_menu_menu_item_get_text (ShellPopupSubMenuMenuItem *self)
{
  ShellPopupSubMenuMenuItemPrivate *priv;

  g_return_val_if_fail (SHELL_IS_POPUP_SUB_MENU_MENU_ITEM (self), NULL);

  priv = shell_popup_sub_menu_menu_item_get_instance_private (self);
  return priv->label != NULL ? st_label_get_text (priv->label) : "";
}

/**
 * shell_popup_sub_menu_menu_item_set_text:
 * @self: a #ShellPopupSubMenuMenuItem
 * @text: (nullable): the new text of the label of the item
 *
 * Sets #ShellPopupSubMenuMenuItem:text.
 */
void
shell_popup_sub_menu_menu_item_set_text (ShellPopupSubMenuMenuItem *self,
                                         const char                *text)
{
  ShellPopupSubMenuMenuItemPrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_SUB_MENU_MENU_ITEM (self));

  priv = shell_popup_sub_menu_menu_item_get_instance_private (self);
  if (priv->label == NULL)
    return;

  if (g_strcmp0 (st_label_get_text (priv->label), text ? text : "") == 0)
    return;

  st_label_set_text (priv->label, text ? text : "");
  g_object_notify_by_pspec (G_OBJECT (self), sub_item_props[SUB_ITEM_PROP_TEXT]);
}

/**
 * shell_popup_sub_menu_menu_item_set_submenu_shown:
 * @self: a #ShellPopupSubMenuMenuItem
 * @open: whether the submenu is open
 *
 * Opens or closes the submenu of the item (`setSubmenuShown ()`).
 */
void
shell_popup_sub_menu_menu_item_set_submenu_shown (ShellPopupSubMenuMenuItem *self,
                                                  gboolean                   open)
{
  ShellPopupSubMenuMenuItemPrivate *priv;

  g_return_if_fail (SHELL_IS_POPUP_SUB_MENU_MENU_ITEM (self));

  priv = shell_popup_sub_menu_menu_item_get_instance_private (self);
  if (open)
    shell_popup_menu_base_open (SHELL_POPUP_MENU_BASE (priv->menu),
                                SHELL_POPUP_ANIMATION_FULL);
  else
    shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (priv->menu),
                                 SHELL_POPUP_ANIMATION_FULL);
}

/* ======================================================================== */
/* ShellPopupMenuManager                                                    */
/* ======================================================================== */

enum
{
  MANAGER_PROP_0,

  MANAGER_PROP_ACTION_MODE,
  MANAGER_PROP_ACTIVE_MENU,

  N_MANAGER_PROPS
};

static GParamSpec *manager_props[N_MANAGER_PROPS] = { NULL, };

struct _ShellPopupMenuManager
{
  GObject parent;

  ShellActionMode action_mode;

  GPtrArray *menus; /* of GObject, strong */
  GObject *active_menu; /* weak */

  ClutterGrab *grab;
  ClutterKeyController *key_controller;
  ClutterClickGesture *click_gesture;
  ClutterMotionController *motion_controller;

  gulong key_focus_id;
  ClutterStage *key_focus_stage;  /* weak; the stage key_focus_id is on */
};

G_DEFINE_FINAL_TYPE (ShellPopupMenuManager, shell_popup_menu_manager, G_TYPE_OBJECT)

/*
 * The menus of a manager are #ShellPopupMenuBase or #ShellPopupDummyMenu,
 * which have the same interface in the JS: the actor, the source actor, the
 * open state and open () / close ().
 */
static gboolean
is_managed_menu (GObject *menu)
{
  return SHELL_IS_POPUP_MENU_BASE (menu) || SHELL_IS_POPUP_DUMMY_MENU (menu);
}

static ClutterActor *
managed_menu_actor (GObject *menu)
{
  if (SHELL_IS_POPUP_MENU_BASE (menu))
    return shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (menu));

  return shell_popup_dummy_menu_get_actor (SHELL_POPUP_DUMMY_MENU (menu));
}

static ClutterActor *
managed_menu_source_actor (GObject *menu)
{
  if (SHELL_IS_POPUP_MENU_BASE (menu))
    return shell_popup_menu_base_get_source_actor (SHELL_POPUP_MENU_BASE (menu));

  return shell_popup_dummy_menu_get_actor (SHELL_POPUP_DUMMY_MENU (menu));
}

static gboolean
managed_menu_is_open (GObject *menu)
{
  if (SHELL_IS_POPUP_MENU_BASE (menu))
    return shell_popup_menu_base_get_is_open (SHELL_POPUP_MENU_BASE (menu));

  return shell_popup_dummy_menu_get_is_open (SHELL_POPUP_DUMMY_MENU (menu));
}

static void
managed_menu_open (GObject             *menu,
                   ShellPopupAnimation  animate)
{
  if (SHELL_IS_POPUP_MENU_BASE (menu))
    shell_popup_menu_base_open (SHELL_POPUP_MENU_BASE (menu), animate);
  else
    shell_popup_dummy_menu_open (SHELL_POPUP_DUMMY_MENU (menu), animate);
}

static void
managed_menu_close (GObject             *menu,
                    ShellPopupAnimation  animate)
{
  if (SHELL_IS_POPUP_MENU_BASE (menu))
    shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (menu), animate);
  else
    shell_popup_dummy_menu_close (SHELL_POPUP_DUMMY_MENU (menu), animate);
}

static void
manager_pop_modal (ShellPopupMenuManager *self,
                   ClutterGrab           *grab)
{
  g_autoptr (GError) error = NULL;

  if (!shell_main_pop_modal (shell_main_get_default (), grab, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

static void
manager_remove_actions (ShellPopupMenuManager *self,
                        ClutterActor          *actor)
{
  ClutterAction *actions[] = {
    CLUTTER_ACTION (self->key_controller),
    CLUTTER_ACTION (self->click_gesture),
    CLUTTER_ACTION (self->motion_controller),
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (actions); i++)
    {
      GList *list = clutter_actor_get_actions (actor);

      /* The actor may be gone already, and with it its actions */
      if (g_list_find (list, actions[i]) != NULL)
        clutter_actor_remove_action (actor, actions[i]);

      g_list_free (list);
    }
}

static void
manager_disconnect_key_focus (ShellPopupMenuManager *self)
{
  /* Runs from dispose too, possibly after ShellGlobal is gone: only use
   * the stage we connected to, never look it up again. */
  if (self->key_focus_stage != NULL)
    g_clear_signal_handler (&self->key_focus_id, self->key_focus_stage);
  else
    self->key_focus_id = 0;
  g_clear_weak_pointer (&self->key_focus_stage);
}

static GObject *
manager_find_menu_for_source (ShellPopupMenuManager *self,
                              ClutterActor          *source)
{
  while (source != NULL)
    {
      guint i;

      for (i = 0; i < self->menus->len; i++)
        {
          GObject *menu = g_ptr_array_index (self->menus, i);

          if (managed_menu_source_actor (menu) == source)
            return menu;
        }

      source = clutter_actor_get_parent (source);
    }

  return NULL;
}

/* _changeMenu () */
static void
manager_change_menu (ShellPopupMenuManager *self,
                     GObject               *new_menu)
{
  managed_menu_open (new_menu,
                     self->active_menu != NULL ? SHELL_POPUP_ANIMATION_FADE
                                               : SHELL_POPUP_ANIMATION_FULL);
}

static void
manager_key_focus_changed_cb (ClutterStage          *stage,
                              GParamSpec            *pspec,
                              ShellPopupMenuManager *self)
{
  ClutterActor *actor;
  GObject *new_menu;

  if (self->active_menu == NULL)
    return;

  actor = clutter_stage_get_key_focus (stage);
  new_menu = manager_find_menu_for_source (self, actor);

  if (new_menu != NULL)
    manager_change_menu (self, new_menu);
}

static gboolean
manager_key_press_cb (ClutterKeyController  *controller,
                      ShellPopupMenuManager *self)
{
  GObject *menu = self->active_menu;
  ClutterActor *menu_actor;
  uint32_t symbol = 0;

  if (menu == NULL)
    return CLUTTER_EVENT_PROPAGATE;

  menu_actor = managed_menu_actor (menu);
  clutter_key_controller_get_key (controller, &symbol, NULL, NULL);

  if (symbol == CLUTTER_KEY_Down &&
      clutter_stage_get_key_focus (get_stage ()) == menu_actor)
    {
      if (ST_IS_WIDGET (menu_actor))
        st_widget_navigate_focus (ST_WIDGET (menu_actor), NULL,
                                  ST_DIR_TAB_FORWARD, FALSE);
      return CLUTTER_EVENT_STOP;
    }
  else if (symbol == CLUTTER_KEY_Escape && managed_menu_is_open (menu))
    {
      managed_menu_close (menu, SHELL_POPUP_ANIMATION_FULL);
      return CLUTTER_EVENT_STOP;
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static gboolean
manager_click_may_recognize_cb (ClutterGesture        *gesture,
                                ShellPopupMenuManager *self)
{
  const ClutterEvent *event;
  ClutterActor *target_actor;

  if (self->active_menu == NULL)
    return FALSE;

  event = clutter_gesture_get_point_event (gesture, 0);
  if (event == NULL)
    return FALSE;

  target_actor = clutter_stage_get_event_actor (get_stage (), event);
  if (target_actor == NULL)
    return TRUE;

  return !clutter_actor_contains (managed_menu_actor (self->active_menu),
                                  target_actor);
}

static void
manager_click_recognize_cb (ClutterGesture        *gesture,
                            ShellPopupMenuManager *self)
{
  if (self->active_menu != NULL)
    managed_menu_close (self->active_menu, SHELL_POPUP_ANIMATION_FULL);
}

/* _checkHoveredMenu () */
static void
manager_motion_enter_cb (ClutterMotionController *controller,
                         ClutterSprite           *sprite,
                         float                    x,
                         float                    y,
                         ShellPopupMenuManager   *self)
{
  graphene_point_t coords;
  ClutterActor *target_actor;
  GObject *hovered_menu;

  clutter_sprite_get_coords (sprite, &coords);
  target_actor = clutter_stage_get_actor_at_pos (get_stage (), CLUTTER_PICK_ALL,
                                                 coords.x, coords.y);
  hovered_menu = manager_find_menu_for_source (self, target_actor);

  if (hovered_menu != NULL && hovered_menu != self->active_menu)
    manager_change_menu (self, hovered_menu);
}

/* _onMenuOpenState () */
static void
manager_menu_open_state_changed_cb (GObject               *menu,
                                    gboolean               open,
                                    ShellPopupMenuManager *self)
{
  ShellMain *main = shell_main_get_default ();
  ClutterActor *actor;

  if (open && self->active_menu == menu)
    return;

  actor = managed_menu_actor (menu);

  if (open)
    {
      g_autoptr (GObject) old_menu = self->active_menu != NULL ?
        g_object_ref (self->active_menu) : NULL;
      g_autoptr (ClutterGrab) old_grab = self->grab;

      self->grab = g_object_ref (shell_main_push_modal (main, actor,
                                                        self->action_mode));
      g_set_weak_pointer (&self->active_menu, menu);
      g_object_notify_by_pspec (G_OBJECT (self), manager_props[MANAGER_PROP_ACTIVE_MENU]);

      if (old_menu != NULL)
        {
          manager_remove_actions (self, managed_menu_actor (old_menu));
          managed_menu_close (old_menu, SHELL_POPUP_ANIMATION_FADE);
        }

      if (old_grab != NULL)
        manager_pop_modal (self, old_grab);

      clutter_actor_add_action_full (actor, "popup-manager-key-controller",
                                     CLUTTER_PHASE_CAPTURE,
                                     CLUTTER_ACTION (self->key_controller));
      clutter_actor_add_action_full (actor, "popup-manager-click-gesture",
                                     CLUTTER_PHASE_CAPTURE,
                                     CLUTTER_ACTION (self->click_gesture));
      clutter_actor_add_action_full (actor, "popup-manager-motion-controller",
                                     CLUTTER_PHASE_CAPTURE,
                                     CLUTTER_ACTION (self->motion_controller));

      if (self->key_focus_id == 0)
        {
          ClutterStage *stage = get_stage ();

          g_set_weak_pointer (&self->key_focus_stage, stage);
          self->key_focus_id =
            g_signal_connect_object (stage, "notify::key-focus",
                                     G_CALLBACK (manager_key_focus_changed_cb),
                                     self, 0);
        }
    }
  else if (self->active_menu == menu)
    {
      g_autoptr (ClutterGrab) grab = g_steal_pointer (&self->grab);

      g_clear_weak_pointer (&self->active_menu);
      g_object_notify_by_pspec (G_OBJECT (self), manager_props[MANAGER_PROP_ACTIVE_MENU]);

      if (grab != NULL)
        manager_pop_modal (self, grab);

      manager_remove_actions (self, actor);
      manager_disconnect_key_focus (self);
    }
}

static void
manager_menu_destroyed_cb (GObject               *menu,
                           ShellPopupMenuManager *self)
{
  shell_popup_menu_manager_remove_menu (self, menu);
}

static void
shell_popup_menu_manager_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellPopupMenuManager *self = SHELL_POPUP_MENU_MANAGER (object);

  switch (prop_id)
    {
    case MANAGER_PROP_ACTION_MODE:
      g_value_set_flags (value, self->action_mode);
      break;

    case MANAGER_PROP_ACTIVE_MENU:
      g_value_set_object (value, self->active_menu);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_menu_manager_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellPopupMenuManager *self = SHELL_POPUP_MENU_MANAGER (object);

  switch (prop_id)
    {
    case MANAGER_PROP_ACTION_MODE:
      self->action_mode = g_value_get_flags (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_popup_menu_manager_constructed (GObject *object)
{
  ShellPopupMenuManager *self = SHELL_POPUP_MENU_MANAGER (object);

  G_OBJECT_CLASS (shell_popup_menu_manager_parent_class)->constructed (object);

  self->key_controller =
    CLUTTER_KEY_CONTROLLER (g_object_ref_sink (clutter_key_controller_new (NULL)));
  g_signal_connect_object (self->key_controller, "key-press",
                           G_CALLBACK (manager_key_press_cb), self, 0);

  self->click_gesture =
    CLUTTER_CLICK_GESTURE (g_object_ref_sink (clutter_click_gesture_new ()));
  clutter_click_gesture_set_recognize_on_press (self->click_gesture, TRUE);
  g_signal_connect_object (self->click_gesture, "may-recognize",
                           G_CALLBACK (manager_click_may_recognize_cb), self, 0);
  g_signal_connect_object (self->click_gesture, "recognize",
                           G_CALLBACK (manager_click_recognize_cb), self, 0);

  self->motion_controller =
    CLUTTER_MOTION_CONTROLLER (g_object_ref_sink (clutter_motion_controller_new ()));
  g_signal_connect_object (self->motion_controller, "enter",
                           G_CALLBACK (manager_motion_enter_cb), self, 0);
}

static void
shell_popup_menu_manager_dispose (GObject *object)
{
  ShellPopupMenuManager *self = SHELL_POPUP_MENU_MANAGER (object);

  manager_disconnect_key_focus (self);

  if (self->menus != NULL)
    {
      guint i;

      for (i = 0; i < self->menus->len; i++)
        g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->menus, i),
                                              self);
    }

  /* Dropping the grab of the active menu is up to its owner closing it */
  g_clear_weak_pointer (&self->active_menu);
  g_clear_object (&self->grab);
  g_clear_pointer (&self->menus, g_ptr_array_unref);
  g_clear_object (&self->key_controller);
  g_clear_object (&self->click_gesture);
  g_clear_object (&self->motion_controller);

  G_OBJECT_CLASS (shell_popup_menu_manager_parent_class)->dispose (object);
}

static void
shell_popup_menu_manager_class_init (ShellPopupMenuManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_popup_menu_manager_constructed;
  object_class->dispose = shell_popup_menu_manager_dispose;
  object_class->get_property = shell_popup_menu_manager_get_property;
  object_class->set_property = shell_popup_menu_manager_set_property;

  /**
   * ShellPopupMenuManager:action-mode:
   *
   * The #ShellActionMode of the modal pushed while a menu is open (the
   * `actionMode` of the grab params of the former constructor). Defaults to
   * %SHELL_ACTION_MODE_POPUP.
   */
  manager_props[MANAGER_PROP_ACTION_MODE] =
    g_param_spec_flags ("action-mode", NULL, NULL,
                        SHELL_TYPE_ACTION_MODE,
                        SHELL_ACTION_MODE_POPUP,
                        G_PARAM_READWRITE |
                        G_PARAM_CONSTRUCT_ONLY |
                        G_PARAM_STATIC_STRINGS);

  /**
   * ShellPopupMenuManager:active-menu:
   *
   * The menu of the manager which is open, if any (`activeMenu`): a
   * #ShellPopupMenuBase or a #ShellPopupDummyMenu.
   */
  manager_props[MANAGER_PROP_ACTIVE_MENU] =
    g_param_spec_object ("active-menu", NULL, NULL,
                         G_TYPE_OBJECT,
                         G_PARAM_READABLE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_MANAGER_PROPS, manager_props);
}

static void
shell_popup_menu_manager_init (ShellPopupMenuManager *self)
{
  self->action_mode = SHELL_ACTION_MODE_POPUP;
  self->menus = g_ptr_array_new_with_free_func (g_object_unref);
}

/**
 * shell_popup_menu_manager_new:
 * @action_mode: the #ShellActionMode of the modal pushed while a menu is open
 *
 * Creates a manager of menus: it lets one menu be open at a time, grabs the
 * input while it is, and closes it when the user clicks elsewhere or presses
 * Escape. `new PopupMenuManager (owner, {actionMode})` of old; the owner was
 * not used.
 *
 * Returns: (transfer full): a new #ShellPopupMenuManager
 */
ShellPopupMenuManager *
shell_popup_menu_manager_new (ShellActionMode action_mode)
{
  return g_object_new (SHELL_TYPE_POPUP_MENU_MANAGER,
                       "action-mode", action_mode,
                       NULL);
}

static void
manager_insert_menu (ShellPopupMenuManager *self,
                     GObject               *menu,
                     int                    position)
{
  guint i;

  for (i = 0; i < self->menus->len; i++)
    if (g_ptr_array_index (self->menus, i) == menu)
      return;

  g_signal_connect_object (menu, "open-state-changed",
                           G_CALLBACK (manager_menu_open_state_changed_cb),
                           self, 0);
  g_signal_connect_object (menu, "destroy",
                           G_CALLBACK (manager_menu_destroyed_cb), self, 0);

  if (position < 0 || (guint) position >= self->menus->len)
    g_ptr_array_add (self->menus, g_object_ref (menu));
  else
    g_ptr_array_insert (self->menus, position, g_object_ref (menu));
}

/**
 * shell_popup_menu_manager_add_menu:
 * @self: a #ShellPopupMenuManager
 * @menu: (type GObject.Object): a #ShellPopupMenuBase or a
 *   #ShellPopupDummyMenu
 *
 * Adds a menu to the manager. Adding a menu twice does nothing. The menu is
 * removed when it is destroyed.
 */
void
shell_popup_menu_manager_add_menu (ShellPopupMenuManager *self,
                                   GObject               *menu)
{
  g_return_if_fail (SHELL_IS_POPUP_MENU_MANAGER (self));
  g_return_if_fail (is_managed_menu (menu));

  manager_insert_menu (self, menu, -1);
}

/**
 * shell_popup_menu_manager_insert_menu:
 * @self: a #ShellPopupMenuManager
 * @menu: (type GObject.Object): a #ShellPopupMenuBase or a
 *   #ShellPopupDummyMenu
 * @position: the index to insert the menu at, among the menus of the manager
 *
 * Like shell_popup_menu_manager_add_menu (), but at a given position
 * (`addMenu (menu, position)`). The order of the menus is not used for
 * anything else.
 */
void
shell_popup_menu_manager_insert_menu (ShellPopupMenuManager *self,
                                      GObject               *menu,
                                      int                    position)
{
  g_return_if_fail (SHELL_IS_POPUP_MENU_MANAGER (self));
  g_return_if_fail (is_managed_menu (menu));

  manager_insert_menu (self, menu, position);
}

/**
 * shell_popup_menu_manager_remove_menu:
 * @self: a #ShellPopupMenuManager
 * @menu: (type GObject.Object): a #ShellPopupMenuBase or a
 *   #ShellPopupDummyMenu
 *
 * Removes a menu from the manager. If the menu is the open one, its grab is
 * dropped.
 */
void
shell_popup_menu_manager_remove_menu (ShellPopupMenuManager *self,
                                      GObject               *menu)
{
  g_autoptr (ShellPopupMenuManager) keep_alive = NULL;
  g_autoptr (GObject) menu_ref = NULL;
  guint position;

  g_return_if_fail (SHELL_IS_POPUP_MENU_MANAGER (self));
  g_return_if_fail (is_managed_menu (menu));

  keep_alive = g_object_ref (self);
  menu_ref = g_object_ref (menu);

  if (menu == self->active_menu)
    {
      g_autoptr (ClutterGrab) grab = g_steal_pointer (&self->grab);
      ClutterActor *actor = managed_menu_actor (menu);

      g_clear_weak_pointer (&self->active_menu);
      g_object_notify_by_pspec (G_OBJECT (self), manager_props[MANAGER_PROP_ACTIVE_MENU]);

      if (grab != NULL)
        manager_pop_modal (self, grab);

      if (actor != NULL)
        manager_remove_actions (self, actor);
      manager_disconnect_key_focus (self);
    }

  if (!g_ptr_array_find (self->menus, menu, &position))
    return; /* not a menu we manage */

  g_signal_handlers_disconnect_by_data (menu, self);
  g_ptr_array_remove_index (self->menus, position);
}

/**
 * shell_popup_menu_manager_get_active_menu:
 * @self: a #ShellPopupMenuManager
 *
 * Returns: (transfer none) (nullable) (type GObject.Object): the open menu,
 *   a #ShellPopupMenuBase or a #ShellPopupDummyMenu
 */
GObject *
shell_popup_menu_manager_get_active_menu (ShellPopupMenuManager *self)
{
  g_return_val_if_fail (SHELL_IS_POPUP_MENU_MANAGER (self), NULL);

  return self->active_menu;
}

/**
 * shell_popup_menu_manager_get_action_mode:
 * @self: a #ShellPopupMenuManager
 *
 * Returns: the #ShellActionMode of the modal pushed while a menu is open
 */
ShellActionMode
shell_popup_menu_manager_get_action_mode (ShellPopupMenuManager *self)
{
  g_return_val_if_fail (SHELL_IS_POPUP_MENU_MANAGER (self), SHELL_ACTION_MODE_NONE);

  return self->action_mode;
}
