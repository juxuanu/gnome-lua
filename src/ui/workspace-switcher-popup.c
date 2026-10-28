/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * workspace-switcher-popup.c: The row of dots flashed at the bottom of the
 * monitor(s) when switching workspaces. Port of
 * js/ui/workspaceSwitcherPopup.js.
 *
 * The popup is an invisible container, added to the ui group, holding one
 * actor per monitor (or a single one on the primary monitor when workspaces
 * only exist there). It destroys itself a short while after the last call
 * to shell_workspace_switcher_popup_display().
 */

#include "config.h"

#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-backend.h>
#include <meta/meta-monitor-manager.h>
#include <meta/meta-workspace-manager.h>
#include <meta/prefs.h>

#include "shell-global.h"
#include "st.h"
#include "ui/environment.h"
#include "ui/workspace-switcher-popup.h"

#define ANIMATION_TIME 100
#define DISPLAY_TIMEOUT 600

/* ------------------------------------------------------------------------ */
/* ShellWsPopupMonitorConstraint: makes an actor cover one monitor.          */
/* Private to this file. The JS version used Layout.MonitorConstraint, which */
/* is still JS (it depends on Main.layoutManager); this one reads the        */
/* geometry from MetaDisplay, which is what the layout manager does too.    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WS_POPUP_MONITOR_CONSTRAINT (shell_ws_popup_monitor_constraint_get_type ())
G_DECLARE_FINAL_TYPE (ShellWsPopupMonitorConstraint,
                      shell_ws_popup_monitor_constraint,
                      SHELL, WS_POPUP_MONITOR_CONSTRAINT, ClutterConstraint)

struct _ShellWsPopupMonitorConstraint
{
  ClutterConstraint parent;

  int monitor_index;            /* -1: the primary monitor */
};

G_DEFINE_FINAL_TYPE (ShellWsPopupMonitorConstraint,
                     shell_ws_popup_monitor_constraint,
                     CLUTTER_TYPE_CONSTRAINT)

static void
monitors_changed_cb (MetaMonitorManager *monitor_manager,
                     ClutterConstraint  *constraint)
{
  ClutterActor *actor =
    clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (constraint));

  if (actor != NULL)
    clutter_actor_queue_relayout (actor);
}

static void
shell_ws_popup_monitor_constraint_set_actor (ClutterActorMeta *meta,
                                             ClutterActor     *new_actor)
{
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (shell_global_get_backend (shell_global_get ()));

  g_signal_handlers_disconnect_by_func (monitor_manager,
                                        monitors_changed_cb, meta);
  if (new_actor != NULL)
    g_signal_connect (monitor_manager, "monitors-changed",
                      G_CALLBACK (monitors_changed_cb), meta);

  CLUTTER_ACTOR_META_CLASS (shell_ws_popup_monitor_constraint_parent_class)->set_actor (meta, new_actor);
}

static void
shell_ws_popup_monitor_constraint_update_allocation (ClutterConstraint *constraint,
                                                     ClutterActor      *actor,
                                                     ClutterActorBox   *allocation)
{
  ShellWsPopupMonitorConstraint *self =
    SHELL_WS_POPUP_MONITOR_CONSTRAINT (constraint);
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  MtkRectangle rect;
  int n_monitors, index;

  n_monitors = meta_display_get_n_monitors (display);
  if (n_monitors == 0)
    return;

  if (self->monitor_index < 0)
    index = meta_display_get_primary_monitor (display);
  else
    index = MIN (self->monitor_index, n_monitors - 1);

  meta_display_get_monitor_geometry (display, index, &rect);
  clutter_actor_box_init_rect (allocation,
                               rect.x, rect.y, rect.width, rect.height);
}

static void
shell_ws_popup_monitor_constraint_dispose (GObject *object)
{
  ShellGlobal *global = shell_global_get ();
  MetaBackend *backend = global ? shell_global_get_backend (global) : NULL;
  MetaMonitorManager *monitor_manager =
    backend ? meta_backend_get_monitor_manager (backend) : NULL;

  if (monitor_manager != NULL)
    g_signal_handlers_disconnect_by_func (monitor_manager,
                                          monitors_changed_cb, object);

  G_OBJECT_CLASS (shell_ws_popup_monitor_constraint_parent_class)->dispose (object);
}

static void
shell_ws_popup_monitor_constraint_class_init (ShellWsPopupMonitorConstraintClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorMetaClass *meta_class = CLUTTER_ACTOR_META_CLASS (klass);
  ClutterConstraintClass *constraint_class = CLUTTER_CONSTRAINT_CLASS (klass);

  object_class->dispose = shell_ws_popup_monitor_constraint_dispose;
  meta_class->set_actor = shell_ws_popup_monitor_constraint_set_actor;
  constraint_class->update_allocation = shell_ws_popup_monitor_constraint_update_allocation;
}

static void
shell_ws_popup_monitor_constraint_init (ShellWsPopupMonitorConstraint *self)
{
  self->monitor_index = -1;
}

static ClutterConstraint *
monitor_constraint_new (int monitor_index)
{
  ShellWsPopupMonitorConstraint *self =
    g_object_new (SHELL_TYPE_WS_POPUP_MONITOR_CONSTRAINT, NULL);

  self->monitor_index = monitor_index;

  return CLUTTER_CONSTRAINT (self);
}

/* ------------------------------------------------------------------------ */
/* ShellWorkspaceSwitcherPopup                                               */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,
  PROP_UI_GROUP,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellWorkspaceSwitcherPopup
{
  ClutterActor parent;

  ClutterActor *ui_group;       /* weak */

  guint timeout_id;
  int active_workspace_index;
};

G_DEFINE_FINAL_TYPE (ShellWorkspaceSwitcherPopup, shell_workspace_switcher_popup,
                     CLUTTER_TYPE_ACTOR)

/* The former MonitorWorkspaceSwitcherPopup: a plain actor stretched over a
 * monitor by a constraint, with the list of indicators as its only child. */
static ClutterActor *
monitor_popup_new (ClutterConstraint *constraint)
{
  ClutterActor *monitor_popup;
  ClutterActor *list;

  monitor_popup = g_object_new (CLUTTER_TYPE_ACTOR,
                                "offscreen-redirect", CLUTTER_OFFSCREEN_REDIRECT_ALWAYS,
                                "x-expand", TRUE,
                                "y-expand", TRUE,
                                "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                "y-align", CLUTTER_ACTOR_ALIGN_END,
                                NULL);
  clutter_actor_add_constraint (monitor_popup, constraint);

  list = g_object_new (ST_TYPE_BOX_LAYOUT,
                       "style-class", "workspace-switcher",
                       NULL);
  clutter_actor_add_child (monitor_popup, list);

  return monitor_popup;
}

static void
monitor_popup_redisplay (ClutterActor *monitor_popup,
                         int           active_workspace_index)
{
  MetaWorkspaceManager *workspace_manager =
    shell_global_get_workspace_manager (shell_global_get ());
  ClutterActor *list = clutter_actor_get_first_child (monitor_popup);
  int n_workspaces, i;

  clutter_actor_destroy_all_children (list);

  n_workspaces = meta_workspace_manager_get_n_workspaces (workspace_manager);
  for (i = 0; i < n_workspaces; i++)
    {
      ClutterActor *indicator = g_object_new (ST_TYPE_BIN,
                                              "style-class", "ws-switcher-indicator",
                                              NULL);

      if (i == active_workspace_index)
        st_widget_add_style_pseudo_class (ST_WIDGET (indicator), "active");

      clutter_actor_add_child (list, indicator);
    }
}

static void
redisplay_all_popups (ShellWorkspaceSwitcherPopup *self)
{
  ClutterActor *child;

  for (child = clutter_actor_get_first_child (CLUTTER_ACTOR (self));
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    monitor_popup_redisplay (child, self->active_workspace_index);
}

static void
workspaces_changed_cb (MetaWorkspaceManager         *workspace_manager,
                       int                           index,
                       ShellWorkspaceSwitcherPopup  *self)
{
  redisplay_all_popups (self);
}

static void
release_sources (ShellWorkspaceSwitcherPopup *self)
{
  MetaWorkspaceManager *workspace_manager =
    shell_global_get_workspace_manager (shell_global_get ());

  g_clear_handle_id (&self->timeout_id, g_source_remove);

  if (workspace_manager != NULL)
    g_signal_handlers_disconnect_by_func (workspace_manager,
                                          workspaces_changed_cb, self);
}

static void
fade_out_done_cb (gboolean finished,
                  gpointer user_data)
{
  GWeakRef *ref = user_data;
  g_autoptr (ShellWorkspaceSwitcherPopup) self = NULL;

  /* onComplete: only when the animation ran to its end */
  if (!finished)
    return;

  self = g_weak_ref_get (ref);
  if (self != NULL)
    clutter_actor_destroy (CLUTTER_ACTOR (self));
}

static void
weak_ref_free (gpointer data)
{
  GWeakRef *ref = data;

  g_weak_ref_clear (ref);
  g_free (ref);
}

static void
on_timeout (gpointer user_data)
{
  ShellWorkspaceSwitcherPopup *self = user_data;
  GWeakRef *ref = g_new0 (GWeakRef, 1);
  ShellEaseParams params = {
    .duration = ANIMATION_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  self->timeout_id = 0;

  g_weak_ref_init (ref, self);
  shell_actor_ease (CLUTTER_ACTOR (self), &params,
                    fade_out_done_cb, ref, weak_ref_free,
                    "opacity", 0u,
                    NULL);
}

/**
 * shell_workspace_switcher_popup_new:
 * @ui_group: (nullable): the actor to add the popup to (`Main.uiGroup`); the
 *   stage when %NULL
 *
 * Creates a hidden popup and adds it to @ui_group. It destroys itself after
 * it has been shown with shell_workspace_switcher_popup_display().
 *
 * Returns: (transfer none): the new #ShellWorkspaceSwitcherPopup, owned by
 *   @ui_group
 */
ShellWorkspaceSwitcherPopup *
shell_workspace_switcher_popup_new (ClutterActor *ui_group)
{
  return g_object_new (SHELL_TYPE_WORKSPACE_SWITCHER_POPUP,
                       "ui-group", ui_group,
                       NULL);
}

/**
 * shell_workspace_switcher_popup_display:
 * @self: a #ShellWorkspaceSwitcherPopup
 * @active_workspace_index: the index of the workspace to highlight
 *
 * Shows the popup, fading it in unless it is already visible, and
 * (re)starts the timer after which it fades out and destroys itself.
 */
void
shell_workspace_switcher_popup_display (ShellWorkspaceSwitcherPopup *self,
                                        int                          active_workspace_index)
{
  ClutterActor *actor;
  ShellEaseParams params = {
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  g_return_if_fail (SHELL_IS_WORKSPACE_SWITCHER_POPUP (self));

  actor = CLUTTER_ACTOR (self);

  self->active_workspace_index = active_workspace_index;

  g_clear_handle_id (&self->timeout_id, g_source_remove);
  self->timeout_id = g_timeout_add_once (DISPLAY_TIMEOUT, on_timeout, self);
  g_source_set_name_by_id (self->timeout_id,
                           "[gnome-shell] shell_workspace_switcher_popup_on_timeout");

  redisplay_all_popups (self);

  params.duration = clutter_actor_is_visible (actor) ? 0 : ANIMATION_TIME;
  clutter_actor_show (actor);
  clutter_actor_set_opacity (actor, 0);
  shell_actor_ease (actor, &params, NULL, NULL, NULL,
                    "opacity", 255u,
                    NULL);
}

static void
shell_workspace_switcher_popup_map (ClutterActor *actor)
{
  meta_compositor_disable_unredirect (shell_global_get_compositor (shell_global_get ()));

  CLUTTER_ACTOR_CLASS (shell_workspace_switcher_popup_parent_class)->map (actor);
}

static void
shell_workspace_switcher_popup_unmap (ClutterActor *actor)
{
  meta_compositor_enable_unredirect (shell_global_get_compositor (shell_global_get ()));

  CLUTTER_ACTOR_CLASS (shell_workspace_switcher_popup_parent_class)->unmap (actor);
}

static void
shell_workspace_switcher_popup_destroy (ClutterActor *actor)
{
  ShellWorkspaceSwitcherPopup *self = SHELL_WORKSPACE_SWITCHER_POPUP (actor);

  release_sources (self);

  CLUTTER_ACTOR_CLASS (shell_workspace_switcher_popup_parent_class)->destroy (actor);
}

static void
shell_workspace_switcher_popup_constructed (GObject *object)
{
  ShellWorkspaceSwitcherPopup *self = SHELL_WORKSPACE_SWITCHER_POPUP (object);
  ShellGlobal *global = shell_global_get ();
  ClutterActor *actor = CLUTTER_ACTOR (self);

  G_OBJECT_CLASS (shell_workspace_switcher_popup_parent_class)->constructed (object);

  if (self->ui_group == NULL)
    {
      g_set_weak_pointer (&self->ui_group,
                          CLUTTER_ACTOR (shell_global_get_stage (global)));
    }

  clutter_actor_add_child (self->ui_group, actor);

  clutter_actor_hide (actor);

  if (meta_prefs_get_workspaces_only_on_primary ())
    {
      clutter_actor_add_child (actor, monitor_popup_new (monitor_constraint_new (-1)));
    }
  else
    {
      int n_monitors = meta_display_get_n_monitors (shell_global_get_display (global));
      int i;

      for (i = 0; i < n_monitors; i++)
        clutter_actor_add_child (actor, monitor_popup_new (monitor_constraint_new (i)));
    }

  g_signal_connect_object (shell_global_get_workspace_manager (global),
                           "workspace-added",
                           G_CALLBACK (workspaces_changed_cb), self, 0);
  g_signal_connect_object (shell_global_get_workspace_manager (global),
                           "workspace-removed",
                           G_CALLBACK (workspaces_changed_cb), self, 0);
}

static void
shell_workspace_switcher_popup_dispose (GObject *object)
{
  ShellWorkspaceSwitcherPopup *self = SHELL_WORKSPACE_SWITCHER_POPUP (object);

  release_sources (self);
  g_clear_weak_pointer (&self->ui_group);

  G_OBJECT_CLASS (shell_workspace_switcher_popup_parent_class)->dispose (object);
}

static void
shell_workspace_switcher_popup_set_property (GObject      *object,
                                             guint         prop_id,
                                             const GValue *value,
                                             GParamSpec   *pspec)
{
  ShellWorkspaceSwitcherPopup *self = SHELL_WORKSPACE_SWITCHER_POPUP (object);

  switch (prop_id)
    {
    case PROP_UI_GROUP:
      g_set_weak_pointer (&self->ui_group, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_workspace_switcher_popup_get_property (GObject    *object,
                                             guint       prop_id,
                                             GValue     *value,
                                             GParamSpec *pspec)
{
  ShellWorkspaceSwitcherPopup *self = SHELL_WORKSPACE_SWITCHER_POPUP (object);

  switch (prop_id)
    {
    case PROP_UI_GROUP:
      g_value_set_object (value, self->ui_group);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_workspace_switcher_popup_class_init (ShellWorkspaceSwitcherPopupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_workspace_switcher_popup_constructed;
  object_class->dispose = shell_workspace_switcher_popup_dispose;
  object_class->set_property = shell_workspace_switcher_popup_set_property;
  object_class->get_property = shell_workspace_switcher_popup_get_property;

  actor_class->map = shell_workspace_switcher_popup_map;
  actor_class->unmap = shell_workspace_switcher_popup_unmap;
  actor_class->destroy = shell_workspace_switcher_popup_destroy;

  /**
   * ShellWorkspaceSwitcherPopup:ui-group:
   *
   * The actor the popup is added to when it is created (`Main.uiGroup`).
   * The stage is used when it is not given.
   */
  props[PROP_UI_GROUP] =
    g_param_spec_object ("ui-group", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_workspace_switcher_popup_init (ShellWorkspaceSwitcherPopup *self)
{
  self->active_workspace_index = -1;
}
