/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * osd-window.c: The on-screen display shown when the volume or brightness
 * changes, one per monitor. Port of js/ui/osdWindow.js.
 *
 * #ShellOsdWindow covers its monitor (what Layout.MonitorConstraint({index})
 * did in JS) and shows an icon, an optional label and an optional level bar
 * centered at the bottom of it. #ShellOsdWindowManager owns one window per
 * monitor of the display.
 */

#include "config.h"

#include <math.h>

#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-backend.h>
#include <meta/meta-monitor-manager.h>

#include "shell-global.h"
#include "ui/bar-level.h"
#include "ui/environment.h"
#include "ui/osd-window.h"

#define HIDE_TIMEOUT 1500
#define FADE_TIME 100
#define LEVEL_ANIMATION_TIME 100

/* ShellOsdWindow */

enum
{
  PROP_0,

  PROP_MONITOR_INDEX,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellOsdWindow
{
  StWidget parent;

  int monitor_index;

  StWidget *hbox;
  StIcon *icon;
  StWidget *vbox;
  StLabel *label;
  ShellBarLevel *level;

  guint hide_timeout_id;
};

G_DEFINE_FINAL_TYPE (ShellOsdWindow, shell_osd_window, ST_TYPE_WIDGET)

static void
set_actor_visible (ClutterActor *actor,
                   gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

static void
update_box_visibility (ShellOsdWindow *self)
{
  ClutterActor *child;
  gboolean any_visible = FALSE;

  for (child = clutter_actor_get_first_child (CLUTTER_ACTOR (self->vbox));
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      if (clutter_actor_is_visible (child))
        {
          any_visible = TRUE;
          break;
        }
    }

  set_actor_visible (CLUTTER_ACTOR (self->vbox), any_visible);
}

static void
reset (ShellOsdWindow *self)
{
  clutter_actor_hide (CLUTTER_ACTOR (self));
  shell_osd_window_set_label (self, NULL);
  shell_osd_window_set_max_level (self, 1);
  shell_osd_window_set_level (self, NAN);
}

static void
hide_complete_cb (gboolean finished,
                  gpointer user_data)
{
  ShellOsdWindow *self = user_data;

  /* onComplete: only when the animation ran to its end */
  if (!finished)
    return;

  reset (self);
  meta_compositor_enable_unredirect (shell_global_get_compositor (shell_global_get ()));
}

static void
hide_osd (ShellOsdWindow *self)
{
  ShellEaseParams params = {
    .duration = FADE_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  self->hide_timeout_id = 0;

  shell_actor_ease (CLUTTER_ACTOR (self), &params,
                    hide_complete_cb, self, NULL,
                    "opacity", 0u,
                    NULL);
}

static void
hide_timeout_cb (gpointer user_data)
{
  hide_osd (SHELL_OSD_WINDOW (user_data));
}

static void
on_shutdown (ShellGlobal    *global,
             ShellOsdWindow *self)
{
  /* Nothing may be eased (and complete into the compositor) while the shell
   * is going away. */
  g_clear_handle_id (&self->hide_timeout_id, g_source_remove);
}

/**
 * shell_osd_window_get_monitor_index:
 * @self: a #ShellOsdWindow
 *
 * Returns: the index of the monitor the window covers
 */
int
shell_osd_window_get_monitor_index (ShellOsdWindow *self)
{
  g_return_val_if_fail (SHELL_IS_OSD_WINDOW (self), -1);

  return self->monitor_index;
}

/**
 * shell_osd_window_set_icon:
 * @self: a #ShellOsdWindow
 * @icon: (nullable): the icon to show. The window is not shown without one
 *
 * Sets the icon of the window.
 */
void
shell_osd_window_set_icon (ShellOsdWindow *self,
                           GIcon          *icon)
{
  g_return_if_fail (SHELL_IS_OSD_WINDOW (self));
  g_return_if_fail (icon == NULL || G_IS_ICON (icon));

  st_icon_set_gicon (self->icon, icon);
}

/**
 * shell_osd_window_set_label:
 * @self: a #ShellOsdWindow
 * @label: (nullable): the text of the label, or %NULL to hide it
 *
 * Sets the label of the window.
 */
void
shell_osd_window_set_label (ShellOsdWindow *self,
                            const char     *label)
{
  g_return_if_fail (SHELL_IS_OSD_WINDOW (self));

  set_actor_visible (CLUTTER_ACTOR (self->label), label != NULL);
  if (label != NULL)
    st_label_set_text (self->label, label);

  update_box_visibility (self);
}

/**
 * shell_osd_window_set_level:
 * @self: a #ShellOsdWindow
 * @level: the new value of the level bar, or NaN to hide it
 *
 * Sets the value of the level bar. The bar animates to the new value if the
 * window is already visible, otherwise it jumps to it.
 */
void
shell_osd_window_set_level (ShellOsdWindow *self,
                            double          level)
{
  gboolean has_level = !isnan (level);

  g_return_if_fail (SHELL_IS_OSD_WINDOW (self));

  set_actor_visible (CLUTTER_ACTOR (self->level), has_level);
  if (has_level)
    {
      if (clutter_actor_is_visible (CLUTTER_ACTOR (self)))
        {
          ShellEaseParams params = {
            .duration = LEVEL_ANIMATION_TIME,
            .delay = -1,
            .mode = CLUTTER_EASE_OUT_QUAD,
            .repeat_count = 0,
            .auto_reverse = FALSE,
            .animation_required = FALSE,
          };
          g_auto (GValue) target = G_VALUE_INIT;
          g_autoptr (GError) error = NULL;

          g_value_init (&target, G_TYPE_DOUBLE);
          g_value_set_double (&target, level);

          if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (self->level),
                                               "value", &target, &params,
                                               NULL, NULL, NULL, &error))
            g_warning ("%s: %s", G_STRFUNC, error->message);
        }
      else
        {
          shell_bar_level_set_value (self->level, level);
        }
    }

  update_box_visibility (self);
}

/**
 * shell_osd_window_set_max_level:
 * @self: a #ShellOsdWindow
 * @max_level: the maximum value of the level bar. Values below 1 mean 1
 *
 * Sets the maximum value of the level bar.
 */
void
shell_osd_window_set_max_level (ShellOsdWindow *self,
                                double          max_level)
{
  g_return_if_fail (SHELL_IS_OSD_WINDOW (self));

  shell_bar_level_set_maximum_value (self->level, max_level);
}

/**
 * shell_osd_window_show_osd:
 * @self: a #ShellOsdWindow
 *
 * Shows the window, fading it in, and arms the timeout that fades it out
 * again. Does nothing if the window has no icon. This is Shell.OsdWindow's
 * counterpart of OsdWindow.show() in JS; it has another name so that it does
 * not shadow Clutter.Actor.show().
 */
void
shell_osd_window_show_osd (ShellOsdWindow *self)
{
  ClutterActor *actor;

  g_return_if_fail (SHELL_IS_OSD_WINDOW (self));

  if (st_icon_get_gicon (self->icon) == NULL)
    return;

  actor = CLUTTER_ACTOR (self);

  if (!clutter_actor_is_visible (actor))
    {
      ShellEaseParams params = {
        .duration = FADE_TIME,
        .delay = -1,
        .mode = CLUTTER_EASE_OUT_QUAD,
        .repeat_count = 0,
        .auto_reverse = FALSE,
        .animation_required = FALSE,
      };
      ClutterActor *parent = clutter_actor_get_parent (actor);

      meta_compositor_disable_unredirect (shell_global_get_compositor (shell_global_get ()));
      clutter_actor_show (actor);
      clutter_actor_set_opacity (actor, 0);
      if (parent != NULL)
        clutter_actor_set_child_above_sibling (parent, actor, NULL);

      shell_actor_ease (actor, &params,
                        NULL, NULL, NULL,
                        "opacity", 255u,
                        NULL);
    }

  g_clear_handle_id (&self->hide_timeout_id, g_source_remove);
  self->hide_timeout_id = g_timeout_add_once (HIDE_TIMEOUT,
                                              hide_timeout_cb, self);
  g_source_set_name_by_id (self->hide_timeout_id,
                           "[gnome-shell] shell_osd_window_hide");
}

/**
 * shell_osd_window_cancel:
 * @self: a #ShellOsdWindow
 *
 * Fades the window out at once, if it is showing and waiting for its
 * timeout.
 */
void
shell_osd_window_cancel (ShellOsdWindow *self)
{
  g_return_if_fail (SHELL_IS_OSD_WINDOW (self));

  if (self->hide_timeout_id == 0)
    return;

  g_clear_handle_id (&self->hide_timeout_id, g_source_remove);
  hide_osd (self);
}

static void
shell_osd_window_allocate (ClutterActor          *actor,
                           const ClutterActorBox *box)
{
  ShellOsdWindow *self = SHELL_OSD_WINDOW (actor);
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  ClutterActorBox monitor_box;
  int n_monitors = meta_display_get_n_monitors (display);

  /* The Layout.MonitorConstraint({index}) of the JS window: the allocation
   * is the geometry of the monitor, and the monitor index is clamped to the
   * existing monitors. */
  if (n_monitors > 0)
    {
      MtkRectangle geometry;

      meta_display_get_monitor_geometry (display,
                                         MIN (self->monitor_index, n_monitors - 1),
                                         &geometry);
      clutter_actor_box_init_rect (&monitor_box,
                                   geometry.x, geometry.y,
                                   geometry.width, geometry.height);
      box = &monitor_box;
    }

  CLUTTER_ACTOR_CLASS (shell_osd_window_parent_class)->allocate (actor, box);
}

static void
shell_osd_window_constructed (GObject *object)
{
  ShellOsdWindow *self = SHELL_OSD_WINDOW (object);

  G_OBJECT_CLASS (shell_osd_window_parent_class)->constructed (object);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);
}

static void
shell_osd_window_dispose (GObject *object)
{
  ShellOsdWindow *self = SHELL_OSD_WINDOW (object);

  g_clear_handle_id (&self->hide_timeout_id, g_source_remove);

  G_OBJECT_CLASS (shell_osd_window_parent_class)->dispose (object);
}

static void
shell_osd_window_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  ShellOsdWindow *self = SHELL_OSD_WINDOW (object);

  switch (prop_id)
    {
    case PROP_MONITOR_INDEX:
      g_value_set_int (value, self->monitor_index);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_osd_window_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  ShellOsdWindow *self = SHELL_OSD_WINDOW (object);

  switch (prop_id)
    {
    case PROP_MONITOR_INDEX:
      self->monitor_index = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_osd_window_class_init (ShellOsdWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_osd_window_constructed;
  object_class->dispose = shell_osd_window_dispose;
  object_class->get_property = shell_osd_window_get_property;
  object_class->set_property = shell_osd_window_set_property;

  actor_class->allocate = shell_osd_window_allocate;

  /**
   * ShellOsdWindow:monitor-index:
   *
   * The index of the monitor the window covers.
   */
  props[PROP_MONITOR_INDEX] =
    g_param_spec_int ("monitor-index", NULL, NULL,
                      0, G_MAXINT, 0,
                      G_PARAM_READWRITE |
                      G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_osd_window_init (ShellOsdWindow *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  /* The window itself covers a whole monitor (see allocate ()); its box is
   * aligned inside it. */
  clutter_actor_set_x_expand (actor, TRUE);
  clutter_actor_set_y_expand (actor, TRUE);
  clutter_actor_set_x_align (actor, CLUTTER_ACTOR_ALIGN_CENTER);
  clutter_actor_set_y_align (actor, CLUTTER_ACTOR_ALIGN_END);
  clutter_actor_set_layout_manager (actor, clutter_bin_layout_new ());

  self->hbox = g_object_new (ST_TYPE_BOX_LAYOUT,
                             "style-class", "osd-window",
                             "x-expand", TRUE,
                             "y-expand", TRUE,
                             "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             "y-align", CLUTTER_ACTOR_ALIGN_END,
                             NULL);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (self->hbox));

  self->icon = g_object_new (ST_TYPE_ICON, "y-expand", TRUE, NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self->hbox), CLUTTER_ACTOR (self->icon));

  self->vbox = g_object_new (ST_TYPE_BOX_LAYOUT,
                             "orientation", CLUTTER_ORIENTATION_VERTICAL,
                             "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self->hbox), CLUTTER_ACTOR (self->vbox));

  self->label = ST_LABEL (st_label_new (NULL));
  clutter_actor_add_child (CLUTTER_ACTOR (self->vbox), CLUTTER_ACTOR (self->label));

  self->level = g_object_new (SHELL_TYPE_BAR_LEVEL,
                              "style-class", "level",
                              "value", 0.0,
                              NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self->vbox), CLUTTER_ACTOR (self->level));

  reset (self);
}

/**
 * shell_osd_window_new:
 * @monitor_index: the index of the monitor the window covers
 *
 * Creates a hidden OSD window. The caller adds it to the stage.
 *
 * Returns: (transfer full): a new #ShellOsdWindow
 */
ShellOsdWindow *
shell_osd_window_new (int monitor_index)
{
  g_return_val_if_fail (monitor_index >= 0, NULL);

  return g_object_new (SHELL_TYPE_OSD_WINDOW,
                       "monitor-index", monitor_index,
                       NULL);
}

/* ShellOsdWindowManager */

enum
{
  MGR_PROP_0,

  MGR_PROP_UI_GROUP,

  MGR_N_PROPS
};

static GParamSpec *mgr_props[MGR_N_PROPS] = { NULL, };

struct _ShellOsdWindowManager
{
  GObject parent;

  ClutterActor *ui_group; /* weak */

  /* ShellOsdWindow *, NULL for a window that was destroyed by someone
   * else. The windows are owned by ui_group. */
  GPtrArray *windows;
};

G_DEFINE_FINAL_TYPE (ShellOsdWindowManager, shell_osd_window_manager, G_TYPE_OBJECT)

static void
window_destroyed_cb (ClutterActor          *window,
                     ShellOsdWindowManager *self)
{
  guint i;

  for (i = 0; i < self->windows->len; i++)
    {
      if (g_ptr_array_index (self->windows, i) == (gpointer) window)
        self->windows->pdata[i] = NULL;
    }
}

static void
destroy_window (ShellOsdWindowManager *self,
                guint                  index)
{
  ClutterActor *window = g_ptr_array_index (self->windows, index);

  if (window == NULL)
    return;

  self->windows->pdata[index] = NULL;
  g_signal_handlers_disconnect_by_func (window, window_destroyed_cb, self);
  clutter_actor_destroy (window);
}

static void
monitors_changed (ShellOsdWindowManager *self)
{
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  guint n_monitors = meta_display_get_n_monitors (display);
  guint i;

  if (self->ui_group == NULL)
    return;

  /* Mirrors Main.layoutManager.monitors, see LayoutManager._updateMonitors () */
  for (i = n_monitors; i < self->windows->len; i++)
    destroy_window (self, i);

  g_ptr_array_set_size (self->windows, n_monitors);

  for (i = 0; i < n_monitors; i++)
    {
      ShellOsdWindow *window;

      if (g_ptr_array_index (self->windows, i) != NULL)
        {
          /* the monitor may have a new geometry */
          clutter_actor_queue_relayout (g_ptr_array_index (self->windows, i));
          continue;
        }

      window = shell_osd_window_new (i);
      g_signal_connect (window, "destroy",
                        G_CALLBACK (window_destroyed_cb), self);
      clutter_actor_add_child (self->ui_group, CLUTTER_ACTOR (window));
      self->windows->pdata[i] = window;
    }
}

static void
monitors_changed_cb (MetaMonitorManager   *monitor_manager,
                     ShellOsdWindowManager *self)
{
  monitors_changed (self);
}

static void
show_window (ShellOsdWindowManager *self,
             guint                  index,
             GIcon                 *icon,
             const char            *label,
             double                 level,
             double                 max_level)
{
  ShellOsdWindow *window = g_ptr_array_index (self->windows, index);

  if (window == NULL)
    return;

  shell_osd_window_set_icon (window, icon);
  shell_osd_window_set_label (window, label);
  shell_osd_window_set_max_level (window, max_level);
  shell_osd_window_set_level (window, level);
  shell_osd_window_show_osd (window);
}

/**
 * shell_osd_window_manager_show:
 * @self: a #ShellOsdWindowManager
 * @icon: (nullable): the icon to show
 * @label: (nullable): the label to show
 * @levels: a variant of type `a(idd)` with one `(monitor, level, max-level)`
 *   for every monitor to show the OSD on. The OSD is hidden on the other
 *   monitors. A NaN level shows no level bar, a maximum level below 1 means 1
 *
 * Shows the OSD on the monitors given in @levels, and cancels it everywhere
 * else.
 */
void
shell_osd_window_manager_show (ShellOsdWindowManager *self,
                               GIcon                 *icon,
                               const char            *label,
                               GVariant              *levels)
{
  guint i;

  g_return_if_fail (SHELL_IS_OSD_WINDOW_MANAGER (self));
  g_return_if_fail (icon == NULL || G_IS_ICON (icon));
  g_return_if_fail (levels != NULL &&
                    g_variant_is_of_type (levels, G_VARIANT_TYPE ("a(idd)")));

  for (i = 0; i < self->windows->len; i++)
    {
      GVariantIter iter;
      int monitor;
      double level, max_level;
      double show_level = NAN, show_max_level = 1;
      gboolean found = FALSE;

      /* a later entry for the same monitor wins */
      g_variant_iter_init (&iter, levels);
      while (g_variant_iter_next (&iter, "(idd)", &monitor, &level, &max_level))
        {
          if (monitor == (int) i)
            {
              found = TRUE;
              show_level = level;
              show_max_level = max_level;
            }
        }

      if (found)
        show_window (self, i, icon, label, show_level, show_max_level);
      else if (g_ptr_array_index (self->windows, i) != NULL)
        shell_osd_window_cancel (g_ptr_array_index (self->windows, i));
    }
}

/**
 * shell_osd_window_manager_show_one:
 * @self: a #ShellOsdWindowManager
 * @monitor_index: the monitor to show the OSD on. The OSD is hidden on every
 *   other monitor, which for an unknown monitor (-1) means on all of them
 * @icon: (nullable): the icon to show
 * @label: (nullable): the label to show
 * @level: the value of the level bar, or NaN for no level bar
 * @max_level: the maximum value of the level bar, values below 1 mean 1
 *
 * Shows the OSD on a single monitor.
 */
void
shell_osd_window_manager_show_one (ShellOsdWindowManager *self,
                                   int                    monitor_index,
                                   GIcon                 *icon,
                                   const char            *label,
                                   double                 level,
                                   double                 max_level)
{
  guint i;

  g_return_if_fail (SHELL_IS_OSD_WINDOW_MANAGER (self));
  g_return_if_fail (icon == NULL || G_IS_ICON (icon));

  for (i = 0; i < self->windows->len; i++)
    {
      if ((int) i == monitor_index)
        show_window (self, i, icon, label, level, max_level);
      else if (g_ptr_array_index (self->windows, i) != NULL)
        shell_osd_window_cancel (g_ptr_array_index (self->windows, i));
    }
}

/**
 * shell_osd_window_manager_show_all:
 * @self: a #ShellOsdWindowManager
 * @icon: (nullable): the icon to show
 * @label: (nullable): the label to show
 * @level: the value of the level bar, or NaN for no level bar
 * @max_level: the maximum value of the level bar, values below 1 mean 1
 *
 * Shows the OSD on every monitor.
 */
void
shell_osd_window_manager_show_all (ShellOsdWindowManager *self,
                                   GIcon                 *icon,
                                   const char            *label,
                                   double                 level,
                                   double                 max_level)
{
  guint i;

  g_return_if_fail (SHELL_IS_OSD_WINDOW_MANAGER (self));
  g_return_if_fail (icon == NULL || G_IS_ICON (icon));

  for (i = 0; i < self->windows->len; i++)
    show_window (self, i, icon, label, level, max_level);
}

/**
 * shell_osd_window_manager_hide_all:
 * @self: a #ShellOsdWindowManager
 *
 * Fades the OSD out on every monitor.
 */
void
shell_osd_window_manager_hide_all (ShellOsdWindowManager *self)
{
  guint i;

  g_return_if_fail (SHELL_IS_OSD_WINDOW_MANAGER (self));

  for (i = 0; i < self->windows->len; i++)
    {
      if (g_ptr_array_index (self->windows, i) != NULL)
        shell_osd_window_cancel (g_ptr_array_index (self->windows, i));
    }
}

static void
shell_osd_window_manager_constructed (GObject *object)
{
  ShellOsdWindowManager *self = SHELL_OSD_WINDOW_MANAGER (object);
  MetaBackend *backend = shell_global_get_backend (shell_global_get ());

  G_OBJECT_CLASS (shell_osd_window_manager_parent_class)->constructed (object);

  if (self->ui_group == NULL)
    {
      g_critical ("ShellOsdWindowManager needs a ui-group");
      return;
    }

  g_signal_connect_object (meta_backend_get_monitor_manager (backend),
                           "monitors-changed",
                           G_CALLBACK (monitors_changed_cb), self, 0);
  monitors_changed (self);
}

static void
shell_osd_window_manager_dispose (GObject *object)
{
  ShellOsdWindowManager *self = SHELL_OSD_WINDOW_MANAGER (object);
  guint i;

  for (i = 0; i < self->windows->len; i++)
    destroy_window (self, i);

  g_clear_weak_pointer (&self->ui_group);

  G_OBJECT_CLASS (shell_osd_window_manager_parent_class)->dispose (object);
}

static void
shell_osd_window_manager_finalize (GObject *object)
{
  ShellOsdWindowManager *self = SHELL_OSD_WINDOW_MANAGER (object);

  g_ptr_array_free (self->windows, TRUE);

  G_OBJECT_CLASS (shell_osd_window_manager_parent_class)->finalize (object);
}

static void
shell_osd_window_manager_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellOsdWindowManager *self = SHELL_OSD_WINDOW_MANAGER (object);

  switch (prop_id)
    {
    case MGR_PROP_UI_GROUP:
      g_value_set_object (value, self->ui_group);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_osd_window_manager_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellOsdWindowManager *self = SHELL_OSD_WINDOW_MANAGER (object);

  switch (prop_id)
    {
    case MGR_PROP_UI_GROUP:
      g_set_weak_pointer (&self->ui_group, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_osd_window_manager_class_init (ShellOsdWindowManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_osd_window_manager_constructed;
  object_class->dispose = shell_osd_window_manager_dispose;
  object_class->finalize = shell_osd_window_manager_finalize;
  object_class->get_property = shell_osd_window_manager_get_property;
  object_class->set_property = shell_osd_window_manager_set_property;

  /**
   * ShellOsdWindowManager:ui-group:
   *
   * The actor the OSD windows are added to. Main.uiGroup in the shell.
   */
  mgr_props[MGR_PROP_UI_GROUP] =
    g_param_spec_object ("ui-group", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, MGR_N_PROPS, mgr_props);
}

static void
shell_osd_window_manager_init (ShellOsdWindowManager *self)
{
  self->windows = g_ptr_array_new ();
}

/**
 * shell_osd_window_manager_new:
 * @ui_group: the actor the OSD windows are added to
 *
 * Returns: (transfer full): a new #ShellOsdWindowManager
 */
ShellOsdWindowManager *
shell_osd_window_manager_new (ClutterActor *ui_group)
{
  g_return_val_if_fail (CLUTTER_IS_ACTOR (ui_group), NULL);

  return g_object_new (SHELL_TYPE_OSD_WINDOW_MANAGER,
                       "ui-group", ui_group,
                       NULL);
}
