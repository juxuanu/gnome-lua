/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * brightness-manager.c: The screen brightness of the monitors that have a
 * backlight, as one global scale plus one scale per monitor. Port of
 * js/misc/brightnessManager.js.
 *
 * What the JS reached through the `Main` module is done like this:
 *
 *  - `Main.osdWindowManager.show (icon, null, levels)` is the
 *    #ShellBrightnessManager::osd-requested signal, which src/ui/main.c
 *    forwards to the OSD window manager.
 *  - `Main.wm.addKeybinding ()` is meta_display_add_keybinding (). The window
 *    manager also records in which action modes a keybinding is allowed, which
 *    is JS only: ShellMain calls `Main.wm.allowKeybinding (name, Shell.ActionMode.ALL)`
 *    for every name of shell_brightness_manager_get_keybinding_names ().
 *  - `Main.layoutManager.monitors` was never used, the scales are keyed by the
 *    #MetaLogicalMonitor of #MetaMonitorManager.
 *
 * The scales are one type, #ShellBrightnessScale: the global scale has no
 * monitor, the scales of the monitors (the JS MonitorBrightnessScale) do.
 *
 * Differences with the JS version:
 *
 *  - #ShellBrightnessScale:value is a double property (the JS declared a
 *    float property but stored a double);
 *  - the number of steps of a scale is at least 1, a backlight with a single
 *    brightness level made the JS divide by zero;
 *  - a missing vendor, product or serial of a monitor is saved as an empty
 *    string;
 *  - a missing org.gnome.settings-daemon.plugins.power schema logs a warning
 *    and the dimming target stays at the default of 30%;
 *  - nothing is emitted after ShellGlobal::shutdown.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>
#include <meta/display.h>
#include <meta/meta-backend.h>
#include <meta/meta-backlight.h>
#include <meta/meta-logical-monitor.h>
#include <meta/meta-monitor.h>
#include <meta/meta-monitor-manager.h>
#include <math.h>

#include "misc/brightness-manager.h"
#include "shell-global.h"

#define SCALE_VALUE_N_STEPS 20
#define SCALE_VALUE_CHANGE_EPSILON 0.001

#define KEYBINDING_SCHEMA "org.gnome.shell.keybindings"
#define POWER_SCHEMA "org.gnome.settings-daemon.plugins.power"

#define SAVE_DELAY_MS 300

/* ------------------------------------------------------------------------
 * ShellBrightnessScale
 * --------------------------------------------------------------------- */

enum
{
  SCALE_PROP_0,

  SCALE_PROP_NAME,
  SCALE_PROP_VALUE,
  SCALE_PROP_N_STEPS,

  SCALE_N_PROPS
};

static GParamSpec *scale_props[SCALE_N_PROPS] = { NULL, };

enum
{
  SCALE_DESTROY,
  SCALE_BACKLIGHTS_CHANGED,

  SCALE_LAST_SIGNAL
};

static guint scale_signals[SCALE_LAST_SIGNAL] = { 0, };

struct _ShellBrightnessScale
{
  GObject parent;

  char *name;
  double value;
  int n_steps;

  /* The scales of the monitors, the global scale has no monitor */
  MetaLogicalMonitor *monitor;
  int current_backlight_brightness;
  double scale_factor;
  gboolean scale_changed;
};

G_DEFINE_FINAL_TYPE (ShellBrightnessScale, shell_brightness_scale, G_TYPE_OBJECT)

static void
scale_set_value_internal (ShellBrightnessScale *self,
                          double                value)
{
  self->value = CLAMP (value, 0.0, 1.0);

  /* The JS notified on every set, also when the value did not change */
  g_object_notify_by_pspec (G_OBJECT (self), scale_props[SCALE_PROP_VALUE]);
}

static void
shell_brightness_scale_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  ShellBrightnessScale *self = SHELL_BRIGHTNESS_SCALE (object);

  switch (prop_id)
    {
    case SCALE_PROP_NAME:
      g_value_set_string (value, self->name);
      break;

    case SCALE_PROP_VALUE:
      g_value_set_double (value, self->value);
      break;

    case SCALE_PROP_N_STEPS:
      g_value_set_int (value, self->n_steps);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_brightness_scale_set_property (GObject      *object,
                                     guint         prop_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  ShellBrightnessScale *self = SHELL_BRIGHTNESS_SCALE (object);

  switch (prop_id)
    {
    case SCALE_PROP_NAME:
      g_free (self->name);
      self->name = g_value_dup_string (value);
      break;

    case SCALE_PROP_VALUE:
      scale_set_value_internal (self, g_value_get_double (value));
      break;

    case SCALE_PROP_N_STEPS:
      self->n_steps = MAX (1, g_value_get_int (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_brightness_scale_finalize (GObject *object)
{
  ShellBrightnessScale *self = SHELL_BRIGHTNESS_SCALE (object);

  g_clear_object (&self->monitor);
  g_free (self->name);

  G_OBJECT_CLASS (shell_brightness_scale_parent_class)->finalize (object);
}

static void
shell_brightness_scale_class_init (ShellBrightnessScaleClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_brightness_scale_get_property;
  object_class->set_property = shell_brightness_scale_set_property;
  object_class->finalize = shell_brightness_scale_finalize;

  scale_props[SCALE_PROP_NAME] =
    g_param_spec_string ("name", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  scale_props[SCALE_PROP_VALUE] =
    g_param_spec_double ("value", NULL, NULL,
                         0.0, 1.0, 1.0,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  scale_props[SCALE_PROP_N_STEPS] =
    g_param_spec_int ("n-steps", NULL, NULL,
                      1, G_MAXINT, SCALE_VALUE_N_STEPS,
                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, SCALE_N_PROPS, scale_props);

  /**
   * ShellBrightnessScale::destroy:
   *
   * Emitted when the scale is not used any more, its signal connections
   * should be dropped (js/misc/signalTracker.js treats it as a destroy
   * signal once the type is registered with registerDestroyableType ()).
   */
  scale_signals[SCALE_DESTROY] =
    g_signal_new ("destroy",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellBrightnessScale::backlights-changed:
   *
   * Emitted by the scale of a monitor when the brightness of one of its
   * backlights changed.
   */
  scale_signals[SCALE_BACKLIGHTS_CHANGED] =
    g_signal_new ("backlights-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_brightness_scale_init (ShellBrightnessScale *self)
{
  self->value = 1.0;
  self->n_steps = SCALE_VALUE_N_STEPS;
  self->current_backlight_brightness = -1;
  self->scale_factor = 1.0;
}

/**
 * shell_brightness_scale_get_name:
 * @self: a #ShellBrightnessScale
 *
 * Returns: the name of the scale, the display name of the monitor for the
 *   scales of the monitors
 */
const char *
shell_brightness_scale_get_name (ShellBrightnessScale *self)
{
  g_return_val_if_fail (SHELL_IS_BRIGHTNESS_SCALE (self), NULL);

  return self->name;
}

/**
 * shell_brightness_scale_get_value:
 * @self: a #ShellBrightnessScale
 *
 * Returns: the brightness of the scale, between 0 and 1
 */
double
shell_brightness_scale_get_value (ShellBrightnessScale *self)
{
  g_return_val_if_fail (SHELL_IS_BRIGHTNESS_SCALE (self), 0.0);

  return self->value;
}

/**
 * shell_brightness_scale_set_value:
 * @self: a #ShellBrightnessScale
 * @value: the new brightness, clamped to 0 to 1
 *
 * Sets the brightness of the scale. #ShellBrightnessScale:value is notified
 * also if the value did not change.
 */
void
shell_brightness_scale_set_value (ShellBrightnessScale *self,
                                  double                value)
{
  g_return_if_fail (SHELL_IS_BRIGHTNESS_SCALE (self));

  scale_set_value_internal (self, value);
}

/**
 * shell_brightness_scale_get_n_steps:
 * @self: a #ShellBrightnessScale
 *
 * Returns: the number of steps from 0 to 1, at most 20 and fewer for
 *   backlights with few brightness levels
 */
int
shell_brightness_scale_get_n_steps (ShellBrightnessScale *self)
{
  g_return_val_if_fail (SHELL_IS_BRIGHTNESS_SCALE (self), 1);

  return self->n_steps;
}

/**
 * shell_brightness_scale_step_up:
 * @self: a #ShellBrightnessScale
 *
 * Increases the brightness by one step.
 */
void
shell_brightness_scale_step_up (ShellBrightnessScale *self)
{
  g_return_if_fail (SHELL_IS_BRIGHTNESS_SCALE (self));

  scale_set_value_internal (self, MIN (1.0, self->value + (1.0 / self->n_steps)));
}

/**
 * shell_brightness_scale_step_down:
 * @self: a #ShellBrightnessScale
 *
 * Decreases the brightness by one step.
 */
void
shell_brightness_scale_step_down (ShellBrightnessScale *self)
{
  g_return_if_fail (SHELL_IS_BRIGHTNESS_SCALE (self));

  scale_set_value_internal (self, MAX (0.0, self->value - (1.0 / self->n_steps)));
}

/**
 * shell_brightness_scale_cycle_up:
 * @self: a #ShellBrightnessScale
 *
 * Increases the brightness by one step, or sets it to 0 if it is at the
 * maximum.
 */
void
shell_brightness_scale_cycle_up (ShellBrightnessScale *self)
{
  g_return_if_fail (SHELL_IS_BRIGHTNESS_SCALE (self));

  if (fabs (1.0 - self->value) < SCALE_VALUE_CHANGE_EPSILON)
    scale_set_value_internal (self, 0.0);
  else
    shell_brightness_scale_step_up (self);
}

/**
 * shell_brightness_scale_destroy:
 * @self: a #ShellBrightnessScale
 *
 * Emits #ShellBrightnessScale::destroy.
 */
void
shell_brightness_scale_destroy (ShellBrightnessScale *self)
{
  g_return_if_fail (SHELL_IS_BRIGHTNESS_SCALE (self));

  g_signal_emit (self, scale_signals[SCALE_DESTROY], 0);
}

/* ---- the scales of the monitors (MonitorBrightnessScale in the JS) ---- */

static gboolean
monitor_has_active_backlight (MetaMonitor *monitor)
{
  return meta_monitor_get_backlight (monitor) != NULL &&
         meta_monitor_is_active (monitor);
}

static gboolean
logical_monitor_has_backlight (MetaLogicalMonitor *logical_monitor)
{
  GList *l;

  for (l = meta_logical_monitor_get_monitors (logical_monitor); l; l = l->next)
    {
      if (monitor_has_active_backlight (l->data))
        return TRUE;
    }

  return FALSE;
}

/* (element-type MetaBacklight) (transfer container) */
static GPtrArray *
scale_get_backlights (ShellBrightnessScale *self)
{
  GPtrArray *backlights = g_ptr_array_new ();
  GList *l;

  for (l = meta_logical_monitor_get_monitors (self->monitor); l; l = l->next)
    {
      if (monitor_has_active_backlight (l->data))
        g_ptr_array_add (backlights, meta_monitor_get_backlight (l->data));
    }

  return backlights;
}

static double
get_relative_brightness (MetaBacklight *backlight)
{
  int min, max;

  meta_backlight_get_brightness_info (backlight, &min, &max);
  if (max <= min)
    return 0.0;

  return ((double) meta_backlight_get_brightness (backlight) - min) / (max - min);
}

static void
set_relative_brightness (MetaBacklight *backlight,
                         double         brightness)
{
  int min, max;

  meta_backlight_get_brightness_info (backlight, &min, &max);
  meta_backlight_set_brightness (backlight,
                                 (int) (min + ((max - min) * brightness)));
}

/* Returns whether the backlight changed since the last time */
static gboolean
scale_sync_with_backlight (ShellBrightnessScale *self)
{
  g_autoptr (GPtrArray) backlights = scale_get_backlights (self);
  MetaBacklight *backlight;
  int brightness;

  if (backlights->len == 0)
    return FALSE;

  backlight = g_ptr_array_index (backlights, 0);
  brightness = meta_backlight_get_brightness (backlight);
  if (brightness == self->current_backlight_brightness)
    return FALSE;

  self->current_backlight_brightness = brightness;
  scale_set_value_internal (self, get_relative_brightness (backlight));
  return TRUE;
}

static void
scale_sync_with_scale (ShellBrightnessScale *self,
                       ShellBrightnessScale *global_scale)
{
  scale_set_value_internal (self, global_scale->value * self->scale_factor);
}

static void
scale_set_backlight (ShellBrightnessScale *self,
                     double                brightness)
{
  g_autoptr (GPtrArray) backlights = scale_get_backlights (self);
  guint i;

  if (backlights->len == 0)
    return;

  for (i = 0; i < backlights->len; i++)
    set_relative_brightness (g_ptr_array_index (backlights, i), brightness);

  self->current_backlight_brightness =
    meta_backlight_get_brightness (g_ptr_array_index (backlights, 0));
}

static void
scale_update_scale_factor (ShellBrightnessScale *self,
                           double                max)
{
  self->scale_factor = self->value / max;
}

static void
on_backlight_brightness_notify (MetaBacklight        *backlight,
                                GParamSpec           *pspec,
                                ShellBrightnessScale *self)
{
  g_signal_emit (self, scale_signals[SCALE_BACKLIGHTS_CHANGED], 0);
}

static void
on_scale_destroy (ShellBrightnessScale *self)
{
  g_autoptr (GPtrArray) backlights = NULL;
  guint i;

  /* The JS tracked the backlight connections with the scale as owner */
  if (self->monitor == NULL)
    return;

  backlights = scale_get_backlights (self);
  for (i = 0; i < backlights->len; i++)
    g_signal_handlers_disconnect_by_func (g_ptr_array_index (backlights, i),
                                          on_backlight_brightness_notify,
                                          self);
}

/* (transfer full) */
static ShellBrightnessScale *
monitor_scale_new (MetaLogicalMonitor *logical_monitor,
                   double              initial_value)
{
  ShellBrightnessScale *self;
  GList *l;
  GList *monitors = meta_logical_monitor_get_monitors (logical_monitor);
  int max_steps = 0;
  g_autoptr (GPtrArray) backlights = NULL;
  guint i;

  /* Handle backlights with just a few steps */
  for (l = monitors; l; l = l->next)
    {
      MetaBacklight *backlight;
      int min, max;

      if (!monitor_has_active_backlight (l->data))
        continue;

      backlight = meta_monitor_get_backlight (l->data);
      meta_backlight_get_brightness_info (backlight, &min, &max);
      max_steps = MAX (max_steps, max - min);
    }

  self = g_object_new (SHELL_TYPE_BRIGHTNESS_SCALE,
                       "name", meta_monitor_get_display_name (monitors->data),
                       "n-steps", CLAMP (max_steps, 1, SCALE_VALUE_N_STEPS),
                       NULL);
  self->value = initial_value >= 0 ? initial_value : 1.0;
  self->monitor = g_object_ref (logical_monitor);

  if (initial_value >= 0)
    scale_set_backlight (self, initial_value);
  else
    scale_sync_with_backlight (self);

  backlights = scale_get_backlights (self);
  for (i = 0; i < backlights->len; i++)
    {
      g_signal_connect_object (g_ptr_array_index (backlights, i),
                               "notify::brightness",
                               G_CALLBACK (on_backlight_brightness_notify),
                               self, 0);
    }

  g_signal_connect (self, "destroy", G_CALLBACK (on_scale_destroy), NULL);

  return self;
}

/* ------------------------------------------------------------------------
 * Saved brightnesses
 * --------------------------------------------------------------------- */

typedef struct
{
  char *vendor;
  char *product;
  char *serial;
  char *color_mode;
  char *connector;
} MonitorId;

static void
monitor_id_free (MonitorId *id)
{
  g_free (id->vendor);
  g_free (id->product);
  g_free (id->serial);
  g_free (id->color_mode);
  g_free (id->connector);
  g_free (id);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MonitorId, monitor_id_free)

static MonitorId *
monitor_id_new (const char *vendor,
                const char *product,
                const char *serial,
                const char *color_mode,
                const char *connector)
{
  MonitorId *id = g_new0 (MonitorId, 1);

  id->vendor = g_strdup (vendor ? vendor : "");
  id->product = g_strdup (product ? product : "");
  id->serial = g_strdup (serial ? serial : "");
  id->color_mode = g_strdup (color_mode ? color_mode : "");
  id->connector = g_strdup (connector ? connector : "");
  return id;
}

static MonitorId *
monitor_id_from_logical_monitor (MetaLogicalMonitor *logical_monitor)
{
  MetaMonitor *monitor = meta_logical_monitor_get_monitors (logical_monitor)->data;

  return monitor_id_new (meta_monitor_get_vendor (monitor),
                         meta_monitor_get_product (monitor),
                         meta_monitor_get_serial (monitor),
                         meta_monitor_get_color_mode_string (monitor),
                         meta_monitor_get_connector (monitor));
}

static gboolean
monitor_id_equals (const MonitorId *a,
                   const MonitorId *b)
{
  return g_str_equal (a->vendor, b->vendor) &&
         g_str_equal (a->product, b->product) &&
         g_str_equal (a->serial, b->serial) &&
         g_str_equal (a->color_mode, b->color_mode);
}

static gboolean
monitor_id_equals_exact (const MonitorId *a,
                         const MonitorId *b)
{
  return monitor_id_equals (a, b) &&
         g_str_equal (a->connector, b->connector);
}

typedef struct
{
  MonitorId *id;
  double brightness;
} SavedBrightness;

static void
saved_brightness_free (SavedBrightness *saved)
{
  g_clear_pointer (&saved->id, monitor_id_free);
  g_free (saved);
}

/* (element-type SavedBrightness) (transfer container) */
static GPtrArray *
get_state (void)
{
  GPtrArray *values = g_ptr_array_new_with_free_func ((GDestroyNotify) saved_brightness_free);
  GVariant *stored;
  g_autoptr (GVariant) state = NULL;
  GVariantIter iter;
  const char *vendor, *product, *serial, *color_mode, *connector;
  double brightness;

  stored = shell_global_get_persistent_state (shell_global_get (),
                                              "a(sssssd)", "brightnesses");
  if (stored == NULL)
    return values;

  state = g_variant_ref_sink (stored);
  g_variant_iter_init (&iter, state);
  while (g_variant_iter_next (&iter, "(&s&s&s&s&sd)",
                              &vendor, &product, &serial, &color_mode,
                              &connector, &brightness))
    {
      SavedBrightness *saved = g_new0 (SavedBrightness, 1);

      saved->id = monitor_id_new (vendor, product, serial, color_mode, connector);
      saved->brightness = brightness;
      g_ptr_array_add (values, saved);
    }

  return values;
}

static void
set_state (GPtrArray *values)
{
  GVariantBuilder builder;
  g_autoptr (GVariant) state = NULL;
  guint i;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sssssd)"));
  for (i = 0; i < values->len; i++)
    {
      SavedBrightness *saved = g_ptr_array_index (values, i);

      g_variant_builder_add (&builder, "(sssssd)",
                             saved->id->vendor, saved->id->product,
                             saved->id->serial, saved->id->color_mode,
                             saved->id->connector, saved->brightness);
    }

  state = g_variant_ref_sink (g_variant_builder_end (&builder));
  shell_global_set_persistent_state (shell_global_get (), "brightnesses", state);
}

/* ------------------------------------------------------------------------
 * ShellBrightnessManager
 * --------------------------------------------------------------------- */

enum
{
  PROP_0,

  PROP_DIMMING,
  PROP_AUTO_BRIGHTNESS_TARGET,
  PROP_GLOBAL_SCALE,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  CHANGED,
  USER_UPDATE,
  OSD_REQUESTED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

typedef enum
{
  KEY_UP,
  KEY_UP_MONITOR,
  KEY_DOWN,
  KEY_DOWN_MONITOR,
  KEY_CYCLE,
  KEY_CYCLE_MONITOR,

  N_KEYS
} KeyAction;

static const char * const keybinding_names[N_KEYS + 1] = {
  [KEY_UP]           = "screen-brightness-up",
  [KEY_UP_MONITOR]   = "screen-brightness-up-monitor",
  [KEY_DOWN]         = "screen-brightness-down",
  [KEY_DOWN_MONITOR] = "screen-brightness-down-monitor",
  [KEY_CYCLE]        = "screen-brightness-cycle",
  [KEY_CYCLE_MONITOR] = "screen-brightness-cycle-monitor",
  [N_KEYS]           = NULL,
};

typedef struct
{
  ShellBrightnessManager *manager;  /* unowned, the keybindings are removed in dispose */
  KeyAction action;
} KeyBindingData;

struct _ShellBrightnessManager
{
  GObject parent;

  ShellBrightnessScale *global_scale;
  GPtrArray *monitor_scales;  /* (element-type ShellBrightnessScale), in the order of the logical monitors */

  double dimming_target;
  gboolean dimming_enabled;
  double ab_target;

  guint save_id;
  gboolean inhibit_updates;
  gboolean global_scale_changed;
  gboolean shutting_down;

  GSettings *keybinding_settings;
  gboolean keybindings_added;

  MetaMonitorManager *monitor_manager;  /* unowned */
};

G_DEFINE_FINAL_TYPE (ShellBrightnessManager, shell_brightness_manager, G_TYPE_OBJECT)

static void sync_scales (ShellBrightnessManager *self,
                         gboolean                show_osd);

static void
emit_changed (ShellBrightnessManager *self)
{
  if (!self->shutting_down)
    g_signal_emit (self, signals[CHANGED], 0);
}

static void
emit_user_update (ShellBrightnessManager *self)
{
  if (!self->shutting_down)
    g_signal_emit (self, signals[USER_UPDATE], 0);
}

static ShellBrightnessScale *
find_monitor_scale (ShellBrightnessManager *self,
                    MetaLogicalMonitor     *logical_monitor)
{
  guint i;

  if (logical_monitor == NULL)
    return NULL;

  for (i = 0; i < self->monitor_scales->len; i++)
    {
      ShellBrightnessScale *scale = g_ptr_array_index (self->monitor_scales, i);

      if (scale->monitor == logical_monitor)
        return scale;
    }

  return NULL;
}

static MetaLogicalMonitor *
get_current_logical_monitor (void)
{
  return meta_backend_get_current_logical_monitor (shell_global_get_backend (shell_global_get ()));
}

static void
on_key_binding (MetaDisplay        *display,
                MetaWindow         *window,
                const ClutterEvent *event,
                MetaKeyBinding     *binding,
                gpointer            user_data)
{
  KeyBindingData *data = user_data;
  ShellBrightnessManager *self = data->manager;
  ShellBrightnessScale *scale = NULL;

  switch (data->action)
    {
    case KEY_UP:
    case KEY_DOWN:
    case KEY_CYCLE:
      scale = self->global_scale;
      break;

    case KEY_UP_MONITOR:
    case KEY_DOWN_MONITOR:
    case KEY_CYCLE_MONITOR:
      scale = find_monitor_scale (self, get_current_logical_monitor ());
      break;

    default:
      g_assert_not_reached ();
    }

  if (scale == NULL)
    return;

  switch (data->action)
    {
    case KEY_UP:
    case KEY_UP_MONITOR:
      shell_brightness_scale_step_up (scale);
      break;

    case KEY_DOWN:
    case KEY_DOWN_MONITOR:
      shell_brightness_scale_step_down (scale);
      break;

    case KEY_CYCLE:
    case KEY_CYCLE_MONITOR:
      shell_brightness_scale_cycle_up (scale);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
add_keybindings (ShellBrightnessManager *self)
{
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  int i;

  self->keybinding_settings = g_settings_new (KEYBINDING_SCHEMA);

  for (i = 0; i < N_KEYS; i++)
    {
      KeyBindingData *data = g_new0 (KeyBindingData, 1);

      data->manager = self;
      data->action = i;

      if (meta_display_add_keybinding (display,
                                       keybinding_names[i],
                                       self->keybinding_settings,
                                       META_KEY_BINDING_NONE,
                                       on_key_binding,
                                       data, g_free) == 0)
        g_warning ("Could not add the keybinding %s", keybinding_names[i]);
    }

  self->keybindings_added = TRUE;
}

static void
remove_keybindings (ShellBrightnessManager *self)
{
  MetaDisplay *display;
  int i;

  if (!self->keybindings_added)
    return;

  self->keybindings_added = FALSE;

  display = shell_global_get_display (shell_global_get ());
  for (i = 0; i < N_KEYS; i++)
    meta_display_remove_keybinding (display, keybinding_names[i]);
}

/* (element-type double) (transfer container): the saved brightness of each
 * of @monitors, or -1 */
static GArray *
get_saved_brightnesses (GPtrArray *monitors)
{
  GArray *brightnesses = g_array_sized_new (FALSE, FALSE, sizeof (double), monitors->len);
  g_autoptr (GPtrArray) saved = get_state ();
  guint i, j;

  for (i = 0; i < monitors->len; i++)
    {
      g_autoptr (MonitorId) id = monitor_id_from_logical_monitor (g_ptr_array_index (monitors, i));
      double brightness = -1.0;
      gboolean found = FALSE;

      for (j = 0; j < saved->len; j++)
        {
          SavedBrightness *s = g_ptr_array_index (saved, j);

          if (monitor_id_equals_exact (s->id, id))
            {
              brightness = s->brightness;
              found = TRUE;
              break;
            }
        }

      if (!found)
        {
          for (j = 0; j < saved->len; j++)
            {
              SavedBrightness *s = g_ptr_array_index (saved, j);

              if (monitor_id_equals (s->id, id))
                {
                  brightness = s->brightness;
                  break;
                }
            }
        }

      g_array_append_val (brightnesses, brightness);
    }

  return brightnesses;
}

static void
save_brightnesses_cb (gpointer data)
{
  ShellBrightnessManager *self = data;
  g_autoptr (GPtrArray) values = NULL;
  guint i, j;

  self->save_id = 0;

  values = get_state ();

  for (i = 0; i < self->monitor_scales->len; i++)
    {
      ShellBrightnessScale *scale = g_ptr_array_index (self->monitor_scales, i);
      MonitorId *id = monitor_id_from_logical_monitor (scale->monitor);
      SavedBrightness *saved;

      for (j = 0; j < values->len;)
        {
          SavedBrightness *s = g_ptr_array_index (values, j);

          if (monitor_id_equals_exact (s->id, id))
            g_ptr_array_remove_index (values, j);
          else
            j++;
        }

      saved = g_new0 (SavedBrightness, 1);
      saved->id = id;
      saved->brightness = scale->value;
      g_ptr_array_add (values, saved);
    }

  set_state (values);
}

static void
save_brightnesses (ShellBrightnessManager *self)
{
  g_clear_handle_id (&self->save_id, g_source_remove);
  self->save_id = g_timeout_add_once (SAVE_DELAY_MS, save_brightnesses_cb, self);
}

static void
show_osd (ShellBrightnessManager *self,
          GPtrArray              *scales)
{
  GVariantBuilder builder;
  g_autoptr (GVariant) levels = NULL;
  g_autoptr (GIcon) icon = NULL;
  guint i;

  if (self->shutting_down)
    return;

  /* (monitor, level, max level); a max level below 1 means 1 */
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(idd)"));
  for (i = 0; i < scales->len; i++)
    {
      ShellBrightnessScale *scale = g_ptr_array_index (scales, i);

      g_variant_builder_add (&builder, "(idd)",
                             meta_logical_monitor_get_number (scale->monitor),
                             scale->value, -1.0);
    }

  levels = g_variant_ref_sink (g_variant_builder_end (&builder));
  icon = g_themed_icon_new ("display-brightness-symbolic");

  g_signal_emit (self, signals[OSD_REQUESTED], 0, icon, levels);
}

static void
on_global_scale_value_notify (ShellBrightnessScale   *scale,
                              GParamSpec             *pspec,
                              ShellBrightnessManager *self)
{
  if (self->inhibit_updates)
    return;

  self->global_scale_changed = TRUE;
  sync_scales (self, TRUE);
  emit_user_update (self);
}

static void
on_monitor_scale_value_notify (ShellBrightnessScale   *scale,
                               GParamSpec             *pspec,
                               ShellBrightnessManager *self)
{
  if (self->inhibit_updates)
    return;

  scale->scale_changed = TRUE;
  sync_scales (self, TRUE);
  emit_user_update (self);
}

static void
on_backlights_changed (ShellBrightnessScale   *scale,
                       ShellBrightnessManager *self)
{
  sync_scales (self, TRUE);
}

static void
sync_scales (ShellBrightnessManager *self,
             gboolean                osd)
{
  g_autoptr (GPtrArray) changed_scales = NULL;
  gboolean scales_updated = TRUE;
  guint i;

  if (self->global_scale == NULL)
    return;
  if (self->inhibit_updates)
    return;
  self->inhibit_updates = TRUE;

  /* Handle changed backlights */
  for (i = 0; i < self->monitor_scales->len; i++)
    {
      if (scale_sync_with_backlight (g_ptr_array_index (self->monitor_scales, i)))
        {
          /* disable dimming for all if we have a single system initiated
           * backlight change */
          if (self->dimming_enabled)
            {
              self->dimming_enabled = FALSE;
              g_object_notify_by_pspec (G_OBJECT (self), props[PROP_DIMMING]);
            }
        }
    }

  /* Find scales which have been changed (and reset scale_changed) */
  changed_scales = g_ptr_array_new ();
  for (i = 0; i < self->monitor_scales->len; i++)
    {
      ShellBrightnessScale *scale = g_ptr_array_index (self->monitor_scales, i);

      if (scale->scale_changed)
        g_ptr_array_add (changed_scales, scale);
      scale->scale_changed = FALSE;
    }

  if (changed_scales->len > 0)
    {
      /* update the factors of all the scales when a scale changes */

      /* normalize everything to the maximum of all scales */
      double max = -INFINITY;

      for (i = 0; i < self->monitor_scales->len; i++)
        {
          ShellBrightnessScale *scale = g_ptr_array_index (self->monitor_scales, i);

          max = MAX (max, scale->value);
        }

      /* if max is 0 we can't deduce any ratios, so don't try */
      if (max > 0.01)
        {
          for (i = 0; i < self->monitor_scales->len; i++)
            scale_update_scale_factor (g_ptr_array_index (self->monitor_scales, i), max);
        }

      /* the global scale always follows the maximum, because one monitor
       * scale is at the maximum and we want the global scale to be a
       * factor we apply on the ratio of the monitor scales. */
      scale_set_value_internal (self->global_scale, max);

      if (osd)
        show_osd (self, changed_scales);
    }
  else if (self->global_scale_changed)
    {
      /* if the global scale changed, update the monitor scales according
       * to their scale factor and the global scale. */
      self->global_scale_changed = FALSE;

      for (i = 0; i < self->monitor_scales->len; i++)
        scale_sync_with_scale (g_ptr_array_index (self->monitor_scales, i),
                               self->global_scale);

      if (osd)
        show_osd (self, self->monitor_scales);
    }
  else
    {
      scales_updated = FALSE;
    }

  /* Update the actual backlight according to the new monitor brightnesses
   * and other factors, such as dimming. */
  for (i = 0; i < self->monitor_scales->len; i++)
    {
      ShellBrightnessScale *scale = g_ptr_array_index (self->monitor_scales, i);
      double target, max, brightness;

      /* If auto brightness is active (ab_target >= 0) we use the scale
       * as a bias for the auto brightness target to determine the target
       * brightness.
       * Otherwise the target brightness just comes from the scale. */
      if (self->ab_target >= 0.0)
        target = CLAMP (self->ab_target + scale->value - 0.5, 0.0, 1.0);
      else
        target = scale->value;

      /* the actual brightness is then determined by clipping to the
       * dimming target, if dimming is enabled */
      max = self->dimming_enabled ? self->dimming_target : 1.0;
      brightness = MIN (max, target);

      scale_set_backlight (scale, brightness);
    }

  if (scales_updated)
    save_brightnesses (self);

  self->inhibit_updates = FALSE;
}

static void
drop_monitor_scales (ShellBrightnessManager *self)
{
  guint i;

  for (i = 0; i < self->monitor_scales->len; i++)
    {
      ShellBrightnessScale *scale = g_ptr_array_index (self->monitor_scales, i);

      shell_brightness_scale_destroy (scale);
      g_signal_handlers_disconnect_by_data (scale, self);
    }

  g_ptr_array_set_size (self->monitor_scales, 0);
}

static void
monitors_changed (ShellBrightnessManager *self)
{
  g_autoptr (GPtrArray) monitors = g_ptr_array_new ();
  g_autoptr (GArray) brightnesses = NULL;
  GList *l;
  guint i;

  for (l = meta_monitor_manager_get_logical_monitors (self->monitor_manager); l; l = l->next)
    {
      if (logical_monitor_has_backlight (l->data))
        g_ptr_array_add (monitors, l->data);
    }

  drop_monitor_scales (self);

  brightnesses = get_saved_brightnesses (monitors);

  for (i = 0; i < monitors->len; i++)
    {
      ShellBrightnessScale *scale;

      scale = monitor_scale_new (g_ptr_array_index (monitors, i),
                                 g_array_index (brightnesses, double, i));
      scale->scale_changed = TRUE;

      g_signal_connect_object (scale, "backlights-changed",
                               G_CALLBACK (on_backlights_changed), self, 0);
      g_signal_connect_object (scale, "notify::value",
                               G_CALLBACK (on_monitor_scale_value_notify), self, 0);

      g_ptr_array_add (self->monitor_scales, scale);
    }

  if (monitors->len == 0)
    {
      if (self->global_scale != NULL)
        {
          g_signal_handlers_disconnect_by_data (self->global_scale, self);
          g_clear_object (&self->global_scale);
          g_object_notify_by_pspec (G_OBJECT (self), props[PROP_GLOBAL_SCALE]);
        }
    }
  else if (self->global_scale == NULL)
    {
      /* Handle scales with just a few steps */
      int max_steps = 0;
      int n_steps;

      for (i = 0; i < self->monitor_scales->len; i++)
        {
          ShellBrightnessScale *scale = g_ptr_array_index (self->monitor_scales, i);

          max_steps = MAX (max_steps, scale->n_steps);
        }
      n_steps = CLAMP (max_steps, 1, SCALE_VALUE_N_STEPS);

      self->global_scale = g_object_new (SHELL_TYPE_BRIGHTNESS_SCALE,
                                         "name", _("Brightness"),
                                         "n-steps", n_steps,
                                         NULL);
      g_signal_connect (self->global_scale, "notify::value",
                        G_CALLBACK (on_global_scale_value_notify), self);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_GLOBAL_SCALE]);
    }

  sync_scales (self, FALSE);

  emit_changed (self);
}

static void
on_shutdown (ShellGlobal            *global,
             ShellBrightnessManager *self)
{
  /* Nothing is emitted to JS from now on, and the display goes away */
  self->shutting_down = TRUE;

  if (self->monitor_manager != NULL)
    g_signal_handlers_disconnect_by_data (self->monitor_manager, self);

  remove_keybindings (self);
}

static void
shell_brightness_manager_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellBrightnessManager *self = SHELL_BRIGHTNESS_MANAGER (object);

  switch (prop_id)
    {
    case PROP_DIMMING:
      g_value_set_boolean (value, self->dimming_enabled);
      break;

    case PROP_AUTO_BRIGHTNESS_TARGET:
      g_value_set_double (value, self->ab_target);
      break;

    case PROP_GLOBAL_SCALE:
      g_value_set_object (value, self->global_scale);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_brightness_manager_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellBrightnessManager *self = SHELL_BRIGHTNESS_MANAGER (object);

  switch (prop_id)
    {
    case PROP_DIMMING:
      shell_brightness_manager_set_dimming (self, g_value_get_boolean (value));
      break;

    case PROP_AUTO_BRIGHTNESS_TARGET:
      shell_brightness_manager_set_auto_brightness_target (self, g_value_get_double (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_brightness_manager_constructed (GObject *object)
{
  ShellBrightnessManager *self = SHELL_BRIGHTNESS_MANAGER (object);
  ShellGlobal *global = shell_global_get ();
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = NULL;

  G_OBJECT_CLASS (shell_brightness_manager_parent_class)->constructed (object);

  /* This is still being used in the power plugin for the keyboard backlight
   * so we just use that setting here */
  self->dimming_target = 0.3;
  if (source != NULL)
    schema = g_settings_schema_source_lookup (source, POWER_SCHEMA, TRUE);
  if (schema != NULL)
    {
      g_autoptr (GSettings) power_settings = g_settings_new_full (schema, NULL, NULL);

      self->dimming_target = g_settings_get_int (power_settings, "idle-brightness") / 100.0;
    }
  else
    {
      g_warning ("GSettings schema %s is not installed", POWER_SCHEMA);
    }

  add_keybindings (self);

  g_signal_connect_object (global, "shutdown", G_CALLBACK (on_shutdown), self, 0);

  self->monitor_manager = meta_backend_get_monitor_manager (shell_global_get_backend (global));
  g_signal_connect_swapped (self->monitor_manager, "monitors-changed",
                            G_CALLBACK (monitors_changed), self);
  monitors_changed (self);
}

static void
shell_brightness_manager_dispose (GObject *object)
{
  ShellBrightnessManager *self = SHELL_BRIGHTNESS_MANAGER (object);

  g_clear_handle_id (&self->save_id, g_source_remove);

  if (self->monitor_manager != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->monitor_manager, self);
      self->monitor_manager = NULL;
    }

  if (!self->shutting_down)
    remove_keybindings (self);
  g_clear_object (&self->keybinding_settings);

  if (self->monitor_scales != NULL)
    {
      guint i;

      for (i = 0; i < self->monitor_scales->len; i++)
        g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->monitor_scales, i),
                                              self);
      g_clear_pointer (&self->monitor_scales, g_ptr_array_unref);
    }

  if (self->global_scale != NULL)
    g_signal_handlers_disconnect_by_data (self->global_scale, self);
  g_clear_object (&self->global_scale);

  G_OBJECT_CLASS (shell_brightness_manager_parent_class)->dispose (object);
}

static void
shell_brightness_manager_class_init (ShellBrightnessManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_brightness_manager_constructed;
  object_class->dispose = shell_brightness_manager_dispose;
  object_class->get_property = shell_brightness_manager_get_property;
  object_class->set_property = shell_brightness_manager_set_property;

  /**
   * ShellBrightnessManager:dimming:
   *
   * Whether the brightness is limited to the idle brightness of the power
   * settings. Reset when a backlight is changed by something else.
   */
  props[PROP_DIMMING] =
    g_param_spec_boolean ("dimming", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellBrightnessManager:auto-brightness-target:
   *
   * The brightness the ambient light sensor asks for, or a negative value
   * if the automatic brightness is not active. The scales then bias it.
   */
  props[PROP_AUTO_BRIGHTNESS_TARGET] =
    g_param_spec_double ("auto-brightness-target", NULL, NULL,
                         -G_MAXDOUBLE, G_MAXDOUBLE, -1.0,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellBrightnessManager:global-scale:
   *
   * The scale that follows the maximum of the scales of the monitors, or
   * %NULL if no monitor has a backlight.
   */
  props[PROP_GLOBAL_SCALE] =
    g_param_spec_object ("global-scale", NULL, NULL,
                         SHELL_TYPE_BRIGHTNESS_SCALE,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellBrightnessManager::changed:
   *
   * Emitted when the monitors that have a backlight changed, and with them
   * the scales.
   */
  signals[CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellBrightnessManager::user-update:
   *
   * Emitted when the user changed the value of one of the scales.
   */
  signals[USER_UPDATE] =
    g_signal_new ("user-update",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellBrightnessManager::osd-requested:
   * @self: the #ShellBrightnessManager
   * @icon: the icon to show
   * @levels: a `a(idd)` variant of `(monitor, level, max-level)`
   *
   * Emitted when the on-screen display should show the brightness of the
   * monitors in @levels. src/ui/main.c shows it with
   * `osdWindowManager.show (icon, null, levels)`.
   */
  signals[OSD_REQUESTED] =
    g_signal_new ("osd-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_ICON, G_TYPE_VARIANT);
}

static void
shell_brightness_manager_init (ShellBrightnessManager *self)
{
  self->monitor_scales = g_ptr_array_new_with_free_func (g_object_unref);
  self->ab_target = -1.0;
}

/**
 * shell_brightness_manager_new:
 *
 * Creates the brightness manager, which adds the brightness keybindings and
 * starts to follow the monitors. src/ui/main.c creates the only instance.
 *
 * Returns: (transfer full): a new #ShellBrightnessManager
 */
ShellBrightnessManager *
shell_brightness_manager_new (void)
{
  return g_object_new (SHELL_TYPE_BRIGHTNESS_MANAGER, NULL);
}

/**
 * shell_brightness_manager_get_keybinding_names:
 *
 * The names of the keybindings that the manager adds, which the window
 * manager has to allow in all action modes.
 *
 * Returns: (transfer none) (array zero-terminated=1) (element-type utf8): the
 *   names of the keybindings
 */
const char * const *
shell_brightness_manager_get_keybinding_names (void)
{
  return keybinding_names;
}

/**
 * shell_brightness_manager_get_dimming:
 * @self: a #ShellBrightnessManager
 *
 * Returns: whether the brightness is limited to the dimming target
 */
gboolean
shell_brightness_manager_get_dimming (ShellBrightnessManager *self)
{
  g_return_val_if_fail (SHELL_IS_BRIGHTNESS_MANAGER (self), FALSE);

  return self->dimming_enabled;
}

/**
 * shell_brightness_manager_set_dimming:
 * @self: a #ShellBrightnessManager
 * @dimming: whether to limit the brightness to the dimming target
 *
 * Enables or disables dimming and updates the backlights.
 */
void
shell_brightness_manager_set_dimming (ShellBrightnessManager *self,
                                      gboolean                dimming)
{
  g_return_if_fail (SHELL_IS_BRIGHTNESS_MANAGER (self));

  if (self->dimming_enabled != dimming)
    {
      self->dimming_enabled = dimming;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_DIMMING]);
    }

  sync_scales (self, TRUE);
}

/**
 * shell_brightness_manager_get_auto_brightness_target:
 * @self: a #ShellBrightnessManager
 *
 * Returns: the automatic brightness target, negative if not active
 */
double
shell_brightness_manager_get_auto_brightness_target (ShellBrightnessManager *self)
{
  g_return_val_if_fail (SHELL_IS_BRIGHTNESS_MANAGER (self), -1.0);

  return self->ab_target;
}

/**
 * shell_brightness_manager_set_auto_brightness_target:
 * @self: a #ShellBrightnessManager
 * @target: the automatic brightness target, negative to turn it off
 *
 * Sets the automatic brightness target and updates the backlights.
 */
void
shell_brightness_manager_set_auto_brightness_target (ShellBrightnessManager *self,
                                                     double                  target)
{
  g_return_if_fail (SHELL_IS_BRIGHTNESS_MANAGER (self));

  if (self->ab_target != target)
    {
      self->ab_target = target;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_AUTO_BRIGHTNESS_TARGET]);
    }

  sync_scales (self, TRUE);
}

/**
 * shell_brightness_manager_get_global_scale:
 * @self: a #ShellBrightnessManager
 *
 * Returns: (transfer none) (nullable): the global scale, or %NULL if no
 *   monitor has a backlight
 */
ShellBrightnessScale *
shell_brightness_manager_get_global_scale (ShellBrightnessManager *self)
{
  g_return_val_if_fail (SHELL_IS_BRIGHTNESS_MANAGER (self), NULL);

  return self->global_scale;
}

/**
 * shell_brightness_manager_get_scales:
 * @self: a #ShellBrightnessManager
 *
 * Returns: (transfer container) (element-type ShellBrightnessScale): the
 *   scales of the monitors that have a backlight
 */
GList *
shell_brightness_manager_get_scales (ShellBrightnessManager *self)
{
  GList *scales = NULL;
  guint i;

  g_return_val_if_fail (SHELL_IS_BRIGHTNESS_MANAGER (self), NULL);

  for (i = self->monitor_scales->len; i > 0; i--)
    scales = g_list_prepend (scales, g_ptr_array_index (self->monitor_scales, i - 1));

  return scales;
}
