/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * osd-monitor-labeler.c: Shows a large label on each monitor, so that the
 * user can tell which is which when configuring displays. Driven over D-Bus
 * by org.gnome.Shell.ShowMonitorLabels (). Port of js/ui/osdMonitorLabeler.js.
 *
 * Only one D-Bus client at a time may have labels shown. The labels go away
 * when it calls HideMonitorLabels () or when it leaves the bus.
 */

#include "config.h"

#include <gio/gio.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-backend.h>
#include <meta/meta-monitor-manager.h>
#include <meta/meta-workspace-manager.h>
#include <meta/workspace.h>

#include "st.h"
#include "shell-global.h"
#include "ui/osd-monitor-labeler.h"

/* ShellOsdMonitorLabel, private */

#define SHELL_TYPE_OSD_MONITOR_LABEL (shell_osd_monitor_label_get_type ())
G_DECLARE_FINAL_TYPE (ShellOsdMonitorLabel, shell_osd_monitor_label,
                      SHELL, OSD_MONITOR_LABEL, StWidget)

struct _ShellOsdMonitorLabel
{
  StWidget parent;

  int monitor;

  ClutterActor *box;
  StLabel *label;
  gboolean unredirect_disabled;
};

G_DEFINE_FINAL_TYPE (ShellOsdMonitorLabel, shell_osd_monitor_label, ST_TYPE_WIDGET)

static void
shell_osd_monitor_label_position (ShellOsdMonitorLabel *self)
{
  ShellGlobal *global = shell_global_get ();
  MetaWorkspaceManager *workspace_manager = shell_global_get_workspace_manager (global);
  MetaWorkspace *workspace;
  MtkRectangle work_area;

  /* Assume that all workspaces will have the same struts and pick the
   * first one. */
  workspace = meta_workspace_manager_get_workspace_by_index (workspace_manager, 0);
  meta_workspace_get_work_area_for_monitor (workspace, self->monitor, &work_area);

  if (clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL)
    clutter_actor_set_x (self->box,
                         work_area.x + (work_area.width - clutter_actor_get_width (self->box)));
  else
    clutter_actor_set_x (self->box, work_area.x);

  clutter_actor_set_y (self->box, work_area.y);
}

static void
shell_osd_monitor_label_destroy (ClutterActor *actor)
{
  ShellOsdMonitorLabel *self = SHELL_OSD_MONITOR_LABEL (actor);

  if (self->unredirect_disabled)
    {
      self->unredirect_disabled = FALSE;
      meta_compositor_enable_unredirect (shell_global_get_compositor (shell_global_get ()));
    }

  CLUTTER_ACTOR_CLASS (shell_osd_monitor_label_parent_class)->destroy (actor);
}

static void
shell_osd_monitor_label_class_init (ShellOsdMonitorLabelClass *klass)
{
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  actor_class->destroy = shell_osd_monitor_label_destroy;
}

static void
shell_osd_monitor_label_init (ShellOsdMonitorLabel *self)
{
  clutter_actor_set_x_expand (CLUTTER_ACTOR (self), TRUE);
  clutter_actor_set_y_expand (CLUTTER_ACTOR (self), TRUE);

  self->box = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "orientation", CLUTTER_ORIENTATION_VERTICAL,
                            NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->box);

  self->label = g_object_new (ST_TYPE_LABEL,
                              "style-class", "osd-monitor-label",
                              NULL);
  clutter_actor_add_child (self->box, CLUTTER_ACTOR (self->label));
}

static ShellOsdMonitorLabel *
shell_osd_monitor_label_new (ClutterActor *ui_group,
                             int           monitor,
                             const char   *text)
{
  ShellOsdMonitorLabel *self = g_object_new (SHELL_TYPE_OSD_MONITOR_LABEL, NULL);

  self->monitor = monitor;
  st_label_set_text (self->label, text);

  clutter_actor_add_child (ui_group, CLUTTER_ACTOR (self));
  clutter_actor_set_child_above_sibling (ui_group, CLUTTER_ACTOR (self), NULL);
  shell_osd_monitor_label_position (self);

  meta_compositor_disable_unredirect (shell_global_get_compositor (shell_global_get ()));
  self->unredirect_disabled = TRUE;

  return self;
}

/* ShellOsdMonitorLabeler */

enum
{
  PROP_0,

  PROP_UI_GROUP,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellOsdMonitorLabeler
{
  GObject parent;

  ClutterActor *ui_group; /* weak */

  char *client;
  guint client_watch_id;

  /* ShellOsdMonitorLabel *, destroyed labels remove themselves */
  GPtrArray *osd_labels;

  /* For every monitor of the display, a GPtrArray of the (char *) labels
   * the client gave it */
  GPtrArray *monitor_labels;
};

G_DEFINE_FINAL_TYPE (ShellOsdMonitorLabeler, shell_osd_monitor_labeler, G_TYPE_OBJECT)

static void
label_destroyed_cb (ClutterActor           *label,
                    ShellOsdMonitorLabeler *self)
{
  g_ptr_array_remove (self->osd_labels, label);
}

static void
reset (ShellOsdMonitorLabeler *self)
{
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  int n_monitors = meta_display_get_n_monitors (display);
  int i;

  while (self->osd_labels->len > 0)
    {
      ClutterActor *label = g_ptr_array_index (self->osd_labels,
                                               self->osd_labels->len - 1);

      g_signal_handlers_disconnect_by_func (label, label_destroyed_cb, self);
      g_ptr_array_remove_index (self->osd_labels, self->osd_labels->len - 1);
      clutter_actor_destroy (label);
    }

  g_ptr_array_set_size (self->monitor_labels, 0);
  for (i = 0; i < n_monitors; i++)
    g_ptr_array_add (self->monitor_labels, g_ptr_array_new_with_free_func (g_free));
}

static void
monitors_changed_cb (MetaMonitorManager     *monitor_manager,
                     ShellOsdMonitorLabeler *self)
{
  reset (self);
}

static void
untrack_client_watch (ShellOsdMonitorLabeler *self)
{
  if (self->client_watch_id != 0)
    g_bus_unwatch_name (self->client_watch_id);
  self->client_watch_id = 0;
  g_clear_pointer (&self->client, g_free);
}

static void
client_vanished_cb (GDBusConnection *connection,
                    const char      *name,
                    gpointer         user_data)
{
  shell_osd_monitor_labeler_hide (SHELL_OSD_MONITOR_LABELER (user_data), name);
}

static gboolean
track_client (ShellOsdMonitorLabeler *self,
              const char             *client)
{
  if (self->client != NULL)
    return g_strcmp0 (self->client, client) == 0;

  self->client = g_strdup (client);
  self->client_watch_id = g_bus_watch_name (G_BUS_TYPE_SESSION,
                                            client,
                                            G_BUS_NAME_WATCHER_FLAGS_NONE,
                                            NULL,
                                            client_vanished_cb,
                                            self, NULL);
  return TRUE;
}

static gboolean
untrack_client (ShellOsdMonitorLabeler *self,
                const char             *client)
{
  if (self->client == NULL || g_strcmp0 (self->client, client) != 0)
    return FALSE;

  untrack_client_watch (self);
  return TRUE;
}

/**
 * shell_osd_monitor_labeler_show:
 * @self: a #ShellOsdMonitorLabeler
 * @client: the unique D-Bus name of the client asking for the labels
 * @params: a variant of type `a{sv}` mapping a monitor connector to the label
 *   (usually a string) to show on it
 *
 * Shows the labels on the monitors, replacing the ones that were shown
 * before. A label is shown on every monitor, empty if nothing was given for
 * it, and several labels for the same monitor are sorted and joined with a
 * space. Does nothing if another client has labels shown already.
 */
void
shell_osd_monitor_labeler_show (ShellOsdMonitorLabeler *self,
                                const char             *client,
                                GVariant               *params)
{
  MetaBackend *backend = shell_global_get_backend (shell_global_get ());
  MetaMonitorManager *monitor_manager = meta_backend_get_monitor_manager (backend);
  GVariantIter iter;
  const char *connector;
  GVariant *value;
  guint i;

  g_return_if_fail (SHELL_IS_OSD_MONITOR_LABELER (self));
  g_return_if_fail (client != NULL);
  g_return_if_fail (self->ui_group != NULL);
  g_return_if_fail (params != NULL &&
                    g_variant_is_of_type (params, G_VARIANT_TYPE_VARDICT));

  if (!track_client (self, client))
    return;

  reset (self);

  g_variant_iter_init (&iter, params);
  while (g_variant_iter_next (&iter, "{&sv}", &connector, &value))
    {
      g_autoptr (GVariant) owned_value = value;
      GPtrArray *labels;
      int monitor;

      monitor = meta_monitor_manager_get_monitor_for_connector (monitor_manager,
                                                                connector);
      if (monitor < 0 || monitor >= (int) self->monitor_labels->len)
        continue;

      labels = g_ptr_array_index (self->monitor_labels, monitor);
      if (g_variant_is_of_type (owned_value, G_VARIANT_TYPE_STRING))
        g_ptr_array_add (labels, g_variant_dup_string (owned_value, NULL));
      else
        g_ptr_array_add (labels, g_variant_print (owned_value, FALSE));
    }

  for (i = 0; i < self->monitor_labels->len; i++)
    {
      GPtrArray *labels = g_ptr_array_index (self->monitor_labels, i);
      g_autoptr (GString) text = g_string_new (NULL);
      ShellOsdMonitorLabel *label;
      guint j;

      g_ptr_array_sort_values (labels, (GCompareFunc) g_strcmp0);
      for (j = 0; j < labels->len; j++)
        {
          if (j > 0)
            g_string_append_c (text, ' ');
          g_string_append (text, g_ptr_array_index (labels, j));
        }

      label = shell_osd_monitor_label_new (self->ui_group, i, text->str);
      g_signal_connect (label, "destroy",
                        G_CALLBACK (label_destroyed_cb), self);
      g_ptr_array_add (self->osd_labels, label);
    }
}

/**
 * shell_osd_monitor_labeler_hide:
 * @self: a #ShellOsdMonitorLabeler
 * @client: the unique D-Bus name of the client that asked for the labels
 *
 * Removes the labels again. Does nothing if @client is not the client the
 * labels are shown for.
 */
void
shell_osd_monitor_labeler_hide (ShellOsdMonitorLabeler *self,
                                const char             *client)
{
  g_return_if_fail (SHELL_IS_OSD_MONITOR_LABELER (self));
  g_return_if_fail (client != NULL);

  if (!untrack_client (self, client))
    return;

  reset (self);
}

static void
shell_osd_monitor_labeler_constructed (GObject *object)
{
  ShellOsdMonitorLabeler *self = SHELL_OSD_MONITOR_LABELER (object);
  MetaBackend *backend = shell_global_get_backend (shell_global_get ());

  G_OBJECT_CLASS (shell_osd_monitor_labeler_parent_class)->constructed (object);

  if (self->ui_group == NULL)
    {
      g_critical ("ShellOsdMonitorLabeler needs a ui-group");
      return;
    }

  g_signal_connect_object (meta_backend_get_monitor_manager (backend),
                           "monitors-changed",
                           G_CALLBACK (monitors_changed_cb), self, 0);
  reset (self);
}

static void
shell_osd_monitor_labeler_dispose (GObject *object)
{
  ShellOsdMonitorLabeler *self = SHELL_OSD_MONITOR_LABELER (object);

  untrack_client_watch (self);

  while (self->osd_labels->len > 0)
    {
      ClutterActor *label = g_ptr_array_index (self->osd_labels,
                                               self->osd_labels->len - 1);

      g_signal_handlers_disconnect_by_func (label, label_destroyed_cb, self);
      g_ptr_array_remove_index (self->osd_labels, self->osd_labels->len - 1);
      clutter_actor_destroy (label);
    }

  g_clear_weak_pointer (&self->ui_group);

  G_OBJECT_CLASS (shell_osd_monitor_labeler_parent_class)->dispose (object);
}

static void
shell_osd_monitor_labeler_finalize (GObject *object)
{
  ShellOsdMonitorLabeler *self = SHELL_OSD_MONITOR_LABELER (object);

  g_ptr_array_free (self->osd_labels, TRUE);
  g_ptr_array_free (self->monitor_labels, TRUE);

  G_OBJECT_CLASS (shell_osd_monitor_labeler_parent_class)->finalize (object);
}

static void
shell_osd_monitor_labeler_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellOsdMonitorLabeler *self = SHELL_OSD_MONITOR_LABELER (object);

  switch (prop_id)
    {
    case PROP_UI_GROUP:
      g_value_set_object (value, self->ui_group);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_osd_monitor_labeler_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellOsdMonitorLabeler *self = SHELL_OSD_MONITOR_LABELER (object);

  switch (prop_id)
    {
    case PROP_UI_GROUP:
      g_set_weak_pointer (&self->ui_group, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_osd_monitor_labeler_class_init (ShellOsdMonitorLabelerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_osd_monitor_labeler_constructed;
  object_class->dispose = shell_osd_monitor_labeler_dispose;
  object_class->finalize = shell_osd_monitor_labeler_finalize;
  object_class->get_property = shell_osd_monitor_labeler_get_property;
  object_class->set_property = shell_osd_monitor_labeler_set_property;

  /**
   * ShellOsdMonitorLabeler:ui-group:
   *
   * The actor the labels are added to. Main.uiGroup in the shell.
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
shell_osd_monitor_labeler_init (ShellOsdMonitorLabeler *self)
{
  self->osd_labels = g_ptr_array_new ();
  self->monitor_labels = g_ptr_array_new_with_free_func ((GDestroyNotify) g_ptr_array_unref);
}

/**
 * shell_osd_monitor_labeler_new:
 * @ui_group: the actor the labels are added to
 *
 * Returns: (transfer full): a new #ShellOsdMonitorLabeler
 */
ShellOsdMonitorLabeler *
shell_osd_monitor_labeler_new (ClutterActor *ui_group)
{
  g_return_val_if_fail (CLUTTER_IS_ACTOR (ui_group), NULL);

  return g_object_new (SHELL_TYPE_OSD_MONITOR_LABELER,
                       "ui-group", ui_group,
                       NULL);
}
