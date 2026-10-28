/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * workspace-animation.c: The animation of switching workspaces. Port of
 * js/ui/workspaceAnimation.js.
 *
 * The strip of workspaces of one monitor is a #ShellWsaMonitorGroup (was
 * MonitorGroup) holding one #ShellWsaWorkspaceGroup (WorkspaceGroup) per
 * workspace plus one for the windows which are on all workspaces. A
 * workspace group is a #ShellWsaBaseGroup (BaseWorkspaceGroup) with the
 * clones of the windows of its workspace; the backgrounds with the clones of
 * the desktop windows are #ShellWsaWorkspaceBackground (WorkspaceBackground).
 * All four are private to this file, nothing else used them in the JS.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `Main.wm.blockWorkspaceUpdates ()`, `Main.createWorkspacesAdjustment ()`,
 *    `Main.layoutManager`, `Main.uiGroup`, `Main.panel` and `Main.pushModal ()`
 *    are the C functions of #ShellWindowManager, #ShellLayoutManager,
 *    #ShellPanel and #ShellMain. The overview's `showing` and `hiding`
 *    signals are the changes of #ShellWindowManager:overview-visible and
 *    #ShellWindowManager:overview-closing.
 *  - The snap points handed to the swipe tracker are sorted (the C tracker
 *    requires it). They already were in the JS in every layout.
 *  - Where the JS computed `NaN` (a progress outside of the snap points of
 *    another monitor, a workspace which is not on the strip) the C code
 *    clamps to the first or last workspace.
 *  - After `_finishWorkspaceSwitch ()` ran, the JS kept starting the
 *    animations of the remaining monitors on destroyed actors; the C loop
 *    stops.
 */

#include "config.h"

#include <math.h>

#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-background-group.h>
#include <meta/meta-workspace-manager.h>
#include <meta/prefs.h>

#include "shell-global.h"
#include "ui/background.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/panel.h"
#include "ui/swipe-tracker.h"
#include "ui/window-manager.h"
#include "ui/workspace-animation.h"

#define WINDOW_ANIMATION_TIME SHELL_WORKSPACE_ANIMATION_WINDOW_ANIMATION_TIME
#define WORKSPACE_SPACING SHELL_WORKSPACE_ANIMATION_WORKSPACE_SPACING

/* ------------------------------------------------------------------------ */
/* ShellWsaBaseGroup (BaseWorkspaceGroup)                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WSA_BASE_GROUP (shell_wsa_base_group_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellWsaBaseGroup, shell_wsa_base_group,
                          SHELL, WSA_BASE_GROUP, ClutterActor)

struct _ShellWsaBaseGroupClass
{
  ClutterActorClass parent_class;

  gboolean (* should_show_window) (ShellWsaBaseGroup *self,
                                   MetaWindow        *window);
  void     (* create_background)  (ShellWsaBaseGroup *self);
  void     (* cleanup)            (ShellWsaBaseGroup *self);
};

typedef struct
{
  ShellWsaBaseGroup *group;     /* not owned */
  MetaWindowActor *window_actor; /* strong */
  MetaWindow *window;           /* strong */
  ClutterActor *clone;          /* weak: a child of the group */
  gulong destroy_id;
  gulong minimized_id;
  gulong clone_destroy_id;
} WindowRecord;

typedef struct
{
  ShellMonitor *monitor;        /* owned copy */
  MetaWorkspace *workspace;     /* weak, NULL for the sticky windows */
  MetaWindow *moving_window;    /* weak */
  GList *window_records;        /* of WindowRecord */
} ShellWsaBaseGroupPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (ShellWsaBaseGroup, shell_wsa_base_group,
                                     CLUTTER_TYPE_ACTOR)

static void window_record_free (WindowRecord *record);

static gboolean
base_group_should_show_window (ShellWsaBaseGroup *self,
                               MetaWindow        *window)
{
  return FALSE;
}

static void
base_group_create_background (ShellWsaBaseGroup *self)
{
}

static void
base_group_remove_windows (ShellWsaBaseGroup *self)
{
  ShellWsaBaseGroupPrivate *priv = shell_wsa_base_group_get_instance_private (self);
  GList *l;

  for (l = priv->window_records; l != NULL; l = l->next)
    {
      WindowRecord *record = l->data;

      if (record->clone != NULL)
        clutter_actor_destroy (record->clone);
    }

  g_list_free_full (priv->window_records, (GDestroyNotify) window_record_free);
  priv->window_records = NULL;
}

static void
base_group_cleanup (ShellWsaBaseGroup *self)
{
  base_group_remove_windows (self);
}

static void
shell_wsa_base_group_destroy (ClutterActor *actor)
{
  ShellWsaBaseGroup *self = SHELL_WSA_BASE_GROUP (actor);

  SHELL_WSA_BASE_GROUP_GET_CLASS (self)->cleanup (self);

  CLUTTER_ACTOR_CLASS (shell_wsa_base_group_parent_class)->destroy (actor);
}

static void
shell_wsa_base_group_dispose (GObject *object)
{
  ShellWsaBaseGroup *self = SHELL_WSA_BASE_GROUP (object);
  ShellWsaBaseGroupPrivate *priv = shell_wsa_base_group_get_instance_private (self);

  /* Destroys the actor, which removes the windows */
  G_OBJECT_CLASS (shell_wsa_base_group_parent_class)->dispose (object);

  g_clear_weak_pointer (&priv->workspace);
  g_clear_weak_pointer (&priv->moving_window);
  g_list_free_full (priv->window_records, (GDestroyNotify) window_record_free);
  priv->window_records = NULL;
}

static void
shell_wsa_base_group_finalize (GObject *object)
{
  ShellWsaBaseGroup *self = SHELL_WSA_BASE_GROUP (object);
  ShellWsaBaseGroupPrivate *priv = shell_wsa_base_group_get_instance_private (self);

  g_clear_pointer (&priv->monitor, shell_monitor_free);

  G_OBJECT_CLASS (shell_wsa_base_group_parent_class)->finalize (object);
}

static void
shell_wsa_base_group_class_init (ShellWsaBaseGroupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->dispose = shell_wsa_base_group_dispose;
  object_class->finalize = shell_wsa_base_group_finalize;

  actor_class->destroy = shell_wsa_base_group_destroy;

  klass->should_show_window = base_group_should_show_window;
  klass->create_background = base_group_create_background;
  klass->cleanup = base_group_cleanup;
}

static void
shell_wsa_base_group_init (ShellWsaBaseGroup *self)
{
}

static MetaWorkspace *
base_group_get_workspace (ShellWsaBaseGroup *self)
{
  ShellWsaBaseGroupPrivate *priv = shell_wsa_base_group_get_instance_private (self);

  return priv->workspace;
}

static const ShellMonitor *
base_group_get_monitor (ShellWsaBaseGroup *self)
{
  ShellWsaBaseGroupPrivate *priv = shell_wsa_base_group_get_instance_private (self);

  return priv->monitor;
}

static MetaWindow *
base_group_get_moving_window (ShellWsaBaseGroup *self)
{
  ShellWsaBaseGroupPrivate *priv = shell_wsa_base_group_get_instance_private (self);

  return priv->moving_window;
}

static gboolean
is_desktop_window (MetaWindow *window)
{
  return meta_window_get_window_type (window) == META_WINDOW_DESKTOP;
}

static gboolean
window_is_on_this_monitor (ShellWsaBaseGroup *self,
                           MetaWindow        *window)
{
  ShellWsaBaseGroupPrivate *priv = shell_wsa_base_group_get_instance_private (self);
  MetaDisplay *display = meta_window_get_display (window);
  MtkRectangle geometry, frame, intersection;

  meta_display_get_monitor_geometry (display, priv->monitor->index, &geometry);
  meta_window_get_frame_rect (window, &frame);

  return mtk_rectangle_intersect (&frame, &geometry, &intersection);
}

static void
sync_clone_visibility (WindowRecord *record)
{
  if (record->clone == NULL)
    return;

  if (meta_window_showing_on_its_workspace (record->window))
    clutter_actor_show (record->clone);
  else
    clutter_actor_hide (record->clone);
}

static void
window_record_free (WindowRecord *record)
{
  g_clear_signal_handler (&record->destroy_id, record->window_actor);
  g_clear_signal_handler (&record->minimized_id, record->window);
  if (record->clone != NULL)
    g_clear_signal_handler (&record->clone_destroy_id, record->clone);
  g_clear_object (&record->window_actor);
  g_clear_object (&record->window);
  g_free (record);
}

static void
window_actor_destroyed_cb (ClutterActor *actor,
                           WindowRecord *record)
{
  ShellWsaBaseGroup *self = record->group;
  ShellWsaBaseGroupPrivate *priv = shell_wsa_base_group_get_instance_private (self);

  if (record->clone != NULL)
    clutter_actor_destroy (record->clone);

  priv->window_records = g_list_remove (priv->window_records, record);
  window_record_free (record);
}

static void
window_minimized_cb (MetaWindow   *window,
                     GParamSpec   *pspec,
                     WindowRecord *record)
{
  sync_clone_visibility (record);
}

static void
clone_destroyed_cb (ClutterActor *clone,
                    WindowRecord *record)
{
  record->clone = NULL;
}

static ClutterActor *
base_group_create_clone (ShellWsaBaseGroup *self,
                         MetaWindowActor   *window_actor)
{
  ShellWsaBaseGroupPrivate *priv = shell_wsa_base_group_get_instance_private (self);
  ClutterActor *source = CLUTTER_ACTOR (window_actor);
  ClutterActor *clone;
  WindowRecord *record;

  clone = clutter_clone_new (source);
  clutter_actor_set_position (clone,
                              clutter_actor_get_x (source) - priv->monitor->x,
                              clutter_actor_get_y (source) - priv->monitor->y);

  record = g_new0 (WindowRecord, 1);
  record->group = self;
  record->window_actor = g_object_ref (window_actor);
  record->window = g_object_ref (meta_window_actor_get_meta_window (window_actor));
  record->clone = clone;

  record->destroy_id = g_signal_connect (window_actor, "destroy",
                                         G_CALLBACK (window_actor_destroyed_cb),
                                         record);
  record->minimized_id = g_signal_connect (record->window, "notify::minimized",
                                           G_CALLBACK (window_minimized_cb),
                                           record);
  record->clone_destroy_id = g_signal_connect (clone, "destroy",
                                               G_CALLBACK (clone_destroyed_cb), record);

  sync_clone_visibility (record);

  priv->window_records = g_list_append (priv->window_records, record);

  return clone;
}

static void
base_group_create_windows (ShellWsaBaseGroup *self)
{
  ShellWsaBaseGroupClass *klass = SHELL_WSA_BASE_GROUP_GET_CLASS (self);
  g_autoptr (GList) actors = shell_global_get_window_actors (shell_global_get ());
  GList *l;

  for (l = actors; l != NULL; l = l->next)
    {
      MetaWindowActor *window_actor = l->data;
      MetaWindow *window = meta_window_actor_get_meta_window (window_actor);
      ClutterActor *clone;

      if (window == NULL || !klass->should_show_window (self, window))
        continue;

      clone = base_group_create_clone (self, window_actor);
      clutter_actor_add_child (CLUTTER_ACTOR (self), clone);
    }
}

/* Called once the instance exists: the JS did it in the constructor, whose
 * virtual calls need the fields set here */
static void
base_group_setup (ShellWsaBaseGroup *self,
                  const ShellMonitor *monitor,
                  MetaWorkspace      *workspace,
                  MetaWindow         *moving_window)
{
  ShellWsaBaseGroupPrivate *priv = shell_wsa_base_group_get_instance_private (self);

  priv->monitor = shell_monitor_copy (monitor);
  g_set_weak_pointer (&priv->workspace, workspace);
  g_set_weak_pointer (&priv->moving_window, moving_window);

  clutter_actor_set_size (CLUTTER_ACTOR (self), monitor->width, monitor->height);
  clutter_actor_set_clip_to_allocation (CLUTTER_ACTOR (self), TRUE);

  SHELL_WSA_BASE_GROUP_GET_CLASS (self)->create_background (self);
  base_group_create_windows (self);
}

/* ------------------------------------------------------------------------ */
/* ShellWsaWorkspaceBackground (WorkspaceBackground)                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WSA_WORKSPACE_BACKGROUND (shell_wsa_workspace_background_get_type ())
G_DECLARE_FINAL_TYPE (ShellWsaWorkspaceBackground, shell_wsa_workspace_background,
                      SHELL, WSA_WORKSPACE_BACKGROUND, ShellWsaBaseGroup)

struct _ShellWsaWorkspaceBackground
{
  ShellWsaBaseGroup parent;

  ShellBackgroundManager *bg_manager;
};

G_DEFINE_FINAL_TYPE (ShellWsaWorkspaceBackground, shell_wsa_workspace_background,
                     SHELL_TYPE_WSA_BASE_GROUP)

static gboolean
workspace_background_should_show_window (ShellWsaBaseGroup *group,
                                         MetaWindow        *window)
{
  return is_desktop_window (window) && window_is_on_this_monitor (group, window);
}

static void
workspace_background_create_background (ShellWsaBaseGroup *group)
{
  ShellWsaWorkspaceBackground *self = SHELL_WSA_WORKSPACE_BACKGROUND (group);
  ClutterActor *background = meta_background_group_new ();

  clutter_actor_add_child (CLUTTER_ACTOR (self), background);

  self->bg_manager = g_object_new (SHELL_TYPE_BACKGROUND_MANAGER,
                                   "container", background,
                                   "monitor-index", base_group_get_monitor (group)->index,
                                   "control-position", FALSE,
                                   NULL);
}

static void
workspace_background_cleanup (ShellWsaBaseGroup *group)
{
  ShellWsaWorkspaceBackground *self = SHELL_WSA_WORKSPACE_BACKGROUND (group);

  SHELL_WSA_BASE_GROUP_CLASS (shell_wsa_workspace_background_parent_class)->cleanup (group);

  if (self->bg_manager != NULL)
    {
      shell_background_manager_destroy (self->bg_manager);
      g_clear_object (&self->bg_manager);
    }
}

static void
shell_wsa_workspace_background_class_init (ShellWsaWorkspaceBackgroundClass *klass)
{
  ShellWsaBaseGroupClass *group_class = SHELL_WSA_BASE_GROUP_CLASS (klass);

  group_class->should_show_window = workspace_background_should_show_window;
  group_class->create_background = workspace_background_create_background;
  group_class->cleanup = workspace_background_cleanup;
}

static void
shell_wsa_workspace_background_init (ShellWsaWorkspaceBackground *self)
{
}

static ClutterActor *
workspace_background_new (const ShellMonitor *monitor)
{
  ShellWsaWorkspaceBackground *self =
    g_object_new (SHELL_TYPE_WSA_WORKSPACE_BACKGROUND, NULL);

  base_group_setup (SHELL_WSA_BASE_GROUP (self), monitor, NULL, NULL);

  return CLUTTER_ACTOR (self);
}

/* ------------------------------------------------------------------------ */
/* ShellWsaWorkspaceGroup (WorkspaceGroup)                                  */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WSA_WORKSPACE_GROUP (shell_wsa_workspace_group_get_type ())
G_DECLARE_FINAL_TYPE (ShellWsaWorkspaceGroup, shell_wsa_workspace_group,
                      SHELL, WSA_WORKSPACE_GROUP, ShellWsaBaseGroup)

struct _ShellWsaWorkspaceGroup
{
  ShellWsaBaseGroup parent;

  ClutterActor *background;     /* weak */
};

G_DEFINE_FINAL_TYPE (ShellWsaWorkspaceGroup, shell_wsa_workspace_group,
                     SHELL_TYPE_WSA_BASE_GROUP)

static gboolean
workspace_group_should_show_window (ShellWsaBaseGroup *group,
                                    MetaWindow        *window)
{
  MetaWorkspace *workspace = base_group_get_workspace (group);
  gboolean is_sticky;

  if (is_desktop_window (window))
    return FALSE;

  if (meta_window_is_override_redirect (window))
    return FALSE;

  if (!window_is_on_this_monitor (group, window))
    return FALSE;

  is_sticky = meta_window_is_on_all_workspaces (window) ||
              window == base_group_get_moving_window (group);

  /* No workspace means we should show windows that are on all workspaces */
  if (workspace == NULL)
    return is_sticky;

  /* Otherwise only show windows that are (only) on that workspace */
  return !is_sticky && meta_window_located_on_workspace (window, workspace);
}

static void
workspace_group_create_background (ShellWsaBaseGroup *group)
{
  ShellWsaWorkspaceGroup *self = SHELL_WSA_WORKSPACE_GROUP (group);
  ClutterActor *background;

  if (base_group_get_workspace (group) == NULL)
    return;

  background = workspace_background_new (base_group_get_monitor (group));
  g_set_weak_pointer (&self->background, background);
  clutter_actor_add_child (CLUTTER_ACTOR (self), background);
}

static int
compare_records (gconstpointer a,
                 gconstpointer b,
                 gpointer      user_data)
{
  const WindowRecord *record_a = a;
  const WindowRecord *record_b = b;
  GHashTable *stack_indices = user_data;
  int seq_a = GPOINTER_TO_INT (g_hash_table_lookup (stack_indices, record_a->window_actor));
  int seq_b = GPOINTER_TO_INT (g_hash_table_lookup (stack_indices, record_b->window_actor));

  return seq_a - seq_b;
}

static void
workspace_group_sync_stacking (ShellWsaWorkspaceGroup *self)
{
  ShellWsaBaseGroupPrivate *priv =
    shell_wsa_base_group_get_instance_private (SHELL_WSA_BASE_GROUP (self));
  g_autoptr (GList) actors = shell_global_get_window_actors (shell_global_get ());
  g_autoptr (GHashTable) stack_indices = g_hash_table_new (NULL, NULL);
  ClutterActor *last_clone = NULL;
  GList *l;
  int i = 0;

  /* The index is stored plus one: 0 is what a missing actor gets */
  for (l = actors; l != NULL; l = l->next)
    g_hash_table_insert (stack_indices, l->data, GINT_TO_POINTER (++i));

  priv->window_records = g_list_sort_with_data (priv->window_records,
                                                compare_records, stack_indices);

  for (l = priv->window_records; l != NULL; l = l->next)
    {
      WindowRecord *record = l->data;

      if (record->clone == NULL)
        continue;

      clutter_actor_set_child_above_sibling (CLUTTER_ACTOR (self), record->clone,
                                             last_clone != NULL ? last_clone
                                                                : self->background);
      last_clone = record->clone;
    }
}

static void
workspace_group_restacked_cb (MetaDisplay            *display,
                              ShellWsaWorkspaceGroup *self)
{
  workspace_group_sync_stacking (self);
}

static void
shell_wsa_workspace_group_dispose (GObject *object)
{
  ShellWsaWorkspaceGroup *self = SHELL_WSA_WORKSPACE_GROUP (object);

  g_clear_weak_pointer (&self->background);

  G_OBJECT_CLASS (shell_wsa_workspace_group_parent_class)->dispose (object);
}

static void
shell_wsa_workspace_group_class_init (ShellWsaWorkspaceGroupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellWsaBaseGroupClass *group_class = SHELL_WSA_BASE_GROUP_CLASS (klass);

  object_class->dispose = shell_wsa_workspace_group_dispose;

  group_class->should_show_window = workspace_group_should_show_window;
  group_class->create_background = workspace_group_create_background;
}

static void
shell_wsa_workspace_group_init (ShellWsaWorkspaceGroup *self)
{
}

static ClutterActor *
workspace_group_new (const ShellMonitor *monitor,
                     MetaWorkspace      *workspace,
                     MetaWindow         *moving_window)
{
  ShellWsaWorkspaceGroup *self = g_object_new (SHELL_TYPE_WSA_WORKSPACE_GROUP, NULL);

  base_group_setup (SHELL_WSA_BASE_GROUP (self), monitor, workspace, moving_window);

  g_signal_connect_object (shell_global_get_display (shell_global_get ()),
                           "restacked",
                           G_CALLBACK (workspace_group_restacked_cb), self, 0);

  return CLUTTER_ACTOR (self);
}

static MetaWorkspace *
workspace_group_get_workspace (ShellWsaWorkspaceGroup *self)
{
  return base_group_get_workspace (SHELL_WSA_BASE_GROUP (self));
}

/* ------------------------------------------------------------------------ */
/* ShellWsaMonitorGroup (MonitorGroup)                                      */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WSA_MONITOR_GROUP (shell_wsa_monitor_group_get_type ())
G_DECLARE_FINAL_TYPE (ShellWsaMonitorGroup, shell_wsa_monitor_group,
                      SHELL, WSA_MONITOR_GROUP, StWidget)

struct _ShellWsaMonitorGroup
{
  StWidget parent;

  ShellMonitor *monitor;        /* owned copy */
  ClutterStage *stage;          /* weak */
  MetaWorkspaceManager *workspace_manager; /* weak */

  ClutterActor *container;      /* weak: a child */
  GPtrArray *workspace_groups;  /* of ShellWsaWorkspaceGroup, not owned */
  StAdjustment *workspaces_adjustment;
};

enum
{
  MG_PROP_0,
  MG_PROP_PROGRESS,
  MG_N_PROPS
};

static GParamSpec *monitor_group_props[MG_N_PROPS] = { NULL, };

G_DEFINE_FINAL_TYPE (ShellWsaMonitorGroup, shell_wsa_monitor_group, ST_TYPE_WIDGET)

static gboolean
monitor_group_is_vertical (ShellWsaMonitorGroup *self)
{
  return meta_workspace_manager_get_layout_rows (self->workspace_manager) == -1;
}

static double
monitor_group_get_base_distance (ShellWsaMonitorGroup *self)
{
  StThemeContext *context = st_theme_context_get_for_stage (self->stage);
  double spacing = WORKSPACE_SPACING * st_theme_context_get_scale_factor (context);

  if (monitor_group_is_vertical (self))
    return self->monitor->height + spacing;
  else
    return self->monitor->width + spacing;
}

static gboolean
monitor_group_is_rtl (ShellWsaMonitorGroup *self)
{
  return clutter_actor_get_text_direction (CLUTTER_ACTOR (self)) ==
         CLUTTER_TEXT_DIRECTION_RTL;
}

static double
monitor_group_get_progress (ShellWsaMonitorGroup *self)
{
  double base_distance = monitor_group_get_base_distance (self);

  if (self->container == NULL)
    return 0.0;

  if (monitor_group_is_vertical (self))
    return -clutter_actor_get_y (self->container) / base_distance;
  else if (monitor_group_is_rtl (self))
    return clutter_actor_get_x (self->container) / base_distance;
  else
    return -clutter_actor_get_x (self->container) / base_distance;
}

static void
monitor_group_set_progress (ShellWsaMonitorGroup *self,
                            double                progress)
{
  double base_distance = monitor_group_get_base_distance (self);

  if (self->container != NULL)
    {
      if (monitor_group_is_vertical (self))
        clutter_actor_set_y (self->container, -round (progress * base_distance));
      else if (monitor_group_is_rtl (self))
        clutter_actor_set_x (self->container, round (progress * base_distance));
      else
        clutter_actor_set_x (self->container, -round (progress * base_distance));
    }

  g_object_notify_by_pspec (G_OBJECT (self), monitor_group_props[MG_PROP_PROGRESS]);
}

static double
monitor_group_get_group_progress (ShellWsaMonitorGroup *self,
                                  ClutterActor         *group)
{
  double base_distance = monitor_group_get_base_distance (self);

  if (monitor_group_is_vertical (self))
    return clutter_actor_get_y (group) / base_distance;
  else if (monitor_group_is_rtl (self))
    return -clutter_actor_get_x (group) / base_distance;
  else
    return clutter_actor_get_x (group) / base_distance;
}

static double
monitor_group_get_workspace_progress (ShellWsaMonitorGroup *self,
                                      MetaWorkspace        *workspace)
{
  int index = meta_workspace_index (workspace);
  guint i;

  for (i = 0; i < self->workspace_groups->len; i++)
    {
      ShellWsaWorkspaceGroup *group = g_ptr_array_index (self->workspace_groups, i);

      if (workspace_group_get_workspace (group) != NULL &&
          meta_workspace_index (workspace_group_get_workspace (group)) == index)
        return monitor_group_get_group_progress (self, CLUTTER_ACTOR (group));
    }

  /* The JS computed NaN for a workspace which is not on the strip */
  return monitor_group_get_progress (self);
}

static GArray *
monitor_group_get_snap_points (ShellWsaMonitorGroup *self)
{
  GArray *points = g_array_sized_new (FALSE, FALSE, sizeof (double),
                                      self->workspace_groups->len);
  guint i;

  for (i = 0; i < self->workspace_groups->len; i++)
    {
      ShellWsaWorkspaceGroup *group = g_ptr_array_index (self->workspace_groups, i);
      double point = monitor_group_get_group_progress (self, CLUTTER_ACTOR (group));

      g_array_append_val (points, point);
    }

  return points;
}

static MetaWorkspace *
monitor_group_find_closest_workspace (ShellWsaMonitorGroup *self,
                                      double                progress)
{
  g_autoptr (GArray) points = monitor_group_get_snap_points (self);
  double min_distance = G_MAXDOUBLE;
  guint closest = 0;
  guint i;

  for (i = 0; i < points->len; i++)
    {
      double distance = fabs (g_array_index (points, double, i) - progress);

      if (distance < min_distance)
        {
          min_distance = distance;
          closest = i;
        }
    }

  return workspace_group_get_workspace (g_ptr_array_index (self->workspace_groups, closest));
}

static double
monitor_group_interpolate_progress (ShellWsaMonitorGroup *self,
                                    double                progress,
                                    ShellWsaMonitorGroup *monitor_group)
{
  g_autoptr (GArray) points1 = NULL;
  g_autoptr (GArray) points2 = NULL;
  int upper = -1, lower = -1;
  guint i;
  double t;

  if (self->monitor->index == monitor_group->monitor->index)
    return progress;

  points1 = monitor_group_get_snap_points (monitor_group);
  points2 = monitor_group_get_snap_points (self);

  if (points1->len == 0 || points1->len != points2->len)
    return progress;

  /* The first point which is >= progress, the last one which is <= progress */
  for (i = 0; i < points1->len; i++)
    {
      if (upper < 0 && g_array_index (points1, double, i) >= progress)
        upper = i;
      if (g_array_index (points1, double, i) <= progress)
        lower = i;
    }

  if (upper < 0)
    upper = points1->len - 1;
  if (lower < 0)
    lower = 0;

  if (g_array_index (points1, double, upper) == g_array_index (points1, double, lower))
    return g_array_index (points2, double, upper);

  t = (progress - g_array_index (points1, double, lower)) /
      (g_array_index (points1, double, upper) - g_array_index (points1, double, lower));

  return g_array_index (points2, double, lower) +
         (g_array_index (points2, double, upper) - g_array_index (points2, double, lower)) * t;
}

static void
monitor_group_update_swipe_for_monitor (ShellWsaMonitorGroup *self,
                                        double                progress,
                                        ShellWsaMonitorGroup *monitor_group)
{
  monitor_group_set_progress (self,
                              monitor_group_interpolate_progress (self, progress,
                                                                  monitor_group));
}

static void
monitor_group_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  ShellWsaMonitorGroup *self = SHELL_WSA_MONITOR_GROUP (object);

  switch (prop_id)
    {
    case MG_PROP_PROGRESS:
      g_value_set_double (value, monitor_group_get_progress (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
monitor_group_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  ShellWsaMonitorGroup *self = SHELL_WSA_MONITOR_GROUP (object);

  switch (prop_id)
    {
    case MG_PROP_PROGRESS:
      monitor_group_set_progress (self, g_value_get_double (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_wsa_monitor_group_destroy (ClutterActor *actor)
{
  ShellWsaMonitorGroup *self = SHELL_WSA_MONITOR_GROUP (actor);

  g_clear_object (&self->workspaces_adjustment);

  CLUTTER_ACTOR_CLASS (shell_wsa_monitor_group_parent_class)->destroy (actor);
}

static void
shell_wsa_monitor_group_dispose (GObject *object)
{
  ShellWsaMonitorGroup *self = SHELL_WSA_MONITOR_GROUP (object);

  g_clear_weak_pointer (&self->stage);
  g_clear_weak_pointer (&self->workspace_manager);
  g_clear_weak_pointer (&self->container);
  g_clear_pointer (&self->workspace_groups, g_ptr_array_unref);
  g_clear_object (&self->workspaces_adjustment);

  G_OBJECT_CLASS (shell_wsa_monitor_group_parent_class)->dispose (object);
}

static void
shell_wsa_monitor_group_finalize (GObject *object)
{
  ShellWsaMonitorGroup *self = SHELL_WSA_MONITOR_GROUP (object);

  g_clear_pointer (&self->monitor, shell_monitor_free);

  G_OBJECT_CLASS (shell_wsa_monitor_group_parent_class)->finalize (object);
}

static void
shell_wsa_monitor_group_class_init (ShellWsaMonitorGroupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->get_property = monitor_group_get_property;
  object_class->set_property = monitor_group_set_property;
  object_class->dispose = shell_wsa_monitor_group_dispose;
  object_class->finalize = shell_wsa_monitor_group_finalize;

  actor_class->destroy = shell_wsa_monitor_group_destroy;

  monitor_group_props[MG_PROP_PROGRESS] =
    g_param_spec_double ("progress", NULL, NULL,
                         -G_MAXDOUBLE, G_MAXDOUBLE, 0,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, MG_N_PROPS, monitor_group_props);
}

static void
shell_wsa_monitor_group_init (ShellWsaMonitorGroup *self)
{
  self->workspace_groups = g_ptr_array_new ();
}

typedef struct
{
  GArray *indices;              /* of int */
} ProgressTransform;

static void
progress_transform_free (gpointer data)
{
  ProgressTransform *transform = data;

  g_array_unref (transform->indices);
  g_free (transform);
}

/* indices = [workspaceIndices[floor (source)], workspaceIndices[ceil (source)]];
 * return Util.lerp (...indices, source % 1.0) */
static gboolean
progress_to_adjustment_value (GBinding     *binding,
                              const GValue *from_value,
                              GValue       *to_value,
                              gpointer      user_data)
{
  ProgressTransform *transform = user_data;
  double source = g_value_get_double (from_value);
  int n = transform->indices->len;
  int lower_pos, upper_pos;
  double start, end;

  if (n == 0)
    return FALSE;

  lower_pos = CLAMP ((int) floor (source), 0, n - 1);
  upper_pos = CLAMP ((int) ceil (source), 0, n - 1);
  start = g_array_index (transform->indices, int, lower_pos);
  end = g_array_index (transform->indices, int, upper_pos);

  g_value_set_double (to_value, start + fmod (source, 1.0) * (end - start));

  return TRUE;
}

static ClutterActor *
monitor_group_new (ShellWindowManager  *window_manager,
                   const ShellMonitor  *monitor,
                   const int           *workspace_indices,
                   guint                n_workspace_indices,
                   MetaWindow          *moving_window)
{
  ShellGlobal *global = shell_global_get ();
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  ShellPanel *panel = shell_main_get_panel (shell_main_get_default ());
  MetaWorkspaceManager *workspace_manager = shell_global_get_workspace_manager (global);
  ShellWsaMonitorGroup *self;
  ShellMonitorConstraint *constraint;
  ClutterActor *container, *sticky_group;
  MetaWorkspace *active_workspace;
  gboolean vertical;
  gboolean rtl;
  float x = 0, y = 0;
  guint i;

  self = g_object_new (SHELL_TYPE_WSA_MONITOR_GROUP,
                       "clip-to-allocation", TRUE,
                       "style-class", "workspace-animation",
                       NULL);
  self->monitor = shell_monitor_copy (monitor);
  g_set_weak_pointer (&self->stage, shell_global_get_stage (global));
  g_set_weak_pointer (&self->workspace_manager, workspace_manager);

  constraint = shell_monitor_constraint_new ();
  shell_monitor_constraint_set_index (constraint, monitor->index);
  clutter_actor_add_constraint (CLUTTER_ACTOR (self), CLUTTER_CONSTRAINT (constraint));

  container = clutter_actor_new ();
  g_set_weak_pointer (&self->container, container);
  clutter_actor_add_child (CLUTTER_ACTOR (self), container);

  sticky_group = workspace_group_new (monitor, NULL, moving_window);
  clutter_actor_add_child (CLUTTER_ACTOR (self), sticky_group);

  vertical = monitor_group_is_vertical (self);
  rtl = clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL;
  active_workspace = meta_workspace_manager_get_active_workspace (workspace_manager);

  for (i = 0; i < n_workspace_indices; i++)
    {
      MetaWorkspace *ws =
        meta_workspace_manager_get_workspace_by_index (workspace_manager,
                                                       workspace_indices[i]);
      gboolean fullscreen = FALSE;
      ClutterActor *group;
      GList *windows, *l;

      windows = meta_workspace_list_windows (ws);
      for (l = windows; l != NULL; l = l->next)
        {
          MetaWindow *window = l->data;

          if (meta_window_get_monitor (window) == monitor->index &&
              meta_window_is_fullscreen (window))
            {
              fullscreen = TRUE;
              break;
            }
        }
      g_list_free (windows);

      if (workspace_indices[i] > 0 && vertical && !fullscreen &&
          monitor->index == shell_layout_manager_get_primary_index (layout_manager) &&
          panel != NULL)
        {
          /* We have to shift windows up or down by the height of the panel to prevent having a
           * visible gap between the windows while switching workspaces. Since fullscreen windows
           * hide the panel, they don't need to be shifted up or down. */
          y -= clutter_actor_get_height (CLUTTER_ACTOR (panel));
        }

      group = workspace_group_new (monitor, ws, moving_window);

      g_ptr_array_add (self->workspace_groups, group);
      clutter_actor_add_child (self->container, group);
      clutter_actor_set_position (group, x, y);

      if (vertical)
        y += monitor_group_get_base_distance (self);
      else if (rtl)
        x -= monitor_group_get_base_distance (self);
      else
        x += monitor_group_get_base_distance (self);
    }

  monitor_group_set_progress (self,
                              monitor_group_get_workspace_progress (self, active_workspace));

  if (monitor->index == shell_layout_manager_get_primary_index (layout_manager))
    {
      ProgressTransform *transform = g_new0 (ProgressTransform, 1);

      transform->indices = g_array_sized_new (FALSE, FALSE, sizeof (int),
                                              n_workspace_indices);
      g_array_append_vals (transform->indices, workspace_indices, n_workspace_indices);

      self->workspaces_adjustment =
        shell_window_manager_create_workspaces_adjustment (window_manager,
                                                           CLUTTER_ACTOR (self));
      g_object_bind_property_full (self, "progress",
                                   self->workspaces_adjustment, "value",
                                   G_BINDING_SYNC_CREATE,
                                   progress_to_adjustment_value, NULL,
                                   transform, progress_transform_free);
    }

  return CLUTTER_ACTOR (self);
}

/* ------------------------------------------------------------------------ */
/* ShellWorkspaceAnimationController                                        */
/* ------------------------------------------------------------------------ */

typedef struct
{
  GPtrArray *monitors;          /* of ShellWsaMonitorGroup, strong */
  gboolean gesture_activated;
  gboolean in_progress;
  ShellWsaMonitorGroup *base_monitor_group; /* not owned */
} SwitchData;

enum
{
  PROP_0,

  PROP_WINDOW_MANAGER,
  PROP_MOVING_WINDOW,
  PROP_GESTURE_ACTIVE,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellWorkspaceAnimationController
{
  GObject parent;

  ShellWindowManager *window_manager; /* weak */
  MetaWindow *moving_window;    /* weak */

  /* Weak pointers to what is needed without asking shell_global_get (),
   * which the teardown paths cannot rely on */
  ShellGlobal *global;
  MetaCompositor *compositor;
  MetaDisplay *display;
  MetaWorkspaceManager *workspace_manager;
  ClutterStage *stage;
  ClutterActor *window_group;

  SwitchData *switch_data;
  ShellSwipeTracker *swipe_tracker;
  GBinding *modifiers_binding;
  ClutterGrab *grab;
};

G_DEFINE_FINAL_TYPE (ShellWorkspaceAnimationController,
                     shell_workspace_animation_controller,
                     G_TYPE_OBJECT)

static void
controller_prepare_workspace_switch (ShellWorkspaceAnimationController *self,
                                     const int                         *workspace_indices,
                                     guint                              n_workspace_indices)
{
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  ClutterActor *ui_group = CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager));
  g_autofree int *all_indices = NULL;
  SwitchData *switch_data;
  int n_monitors, i;

  if (self->switch_data != NULL)
    return;

  switch_data = g_new0 (SwitchData, 1);
  switch_data->monitors = g_ptr_array_new_with_free_func (g_object_unref);
  self->switch_data = switch_data;

  if (workspace_indices == NULL)
    {
      n_workspace_indices = meta_workspace_manager_get_n_workspaces (self->workspace_manager);
      all_indices = g_new (int, n_workspace_indices);
      for (i = 0; i < (int) n_workspace_indices; i++)
        all_indices[i] = i;
      workspace_indices = all_indices;
    }

  n_monitors = shell_layout_manager_get_n_monitors (layout_manager);
  for (i = 0; i < n_monitors; i++)
    {
      ShellMonitor *monitor;
      ClutterActor *group;

      if (meta_prefs_get_workspaces_only_on_primary () &&
          i != shell_layout_manager_get_primary_index (layout_manager))
        continue;

      monitor = shell_layout_manager_get_monitor (layout_manager, i);
      if (monitor == NULL)
        continue;

      group = monitor_group_new (self->window_manager, monitor,
                                 workspace_indices, n_workspace_indices,
                                 self->moving_window);
      shell_monitor_free (monitor);

      clutter_actor_insert_child_above (ui_group, group, self->window_group);

      g_ptr_array_add (switch_data->monitors, g_object_ref (group));
    }

  shell_window_manager_block_workspace_updates (self->window_manager);
  meta_compositor_disable_unredirect (self->compositor);
  self->grab = g_object_ref (shell_main_push_modal (shell_main_get_default (),
                                                    CLUTTER_ACTOR (self->stage),
                                                    SHELL_ACTION_MODE_NORMAL));
}

static void
controller_finish_workspace_switch (ShellWorkspaceAnimationController *self,
                                    SwitchData                        *switch_data)
{
  g_autoptr (GPtrArray) monitors = NULL;
  guint i;

  if (self->switch_data != switch_data)
    return;

  if (self->window_manager != NULL)
    shell_window_manager_unblock_workspace_updates (self->window_manager);
  if (self->compositor != NULL)
    meta_compositor_enable_unredirect (self->compositor);

  if (self->grab != NULL)
    {
      g_autoptr (GError) error = NULL;

      if (!shell_main_pop_modal (shell_main_get_default (), self->grab, &error))
        g_warning ("%s: %s", G_STRFUNC, error->message);

      g_clear_object (&self->grab);
    }

  self->switch_data = NULL;

  monitors = g_steal_pointer (&switch_data->monitors);
  g_free (switch_data);

  for (i = 0; i < monitors->len; i++)
    clutter_actor_destroy (g_ptr_array_index (monitors, i));

  shell_workspace_animation_controller_set_moving_window (self, NULL);
}

/* The data of the completion of the animation of the primary monitor */
typedef struct
{
  ShellWorkspaceAnimationController *controller;
  SwitchData *switch_data;

  /* animate_switch () */
  ShellWorkspaceAnimationCallback on_complete;
  gpointer user_data;
  GDestroyNotify destroy;

  /* swipe end */
  MetaWorkspace *new_workspace;
  uint32_t end_time;
} CompleteData;

static void
complete_data_free (gpointer data)
{
  CompleteData *complete = data;

  if (complete->destroy != NULL)
    complete->destroy (complete->user_data);

  g_clear_object (&complete->new_workspace);
  g_clear_object (&complete->controller);
  g_free (complete);
}

static void
ease_progress (ShellWsaMonitorGroup *group,
               double                progress,
               guint                 duration,
               ShellEaseCallback     callback,
               gpointer              user_data,
               GDestroyNotify        destroy)
{
  g_autoptr (GError) error = NULL;
  g_auto (GValue) value = G_VALUE_INIT;
  ShellEaseParams params = {
    .duration = duration,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_CUBIC,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  g_value_init (&value, G_TYPE_DOUBLE);
  g_value_set_double (&value, progress);

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (group), "progress",
                                       &value, &params,
                                       callback, user_data, destroy, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

static void
animate_switch_complete_cb (gboolean  finished,
                            gpointer  user_data)
{
  CompleteData *complete = user_data;
  ShellWorkspaceAnimationController *self = complete->controller;

  if (!finished)
    return;

  controller_finish_workspace_switch (self, complete->switch_data);

  if (complete->on_complete != NULL)
    {
      ShellWorkspaceAnimationCallback on_complete = complete->on_complete;

      complete->on_complete = NULL;
      on_complete (complete->user_data);
    }

  if (self->swipe_tracker != NULL)
    shell_swipe_tracker_set_enabled (self->swipe_tracker, TRUE);
}

/**
 * shell_workspace_animation_controller_animate_switch:
 * @self: a #ShellWorkspaceAnimationController
 * @from: the index of the workspace which is left
 * @to: the index of the workspace which is activated
 * @direction: the direction of the switch
 * @on_complete: (scope notified) (nullable) (closure user_data) (destroy destroy):
 *   called when the animation is done and the clones are gone
 * @user_data: data for @on_complete
 * @destroy: called on @user_data once it is no longer needed
 *
 * Slides from the workspace @from to the workspace @to (`animateSwitch ()`).
 */
void
shell_workspace_animation_controller_animate_switch (ShellWorkspaceAnimationController *self,
                                                     int                                from,
                                                     int                                to,
                                                     MetaMotionDirection                direction,
                                                     ShellWorkspaceAnimationCallback    on_complete,
                                                     gpointer                           user_data,
                                                     GDestroyNotify                     destroy)
{
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  g_autoptr (GPtrArray) groups = NULL;
  MetaWorkspace *from_ws, *to_ws;
  SwitchData *switch_data;
  int workspace_indices[2] = { from, to };
  guint i;

  g_return_if_fail (SHELL_IS_WORKSPACE_ANIMATION_CONTROLLER (self));

  shell_swipe_tracker_set_enabled (self->swipe_tracker, FALSE);

  switch (direction)
    {
    case META_MOTION_UP:
    case META_MOTION_LEFT:
    case META_MOTION_UP_LEFT:
    case META_MOTION_UP_RIGHT:
      workspace_indices[0] = to;
      workspace_indices[1] = from;
      break;

    case META_MOTION_DOWN:
    case META_MOTION_RIGHT:
    case META_MOTION_DOWN_LEFT:
    case META_MOTION_DOWN_RIGHT:
      workspace_indices[0] = from;
      workspace_indices[1] = to;
      break;

    default:
      /* The JS left the list empty */
      workspace_indices[0] = from;
      workspace_indices[1] = to;
      break;
    }

  if (clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL &&
      direction != META_MOTION_UP &&
      direction != META_MOTION_DOWN)
    {
      int tmp = workspace_indices[0];

      workspace_indices[0] = workspace_indices[1];
      workspace_indices[1] = tmp;
    }

  controller_prepare_workspace_switch (self, workspace_indices, 2);
  switch_data = self->switch_data;
  switch_data->in_progress = TRUE;

  from_ws = meta_workspace_manager_get_workspace_by_index (self->workspace_manager, from);
  to_ws = meta_workspace_manager_get_workspace_by_index (self->workspace_manager, to);

  groups = g_ptr_array_ref (switch_data->monitors);
  for (i = 0; i < groups->len; i++)
    {
      ShellWsaMonitorGroup *group = g_ptr_array_index (groups, i);
      gboolean is_primary = group->monitor->index ==
                            shell_layout_manager_get_primary_index (layout_manager);
      double progress;

      /* The animation of an earlier monitor finished at once */
      if (self->switch_data != switch_data)
        break;

      monitor_group_set_progress (group, monitor_group_get_workspace_progress (group, from_ws));
      progress = monitor_group_get_workspace_progress (group, to_ws);

      if (is_primary)
        {
          CompleteData *complete = g_new0 (CompleteData, 1);

          complete->controller = g_object_ref (self);
          complete->switch_data = switch_data;
          complete->on_complete = on_complete;
          complete->user_data = user_data;
          complete->destroy = destroy;

          /* The callback is now owned by the data */
          on_complete = NULL;
          user_data = NULL;
          destroy = NULL;

          ease_progress (group, progress, WINDOW_ANIMATION_TIME,
                         animate_switch_complete_cb, complete, complete_data_free);
        }
      else
        {
          ease_progress (group, progress, WINDOW_ANIMATION_TIME, NULL, NULL, NULL);
        }
    }

  /* Nobody is waiting for the primary monitor (it has no group): do not
   * leave the window manager waiting for the callback */
  if (on_complete != NULL || destroy != NULL)
    {
      if (self->switch_data == switch_data)
        controller_finish_workspace_switch (self, switch_data);
      if (on_complete != NULL)
        on_complete (user_data);
      if (destroy != NULL)
        destroy (user_data);
      if (self->swipe_tracker != NULL)
        shell_swipe_tracker_set_enabled (self->swipe_tracker, TRUE);
    }
}

/**
 * shell_workspace_animation_controller_can_handle_scroll_event:
 * @self: a #ShellWorkspaceAnimationController
 * @event: a scroll event
 *
 * Returns: whether the swipe tracker handles the scroll @event, in which
 *   case the workspace scrolling of the window manager must leave it alone
 */
gboolean
shell_workspace_animation_controller_can_handle_scroll_event (ShellWorkspaceAnimationController *self,
                                                              const ClutterEvent                 *event)
{
  g_return_val_if_fail (SHELL_IS_WORKSPACE_ANIMATION_CONTROLLER (self), FALSE);

  return shell_swipe_tracker_can_handle_scroll_event (self->swipe_tracker, event);
}

static ShellWsaMonitorGroup *
controller_find_monitor_group (ShellWorkspaceAnimationController *self,
                               int                                monitor_index)
{
  guint i;

  if (self->switch_data == NULL)
    return NULL;

  for (i = 0; i < self->switch_data->monitors->len; i++)
    {
      ShellWsaMonitorGroup *group = g_ptr_array_index (self->switch_data->monitors, i);

      if (group->monitor->index == monitor_index)
        return group;
    }

  return NULL;
}

static int
compare_doubles (gconstpointer a,
                 gconstpointer b)
{
  double da = *(const double *) a;
  double db = *(const double *) b;

  return (da > db) - (da < db);
}

static void
switch_workspace_begin_cb (ShellSwipeTracker                 *tracker,
                           guint                              monitor,
                           ShellWorkspaceAnimationController *self)
{
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  ShellWsaMonitorGroup *monitor_group;
  g_autoptr (GArray) points = NULL;
  g_autoptr (GArray) sorted = NULL;
  MetaWorkspace *closest_ws;
  double base_distance, progress, cancel_progress;
  gboolean horiz;

  if (meta_prefs_get_workspaces_only_on_primary () &&
      (int) monitor != shell_layout_manager_get_primary_index (layout_manager))
    return;

  horiz = meta_workspace_manager_get_layout_rows (self->workspace_manager) != -1;
  shell_swipe_tracker_set_orientation (tracker,
                                       horiz ? CLUTTER_ORIENTATION_HORIZONTAL
                                             : CLUTTER_ORIENTATION_VERTICAL);

  if (self->switch_data != NULL && self->switch_data->gesture_activated)
    {
      guint i;

      for (i = 0; i < self->switch_data->monitors->len; i++)
        clutter_actor_remove_all_transitions (g_ptr_array_index (self->switch_data->monitors, i));
    }
  else
    {
      controller_prepare_workspace_switch (self, NULL, 0);
    }

  monitor_group = controller_find_monitor_group (self, monitor);
  if (monitor_group == NULL)
    return;

  base_distance = monitor_group_get_base_distance (monitor_group);
  progress = monitor_group_get_progress (monitor_group);

  closest_ws = monitor_group_find_closest_workspace (monitor_group, progress);
  cancel_progress = monitor_group_get_workspace_progress (monitor_group, closest_ws);
  points = monitor_group_get_snap_points (monitor_group);

  self->switch_data->base_monitor_group = monitor_group;

  if (points->len == 0)
    return;

  /* The tracker wants the snap points in ascending order */
  sorted = g_array_sized_new (FALSE, FALSE, sizeof (double), points->len);
  g_array_append_vals (sorted, points->data, points->len);
  g_array_sort (sorted, compare_doubles);

  shell_swipe_tracker_confirm_swipe (tracker, base_distance,
                                     (const double *) sorted->data, sorted->len,
                                     progress, cancel_progress);
}

static void
switch_workspace_update_cb (ShellSwipeTracker                 *tracker,
                            double                             progress,
                            ShellWorkspaceAnimationController *self)
{
  guint i;

  if (self->switch_data == NULL || self->switch_data->base_monitor_group == NULL)
    return;

  for (i = 0; i < self->switch_data->monitors->len; i++)
    monitor_group_update_swipe_for_monitor (g_ptr_array_index (self->switch_data->monitors, i),
                                            progress,
                                            self->switch_data->base_monitor_group);
}

static void
switch_workspace_end_complete_cb (gboolean  finished,
                                  gpointer  user_data)
{
  CompleteData *complete = user_data;

  if (!finished)
    return;

  if (complete->controller->workspace_manager != NULL &&
      meta_workspace_manager_get_active_workspace (complete->controller->workspace_manager) !=
      complete->new_workspace)
    meta_workspace_activate (complete->new_workspace, complete->end_time);
  controller_finish_workspace_switch (complete->controller, complete->switch_data);
}

static void
switch_workspace_end_cb (ShellSwipeTracker                 *tracker,
                         guint64                            duration,
                         double                             end_progress,
                         ShellWorkspaceAnimationController *self)
{
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  g_autoptr (GPtrArray) groups = NULL;
  SwitchData *switch_data = self->switch_data;
  MetaWorkspace *new_ws;
  uint32_t end_time;
  guint i;

  if (switch_data == NULL || switch_data->base_monitor_group == NULL)
    return;

  switch_data->gesture_activated = TRUE;

  new_ws = monitor_group_find_closest_workspace (switch_data->base_monitor_group,
                                                 end_progress);
  end_time = clutter_get_current_event_time ();

  groups = g_ptr_array_ref (switch_data->monitors);
  for (i = 0; i < groups->len; i++)
    {
      ShellWsaMonitorGroup *group = g_ptr_array_index (groups, i);
      double progress;

      if (self->switch_data != switch_data)
        break;

      progress = monitor_group_get_workspace_progress (group, new_ws);

      if (group->monitor->index == shell_layout_manager_get_primary_index (layout_manager))
        {
          CompleteData *complete = g_new0 (CompleteData, 1);

          complete->controller = g_object_ref (self);
          complete->switch_data = switch_data;
          complete->new_workspace = g_object_ref (new_ws);
          complete->end_time = end_time;

          ease_progress (group, progress, duration,
                         switch_workspace_end_complete_cb, complete, complete_data_free);
        }
      else
        {
          ease_progress (group, progress, duration, NULL, NULL, NULL);
        }
    }
}

/**
 * shell_workspace_animation_controller_get_gesture_active:
 * @self: a #ShellWorkspaceAnimationController
 *
 * Returns: whether a swipe has ended and the animation to the workspace it
 *   chose is running (`gestureActive`)
 */
gboolean
shell_workspace_animation_controller_get_gesture_active (ShellWorkspaceAnimationController *self)
{
  g_return_val_if_fail (SHELL_IS_WORKSPACE_ANIMATION_CONTROLLER (self), FALSE);

  return self->switch_data != NULL && self->switch_data->gesture_activated;
}

/**
 * shell_workspace_animation_controller_cancel_switch_animation:
 * @self: a #ShellWorkspaceAnimationController
 *
 * Ends the animation of a keybinding at once (`cancelSwitchAnimation ()`).
 * A swipe which already ended is left to finish.
 */
void
shell_workspace_animation_controller_cancel_switch_animation (ShellWorkspaceAnimationController *self)
{
  g_return_if_fail (SHELL_IS_WORKSPACE_ANIMATION_CONTROLLER (self));

  if (self->switch_data == NULL)
    return;

  if (self->switch_data->gesture_activated)
    return;

  controller_finish_workspace_switch (self, self->switch_data);
}

/**
 * shell_workspace_animation_controller_get_moving_window:
 * @self: a #ShellWorkspaceAnimationController
 *
 * Returns: (transfer none) (nullable): the window which is being moved to
 *   another workspace, which stays on top of the strip
 */
MetaWindow *
shell_workspace_animation_controller_get_moving_window (ShellWorkspaceAnimationController *self)
{
  g_return_val_if_fail (SHELL_IS_WORKSPACE_ANIMATION_CONTROLLER (self), NULL);

  return self->moving_window;
}

/**
 * shell_workspace_animation_controller_set_moving_window:
 * @self: a #ShellWorkspaceAnimationController
 * @moving_window: (nullable): the window which is being moved to another
 *   workspace
 *
 * Sets #ShellWorkspaceAnimationController:moving-window.
 */
void
shell_workspace_animation_controller_set_moving_window (ShellWorkspaceAnimationController *self,
                                                        MetaWindow                        *moving_window)
{
  g_return_if_fail (SHELL_IS_WORKSPACE_ANIMATION_CONTROLLER (self));
  g_return_if_fail (moving_window == NULL || META_IS_WINDOW (moving_window));

  if (self->moving_window == moving_window)
    return;

  g_clear_weak_pointer (&self->moving_window);
  if (moving_window != NULL)
    g_set_weak_pointer (&self->moving_window, moving_window);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_MOVING_WINDOW]);
}

static void
overview_visible_changed_cb (ShellWindowManager                *window_manager,
                             GParamSpec                        *pspec,
                             ShellWorkspaceAnimationController *self)
{
  /* 'showing' */
  if (!shell_window_manager_get_overview_visible (window_manager))
    return;

  if (self->switch_data != NULL && self->switch_data->gesture_activated)
    controller_finish_workspace_switch (self, self->switch_data);

  shell_swipe_tracker_set_enabled (self->swipe_tracker, FALSE);
}

static void
overview_closing_changed_cb (ShellWindowManager                *window_manager,
                             GParamSpec                        *pspec,
                             ShellWorkspaceAnimationController *self)
{
  /* 'hiding' */
  if (!shell_window_manager_get_overview_closing (window_manager))
    return;

  shell_swipe_tracker_set_enabled (self->swipe_tracker, TRUE);
}

static void
shell_workspace_animation_controller_get_property (GObject    *object,
                                                   guint       prop_id,
                                                   GValue     *value,
                                                   GParamSpec *pspec)
{
  ShellWorkspaceAnimationController *self = SHELL_WORKSPACE_ANIMATION_CONTROLLER (object);

  switch (prop_id)
    {
    case PROP_WINDOW_MANAGER:
      g_value_set_object (value, self->window_manager);
      break;

    case PROP_MOVING_WINDOW:
      g_value_set_object (value, self->moving_window);
      break;

    case PROP_GESTURE_ACTIVE:
      g_value_set_boolean (value,
                           shell_workspace_animation_controller_get_gesture_active (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_workspace_animation_controller_set_property (GObject      *object,
                                                   guint         prop_id,
                                                   const GValue *value,
                                                   GParamSpec   *pspec)
{
  ShellWorkspaceAnimationController *self = SHELL_WORKSPACE_ANIMATION_CONTROLLER (object);

  switch (prop_id)
    {
    case PROP_WINDOW_MANAGER:
      g_set_weak_pointer (&self->window_manager, g_value_get_object (value));
      break;

    case PROP_MOVING_WINDOW:
      shell_workspace_animation_controller_set_moving_window (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_workspace_animation_controller_constructed (GObject *object)
{
  ShellWorkspaceAnimationController *self = SHELL_WORKSPACE_ANIMATION_CONTROLLER (object);
  ShellGlobal *global = shell_global_get ();

  G_OBJECT_CLASS (shell_workspace_animation_controller_parent_class)->constructed (object);

  g_set_weak_pointer (&self->global, global);
  g_set_weak_pointer (&self->compositor, shell_global_get_compositor (global));
  g_set_weak_pointer (&self->display, shell_global_get_display (global));
  g_set_weak_pointer (&self->workspace_manager, shell_global_get_workspace_manager (global));
  g_set_weak_pointer (&self->stage, shell_global_get_stage (global));
  g_set_weak_pointer (&self->window_group, shell_global_get_window_group (global));

  if (self->window_manager != NULL)
    {
      g_signal_connect_object (self->window_manager, "notify::overview-visible",
                               G_CALLBACK (overview_visible_changed_cb), self, 0);
      g_signal_connect_object (self->window_manager, "notify::overview-closing",
                               G_CALLBACK (overview_closing_changed_cb), self, 0);
    }

  self->swipe_tracker = g_object_new (SHELL_TYPE_SWIPE_TRACKER,
                                      "actor", self->stage,
                                      "orientation", CLUTTER_ORIENTATION_HORIZONTAL,
                                      "allowed-modes", SHELL_ACTION_MODE_NORMAL,
                                      "allow-drag", FALSE,
                                      "phase", CLUTTER_PHASE_CAPTURE,
                                      "name", "WorkspaceAnimation swipe tracker",
                                      NULL);
  g_signal_connect (self->swipe_tracker, "begin",
                    G_CALLBACK (switch_workspace_begin_cb), self);
  g_signal_connect (self->swipe_tracker, "update",
                    G_CALLBACK (switch_workspace_update_cb), self);
  g_signal_connect (self->swipe_tracker, "end",
                    G_CALLBACK (switch_workspace_end_cb), self);

  self->modifiers_binding = g_object_bind_property (self->display, "compositor-modifiers",
                                                    self->swipe_tracker, "scroll-modifiers",
                                                    G_BINDING_SYNC_CREATE);
  g_object_add_weak_pointer (G_OBJECT (self->modifiers_binding),
                             (gpointer *) &self->modifiers_binding);
}

static void
shell_workspace_animation_controller_dispose (GObject *object)
{
  ShellWorkspaceAnimationController *self = SHELL_WORKSPACE_ANIMATION_CONTROLLER (object);

  if (self->modifiers_binding != NULL)
    g_binding_unbind (self->modifiers_binding);

  if (self->swipe_tracker != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->swipe_tracker, self);
      shell_swipe_tracker_destroy (self->swipe_tracker);
      g_clear_object (&self->swipe_tracker);
    }

  /* A switch which is still running dies with the shell: its actors are
   * destroyed with the stage. No signal may reach the other objects. */
  if (self->switch_data != NULL)
    {
      g_clear_pointer (&self->switch_data->monitors, g_ptr_array_unref);
      g_clear_pointer (&self->switch_data, g_free);
    }
  g_clear_object (&self->grab);

  g_clear_weak_pointer (&self->window_manager);
  g_clear_weak_pointer (&self->moving_window);
  g_clear_weak_pointer (&self->global);
  g_clear_weak_pointer (&self->compositor);
  g_clear_weak_pointer (&self->display);
  g_clear_weak_pointer (&self->workspace_manager);
  g_clear_weak_pointer (&self->stage);
  g_clear_weak_pointer (&self->window_group);

  G_OBJECT_CLASS (shell_workspace_animation_controller_parent_class)->dispose (object);
}

static void
shell_workspace_animation_controller_class_init (ShellWorkspaceAnimationControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_workspace_animation_controller_constructed;
  object_class->dispose = shell_workspace_animation_controller_dispose;
  object_class->get_property = shell_workspace_animation_controller_get_property;
  object_class->set_property = shell_workspace_animation_controller_set_property;

  /**
   * ShellWorkspaceAnimationController:window-manager:
   *
   * The #ShellWindowManager which owns the controller. The controller blocks
   * its workspace updates during a switch and follows the overview state it
   * is told about.
   */
  props[PROP_WINDOW_MANAGER] =
    g_param_spec_object ("window-manager", NULL, NULL,
                         SHELL_TYPE_WINDOW_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellWorkspaceAnimationController:moving-window:
   *
   * The window which is being moved to another workspace
   * (`movingWindow`): it is shown on top of all workspaces during the
   * animation. Cleared when the animation ends. Held weakly.
   */
  props[PROP_MOVING_WINDOW] =
    g_param_spec_object ("moving-window", NULL, NULL,
                         META_TYPE_WINDOW,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellWorkspaceAnimationController:gesture-active:
   *
   * Whether a swipe has ended and the animation to the workspace it chose is
   * still running (`gestureActive`). Not notified.
   */
  props[PROP_GESTURE_ACTIVE] =
    g_param_spec_boolean ("gesture-active", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_workspace_animation_controller_init (ShellWorkspaceAnimationController *self)
{
}

/**
 * shell_workspace_animation_controller_new:
 * @window_manager: the #ShellWindowManager which owns the controller
 *
 * Creates the controller (`new WorkspaceAnimationController ()`). It sets up
 * the swipe tracker on the stage right away.
 *
 * Returns: (transfer full): a new #ShellWorkspaceAnimationController
 */
ShellWorkspaceAnimationController *
shell_workspace_animation_controller_new (ShellWindowManager *window_manager)
{
  return g_object_new (SHELL_TYPE_WORKSPACE_ANIMATION_CONTROLLER,
                       "window-manager", window_manager,
                       NULL);
}
