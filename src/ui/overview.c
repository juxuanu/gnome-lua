/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * overview.c: The overview and the state machine that shows and hides it.
 * Port of js/ui/overview.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `new Overview ()` and `init ()` are one construction: #ShellOverview is
 *    created with the objects it used to reach through `Main` (the window
 *    manager, the layout manager, the XDND handler, the Ctrl+Alt+Tab manager
 *    and the session mode), and creates its actor when it is constructed, or
 *    when the session mode gets an overview.
 *  - The bridges in ShellMain and overviewControls.js are gone: the overview
 *    connects to #ShellWindowManager::overview-hide-requested and
 *    ::item-drag-begin and its siblings, to #ShellLayoutManager::hot-corner-activated,
 *    ::startup-animation-requested and ::monitors-changed, and to the XDND
 *    handler. It sets #ShellWindowManager:overview-visible, :overview-closing
 *    and :overview-animating and #ShellPanel:overview-visible itself.
 *  - `OverviewActor` is a plain #StBoxLayout named "overview"; the controls
 *    manager is kept in a field instead of being reached through the actor.
 *  - The `windows-restacked` and `scroll-event` signals are gone, the
 *    workspaces follow the display and the overview group themselves.
 *  - `show ()` takes no state: shell_overview_show () shows the window
 *    picker, shell_overview_show_apps () the application grid and
 *    shell_overview_show_for_reason () any state, with the reason that is
 *    given to the hooks. `dashIconSize` and `getShowAppsButton ()`, which
 *    only logged that they were deprecated, are gone. `runStartupAnimation ()`
 *    is run by the layout manager's startup-animation-requested signal.
 *  - The hooks of the overview (docs/api/hooks.md) are declared here. The
 *    "before" phases of "overview.showing" and "overview.hiding" are emitted
 *    by show and hide requests, not by gestures that end by hiding.
 */

#include "config.h"

#include <glib/gi18n-lib.h>
#include <meta/meta-workspace-manager.h>

#include "shell-global.h"
#include "shell-hooks.h"
#include "ui/dnd.h"
#include "ui/main.h"
#include "ui/overview-controls.h"
#include "ui/overview.h"
#include "ui/swipe-tracker.h"
#include "ui/window-preview.h"

#define SHELL_KEYBINDINGS_SCHEMA "org.gnome.shell.keybindings"

#define DND_WINDOW_SWITCH_TIMEOUT 750

#define OVERVIEW_ACTIVATION_TIMEOUT 0.5

typedef enum
{
  OVERVIEW_STATE_HIDDEN,
  OVERVIEW_STATE_HIDING,
  OVERVIEW_STATE_SHOWING,
  OVERVIEW_STATE_SHOWN,
} OverviewShownState;

enum
{
  PROP_0,

  PROP_WINDOW_MANAGER,
  PROP_LAYOUT_MANAGER,
  PROP_XDND_HANDLER,
  PROP_CTRL_ALT_TAB_MANAGER,
  PROP_SESSION_MODE,
  PROP_VISIBLE,
  PROP_VISIBLE_TARGET,
  PROP_ANIMATION_IN_PROGRESS,
  PROP_CLOSING,
  PROP_IS_DUMMY,
  PROP_DASH,
  PROP_SEARCH_CONTROLLER,
  PROP_SEARCH_ENTRY,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  SHOWING,
  SHOWN,
  HIDING,
  HIDDEN,
  ITEM_DRAG_BEGIN,
  ITEM_DRAG_CANCELLED,
  ITEM_DRAG_END,
  WINDOW_DRAG_BEGIN,
  WINDOW_DRAG_CANCELLED,
  WINDOW_DRAG_END,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellOverview
{
  GObject parent;

  /* Construct properties, held weakly: the owner is ShellMain */
  ShellWindowManager *window_manager;
  ShellLayoutManager *layout_manager;
  ShellXdndHandler *xdnd_handler;
  ShellCtrlAltTabManager *ctrl_alt_tab_manager;
  ShellSessionMode *session_mode;

  gboolean is_dummy;
  gboolean created;

  gboolean visible;                 /* animating to overview, in overview, animating out */
  gboolean shown;                   /* show () and not hide () */
  gboolean modal;                   /* have a modal grab */
  gboolean animation_in_progress;
  gboolean visible_target;
  OverviewShownState shown_state;
  double activation_time;

  const char *show_reason;
  const char *hide_reason;
  const char *show_owner;           /* the extension of reason "extension", interned */
  const char *hide_owner;
  ShellControlsState show_target;

  /* During transitions, we raise this to the top to avoid having the overview
   * area be reactive; it causes too many issues such as double clicks on
   * Dash elements, or mouseover handlers in the workspaces. */
  ClutterActor *cover_pane;         /* a child of the overview group */
  ClutterActor *overview_actor;     /* a child of the overview group */
  ShellControlsManager *controls;   /* a child of the actor */
  ClutterGrab *grab;
  ShellSwipeTracker *swipe_tracker;

  /* XDND */
  guint drag_monitor;
  gboolean in_xdnd_drag;
  gboolean in_item_drag;
  gboolean in_window_drag;

  guint window_switch_timeout_id;
  guint32 window_switch_timestamp;
  int last_active_workspace_index;
  MetaWindow *last_hovered_window;  /* weak */
};

G_DEFINE_FINAL_TYPE (ShellOverview, shell_overview, G_TYPE_OBJECT)

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static GWeakRef *
weak_ref_new (gpointer object)
{
  GWeakRef *ref = g_new0 (GWeakRef, 1);

  g_weak_ref_init (ref, object);

  return ref;
}

static void
weak_ref_free (gpointer data)
{
  GWeakRef *ref = data;

  g_weak_ref_clear (ref);
  g_free (ref);
}

static ShellPanel *
get_panel (void)
{
  return shell_main_get_panel (shell_main_get_default ());
}

static void
set_panel_style (const char *style)
{
  ShellPanel *panel = get_panel ();

  if (panel != NULL)
    st_widget_set_style (ST_WIDGET (panel), style);
}

static double
get_monotonic_seconds (void)
{
  return (double) g_get_monotonic_time () / G_USEC_PER_SEC;
}

static const char *
target_name (ShellControlsState state)
{
  return state == SHELL_CONTROLS_STATE_APP_GRID ? "app-grid" : "window-picker";
}

/* ------------------------------------------------------------------------ */
/* The hooks                                                                 */
/* ------------------------------------------------------------------------ */

/* Number of overview.* hooks being emitted: an extension that asks for the
 * overview from one of them is deferred (docs/api/overview.md) */
static guint hook_emit_depth = 0;

static gboolean
hooks_declared (const char *name)
{
  return shell_hook_registry_is_declared (shell_hook_registry_get_default (), name);
}

/* Whether the reason is one whose hiding an extension may cancel */
static gboolean
hide_reason_is_cancellable (const char *reason)
{
  static const char * const cancellable[] = {
    "hot-corner", "activities-button", "overlay-key", "keybinding", "gesture", NULL
  };

  return g_strv_contains (cancellable, reason);
}

/* Emits the "before" phase of overview.showing. Returns FALSE if an extension
 * cancelled the showing. @state may be amended */
static gboolean
emit_showing_before (ShellControlsState *state,
                     const char         *reason,
                     const char         *owner)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  g_autoptr (GVariant) amendments = NULL;
  GVariantBuilder builder;
  ShellHookVerdict verdict;

  if (!hooks_declared ("overview.showing"))
    return TRUE;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "reason", g_variant_new_string (reason));
  g_variant_builder_add (&builder, "{sv}", "target", g_variant_new_string (target_name (*state)));
  if (owner != NULL)
    g_variant_builder_add (&builder, "{sv}", "owner", g_variant_new_string (owner));

  hook_emit_depth++;
  verdict = shell_hook_registry_emit_before (registry, "overview.showing",
                                             g_variant_builder_end (&builder), &amendments);
  hook_emit_depth--;

  if (verdict == SHELL_HOOK_VERDICT_CANCEL && g_strcmp0 (reason, "dbus") != 0)
    return FALSE;

  if (amendments != NULL)
    {
      const char *target = NULL;

      if (g_variant_lookup (amendments, "target", "&s", &target))
        {
          if (g_strcmp0 (target, "window-picker") == 0)
            *state = SHELL_CONTROLS_STATE_WINDOW_PICKER;
          else if (g_strcmp0 (target, "app-grid") == 0)
            *state = SHELL_CONTROLS_STATE_APP_GRID;
        }
    }

  return TRUE;
}

/* The "before" phase of overview.hiding. Returns FALSE if an extension
 * cancelled the hiding and it may do so */
static gboolean
emit_hiding_before (const char *reason,
                    const char *owner)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  GVariantBuilder builder;
  ShellHookVerdict verdict;

  if (!hooks_declared ("overview.hiding"))
    return TRUE;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "reason", g_variant_new_string (reason));
  if (owner != NULL)
    g_variant_builder_add (&builder, "{sv}", "owner", g_variant_new_string (owner));

  hook_emit_depth++;
  verdict = shell_hook_registry_emit_before (registry, "overview.hiding",
                                             g_variant_builder_end (&builder), NULL);
  hook_emit_depth--;
  if (verdict != SHELL_HOOK_VERDICT_CANCEL)
    return TRUE;

  if (hide_reason_is_cancellable (reason))
    return FALSE;

  g_message ("An extension tried to cancel the hiding of the overview (%s), which is not allowed",
             reason);

  return TRUE;
}

static void
emit_after_hook (ShellOverview *self,
                 const char    *name)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  GVariantBuilder builder;

  if (!hooks_declared (name))
    return;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (g_str_equal (name, "overview.showing"))
    {
      g_variant_builder_add (&builder, "{sv}", "reason", g_variant_new_string (self->show_reason));
      g_variant_builder_add (&builder, "{sv}", "target",
                             g_variant_new_string (target_name (self->show_target)));
      if (g_str_equal (self->show_reason, "extension") && self->show_owner != NULL)
        g_variant_builder_add (&builder, "{sv}", "owner", g_variant_new_string (self->show_owner));
    }
  else if (g_str_equal (name, "overview.shown"))
    {
      g_variant_builder_add (&builder, "{sv}", "target",
                             g_variant_new_string (target_name (self->show_target)));
    }
  else if (g_str_equal (name, "overview.hiding"))
    {
      g_variant_builder_add (&builder, "{sv}", "reason", g_variant_new_string (self->hide_reason));
      if (g_str_equal (self->hide_reason, "extension") && self->hide_owner != NULL)
        g_variant_builder_add (&builder, "{sv}", "owner", g_variant_new_string (self->hide_owner));
    }

  hook_emit_depth++;
  shell_hook_registry_emit_after (registry, name, g_variant_builder_end (&builder));
  hook_emit_depth--;
}

/* ------------------------------------------------------------------------ */
/* The state                                                                 */
/* ------------------------------------------------------------------------ */

static void
set_visible (ShellOverview *self,
             gboolean       visible)
{
  if (self->visible == visible)
    return;

  self->visible = visible;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_VISIBLE]);
}

/* The workspaces (Shell.Workspace) read the state of the animation from
 * the window manager */
static void
set_animation_in_progress (ShellOverview *self,
                           gboolean       animating)
{
  if (self->animation_in_progress != animating)
    {
      self->animation_in_progress = animating;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ANIMATION_IN_PROGRESS]);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CLOSING]);
    }

  if (self->window_manager != NULL)
    shell_window_manager_set_overview_animating (self->window_manager, animating);
}

static gboolean
change_shown_state (ShellOverview      *self,
                    OverviewShownState  state)
{
  static const struct
  {
    int signal;
    OverviewShownState allowed[2];
    const char *hook;
  } transitions[] = {
    [OVERVIEW_STATE_HIDDEN]  = { HIDDEN,  { OVERVIEW_STATE_SHOWING, OVERVIEW_STATE_SHOWING }, "overview.hidden" },
    [OVERVIEW_STATE_HIDING]  = { HIDING,  { OVERVIEW_STATE_HIDDEN, OVERVIEW_STATE_SHOWING }, "overview.hiding" },
    [OVERVIEW_STATE_SHOWING] = { SHOWING, { OVERVIEW_STATE_SHOWN, OVERVIEW_STATE_HIDING }, "overview.showing" },
    [OVERVIEW_STATE_SHOWN]   = { SHOWN,   { OVERVIEW_STATE_HIDING, OVERVIEW_STATE_HIDING }, "overview.shown" },
  };
  MetaCompositor *compositor = shell_global_get_compositor (shell_global_get ());
  ShellPanel *panel = get_panel ();

  if (state != transitions[self->shown_state].allowed[0] &&
      state != transitions[self->shown_state].allowed[1])
    {
      g_warning ("Invalid overview shown transition from %d to %d", self->shown_state, state);
      return FALSE;
    }

  if (self->shown_state == OVERVIEW_STATE_HIDDEN)
    meta_compositor_disable_unredirect (compositor);
  else if (state == OVERVIEW_STATE_HIDDEN)
    meta_compositor_enable_unredirect (compositor);

  self->shown_state = state;

  /* The window manager, the workspaces and the panel follow the state */
  if (self->window_manager != NULL)
    {
      switch (state)
        {
        case OVERVIEW_STATE_SHOWING:
          shell_window_manager_set_overview_closing (self->window_manager, FALSE);
          shell_window_manager_set_overview_visible (self->window_manager, TRUE);
          break;
        case OVERVIEW_STATE_SHOWN:
          shell_window_manager_set_overview_closing (self->window_manager, FALSE);
          break;
        case OVERVIEW_STATE_HIDING:
          shell_window_manager_set_overview_closing (self->window_manager, TRUE);
          break;
        case OVERVIEW_STATE_HIDDEN:
          shell_window_manager_set_overview_closing (self->window_manager, FALSE);
          shell_window_manager_set_overview_visible (self->window_manager, FALSE);
          break;
        default:
          break;
        }
    }
  if (panel != NULL)
    {
      if (state == OVERVIEW_STATE_SHOWING)
        shell_panel_set_overview_visible (panel, TRUE);
      else if (state == OVERVIEW_STATE_HIDING)
        shell_panel_set_overview_visible (panel, FALSE);
    }

  g_signal_emit (self, signals[transitions[state].signal], 0);
  emit_after_hook (self, transitions[state].hook);

  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* The grab                                                                  */
/* ------------------------------------------------------------------------ */

static gboolean
sync_grab (ShellOverview *self)
{
  /* We delay grab changes during animation so that when removing the
   * overview we don't have a problem with the release of a press/release
   * going to an application. */
  if (self->animation_in_progress)
    return TRUE;

  if (self->shown)
    {
      gboolean should_be_modal = !self->in_xdnd_drag;

      if (should_be_modal && !self->modal)
        {
          ShellGlobal *global = shell_global_get ();
          ClutterGrab *grab;

          if (meta_display_is_grabbed (shell_global_get_display (global)))
            {
              shell_overview_hide (self);
              return FALSE;
            }

          grab = shell_main_push_modal (shell_main_get_default (),
                                        CLUTTER_ACTOR (shell_global_get_stage (global)),
                                        SHELL_ACTION_MODE_OVERVIEW);
          g_set_object (&self->grab, grab);
          self->modal = TRUE;
        }
    }
  else
    {
      if (self->modal)
        {
          g_autoptr (GError) error = NULL;
          g_autoptr (ClutterGrab) grab = g_steal_pointer (&self->grab);

          if (!shell_main_pop_modal (shell_main_get_default (), grab, &error))
            g_warning ("%s: %s", G_STRFUNC, error->message);
          self->modal = FALSE;
        }
    }

  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Showing and hiding                                                        */
/* ------------------------------------------------------------------------ */

static void animate_not_visible (ShellOverview *self);

static void
raise_cover_pane (ShellOverview *self)
{
  clutter_actor_set_child_above_sibling (CLUTTER_ACTOR (shell_layout_manager_get_overview_group (self->layout_manager)),
                                         self->cover_pane, NULL);
  clutter_actor_show (self->cover_pane);
}

static void
show_done (ShellOverview *self)
{
  set_animation_in_progress (self, FALSE);
  clutter_actor_hide (self->cover_pane);

  if (self->shown_state != OVERVIEW_STATE_SHOWN)
    change_shown_state (self, OVERVIEW_STATE_SHOWN);

  /* Handle any calls to hide* while we were showing */
  if (!self->shown)
    animate_not_visible (self);

  sync_grab (self);
}

static void
animate_to_overview_done_cb (gboolean finished,
                             gpointer user_data)
{
  g_autoptr (ShellOverview) self = g_weak_ref_get (user_data);

  if (self != NULL)
    show_done (self);
}

static void
animate_visible (ShellOverview      *self,
                 ShellControlsState  state)
{
  if (self->visible || self->animation_in_progress)
    return;

  set_visible (self, TRUE);
  set_animation_in_progress (self, TRUE);
  self->visible_target = TRUE;
  self->activation_time = get_monotonic_seconds ();
  self->show_target = state;

  raise_cover_pane (self);

  shell_controls_manager_prepare_to_enter_overview (self->controls);
  change_shown_state (self, OVERVIEW_STATE_SHOWING);
  shell_controls_manager_animate_to_overview (self->controls, state,
                                              animate_to_overview_done_cb,
                                              weak_ref_new (self), weak_ref_free);
}

static void
hide_done (ShellOverview *self)
{
  clutter_actor_hide (self->cover_pane);

  set_visible (self, FALSE);
  set_animation_in_progress (self, FALSE);

  /* Handle any calls to show* while we were hiding */
  if (self->shown)
    {
      change_shown_state (self, OVERVIEW_STATE_HIDDEN);
      animate_visible (self, SHELL_CONTROLS_STATE_WINDOW_PICKER);
    }
  else
    {
      shell_layout_manager_hide_overview (self->layout_manager);
      change_shown_state (self, OVERVIEW_STATE_HIDDEN);
    }

  set_panel_style (NULL);

  sync_grab (self);
}

static void
animate_from_overview_done_cb (gboolean finished,
                               gpointer user_data)
{
  g_autoptr (ShellOverview) self = g_weak_ref_get (user_data);

  if (self != NULL)
    hide_done (self);
}

static void
animate_not_visible (ShellOverview *self)
{
  if (!self->visible || self->animation_in_progress)
    return;

  set_animation_in_progress (self, TRUE);
  self->visible_target = FALSE;

  raise_cover_pane (self);

  shell_controls_manager_prepare_to_leave_overview (self->controls);
  change_shown_state (self, OVERVIEW_STATE_HIDING);
  shell_controls_manager_animate_from_overview (self->controls,
                                                animate_from_overview_done_cb,
                                                weak_ref_new (self), weak_ref_free);
}

static void
overview_show (ShellOverview      *self,
               ShellControlsState  state,
               const char         *reason,
               const char         *owner)
{
  if (self->is_dummy || self->controls == NULL)
    return;
  if (self->shown)
    return;

  if (!emit_showing_before (&state, reason, owner))
    return;

  self->show_reason = g_intern_string (reason);
  self->show_owner = owner != NULL ? g_intern_string (owner) : NULL;
  self->shown = TRUE;

  if (!sync_grab (self))
    return;

  shell_layout_manager_show_overview (self->layout_manager);
  animate_visible (self, state);
}

/**
 * shell_overview_show_for_reason:
 * @self: a #ShellOverview
 * @state: the state to show, not %SHELL_CONTROLS_STATE_HIDDEN
 * @reason: why the overview is shown, one of the values of the `reason` key
 *   of the "overview.showing" hook (docs/api/hooks.md), or `other`
 *
 * Animates the overview visible and grabs mouse and keyboard input (`show ()`).
 * An extension can cancel it by the "overview.showing" hook.
 */
void
shell_overview_show_for_reason (ShellOverview      *self,
                                ShellControlsState  state,
                                const char         *reason)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (self));
  g_return_if_fail (state != SHELL_CONTROLS_STATE_HIDDEN);
  g_return_if_fail (reason != NULL);

  overview_show (self, state, reason, NULL);
}

/**
 * shell_overview_show_for_extension:
 * @self: a #ShellOverview
 * @state: the state to show, not %SHELL_CONTROLS_STATE_HIDDEN
 * @owner: the uuid of the extension that asks
 *
 * As shell_overview_show_for_reason () with the reason `extension`: the
 * "overview.showing" hooks carry @owner in their `owner` key
 * (docs/api/overview.md).
 */
void
shell_overview_show_for_extension (ShellOverview      *self,
                                   ShellControlsState  state,
                                   const char         *owner)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (self));
  g_return_if_fail (state != SHELL_CONTROLS_STATE_HIDDEN);
  g_return_if_fail (owner != NULL);

  overview_show (self, state, "extension", owner);
}

/**
 * shell_overview_show:
 * @self: a #ShellOverview
 *
 * Animates the overview visible with the window picker, and grabs mouse and
 * keyboard input (`show ()`).
 */
void
shell_overview_show (ShellOverview *self)
{
  shell_overview_show_for_reason (self, SHELL_CONTROLS_STATE_WINDOW_PICKER, "other");
}

/**
 * shell_overview_show_apps:
 * @self: a #ShellOverview
 *
 * Shows the overview with the application grid (`showApps ()`).
 */
void
shell_overview_show_apps (ShellOverview *self)
{
  shell_overview_show_for_reason (self, SHELL_CONTROLS_STATE_APP_GRID, "other");
}

static void
overview_hide (ShellOverview *self,
               const char    *reason,
               const char    *owner)
{
  const ClutterEvent *event;

  if (self->is_dummy || self->controls == NULL)
    return;

  if (!self->shown)
    return;

  event = clutter_get_current_event ();
  if (event != NULL)
    {
      ClutterEventType type = clutter_event_type (event);
      gboolean button = type == CLUTTER_BUTTON_PRESS || type == CLUTTER_BUTTON_RELEASE;
      gboolean ctrl = (clutter_event_get_state (event) & CLUTTER_CONTROL_MASK) != 0;

      if (button && ctrl)
        return;
    }

  if (!emit_hiding_before (reason, owner))
    return;

  self->hide_reason = g_intern_string (reason);
  self->hide_owner = owner != NULL ? g_intern_string (owner) : NULL;
  self->shown = FALSE;

  animate_not_visible (self);
  sync_grab (self);
}

/**
 * shell_overview_hide_for_reason:
 * @self: a #ShellOverview
 * @reason: why the overview is hidden, one of the values of the `reason` key
 *   of the "overview.hiding" hook (docs/api/hooks.md), or `other`
 *
 * Reverses the effect of shell_overview_show () (`hide ()`). An extension
 * can cancel it by the "overview.hiding" hook for some reasons.
 */
void
shell_overview_hide_for_reason (ShellOverview *self,
                                const char    *reason)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (self));
  g_return_if_fail (reason != NULL);

  overview_hide (self, reason, NULL);
}

/**
 * shell_overview_hide_for_extension:
 * @self: a #ShellOverview
 * @owner: the uuid of the extension that asks
 *
 * As shell_overview_hide_for_reason () with the reason `extension`: the
 * "overview.hiding" hooks carry @owner in their `owner` key, and an
 * extension cannot cancel it (docs/api/overview.md).
 */
void
shell_overview_hide_for_extension (ShellOverview *self,
                                   const char    *owner)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (self));
  g_return_if_fail (owner != NULL);

  overview_hide (self, "extension", owner);
}

/**
 * shell_overview_hide:
 * @self: a #ShellOverview
 *
 * Reverses the effect of shell_overview_show () (`hide ()`).
 */
void
shell_overview_hide (ShellOverview *self)
{
  shell_overview_hide_for_reason (self, "other");
}

/**
 * shell_overview_toggle_for_reason:
 * @self: a #ShellOverview
 * @reason: why the overview is toggled
 *
 * Shows the overview if it is not visible, hides it otherwise (`toggle ()`).
 */
void
shell_overview_toggle_for_reason (ShellOverview *self,
                                  const char    *reason)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (self));
  g_return_if_fail (reason != NULL);

  if (self->is_dummy)
    return;

  if (self->visible)
    overview_hide (self, reason, NULL);
  else
    overview_show (self, SHELL_CONTROLS_STATE_WINDOW_PICKER, reason, NULL);
}

/**
 * shell_overview_toggle_for_extension:
 * @self: a #ShellOverview
 * @owner: the uuid of the extension that asks
 *
 * As shell_overview_toggle_for_reason () with the reason `extension`.
 */
void
shell_overview_toggle_for_extension (ShellOverview *self,
                                     const char    *owner)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (self));
  g_return_if_fail (owner != NULL);

  if (self->is_dummy)
    return;

  if (self->visible)
    overview_hide (self, "extension", owner);
  else
    overview_show (self, SHELL_CONTROLS_STATE_WINDOW_PICKER, "extension", owner);
}

/**
 * shell_overview_toggle:
 * @self: a #ShellOverview
 *
 * Shows the overview if it is not visible, hides it otherwise (`toggle ()`).
 */
void
shell_overview_toggle (ShellOverview *self)
{
  shell_overview_toggle_for_reason (self, "other");
}

/**
 * shell_overview_focus_search:
 * @self: a #ShellOverview
 *
 * Shows the overview and gives the key focus to the search entry
 * (`focusSearch ()`).
 */
void
shell_overview_focus_search (ShellOverview *self)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (self));

  shell_overview_show_for_reason (self, SHELL_CONTROLS_STATE_WINDOW_PICKER, "dbus");
  if (self->controls != NULL)
    clutter_actor_grab_key_focus (CLUTTER_ACTOR (shell_controls_manager_get_search_entry (self->controls)));
}

/**
 * shell_overview_select_app:
 * @self: a #ShellOverview
 * @id: the id of an application
 *
 * Shows the application grid and selects the application (`selectApp ()`).
 */
void
shell_overview_select_app (ShellOverview *self,
                           const char    *id)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (self));
  g_return_if_fail (id != NULL);

  shell_overview_show_for_reason (self, SHELL_CONTROLS_STATE_APP_GRID, "dbus");
  if (self->controls != NULL)
    shell_base_app_view_select_app (SHELL_BASE_APP_VIEW (shell_controls_manager_get_app_display (self->controls)),
                                    id);
}

/**
 * shell_overview_should_toggle_by_corner_or_button:
 * @self: a #ShellOverview
 *
 * Checks if the Activities button is currently sensitive to clicks. The
 * first call to this function within the 0.5 seconds of the hot corner being
 * triggered will return %FALSE. This avoids opening and closing the overview
 * if the user both triggered the hot corner and clicked the Activities
 * button (`shouldToggleByCornerOrButton ()`).
 *
 * Returns: whether the overview can be toggled by a hot corner or button
 */
gboolean
shell_overview_should_toggle_by_corner_or_button (ShellOverview *self)
{
  g_return_val_if_fail (SHELL_IS_OVERVIEW (self), FALSE);

  if (self->animation_in_progress)
    return FALSE;
  if (self->in_item_drag || self->in_window_drag)
    return FALSE;
  if (self->activation_time == 0 ||
      get_monotonic_seconds () - self->activation_time > OVERVIEW_ACTIVATION_TIMEOUT)
    return TRUE;

  return FALSE;
}

/* ------------------------------------------------------------------------ */
/* Drags                                                                     */
/* ------------------------------------------------------------------------ */

/**
 * shell_overview_begin_item_drag:
 * @self: a #ShellOverview
 * @source: (nullable): the dragged item
 *
 * Tells that the drag of an item of the dash, the application grid or the
 * search results begins (`beginItemDrag ()`).
 */
void
shell_overview_begin_item_drag (ShellOverview *self,
                                GObject       *source)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (self));

  g_signal_emit (self, signals[ITEM_DRAG_BEGIN], 0, source);
  self->in_item_drag = TRUE;
}

/**
 * shell_overview_cancelled_item_drag:
 * @self: a #ShellOverview
 * @source: (nullable): the dragged item
 *
 * Tells that the drag of an item was cancelled (`cancelledItemDrag ()`).
 */
void
shell_overview_cancelled_item_drag (ShellOverview *self,
                                    GObject       *source)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (self));

  g_signal_emit (self, signals[ITEM_DRAG_CANCELLED], 0, source);
}

/**
 * shell_overview_end_item_drag:
 * @self: a #ShellOverview
 * @source: (nullable): the dragged item
 *
 * Tells that the drag of an item ended (`endItemDrag ()`).
 */
void
shell_overview_end_item_drag (ShellOverview *self,
                              GObject       *source)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (self));

  if (!self->in_item_drag)
    return;

  g_signal_emit (self, signals[ITEM_DRAG_END], 0, source);
  self->in_item_drag = FALSE;
}

/**
 * shell_overview_begin_window_drag:
 * @self: a #ShellOverview
 * @window: the dragged window
 *
 * Tells that the drag of the preview of a window begins (`beginWindowDrag ()`).
 */
void
shell_overview_begin_window_drag (ShellOverview *self,
                                  MetaWindow    *window)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (self));

  g_signal_emit (self, signals[WINDOW_DRAG_BEGIN], 0, window);
  self->in_window_drag = TRUE;
}

/**
 * shell_overview_cancelled_window_drag:
 * @self: a #ShellOverview
 * @window: the dragged window
 *
 * Tells that the drag of a window was cancelled (`cancelledWindowDrag ()`).
 */
void
shell_overview_cancelled_window_drag (ShellOverview *self,
                                      MetaWindow    *window)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (self));

  g_signal_emit (self, signals[WINDOW_DRAG_CANCELLED], 0, window);
}

/**
 * shell_overview_end_window_drag:
 * @self: a #ShellOverview
 * @window: the dragged window
 *
 * Tells that the drag of a window ended (`endWindowDrag ()`).
 */
void
shell_overview_end_window_drag (ShellOverview *self,
                                MetaWindow    *window)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (self));

  if (!self->in_window_drag)
    return;

  g_signal_emit (self, signals[WINDOW_DRAG_END], 0, window);
  self->in_window_drag = FALSE;
}

static void
item_drag_begin_cb (ShellWindowManager *wm,
                    GObject            *source,
                    ShellOverview      *self)
{
  shell_overview_begin_item_drag (self, source);
}

static void
item_drag_cancelled_cb (ShellWindowManager *wm,
                        GObject            *source,
                        ShellOverview      *self)
{
  shell_overview_cancelled_item_drag (self, source);
}

static void
item_drag_end_cb (ShellWindowManager *wm,
                  GObject            *source,
                  ShellOverview      *self)
{
  shell_overview_end_item_drag (self, source);
}

static void
overview_hide_requested_cb (ShellWindowManager *wm,
                            ShellOverview      *self)
{
  shell_overview_hide_for_reason (self, "window-activated");
}

/* What Main.activateWindow () does */
static void
activate_window (ShellOverview *self,
                 MetaWindow    *window,
                 guint32        timestamp)
{
  MetaWorkspaceManager *workspace_manager = shell_global_get_workspace_manager (shell_global_get ());
  MetaWorkspace *workspace = meta_window_get_workspace (window);
  ShellPanel *panel = get_panel ();

  if (timestamp == 0)
    timestamp = shell_global_get_current_time (shell_global_get ());

  if (workspace != NULL &&
      meta_workspace_index (workspace) !=
      meta_workspace_manager_get_active_workspace_index (workspace_manager))
    meta_workspace_activate_with_focus (workspace, window, timestamp);
  else
    meta_window_activate (window, timestamp);

  shell_overview_hide_for_reason (self, "window-activated");
  if (panel != NULL)
    shell_panel_close_calendar (panel);
}

typedef struct
{
  GWeakRef *overview;
  MetaWindow *window;
} SwitchTimeoutData;

static void
switch_timeout_data_free (gpointer user_data)
{
  SwitchTimeoutData *data = user_data;

  weak_ref_free (data->overview);
  g_clear_object (&data->window);
  g_free (data);
}

static gboolean
window_switch_timeout_cb (gpointer user_data)
{
  SwitchTimeoutData *data = user_data;
  g_autoptr (ShellOverview) self = g_weak_ref_get (data->overview);

  if (self == NULL)
    return G_SOURCE_REMOVE;

  self->window_switch_timeout_id = 0;
  activate_window (self, data->window, self->window_switch_timestamp);
  g_clear_weak_pointer (&self->last_hovered_window);

  return G_SOURCE_REMOVE;
}

static void
reset_window_switch_timeout (ShellOverview *self)
{
  g_clear_handle_id (&self->window_switch_timeout_id, g_source_remove);
}

static ShellDragMotionResult
drag_motion_cb (ShellDragEvent *event,
                gpointer        user_data)
{
  g_autoptr (ShellOverview) self = g_weak_ref_get (user_data);
  ClutterActor *target = shell_drag_event_get_target_actor (event);
  MetaWindow *hovered_window = NULL;
  gboolean target_is_window;

  if (self == NULL)
    return SHELL_DRAG_MOTION_RESULT_CONTINUE;

  if (target != NULL && SHELL_IS_WINDOW_PREVIEW (target))
    hovered_window = shell_window_preview_get_meta_window (SHELL_WINDOW_PREVIEW (target));
  target_is_window = hovered_window != NULL;

  self->window_switch_timestamp = shell_global_get_current_time (shell_global_get ());

  if (target_is_window && hovered_window == self->last_hovered_window)
    return SHELL_DRAG_MOTION_RESULT_CONTINUE;

  g_clear_weak_pointer (&self->last_hovered_window);

  reset_window_switch_timeout (self);

  if (target_is_window)
    {
      SwitchTimeoutData *data = g_new0 (SwitchTimeoutData, 1);

      g_set_weak_pointer (&self->last_hovered_window, hovered_window);

      data->overview = weak_ref_new (self);
      data->window = g_object_ref (hovered_window);
      self->window_switch_timeout_id =
        g_timeout_add_full (G_PRIORITY_DEFAULT, DND_WINDOW_SWITCH_TIMEOUT,
                            window_switch_timeout_cb, data, switch_timeout_data_free);
      g_source_set_name_by_id (self->window_switch_timeout_id, "[gnome-shell] Main.activateWindow");
    }

  return SHELL_DRAG_MOTION_RESULT_CONTINUE;
}

static void
xdnd_drag_begin_cb (ShellXdndHandler *handler,
                    guint             time,
                    ShellOverview    *self)
{
  MetaWorkspaceManager *workspace_manager = shell_global_get_workspace_manager (shell_global_get ());

  self->in_xdnd_drag = TRUE;

  if (self->drag_monitor == 0)
    {
      self->drag_monitor = shell_dnd_add_drag_monitor (drag_motion_cb, weak_ref_new (self),
                                                       weak_ref_free, NULL, NULL, NULL);
    }

  /* Remember the workspace we started from */
  self->last_active_workspace_index =
    meta_workspace_manager_get_active_workspace_index (workspace_manager);
}

static void
xdnd_drag_end_cb (ShellXdndHandler *handler,
                  ShellOverview    *self)
{
  self->in_xdnd_drag = FALSE;

  /* In case the drag was canceled while in the overview
   * we have to go back to where we started and hide
   * the overview */
  if (self->shown)
    {
      MetaWorkspaceManager *workspace_manager =
        shell_global_get_workspace_manager (shell_global_get ());
      MetaWorkspace *workspace =
        meta_workspace_manager_get_workspace_by_index (workspace_manager,
                                                       self->last_active_workspace_index);

      if (workspace != NULL)
        meta_workspace_activate (workspace, shell_global_get_current_time (shell_global_get ()));
      shell_overview_hide_for_reason (self, "drag");
    }

  reset_window_switch_timeout (self);
  g_clear_weak_pointer (&self->last_hovered_window);
  shell_dnd_remove_drag_monitor (self->drag_monitor);
  self->drag_monitor = 0;
  shell_overview_end_item_drag (self, NULL);
}

/* ------------------------------------------------------------------------ */
/* Gestures                                                                  */
/* ------------------------------------------------------------------------ */

static void
gesture_begin_cb (ShellSwipeTracker *tracker,
                  guint              monitor,
                  ShellOverview     *self)
{
  ShellControlsState state = SHELL_CONTROLS_STATE_WINDOW_PICKER;

  if (self->controls == NULL)
    return;

  /* An extension can keep the overview from being shown by a gesture */
  if (!self->shown && !emit_showing_before (&state, "gesture", NULL))
    return;

  shell_controls_manager_gesture_begin (self->controls, tracker);
}

static void
gesture_update_cb (ShellSwipeTracker *tracker,
                   double             progress,
                   ShellOverview     *self)
{
  if (self->controls == NULL || progress == 0)
    return;

  if (!self->shown)
    {
      self->shown = TRUE;
      set_visible (self, TRUE);
      self->visible_target = TRUE;
      self->show_reason = "gesture";
      self->show_target = SHELL_CONTROLS_STATE_WINDOW_PICKER;

      raise_cover_pane (self);
      change_shown_state (self, OVERVIEW_STATE_SHOWING);

      shell_layout_manager_show_overview (self->layout_manager);
      sync_grab (self);

      set_animation_in_progress (self, TRUE);
    }

  shell_controls_manager_gesture_progress (self->controls, progress);
}

typedef struct
{
  GWeakRef *overview;
  gboolean hide;
} GestureEndData;

static void
gesture_end_data_free (gpointer user_data)
{
  GestureEndData *data = user_data;

  weak_ref_free (data->overview);
  g_free (data);
}

static void
gesture_end_done_cb (gboolean finished,
                     gpointer user_data)
{
  GestureEndData *data = user_data;
  g_autoptr (ShellOverview) self = g_weak_ref_get (data->overview);

  /* onComplete */
  if (self == NULL || !finished)
    return;

  if (data->hide)
    hide_done (self);
  else
    show_done (self);
}

static void
gesture_end_cb (ShellSwipeTracker *tracker,
                guint64            duration,
                double             end_progress,
                ShellOverview     *self)
{
  GestureEndData *data;

  if (self->controls == NULL)
    return;

  data = g_new0 (GestureEndData, 1);
  data->overview = weak_ref_new (self);

  if (end_progress == 0)
    {
      g_autofree char *style = g_strdup_printf ("transition-duration: %" G_GUINT64_FORMAT "ms;",
                                                duration);

      self->shown = FALSE;
      self->visible_target = FALSE;
      self->hide_reason = "gesture";
      change_shown_state (self, OVERVIEW_STATE_HIDING);
      set_panel_style (style);
      data->hide = TRUE;
    }

  shell_controls_manager_gesture_end (self->controls, end_progress, duration,
                                      gesture_end_done_cb, data, gesture_end_data_free);
}

/* ------------------------------------------------------------------------ */
/* The startup animation                                                     */
/* ------------------------------------------------------------------------ */

static void
startup_animation_done_cb (gboolean finished,
                           gpointer user_data)
{
  g_autoptr (ShellOverview) self = g_weak_ref_get (user_data);

  if (self == NULL)
    return;

  /* Overview got hidden during startup animation */
  if (self->shown_state == OVERVIEW_STATE_SHOWING)
    {
      if (!sync_grab (self))
        {
          shell_overview_hide (self);
        }
      else
        {
          set_panel_style (NULL);
          change_shown_state (self, OVERVIEW_STATE_SHOWN);
        }
    }

  if (self->layout_manager != NULL)
    shell_layout_manager_startup_animation_finished (self->layout_manager);
}

static gboolean
startup_animation_requested_cb (ShellLayoutManager *layout_manager,
                                ShellOverview      *self)
{
  if (self->is_dummy || self->controls == NULL)
    return FALSE;

  set_panel_style ("transition-duration: 0ms;");

  self->shown = TRUE;
  set_visible (self, TRUE);
  self->visible_target = TRUE;
  self->show_reason = "other";
  self->show_target = SHELL_CONTROLS_STATE_WINDOW_PICKER;
  shell_layout_manager_show_overview (self->layout_manager);
  /* We should call sync_grab () here, but moved it to happen after
   * the animation because of a race in the xserver where the grab
   * fails when requested very early during startup. */

  change_shown_state (self, OVERVIEW_STATE_SHOWING);

  shell_controls_manager_run_startup_animation (self->controls, startup_animation_done_cb,
                                                weak_ref_new (self), weak_ref_free);

  return TRUE;
}

static gboolean
hot_corner_activated_cb (ShellLayoutManager *layout_manager,
                         int                 monitor_index,
                         gboolean            in_fullscreen,
                         ShellOverview      *self)
{
  if (in_fullscreen && !self->visible)
    return FALSE;

  if (!shell_overview_should_toggle_by_corner_or_button (self))
    return FALSE;

  shell_overview_toggle_for_reason (self, "hot-corner");

  return self->animation_in_progress;
}

/* ------------------------------------------------------------------------ */
/* Creation                                                                  */
/* ------------------------------------------------------------------------ */

static void
relayout (ShellOverview *self)
{
  ShellGlobal *global = shell_global_get ();

  /* To avoid updating the position and size of the workspaces
   * we just hide the overview. The positions will be updated
   * when it is next shown. */
  shell_overview_hide (self);

  if (self->cover_pane != NULL)
    {
      clutter_actor_set_position (self->cover_pane, 0, 0);
      clutter_actor_set_size (self->cover_pane,
                              shell_global_get_screen_width (global),
                              shell_global_get_screen_height (global));
    }
}

static void
monitors_changed_cb (ShellLayoutManager *layout_manager,
                     ShellOverview      *self)
{
  relayout (self);
}

static void
toggle_overview_keybinding_cb (MetaDisplay        *display,
                               MetaWindow         *window,
                               const ClutterEvent *event,
                               MetaKeyBinding     *binding,
                               gpointer            user_data)
{
  g_autoptr (ShellOverview) self = g_weak_ref_get (user_data);

  if (self != NULL)
    shell_overview_toggle_for_reason (self, "keybinding");
}

static void
create_overview (ShellOverview *self)
{
  ShellGlobal *global = shell_global_get ();
  ClutterActor *overview_group =
    CLUTTER_ACTOR (shell_layout_manager_get_overview_group (self->layout_manager));
  g_autoptr (GSettings) settings = NULL;
  ClutterConstraint *constraint;
  ShellControlsManager *controls;
  ShellSwipeTracker *swipe_tracker;

  if (self->created || self->is_dummy)
    return;
  self->created = TRUE;

  self->activation_time = 0;

  self->visible = FALSE;
  self->shown = FALSE;
  self->modal = FALSE;
  self->visible_target = FALSE;
  self->shown_state = OVERVIEW_STATE_HIDDEN;
  set_animation_in_progress (self, FALSE);

  self->cover_pane = g_object_new (CLUTTER_TYPE_ACTOR,
                                   "opacity", 0,
                                   "reactive", TRUE,
                                   NULL);
  clutter_actor_add_child (overview_group, self->cover_pane);
  clutter_actor_hide (self->cover_pane);

  /* XDND */
  self->drag_monitor = 0;
  if (self->xdnd_handler != NULL)
    {
      g_signal_connect_object (self->xdnd_handler, "drag-begin",
                               G_CALLBACK (xdnd_drag_begin_cb), self, 0);
      g_signal_connect_object (self->xdnd_handler, "drag-end",
                               G_CALLBACK (xdnd_drag_end_cb), self, 0);
    }

  self->window_switch_timeout_id = 0;
  self->window_switch_timestamp = 0;
  self->last_active_workspace_index = -1;

  /* The actor of the overview, on the primary monitor */
  self->overview_actor = g_object_new (ST_TYPE_BOX_LAYOUT,
                                       "name", "overview",
                                       /* Translators: This is the main view to select
                                          activities. See also note for "Activities" string. */
                                       "accessible-name", _("Overview"),
                                       "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                       NULL);
  constraint = g_object_new (SHELL_TYPE_MONITOR_CONSTRAINT, "primary", TRUE, NULL);
  clutter_actor_add_constraint (self->overview_actor, constraint);

  controls = shell_controls_manager_new (self, self->window_manager, self->layout_manager,
                                          self->ctrl_alt_tab_manager);
  clutter_actor_add_child (self->overview_actor, CLUTTER_ACTOR (controls));
  g_set_weak_pointer (&self->controls, controls);

  clutter_actor_add_child (overview_group, self->overview_actor);

  g_signal_connect_object (self->layout_manager, "monitors-changed",
                           G_CALLBACK (monitors_changed_cb), self, 0);
  relayout (self);

  settings = g_settings_new (SHELL_KEYBINDINGS_SCHEMA);
  shell_window_manager_add_keybinding (self->window_manager, "toggle-overview", settings,
                                       META_KEY_BINDING_IGNORE_AUTOREPEAT,
                                       SHELL_ACTION_MODE_NORMAL | SHELL_ACTION_MODE_OVERVIEW,
                                       toggle_overview_keybinding_cb,
                                       weak_ref_new (self), weak_ref_free);

  swipe_tracker = g_object_new (SHELL_TYPE_SWIPE_TRACKER,
                                "actor", shell_global_get_stage (global),
                                "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                "allowed-modes", SHELL_ACTION_MODE_NORMAL | SHELL_ACTION_MODE_OVERVIEW,
                                "allow-drag", FALSE,
                                "allow-scroll", FALSE,
                                "name", "Overview swipe tracker",
                                "phase", CLUTTER_PHASE_CAPTURE,
                                NULL);
  g_signal_connect (swipe_tracker, "begin", G_CALLBACK (gesture_begin_cb), self);
  g_signal_connect (swipe_tracker, "update", G_CALLBACK (gesture_update_cb), self);
  g_signal_connect (swipe_tracker, "end", G_CALLBACK (gesture_end_cb), self);
  self->swipe_tracker = swipe_tracker;

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_DASH]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SEARCH_CONTROLLER]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SEARCH_ENTRY]);
}

static void
session_updated (ShellOverview *self)
{
  gboolean has_overview = FALSE;

  if (self->session_mode != NULL)
    g_object_get (self->session_mode, "has-overview", &has_overview, NULL);

  if (!has_overview)
    shell_overview_hide_for_reason (self, "session");

  if (self->is_dummy == !has_overview)
    {
      create_overview (self);
      return;
    }

  self->is_dummy = !has_overview;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_IS_DUMMY]);
  create_overview (self);
}

static void
session_updated_cb (ShellSessionMode *session_mode,
                    ShellOverview    *self)
{
  session_updated (self);
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                   */
/* ------------------------------------------------------------------------ */

static void
shell_overview_constructed (GObject *object)
{
  ShellOverview *self = SHELL_OVERVIEW (object);

  G_OBJECT_CLASS (shell_overview_parent_class)->constructed (object);

  g_return_if_fail (SHELL_IS_WINDOW_MANAGER (self->window_manager));
  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self->layout_manager));
  g_return_if_fail (SHELL_IS_SESSION_MODE (self->session_mode));

  g_signal_connect_object (self->window_manager, "overview-hide-requested",
                           G_CALLBACK (overview_hide_requested_cb), self, 0);
  g_signal_connect_object (self->window_manager, "item-drag-begin",
                           G_CALLBACK (item_drag_begin_cb), self, 0);
  g_signal_connect_object (self->window_manager, "item-drag-cancelled",
                           G_CALLBACK (item_drag_cancelled_cb), self, 0);
  g_signal_connect_object (self->window_manager, "item-drag-end",
                           G_CALLBACK (item_drag_end_cb), self, 0);

  g_signal_connect_object (self->layout_manager, "hot-corner-activated",
                           G_CALLBACK (hot_corner_activated_cb), self, 0);
  g_signal_connect_object (self->layout_manager, "startup-animation-requested",
                           G_CALLBACK (startup_animation_requested_cb), self, 0);

  g_signal_connect_object (self->session_mode, "updated",
                           G_CALLBACK (session_updated_cb), self, 0);

  /* The session mode may already have an overview: the dummy overview is
   * the one of the sessions without */
  self->is_dummy = TRUE;
  session_updated (self);
}

static void
shell_overview_dispose (GObject *object)
{
  ShellOverview *self = SHELL_OVERVIEW (object);

  reset_window_switch_timeout (self);
  if (self->drag_monitor != 0)
    {
      shell_dnd_remove_drag_monitor (self->drag_monitor);
      self->drag_monitor = 0;
    }

  if (self->swipe_tracker != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->swipe_tracker, self);
      shell_swipe_tracker_destroy (self->swipe_tracker);
      g_clear_object (&self->swipe_tracker);
    }

  g_clear_object (&self->grab);
  g_clear_weak_pointer (&self->last_hovered_window);
  g_clear_weak_pointer (&self->controls);
  self->cover_pane = NULL;
  self->overview_actor = NULL;

  g_clear_weak_pointer (&self->window_manager);
  g_clear_weak_pointer (&self->layout_manager);
  g_clear_weak_pointer (&self->xdnd_handler);
  g_clear_weak_pointer (&self->ctrl_alt_tab_manager);
  g_clear_weak_pointer (&self->session_mode);

  G_OBJECT_CLASS (shell_overview_parent_class)->dispose (object);
}

static void
shell_overview_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  ShellOverview *self = SHELL_OVERVIEW (object);

  switch (prop_id)
    {
    case PROP_WINDOW_MANAGER:
      g_value_set_object (value, self->window_manager);
      break;

    case PROP_LAYOUT_MANAGER:
      g_value_set_object (value, self->layout_manager);
      break;

    case PROP_XDND_HANDLER:
      g_value_set_object (value, self->xdnd_handler);
      break;

    case PROP_CTRL_ALT_TAB_MANAGER:
      g_value_set_object (value, self->ctrl_alt_tab_manager);
      break;

    case PROP_SESSION_MODE:
      g_value_set_object (value, self->session_mode);
      break;

    case PROP_VISIBLE:
      g_value_set_boolean (value, self->visible);
      break;

    case PROP_VISIBLE_TARGET:
      g_value_set_boolean (value, self->visible_target);
      break;

    case PROP_ANIMATION_IN_PROGRESS:
      g_value_set_boolean (value, self->animation_in_progress);
      break;

    case PROP_CLOSING:
      g_value_set_boolean (value, shell_overview_get_closing (self));
      break;

    case PROP_IS_DUMMY:
      g_value_set_boolean (value, self->is_dummy);
      break;

    case PROP_DASH:
      g_value_set_object (value, shell_overview_get_dash (self));
      break;

    case PROP_SEARCH_CONTROLLER:
      g_value_set_object (value, shell_overview_get_search_controller (self));
      break;

    case PROP_SEARCH_ENTRY:
      g_value_set_object (value, shell_overview_get_search_entry (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_overview_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  ShellOverview *self = SHELL_OVERVIEW (object);

  switch (prop_id)
    {
    case PROP_WINDOW_MANAGER:
      g_set_weak_pointer (&self->window_manager, g_value_get_object (value));
      break;

    case PROP_LAYOUT_MANAGER:
      g_set_weak_pointer (&self->layout_manager, g_value_get_object (value));
      break;

    case PROP_XDND_HANDLER:
      g_set_weak_pointer (&self->xdnd_handler, g_value_get_object (value));
      break;

    case PROP_CTRL_ALT_TAB_MANAGER:
      g_set_weak_pointer (&self->ctrl_alt_tab_manager, g_value_get_object (value));
      break;

    case PROP_SESSION_MODE:
      g_set_weak_pointer (&self->session_mode, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_overview_class_init (ShellOverviewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellHookRegistry *registry;

  object_class->constructed = shell_overview_constructed;
  object_class->dispose = shell_overview_dispose;
  object_class->get_property = shell_overview_get_property;
  object_class->set_property = shell_overview_set_property;

  /**
   * ShellOverview:window-manager:
   *
   * The window manager (`Main.wm`), whose #ShellWindowManager:overview-visible,
   * :overview-closing and :overview-animating properties follow the overview,
   * and which asks the overview to hide and tells it about the drags of items.
   */
  props[PROP_WINDOW_MANAGER] =
    g_param_spec_object ("window-manager", NULL, NULL,
                         SHELL_TYPE_WINDOW_MANAGER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellOverview:layout-manager:
   *
   * The layout manager (`Main.layoutManager`) the overview is added to.
   */
  props[PROP_LAYOUT_MANAGER] =
    g_param_spec_object ("layout-manager", NULL, NULL,
                         SHELL_TYPE_LAYOUT_MANAGER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellOverview:xdnd-handler:
   *
   * The handler of the drags of X11 clients (`Main.xdndHandler`).
   */
  props[PROP_XDND_HANDLER] =
    g_param_spec_object ("xdnd-handler", NULL, NULL,
                         SHELL_TYPE_XDND_HANDLER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellOverview:ctrl-alt-tab-manager:
   *
   * The manager the application grid, the workspaces and the search entry are
   * added to as keyboard navigation groups (`Main.ctrlAltTabManager`).
   */
  props[PROP_CTRL_ALT_TAB_MANAGER] =
    g_param_spec_object ("ctrl-alt-tab-manager", NULL, NULL,
                         SHELL_TYPE_CTRL_ALT_TAB_MANAGER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellOverview:session-mode:
   *
   * The session mode (`Main.sessionMode`): the overview is a dummy while the
   * mode has no overview.
   */
  props[PROP_SESSION_MODE] =
    g_param_spec_object ("session-mode", NULL, NULL,
                         SHELL_TYPE_SESSION_MODE,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellOverview:visible:
   *
   * Whether the overview is animating in, shown or animating out (`visible`).
   */
  props[PROP_VISIBLE] =
    g_param_spec_boolean ("visible", NULL, NULL, FALSE,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellOverview:visible-target:
   *
   * Whether the overview is visible or animating in (`visibleTarget`).
   */
  props[PROP_VISIBLE_TARGET] =
    g_param_spec_boolean ("visible-target", NULL, NULL, FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellOverview:animation-in-progress:
   *
   * Whether the overview is animating in or out (`animationInProgress`).
   */
  props[PROP_ANIMATION_IN_PROGRESS] =
    g_param_spec_boolean ("animation-in-progress", NULL, NULL, FALSE,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellOverview:closing:
   *
   * Whether the overview is animating out (`closing`).
   */
  props[PROP_CLOSING] =
    g_param_spec_boolean ("closing", NULL, NULL, FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellOverview:is-dummy:
   *
   * Whether the session mode has no overview (`isDummy`).
   */
  props[PROP_IS_DUMMY] =
    g_param_spec_boolean ("is-dummy", NULL, NULL, TRUE,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellOverview:dash:
   *
   * The dash (`dash`), %NULL for a dummy overview.
   */
  props[PROP_DASH] =
    g_param_spec_object ("dash", NULL, NULL,
                         SHELL_TYPE_DASH,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellOverview:search-controller:
   *
   * The controller of the search (`searchController`), %NULL for a dummy
   * overview.
   */
  props[PROP_SEARCH_CONTROLLER] =
    g_param_spec_object ("search-controller", NULL, NULL,
                         SHELL_TYPE_SEARCH_CONTROLLER,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellOverview:search-entry:
   *
   * The entry the user types the search in (`searchEntry`), %NULL for a dummy
   * overview.
   */
  props[PROP_SEARCH_ENTRY] =
    g_param_spec_object ("search-entry", NULL, NULL,
                         ST_TYPE_ENTRY,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellOverview::showing:
   * @self: the #ShellOverview
   *
   * Emitted when the overview starts to show.
   */
  signals[SHOWING] =
    g_signal_new ("showing", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ShellOverview::shown:
   * @self: the #ShellOverview
   *
   * Emitted when the overview is completely shown.
   */
  signals[SHOWN] =
    g_signal_new ("shown", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ShellOverview::hiding:
   * @self: the #ShellOverview
   *
   * Emitted when the overview starts to hide.
   */
  signals[HIDING] =
    g_signal_new ("hiding", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ShellOverview::hidden:
   * @self: the #ShellOverview
   *
   * Emitted when the overview is completely hidden.
   */
  signals[HIDDEN] =
    g_signal_new ("hidden", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ShellOverview::item-drag-begin:
   * @self: the #ShellOverview
   * @source: (nullable): the dragged item
   *
   * Emitted when the drag of an item of the dash, the application grid or
   * the search results begins.
   */
  signals[ITEM_DRAG_BEGIN] =
    g_signal_new ("item-drag-begin", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_OBJECT);

  /**
   * ShellOverview::item-drag-cancelled:
   * @self: the #ShellOverview
   * @source: (nullable): the dragged item
   *
   * Emitted when the drag of an item was cancelled.
   */
  signals[ITEM_DRAG_CANCELLED] =
    g_signal_new ("item-drag-cancelled", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_OBJECT);

  /**
   * ShellOverview::item-drag-end:
   * @self: the #ShellOverview
   * @source: (nullable): the dragged item
   *
   * Emitted when the drag of an item ended.
   */
  signals[ITEM_DRAG_END] =
    g_signal_new ("item-drag-end", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_OBJECT);

  /**
   * ShellOverview::window-drag-begin:
   * @self: the #ShellOverview
   * @window: the dragged window
   *
   * Emitted when the drag of the preview of a window begins.
   */
  signals[WINDOW_DRAG_BEGIN] =
    g_signal_new ("window-drag-begin", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, META_TYPE_WINDOW);

  /**
   * ShellOverview::window-drag-cancelled:
   * @self: the #ShellOverview
   * @window: the dragged window
   *
   * Emitted when the drag of a window was cancelled.
   */
  signals[WINDOW_DRAG_CANCELLED] =
    g_signal_new ("window-drag-cancelled", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, META_TYPE_WINDOW);

  /**
   * ShellOverview::window-drag-end:
   * @self: the #ShellOverview
   * @window: the dragged window
   *
   * Emitted when the drag of a window ended.
   */
  signals[WINDOW_DRAG_END] =
    g_signal_new ("window-drag-end", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, META_TYPE_WINDOW);

  /* Lua extensions (gnome-lua): the hook points of the overview */
  registry = shell_hook_registry_get_default ();
  shell_hook_registry_declare (registry, "overview.showing", "reason:s target:s owner:s", TRUE,
                               "The overview starts to show");
  shell_hook_registry_declare (registry, "overview.shown", "target:s", FALSE,
                               "The overview is completely shown");
  shell_hook_registry_declare (registry, "overview.hiding", "reason:s owner:s", TRUE,
                               "The overview starts to hide");
  shell_hook_registry_declare (registry, "overview.hidden", "", FALSE,
                               "The overview is completely hidden");
}

static void
shell_overview_init (ShellOverview *self)
{
  self->is_dummy = TRUE;
  self->show_reason = "other";
  self->hide_reason = "other";
  self->show_target = SHELL_CONTROLS_STATE_WINDOW_PICKER;
}

/**
 * shell_overview_new:
 * @window_manager: the window manager
 * @layout_manager: the layout manager
 * @xdnd_handler: (nullable): the handler of the drags of X11 clients
 * @ctrl_alt_tab_manager: (nullable): the Ctrl+Alt+Tab manager
 * @session_mode: the session mode
 *
 * Creates the overview (`new Overview ()`). ShellMain creates it once, after
 * the window manager.
 *
 * Returns: (transfer full): a new #ShellOverview
 */
ShellOverview *
shell_overview_new (ShellWindowManager     *window_manager,
                    ShellLayoutManager     *layout_manager,
                    ShellXdndHandler       *xdnd_handler,
                    ShellCtrlAltTabManager *ctrl_alt_tab_manager,
                    ShellSessionMode       *session_mode)
{
  return g_object_new (SHELL_TYPE_OVERVIEW,
                       "window-manager", window_manager,
                       "layout-manager", layout_manager,
                       "xdnd-handler", xdnd_handler,
                       "ctrl-alt-tab-manager", ctrl_alt_tab_manager,
                       "session-mode", session_mode,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* The state, the parts                                                      */
/* ------------------------------------------------------------------------ */

/**
 * shell_overview_get_visible:
 * @self: a #ShellOverview
 *
 * Returns: the value of #ShellOverview:visible
 */
gboolean
shell_overview_get_visible (ShellOverview *self)
{
  g_return_val_if_fail (SHELL_IS_OVERVIEW (self), FALSE);

  return self->visible;
}

/**
 * shell_overview_get_visible_target:
 * @self: a #ShellOverview
 *
 * Returns: the value of #ShellOverview:visible-target
 */
gboolean
shell_overview_get_visible_target (ShellOverview *self)
{
  g_return_val_if_fail (SHELL_IS_OVERVIEW (self), FALSE);

  return self->visible_target;
}

/**
 * shell_overview_get_animation_in_progress:
 * @self: a #ShellOverview
 *
 * Returns: the value of #ShellOverview:animation-in-progress
 */
gboolean
shell_overview_get_animation_in_progress (ShellOverview *self)
{
  g_return_val_if_fail (SHELL_IS_OVERVIEW (self), FALSE);

  return self->animation_in_progress;
}

/**
 * shell_overview_get_closing:
 * @self: a #ShellOverview
 *
 * Returns: the value of #ShellOverview:closing
 */
gboolean
shell_overview_get_closing (ShellOverview *self)
{
  g_return_val_if_fail (SHELL_IS_OVERVIEW (self), FALSE);

  return self->animation_in_progress && !self->visible_target;
}

/**
 * shell_overview_get_target_state:
 * @self: a #ShellOverview
 *
 * Gets the page the overview is going to or resting on: %SHELL_CONTROLS_STATE_HIDDEN
 * when it is hidden or on its way out, the window picker or the application
 * grid otherwise (what a gesture or an animation is nearest to), as
 * `shell.overview.target` of docs/api/overview.md.
 *
 * Returns: the target state
 */
ShellControlsState
shell_overview_get_target_state (ShellOverview *self)
{
  ShellOverviewAdjustment *adjustment;
  ShellStateTransitionParams params;
  double state;

  g_return_val_if_fail (SHELL_IS_OVERVIEW (self), SHELL_CONTROLS_STATE_HIDDEN);

  if (!self->visible_target || self->controls == NULL)
    return SHELL_CONTROLS_STATE_HIDDEN;

  adjustment = shell_controls_manager_get_state_adjustment (self->controls);
  if (adjustment == NULL)
    return self->show_target;

  shell_overview_adjustment_get_state_transition_params (adjustment, &params);
  state = params.transitioning ? params.final_state : params.current_state;

  return state >= 1.5 ? SHELL_CONTROLS_STATE_APP_GRID : SHELL_CONTROLS_STATE_WINDOW_PICKER;
}

/**
 * shell_overview_hooks_are_emitting: (skip)
 *
 * Whether an "overview.*" hook is being emitted by the overview, so that an
 * extension that calls shell_overview_show_for_extension () and its siblings
 * from one of the listeners must wait for the hook to return.
 *
 * Returns: %TRUE inside an emission of an "overview.*" hook
 */
gboolean
shell_overview_hooks_are_emitting (void)
{
  return hook_emit_depth > 0;
}

/**
 * shell_overview_get_is_dummy:
 * @self: a #ShellOverview
 *
 * Returns: the value of #ShellOverview:is-dummy
 */
gboolean
shell_overview_get_is_dummy (ShellOverview *self)
{
  g_return_val_if_fail (SHELL_IS_OVERVIEW (self), TRUE);

  return self->is_dummy;
}

/**
 * shell_overview_get_dash:
 * @self: a #ShellOverview
 *
 * Returns: (transfer none) (nullable): the value of #ShellOverview:dash
 */
ShellDash *
shell_overview_get_dash (ShellOverview *self)
{
  g_return_val_if_fail (SHELL_IS_OVERVIEW (self), NULL);

  return self->controls != NULL ? shell_controls_manager_get_dash (self->controls) : NULL;
}

/**
 * shell_overview_get_search_controller:
 * @self: a #ShellOverview
 *
 * Returns: (transfer none) (nullable): the value of #ShellOverview:search-controller
 */
ShellSearchController *
shell_overview_get_search_controller (ShellOverview *self)
{
  g_return_val_if_fail (SHELL_IS_OVERVIEW (self), NULL);

  return self->controls != NULL ? shell_controls_manager_get_search_controller (self->controls) : NULL;
}

/**
 * shell_overview_get_search_entry:
 * @self: a #ShellOverview
 *
 * Returns: (transfer none) (nullable): the value of #ShellOverview:search-entry
 */
StEntry *
shell_overview_get_search_entry (ShellOverview *self)
{
  g_return_val_if_fail (SHELL_IS_OVERVIEW (self), NULL);

  return self->controls != NULL ? shell_controls_manager_get_search_entry (self->controls) : NULL;
}
