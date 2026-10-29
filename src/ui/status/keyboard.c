/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * keyboard.c: The input sources (keyboard layouts and IBus engines) of the
 * session, the manager which switches between them, and the menu of the top
 * bar which lists them. Port of js/ui/status/keyboard.js.
 *
 * The public types are #ShellInputSource (`InputSource`),
 * #ShellInputSourceManager (`InputSourceManager`) and
 * #ShellInputSourceIndicator (`InputSourceIndicator`). Private to this file:
 * ShellInputSourcePopup and ShellInputSourceSwitcher (`InputSourcePopup`,
 * `InputSourceSwitcher`), ShellInputSourceSettings with its system and
 * session subclasses (`InputSourceSettings`, `InputSourceSystemSettings`,
 * `InputSourceSessionSettings`), ShellInputSourceLayoutMenuItem
 * (`LayoutMenuItem`) and ShellInputSourceIndicatorContainer.
 *
 * `Main.wm`, `Main.inputMethod` and `Main.keyboard` are read from
 * #ShellMain (the properties `window-manager`, `input-method` and
 * `keyboard`), `Main.overview.visible` from
 * #ShellWindowManager:overview-visible. The on-screen keyboard (ui/keyboard.c)
 * reads the input sources straight from the manager.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The state kept per window (the input sources it saw and the source
 *    which was current in it) is a record attached to the #MetaWindow with
 *    g_object_set_qdata (the JS set expandos on the window); the overview
 *    record is a field of the manager. A generation number of the list of
 *    input sources replaces the comparison of the lists.
 *  - The switcher popup holds a copy of the MRU list, not the list itself.
 *  - `connectOsk ()`, which pushed the state of the manager to the on-screen
 *    keyboard, is gone.
 *  - A GSettings schema that is not installed leaves the settings empty (no
 *    input source but the default layout) instead of throwing, and a system
 *    bus that is not there leaves the greeter settings empty.
 *  - `_updateSubProperty ()` and the other IBus property walks stop at the
 *    first missing property like the JS did, but a missing symbol or label
 *    is an empty string instead of a TypeError.
 *  - The input mode label and `_setSourceAsInactive ()` do nothing instead
 *    of throwing when the indicator has no label for the source.
 *  - After ShellGlobal::shutdown the key bindings and the D-Bus completions
 *    of the manager do nothing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n-lib.h>
#include <clutter/clutter.h>
#include <meta/display.h>
#include <meta/meta-window-actor.h>
#include <meta/window.h>
#include <pango/pango.h>

#include "shell-app-system.h"
#include "shell-global.h"
#include "misc/ibus-manager.h"
#include "misc/input-method.h"
#include "misc/keyboard-manager.h"
#include "ui/ibus-candidate-popup.h"
#include "ui/main.h"
#include "ui/popup-menu.h"
#include "ui/status/keyboard.h"
#include "ui/status/status-private.h"
#include "ui/switcher-popup.h"
#include "ui/window-manager.h"

#define DESKTOP_INPUT_SOURCES_SCHEMA "org.gnome.desktop.input-sources"
#define KEY_INPUT_SOURCES            "sources"
#define KEY_MRU_SOURCES              "mru-sources"
#define KEY_KEYBOARD_OPTIONS         "xkb-options"
#define KEY_KEYBOARD_MODEL           "xkb-model"
#define KEY_PER_WINDOW               "per-window"

#define WM_KEYBINDINGS_SCHEMA        "org.gnome.desktop.wm.keybindings"

#define LOCALE1_BUS_NAME             "org.freedesktop.locale1"
#define LOCALE1_BUS_PATH             "/org/freedesktop/locale1"
#define LOCALE1_BUS_IFACE            "org.freedesktop.locale1"
#define DBUS_PROPS_IFACE             "org.freedesktop.DBus.Properties"

/* The glyph of a keyboard, for an engine with no symbol and no language */
#define KEYBOARD_GLYPH               "\xe2\x8c\xa8"

/* ======================================================================== */
/* ShellInputSource                                                         */
/* ======================================================================== */

enum
{
  SOURCE_PROP_0,

  SOURCE_PROP_SOURCE_TYPE,
  SOURCE_PROP_ID,
  SOURCE_PROP_DISPLAY_NAME,
  SOURCE_PROP_SHORT_NAME,
  SOURCE_PROP_INDEX,
  SOURCE_PROP_XKB_ID,
  SOURCE_PROP_PROPERTIES,
  SOURCE_PROP_LOCKED,

  SOURCE_N_PROPS
};

static GParamSpec *source_props[SOURCE_N_PROPS] = { NULL, };

enum
{
  SOURCE_ACTIVATE,
  SOURCE_CHANGED,
  SOURCE_LAST_SIGNAL
};

static guint source_signals[SOURCE_LAST_SIGNAL] = { 0, };

struct _ShellInputSource
{
  GObject parent;

  char *source_type;
  char *id;
  char *display_name;
  char *short_name;
  int index;
  char *xkb_id;
  IBusPropList *properties;
  gboolean locked;
};

G_DEFINE_FINAL_TYPE (ShellInputSource, shell_input_source, G_TYPE_OBJECT)

/* _getXkbId () */
static char *
input_source_dup_xkb_id (ShellInputSource *self)
{
  IBusEngineDesc *engine_desc = shell_ibus_manager_get_engine_desc (shell_ibus_manager_get_default (),
                                                                    self->id);
  const char *layout, *variant;

  if (engine_desc == NULL)
    return g_strdup (self->id);

  layout = ibus_engine_desc_get_layout (engine_desc);
  variant = ibus_engine_desc_get_layout_variant (engine_desc);
  if (layout == NULL)
    layout = "";

  if (variant != NULL && variant[0] != '\0')
    return g_strdup_printf ("%s+%s", layout, variant);

  return g_strdup (layout);
}

static void
shell_input_source_constructed (GObject *object)
{
  ShellInputSource *self = SHELL_INPUT_SOURCE (object);

  G_OBJECT_CLASS (shell_input_source_parent_class)->constructed (object);

  self->xkb_id = input_source_dup_xkb_id (self);
}

static void
shell_input_source_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellInputSource *self = SHELL_INPUT_SOURCE (object);

  switch (prop_id)
    {
    case SOURCE_PROP_SOURCE_TYPE:
      g_value_set_string (value, self->source_type);
      break;

    case SOURCE_PROP_ID:
      g_value_set_string (value, self->id);
      break;

    case SOURCE_PROP_DISPLAY_NAME:
      g_value_set_string (value, self->display_name);
      break;

    case SOURCE_PROP_SHORT_NAME:
      g_value_set_string (value, self->short_name);
      break;

    case SOURCE_PROP_INDEX:
      g_value_set_int (value, self->index);
      break;

    case SOURCE_PROP_XKB_ID:
      g_value_set_string (value, self->xkb_id);
      break;

    case SOURCE_PROP_PROPERTIES:
      g_value_set_object (value, self->properties);
      break;

    case SOURCE_PROP_LOCKED:
      g_value_set_boolean (value, self->locked);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_input_source_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellInputSource *self = SHELL_INPUT_SOURCE (object);

  switch (prop_id)
    {
    case SOURCE_PROP_SOURCE_TYPE:
      g_free (self->source_type);
      self->source_type = g_value_dup_string (value);
      break;

    case SOURCE_PROP_ID:
      g_free (self->id);
      self->id = g_value_dup_string (value);
      break;

    case SOURCE_PROP_DISPLAY_NAME:
      g_free (self->display_name);
      self->display_name = g_value_dup_string (value);
      break;

    case SOURCE_PROP_SHORT_NAME:
      shell_input_source_set_short_name (self, g_value_get_string (value));
      break;

    case SOURCE_PROP_INDEX:
      self->index = g_value_get_int (value);
      break;

    case SOURCE_PROP_PROPERTIES:
      shell_input_source_set_properties (self, g_value_get_object (value));
      break;

    case SOURCE_PROP_LOCKED:
      shell_input_source_set_locked (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_input_source_dispose (GObject *object)
{
  ShellInputSource *self = SHELL_INPUT_SOURCE (object);

  g_clear_object (&self->properties);

  G_OBJECT_CLASS (shell_input_source_parent_class)->dispose (object);
}

static void
shell_input_source_finalize (GObject *object)
{
  ShellInputSource *self = SHELL_INPUT_SOURCE (object);

  g_free (self->source_type);
  g_free (self->id);
  g_free (self->display_name);
  g_free (self->short_name);
  g_free (self->xkb_id);

  G_OBJECT_CLASS (shell_input_source_parent_class)->finalize (object);
}

static void
shell_input_source_class_init (ShellInputSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_input_source_constructed;
  object_class->get_property = shell_input_source_get_property;
  object_class->set_property = shell_input_source_set_property;
  object_class->dispose = shell_input_source_dispose;
  object_class->finalize = shell_input_source_finalize;

  /**
   * ShellInputSource:source-type:
   *
   * The type of the input source: "xkb", "ibus" or "external" (`type`).
   */
  source_props[SOURCE_PROP_SOURCE_TYPE] =
    g_param_spec_string ("source-type", NULL, NULL, NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellInputSource:id:
   *
   * The id of the XKB layout, or the name of the IBus engine.
   */
  source_props[SOURCE_PROP_ID] =
    g_param_spec_string ("id", NULL, NULL, NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellInputSource:display-name:
   *
   * The name of the input source in the menu (`displayName`).
   */
  source_props[SOURCE_PROP_DISPLAY_NAME] =
    g_param_spec_string ("display-name", NULL, NULL, NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellInputSource:short-name:
   *
   * The short name of the input source, shown in the top bar (`shortName`).
   * #ShellInputSource::changed is emitted whenever it is set.
   */
  source_props[SOURCE_PROP_SHORT_NAME] =
    g_param_spec_string ("short-name", NULL, NULL, NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellInputSource:index:
   *
   * The position of the input source in the list of the settings.
   */
  source_props[SOURCE_PROP_INDEX] =
    g_param_spec_int ("index", NULL, NULL,
                      G_MININT, G_MAXINT, 0,
                      G_PARAM_READWRITE |
                      G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellInputSource:xkb-id:
   *
   * The id of the XKB layout to apply for the input source (`xkbId`):
   * the layout of the engine for an IBus input source.
   */
  source_props[SOURCE_PROP_XKB_ID] =
    g_param_spec_string ("xkb-id", NULL, NULL, NULL,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellInputSource:properties:
   *
   * The properties the IBus engine registered for its menu, if it did.
   */
  source_props[SOURCE_PROP_PROPERTIES] =
    g_param_spec_object ("properties", NULL, NULL,
                         IBUS_TYPE_PROP_LIST,
                         G_PARAM_READWRITE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellInputSource:locked:
   *
   * Whether the keyboard has a fixed layout (only set on the external input
   * source of the indicator).
   */
  source_props[SOURCE_PROP_LOCKED] =
    g_param_spec_boolean ("locked", NULL, NULL, FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, SOURCE_N_PROPS, source_props);

  /**
   * ShellInputSource::activate:
   * @self: the #ShellInputSource
   * @interactive: whether the user asked for the switch
   *
   * Emitted by shell_input_source_activate (); the input source manager
   * switches to the input source.
   */
  source_signals[SOURCE_ACTIVATE] =
    g_signal_new ("activate",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN);

  /**
   * ShellInputSource::changed:
   * @self: the #ShellInputSource
   *
   * Emitted when the short name is set.
   */
  source_signals[SOURCE_CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_input_source_init (ShellInputSource *self)
{
}

/**
 * shell_input_source_new:
 * @source_type: the type of the input source, one of the
 *   `SHELL_INPUT_SOURCE_TYPE_*` strings
 * @id: the id of the layout or the name of the engine
 * @display_name: the name of the input source in the menu
 * @short_name: the short name of the input source
 * @index: the position of the input source in the list of the settings
 *
 * Creates an input source (`new InputSource ()`).
 *
 * Returns: (transfer full): a new #ShellInputSource
 */
ShellInputSource *
shell_input_source_new (const char *source_type,
                        const char *id,
                        const char *display_name,
                        const char *short_name,
                        int         index)
{
  return g_object_new (SHELL_TYPE_INPUT_SOURCE,
                       "source-type", source_type,
                       "id", id,
                       "display-name", display_name,
                       "short-name", short_name,
                       "index", index,
                       NULL);
}

/**
 * shell_input_source_get_source_type:
 * @self: a #ShellInputSource
 *
 * Returns: #ShellInputSource:source-type
 */
const char *
shell_input_source_get_source_type (ShellInputSource *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_SOURCE (self), NULL);

  return self->source_type;
}

/**
 * shell_input_source_get_id:
 * @self: a #ShellInputSource
 *
 * Returns: #ShellInputSource:id
 */
const char *
shell_input_source_get_id (ShellInputSource *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_SOURCE (self), NULL);

  return self->id;
}

/**
 * shell_input_source_get_display_name:
 * @self: a #ShellInputSource
 *
 * Returns: #ShellInputSource:display-name
 */
const char *
shell_input_source_get_display_name (ShellInputSource *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_SOURCE (self), NULL);

  return self->display_name;
}

/**
 * shell_input_source_get_short_name:
 * @self: a #ShellInputSource
 *
 * Returns: #ShellInputSource:short-name
 */
const char *
shell_input_source_get_short_name (ShellInputSource *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_SOURCE (self), NULL);

  return self->short_name;
}

/**
 * shell_input_source_set_short_name:
 * @self: a #ShellInputSource
 * @short_name: the new short name
 *
 * Sets #ShellInputSource:short-name and emits #ShellInputSource::changed.
 */
void
shell_input_source_set_short_name (ShellInputSource *self,
                                   const char       *short_name)
{
  g_return_if_fail (SHELL_IS_INPUT_SOURCE (self));

  if (self->short_name != short_name)
    {
      char *copy = g_strdup (short_name);

      g_free (self->short_name);
      self->short_name = copy;
    }

  g_signal_emit (self, source_signals[SOURCE_CHANGED], 0);
}

/**
 * shell_input_source_get_index:
 * @self: a #ShellInputSource
 *
 * Returns: #ShellInputSource:index
 */
int
shell_input_source_get_index (ShellInputSource *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_SOURCE (self), -1);

  return self->index;
}

/**
 * shell_input_source_get_xkb_id:
 * @self: a #ShellInputSource
 *
 * Returns: #ShellInputSource:xkb-id
 */
const char *
shell_input_source_get_xkb_id (ShellInputSource *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_SOURCE (self), NULL);

  return self->xkb_id;
}

/**
 * shell_input_source_get_properties:
 * @self: a #ShellInputSource
 *
 * Returns: (transfer none) (nullable): #ShellInputSource:properties
 */
IBusPropList *
shell_input_source_get_properties (ShellInputSource *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_SOURCE (self), NULL);

  return self->properties;
}

/**
 * shell_input_source_set_properties:
 * @self: a #ShellInputSource
 * @properties: (nullable): the properties of the engine
 *
 * Sets #ShellInputSource:properties.
 */
void
shell_input_source_set_properties (ShellInputSource *self,
                                   IBusPropList     *properties)
{
  g_return_if_fail (SHELL_IS_INPUT_SOURCE (self));
  g_return_if_fail (properties == NULL || IBUS_IS_PROP_LIST (properties));

  if (g_set_object (&self->properties, properties))
    g_object_notify_by_pspec (G_OBJECT (self), source_props[SOURCE_PROP_PROPERTIES]);
}

/**
 * shell_input_source_get_locked:
 * @self: a #ShellInputSource
 *
 * Returns: #ShellInputSource:locked
 */
gboolean
shell_input_source_get_locked (ShellInputSource *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_SOURCE (self), FALSE);

  return self->locked;
}

/**
 * shell_input_source_set_locked:
 * @self: a #ShellInputSource
 * @locked: whether the layout is fixed
 *
 * Sets #ShellInputSource:locked.
 */
void
shell_input_source_set_locked (ShellInputSource *self,
                               gboolean          locked)
{
  g_return_if_fail (SHELL_IS_INPUT_SOURCE (self));

  if (self->locked == !!locked)
    return;

  self->locked = !!locked;
  g_object_notify_by_pspec (G_OBJECT (self), source_props[SOURCE_PROP_LOCKED]);
}

/**
 * shell_input_source_activate:
 * @self: a #ShellInputSource
 * @interactive: whether the user asked for the switch
 *
 * Switches to the input source (`activate ()`) by emitting
 * #ShellInputSource::activate.
 */
void
shell_input_source_activate (ShellInputSource *self,
                             gboolean          interactive)
{
  g_return_if_fail (SHELL_IS_INPUT_SOURCE (self));

  g_signal_emit (self, source_signals[SOURCE_ACTIVATE], 0, !!interactive);
}

static gboolean
input_source_matches (ShellInputSource *a,
                      ShellInputSource *b)
{
  return g_strcmp0 (a->source_type, b->source_type) == 0 &&
         g_strcmp0 (a->id, b->id) == 0;
}

/* ======================================================================== */
/* ShellInputSourceSettings (abstract), with the system and session ones    */
/* ======================================================================== */

/* One entry of the `sources` and `mru-sources` lists: {type, id} */
typedef struct
{
  char *source_type;
  char *id;
} SourceDesc;

static SourceDesc *
source_desc_new (const char *source_type,
                 const char *id)
{
  SourceDesc *desc = g_new0 (SourceDesc, 1);

  desc->source_type = g_strdup (source_type);
  desc->id = g_strdup (id);
  return desc;
}

static void
source_desc_free (SourceDesc *desc)
{
  g_free (desc->source_type);
  g_free (desc->id);
  g_free (desc);
}

static GPtrArray *
source_desc_array_new (void)
{
  return g_ptr_array_new_with_free_func ((GDestroyNotify) source_desc_free);
}

#define SHELL_TYPE_INPUT_SOURCE_SETTINGS (shell_input_source_settings_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellInputSourceSettings, shell_input_source_settings,
                          SHELL, INPUT_SOURCE_SETTINGS, GObject)

struct _ShellInputSourceSettingsClass
{
  GObjectClass parent_class;

  GPtrArray *(* get_input_sources)    (ShellInputSourceSettings *self);
  GPtrArray *(* get_mru_sources)      (ShellInputSourceSettings *self);
  void       (* set_mru_sources)      (ShellInputSourceSettings *self,
                                       GPtrArray                *sources);
  char     **(* get_keyboard_options) (ShellInputSourceSettings *self);
  char      *(* get_keyboard_model)   (ShellInputSourceSettings *self);
  gboolean   (* get_per_window)       (ShellInputSourceSettings *self);
};

enum
{
  SETTINGS_INPUT_SOURCES_CHANGED,
  SETTINGS_KEYBOARD_OPTIONS_CHANGED,
  SETTINGS_KEYBOARD_MODEL_CHANGED,
  SETTINGS_PER_WINDOW_CHANGED,
  SETTINGS_LAST_SIGNAL
};

static guint settings_signals[SETTINGS_LAST_SIGNAL] = { 0, };

G_DEFINE_ABSTRACT_TYPE (ShellInputSourceSettings, shell_input_source_settings, G_TYPE_OBJECT)

static GPtrArray *
settings_real_get_sources (ShellInputSourceSettings *self)
{
  return source_desc_array_new ();
}

static void
settings_real_set_mru_sources (ShellInputSourceSettings *self,
                               GPtrArray                *sources)
{
  /* do nothing */
}

static char **
settings_real_get_keyboard_options (ShellInputSourceSettings *self)
{
  return g_new0 (char *, 1);
}

static char *
settings_real_get_keyboard_model (ShellInputSourceSettings *self)
{
  return g_strdup ("");
}

static gboolean
settings_real_get_per_window (ShellInputSourceSettings *self)
{
  return FALSE;
}

static void
shell_input_source_settings_class_init (ShellInputSourceSettingsClass *klass)
{
  GType type = G_TYPE_FROM_CLASS (klass);

  klass->get_input_sources = settings_real_get_sources;
  klass->get_mru_sources = settings_real_get_sources;
  klass->set_mru_sources = settings_real_set_mru_sources;
  klass->get_keyboard_options = settings_real_get_keyboard_options;
  klass->get_keyboard_model = settings_real_get_keyboard_model;
  klass->get_per_window = settings_real_get_per_window;

  settings_signals[SETTINGS_INPUT_SOURCES_CHANGED] =
    g_signal_new ("input-sources-changed", type, G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  settings_signals[SETTINGS_KEYBOARD_OPTIONS_CHANGED] =
    g_signal_new ("keyboard-options-changed", type, G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  settings_signals[SETTINGS_KEYBOARD_MODEL_CHANGED] =
    g_signal_new ("keyboard-model-changed", type, G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  settings_signals[SETTINGS_PER_WINDOW_CHANGED] =
    g_signal_new ("per-window-changed", type, G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
shell_input_source_settings_init (ShellInputSourceSettings *self)
{
}

static GPtrArray *
settings_get_input_sources (ShellInputSourceSettings *self)
{
  return SHELL_INPUT_SOURCE_SETTINGS_GET_CLASS (self)->get_input_sources (self);
}

static GPtrArray *
settings_get_mru_sources (ShellInputSourceSettings *self)
{
  return SHELL_INPUT_SOURCE_SETTINGS_GET_CLASS (self)->get_mru_sources (self);
}

static void
settings_set_mru_sources (ShellInputSourceSettings *self,
                          GPtrArray                *sources)
{
  SHELL_INPUT_SOURCE_SETTINGS_GET_CLASS (self)->set_mru_sources (self, sources);
}

static char **
settings_get_keyboard_options (ShellInputSourceSettings *self)
{
  return SHELL_INPUT_SOURCE_SETTINGS_GET_CLASS (self)->get_keyboard_options (self);
}

static char *
settings_get_keyboard_model (ShellInputSourceSettings *self)
{
  return SHELL_INPUT_SOURCE_SETTINGS_GET_CLASS (self)->get_keyboard_model (self);
}

static gboolean
settings_get_per_window (ShellInputSourceSettings *self)
{
  return SHELL_INPUT_SOURCE_SETTINGS_GET_CLASS (self)->get_per_window (self);
}

/* ---- the system settings (the login screen): org.freedesktop.locale1 --- */

#define SHELL_TYPE_INPUT_SOURCE_SYSTEM_SETTINGS (shell_input_source_system_settings_get_type ())
G_DECLARE_FINAL_TYPE (ShellInputSourceSystemSettings, shell_input_source_system_settings,
                      SHELL, INPUT_SOURCE_SYSTEM_SETTINGS, ShellInputSourceSettings)

struct _ShellInputSourceSystemSettings
{
  ShellInputSourceSettings parent;

  char *layouts;
  char *variants;
  char *options;
  char *model;

  GCancellable *cancellable;
  GDBusConnection *connection;
  guint subscription_id;
};

G_DEFINE_FINAL_TYPE (ShellInputSourceSystemSettings, shell_input_source_system_settings,
                     SHELL_TYPE_INPUT_SOURCE_SETTINGS)

static void
system_settings_get_all_cb (GObject      *source,
                            GAsyncResult *res,
                            gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) result = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  g_autoptr (GVariant) props = NULL;
  ShellInputSourceSystemSettings *self;
  const char *layouts = "", *variants = "", *options = "", *model = "";

  if (result == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Could not get properties from %s", LOCALE1_BUS_NAME);
      return;
    }

  self = SHELL_INPUT_SOURCE_SYSTEM_SETTINGS (user_data);
  g_variant_get (result, "(@a{sv})", &props);

  g_variant_lookup (props, "X11Layout", "&s", &layouts);
  g_variant_lookup (props, "X11Variant", "&s", &variants);
  g_variant_lookup (props, "X11Options", "&s", &options);
  g_variant_lookup (props, "X11Model", "&s", &model);

  if (g_strcmp0 (layouts, self->layouts) != 0 ||
      g_strcmp0 (variants, self->variants) != 0)
    {
      g_free (self->layouts);
      g_free (self->variants);
      self->layouts = g_strdup (layouts);
      self->variants = g_strdup (variants);
      g_signal_emit (self, settings_signals[SETTINGS_INPUT_SOURCES_CHANGED], 0);
    }
  if (g_strcmp0 (options, self->options) != 0)
    {
      g_free (self->options);
      self->options = g_strdup (options);
      g_signal_emit (self, settings_signals[SETTINGS_KEYBOARD_OPTIONS_CHANGED], 0);
    }
  if (g_strcmp0 (model, self->model) != 0)
    {
      g_free (self->model);
      self->model = g_strdup (model);
      g_signal_emit (self, settings_signals[SETTINGS_KEYBOARD_MODEL_CHANGED], 0);
    }
}

/* _reload () */
static void
system_settings_reload (ShellInputSourceSystemSettings *self)
{
  if (self->connection == NULL)
    return;

  g_dbus_connection_call (self->connection,
                          LOCALE1_BUS_NAME, LOCALE1_BUS_PATH,
                          DBUS_PROPS_IFACE, "GetAll",
                          g_variant_new ("(s)", LOCALE1_BUS_IFACE),
                          G_VARIANT_TYPE ("(a{sv})"),
                          G_DBUS_CALL_FLAGS_NONE, -1,
                          self->cancellable,
                          system_settings_get_all_cb, self);
}

static void
system_settings_properties_changed_cb (GDBusConnection *connection,
                                       const char      *sender_name,
                                       const char      *object_path,
                                       const char      *interface_name,
                                       const char      *signal_name,
                                       GVariant        *parameters,
                                       gpointer         user_data)
{
  system_settings_reload (user_data);
}

static void
system_settings_bus_cb (GObject      *source,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  GDBusConnection *connection = g_bus_get_finish (res, &error);
  ShellInputSourceSystemSettings *self;

  if (connection == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Could not get properties from %s: %s", LOCALE1_BUS_NAME, error->message);
      return;
    }

  self = SHELL_INPUT_SOURCE_SYSTEM_SETTINGS (user_data);
  self->connection = connection;

  self->subscription_id =
    g_dbus_connection_signal_subscribe (self->connection,
                                        LOCALE1_BUS_NAME, DBUS_PROPS_IFACE,
                                        "PropertiesChanged", LOCALE1_BUS_PATH,
                                        NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                        system_settings_properties_changed_cb,
                                        self, NULL);
  system_settings_reload (self);
}

static void
system_settings_shutdown (ShellInputSourceSystemSettings *self)
{
  g_cancellable_cancel (self->cancellable);

  if (self->connection != NULL && self->subscription_id != 0)
    g_dbus_connection_signal_unsubscribe (self->connection, self->subscription_id);
  self->subscription_id = 0;
}

static GPtrArray *
system_settings_get_input_sources (ShellInputSourceSettings *settings)
{
  ShellInputSourceSystemSettings *self = SHELL_INPUT_SOURCE_SYSTEM_SETTINGS (settings);
  GPtrArray *sources_list = source_desc_array_new ();
  g_auto (GStrv) layouts = g_strsplit (self->layouts, ",", -1);
  g_auto (GStrv) variants = g_strsplit (self->variants, ",", -1);
  guint n_variants = g_strv_length (variants);
  guint i;

  for (i = 0; layouts[i] != NULL && layouts[i][0] != '\0'; i++)
    {
      g_autofree char *id = NULL;

      if (i < n_variants && variants[i][0] != '\0')
        id = g_strdup_printf ("%s+%s", layouts[i], variants[i]);
      else
        id = g_strdup (layouts[i]);

      g_ptr_array_add (sources_list, source_desc_new (SHELL_INPUT_SOURCE_TYPE_XKB, id));
    }

  return sources_list;
}

static char **
system_settings_get_keyboard_options (ShellInputSourceSettings *settings)
{
  return g_strsplit (SHELL_INPUT_SOURCE_SYSTEM_SETTINGS (settings)->options, ",", -1);
}

static char *
system_settings_get_keyboard_model (ShellInputSourceSettings *settings)
{
  return g_strdup (SHELL_INPUT_SOURCE_SYSTEM_SETTINGS (settings)->model);
}

static void
system_settings_global_shutdown_cb (ShellGlobal                    *global,
                                    ShellInputSourceSystemSettings *self)
{
  system_settings_shutdown (self);
}

static void
shell_input_source_system_settings_constructed (GObject *object)
{
  ShellInputSourceSystemSettings *self = SHELL_INPUT_SOURCE_SYSTEM_SETTINGS (object);

  G_OBJECT_CLASS (shell_input_source_system_settings_parent_class)->constructed (object);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (system_settings_global_shutdown_cb), self, 0);

  self->cancellable = g_cancellable_new ();
  g_bus_get (G_BUS_TYPE_SYSTEM, self->cancellable, system_settings_bus_cb, self);
}

static void
shell_input_source_system_settings_dispose (GObject *object)
{
  ShellInputSourceSystemSettings *self = SHELL_INPUT_SOURCE_SYSTEM_SETTINGS (object);

  if (self->cancellable != NULL)
    system_settings_shutdown (self);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->connection);

  G_OBJECT_CLASS (shell_input_source_system_settings_parent_class)->dispose (object);
}

static void
shell_input_source_system_settings_finalize (GObject *object)
{
  ShellInputSourceSystemSettings *self = SHELL_INPUT_SOURCE_SYSTEM_SETTINGS (object);

  g_free (self->layouts);
  g_free (self->variants);
  g_free (self->options);
  g_free (self->model);

  G_OBJECT_CLASS (shell_input_source_system_settings_parent_class)->finalize (object);
}

static void
shell_input_source_system_settings_class_init (ShellInputSourceSystemSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellInputSourceSettingsClass *settings_class = SHELL_INPUT_SOURCE_SETTINGS_CLASS (klass);

  object_class->constructed = shell_input_source_system_settings_constructed;
  object_class->dispose = shell_input_source_system_settings_dispose;
  object_class->finalize = shell_input_source_system_settings_finalize;

  settings_class->get_input_sources = system_settings_get_input_sources;
  settings_class->get_keyboard_options = system_settings_get_keyboard_options;
  settings_class->get_keyboard_model = system_settings_get_keyboard_model;
}

static void
shell_input_source_system_settings_init (ShellInputSourceSystemSettings *self)
{
  self->layouts = g_strdup ("");
  self->variants = g_strdup ("");
  self->options = g_strdup ("");
  self->model = g_strdup ("");
}

/* ---- the session settings: org.gnome.desktop.input-sources ------------- */

#define SHELL_TYPE_INPUT_SOURCE_SESSION_SETTINGS (shell_input_source_session_settings_get_type ())
G_DECLARE_FINAL_TYPE (ShellInputSourceSessionSettings, shell_input_source_session_settings,
                      SHELL, INPUT_SOURCE_SESSION_SETTINGS, ShellInputSourceSettings)

struct _ShellInputSourceSessionSettings
{
  ShellInputSourceSettings parent;

  GSettings *settings;
};

G_DEFINE_FINAL_TYPE (ShellInputSourceSessionSettings, shell_input_source_session_settings,
                     SHELL_TYPE_INPUT_SOURCE_SETTINGS)

/* _getSourcesList () */
static GPtrArray *
session_settings_get_sources_list (ShellInputSourceSessionSettings *self,
                                   const char                      *key)
{
  GPtrArray *sources_list = source_desc_array_new ();
  g_autoptr (GVariant) sources = NULL;
  GVariantIter iter;
  const char *source_type, *id;

  if (self->settings == NULL)
    return sources_list;

  sources = g_settings_get_value (self->settings, key);
  g_variant_iter_init (&iter, sources);
  while (g_variant_iter_next (&iter, "(&s&s)", &source_type, &id))
    g_ptr_array_add (sources_list, source_desc_new (source_type, id));

  return sources_list;
}

static GPtrArray *
session_settings_get_input_sources (ShellInputSourceSettings *settings)
{
  return session_settings_get_sources_list (SHELL_INPUT_SOURCE_SESSION_SETTINGS (settings),
                                            KEY_INPUT_SOURCES);
}

static GPtrArray *
session_settings_get_mru_sources (ShellInputSourceSettings *settings)
{
  return session_settings_get_sources_list (SHELL_INPUT_SOURCE_SESSION_SETTINGS (settings),
                                            KEY_MRU_SOURCES);
}

static void
session_settings_set_mru_sources (ShellInputSourceSettings *settings,
                                  GPtrArray                *sources_list)
{
  ShellInputSourceSessionSettings *self = SHELL_INPUT_SOURCE_SESSION_SETTINGS (settings);
  GVariantBuilder builder;
  guint i;

  if (self->settings == NULL)
    return;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ss)"));
  for (i = 0; i < sources_list->len; i++)
    {
      SourceDesc *desc = g_ptr_array_index (sources_list, i);

      g_variant_builder_add (&builder, "(ss)", desc->source_type, desc->id);
    }

  g_settings_set_value (self->settings, KEY_MRU_SOURCES, g_variant_builder_end (&builder));
}

static char **
session_settings_get_keyboard_options (ShellInputSourceSettings *settings)
{
  ShellInputSourceSessionSettings *self = SHELL_INPUT_SOURCE_SESSION_SETTINGS (settings);

  if (self->settings == NULL)
    return g_new0 (char *, 1);

  return g_settings_get_strv (self->settings, KEY_KEYBOARD_OPTIONS);
}

static char *
session_settings_get_keyboard_model (ShellInputSourceSettings *settings)
{
  ShellInputSourceSessionSettings *self = SHELL_INPUT_SOURCE_SESSION_SETTINGS (settings);

  if (self->settings == NULL)
    return g_strdup ("");

  return g_settings_get_string (self->settings, KEY_KEYBOARD_MODEL);
}

static gboolean
session_settings_get_per_window (ShellInputSourceSettings *settings)
{
  ShellInputSourceSessionSettings *self = SHELL_INPUT_SOURCE_SESSION_SETTINGS (settings);

  return self->settings != NULL && g_settings_get_boolean (self->settings, KEY_PER_WINDOW);
}

static void
session_settings_changed_cb (GSettings  *gsettings,
                             const char *key,
                             gpointer    user_data)
{
  ShellInputSourceSessionSettings *self = user_data;
  guint signal_id;

  if (g_strcmp0 (key, KEY_INPUT_SOURCES) == 0)
    signal_id = settings_signals[SETTINGS_INPUT_SOURCES_CHANGED];
  else if (g_strcmp0 (key, KEY_KEYBOARD_OPTIONS) == 0)
    signal_id = settings_signals[SETTINGS_KEYBOARD_OPTIONS_CHANGED];
  else if (g_strcmp0 (key, KEY_KEYBOARD_MODEL) == 0)
    signal_id = settings_signals[SETTINGS_KEYBOARD_MODEL_CHANGED];
  else if (g_strcmp0 (key, KEY_PER_WINDOW) == 0)
    signal_id = settings_signals[SETTINGS_PER_WINDOW_CHANGED];
  else
    return;

  g_signal_emit (self, signal_id, 0);
}

static void
shell_input_source_session_settings_constructed (GObject *object)
{
  ShellInputSourceSessionSettings *self = SHELL_INPUT_SOURCE_SESSION_SETTINGS (object);

  G_OBJECT_CLASS (shell_input_source_session_settings_parent_class)->constructed (object);

  self->settings = shell_status_settings_new (DESKTOP_INPUT_SOURCES_SCHEMA);
  if (self->settings != NULL)
    g_signal_connect_object (self->settings, "changed",
                             G_CALLBACK (session_settings_changed_cb), self, 0);
}

static void
shell_input_source_session_settings_dispose (GObject *object)
{
  ShellInputSourceSessionSettings *self = SHELL_INPUT_SOURCE_SESSION_SETTINGS (object);

  g_clear_object (&self->settings);

  G_OBJECT_CLASS (shell_input_source_session_settings_parent_class)->dispose (object);
}

static void
shell_input_source_session_settings_class_init (ShellInputSourceSessionSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellInputSourceSettingsClass *settings_class = SHELL_INPUT_SOURCE_SETTINGS_CLASS (klass);

  object_class->constructed = shell_input_source_session_settings_constructed;
  object_class->dispose = shell_input_source_session_settings_dispose;

  settings_class->get_input_sources = session_settings_get_input_sources;
  settings_class->get_mru_sources = session_settings_get_mru_sources;
  settings_class->set_mru_sources = session_settings_set_mru_sources;
  settings_class->get_keyboard_options = session_settings_get_keyboard_options;
  settings_class->get_keyboard_model = session_settings_get_keyboard_model;
  settings_class->get_per_window = session_settings_get_per_window;
}

static void
shell_input_source_session_settings_init (ShellInputSourceSessionSettings *self)
{
}

/* ======================================================================== */
/* ShellInputSourceSwitcher and ShellInputSourcePopup                       */
/* ======================================================================== */

#define SHELL_TYPE_INPUT_SOURCE_SWITCHER (shell_input_source_switcher_get_type ())
G_DECLARE_FINAL_TYPE (ShellInputSourceSwitcher, shell_input_source_switcher,
                      SHELL, INPUT_SOURCE_SWITCHER, ShellSwitcherList)

struct _ShellInputSourceSwitcher
{
  ShellSwitcherList parent;
};

G_DEFINE_FINAL_TYPE (ShellInputSourceSwitcher, shell_input_source_switcher,
                     SHELL_TYPE_SWITCHER_LIST)

static void
shell_input_source_switcher_class_init (ShellInputSourceSwitcherClass *klass)
{
}

static void
shell_input_source_switcher_init (ShellInputSourceSwitcher *self)
{
}

/* _addIcon () */
static void
switcher_add_icon (ShellInputSourceSwitcher *self,
                   ShellInputSource         *item)
{
  ClutterActor *box, *label, *symbol, *text;

  box = g_object_new (ST_TYPE_BOX_LAYOUT,
                      "orientation", CLUTTER_ORIENTATION_VERTICAL,
                      NULL);

  label = g_object_new (ST_TYPE_LABEL,
                        "text", item->short_name,
                        "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  symbol = g_object_new (ST_TYPE_BIN,
                         "style-class", "input-source-switcher-symbol",
                         "child", label,
                         NULL);
  clutter_actor_add_child (box, symbol);

  text = g_object_new (ST_TYPE_LABEL,
                       "text", item->display_name,
                       "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       NULL);
  clutter_actor_add_child (box, text);

  shell_switcher_list_add_item (SHELL_SWITCHER_LIST (self), box, text);
}

static ShellInputSourceSwitcher *
switcher_new (GPtrArray *items)
{
  ShellInputSourceSwitcher *self;
  guint i;

  self = g_object_new (SHELL_TYPE_INPUT_SOURCE_SWITCHER,
                       "square-items", TRUE,
                       NULL);

  for (i = 0; i < items->len; i++)
    switcher_add_icon (self, g_ptr_array_index (items, i));

  return self;
}

#define SHELL_TYPE_INPUT_SOURCE_POPUP (shell_input_source_popup_get_type ())
G_DECLARE_FINAL_TYPE (ShellInputSourcePopup, shell_input_source_popup,
                      SHELL, INPUT_SOURCE_POPUP, ShellSwitcherPopup)

struct _ShellInputSourcePopup
{
  ShellSwitcherPopup parent;

  GPtrArray *items;             /* of ShellInputSource */
  uint32_t action;
  uint32_t action_backward;
};

G_DEFINE_FINAL_TYPE (ShellInputSourcePopup, shell_input_source_popup,
                     SHELL_TYPE_SWITCHER_POPUP)

static int
input_source_popup_get_n_items (ShellSwitcherPopup *popup)
{
  ShellInputSourcePopup *self = SHELL_INPUT_SOURCE_POPUP (popup);

  return self->items != NULL ? self->items->len : 0;
}

static gboolean
input_source_popup_key_press_handler (ShellSwitcherPopup *popup,
                                      uint32_t            keysym,
                                      uint32_t            action)
{
  ShellInputSourcePopup *self = SHELL_INPUT_SOURCE_POPUP (popup);

  if (action == self->action)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_next_index (popup));
  else if (action == self->action_backward)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_previous_index (popup));
  else if (keysym == CLUTTER_KEY_Left)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_previous_index (popup));
  else if (keysym == CLUTTER_KEY_Right)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_next_index (popup));
  else
    return CLUTTER_EVENT_PROPAGATE;

  return CLUTTER_EVENT_STOP;
}

static void
input_source_popup_finish (ShellSwitcherPopup *popup,
                           uint32_t            timestamp)
{
  ShellInputSourcePopup *self = SHELL_INPUT_SOURCE_POPUP (popup);
  int selected_index = shell_switcher_popup_get_selected_index (popup);

  SHELL_SWITCHER_POPUP_CLASS (shell_input_source_popup_parent_class)->finish (popup, timestamp);

  if (selected_index >= 0 && (guint) selected_index < self->items->len)
    shell_input_source_activate (g_ptr_array_index (self->items, selected_index), TRUE);
}

static void
shell_input_source_popup_finalize (GObject *object)
{
  ShellInputSourcePopup *self = SHELL_INPUT_SOURCE_POPUP (object);

  g_clear_pointer (&self->items, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_input_source_popup_parent_class)->finalize (object);
}

static void
shell_input_source_popup_class_init (ShellInputSourcePopupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellSwitcherPopupClass *popup_class = SHELL_SWITCHER_POPUP_CLASS (klass);

  object_class->finalize = shell_input_source_popup_finalize;

  popup_class->get_n_items = input_source_popup_get_n_items;
  popup_class->key_press_handler = input_source_popup_key_press_handler;
  popup_class->finish = input_source_popup_finish;
}

static void
shell_input_source_popup_init (ShellInputSourcePopup *self)
{
  self->items = g_ptr_array_new_with_free_func (g_object_unref);
}

static ShellInputSourcePopup *
input_source_popup_new (GPtrArray *items,
                        uint32_t   action,
                        uint32_t   action_backward)
{
  ShellInputSourcePopup *self = g_object_new (SHELL_TYPE_INPUT_SOURCE_POPUP, NULL);
  guint i;

  for (i = 0; i < items->len; i++)
    g_ptr_array_add (self->items, g_object_ref (g_ptr_array_index (items, i)));
  self->action = action;
  self->action_backward = action_backward;

  shell_switcher_popup_set_switcher_list (SHELL_SWITCHER_POPUP (self),
                                          ST_WIDGET (switcher_new (self->items)));
  return self;
}

/* ======================================================================== */
/* ShellInputSourceManager                                                  */
/* ======================================================================== */

enum
{
  MANAGER_PROP_0,

  MANAGER_PROP_CURRENT_SOURCE,

  MANAGER_N_PROPS
};

static GParamSpec *manager_props[MANAGER_N_PROPS] = { NULL, };

enum
{
  MANAGER_SOURCES_CHANGED,
  MANAGER_CURRENT_SOURCE_CHANGED,
  MANAGER_KEYMAP_CHANGED,
  MANAGER_LAST_SIGNAL
};

static guint manager_signals[MANAGER_LAST_SIGNAL] = { 0, };

/* What `window._inputSources` and `window._currentSource` were: the
 * generation of the list of input sources the window saw and the input
 * source that was current in it */
typedef struct
{
  guint64 generation;
  ShellInputSource *current;
} PerWindowState;

struct _ShellInputSourceManager
{
  GObject parent;

  /* All valid input sources currently in the gsettings KEY_INPUT_SOURCES
   * list, indexed by their index there */
  GPtrArray *input_sources;
  /* The same list, of type SHELL_INPUT_SOURCE_TYPE_IBUS, indexed by the
   * IBus id */
  GHashTable *ibus_sources;
  /* Bumped whenever input_sources is replaced */
  guint64 sources_generation;

  ShellInputSource *current_source;

  /* All valid input sources currently in the gsettings KEY_INPUT_SOURCES
   * list ordered by most recently used */
  GPtrArray *mru_sources;
  GPtrArray *mru_sources_backup;
  guint keybinding_action;
  guint keybinding_action_backward;

  ShellWindowManager *window_manager;   /* weak */
  ShellInputMethod *input_method;       /* weak */
  MetaDisplay *display;                 /* weak */
  ShellInputSourceSettings *settings;
  ShellKeyboardManager *keyboard_manager;
  GnomeXkbInfo *xkb_info;

  gboolean ibus_ready;
  ShellIBusManager *ibus_manager;
  ShellIBusCandidatePopup *candidate_popup; /* weak */
  gulong modifiers_accelerator_id;

  gboolean sources_per_window;
  gulong focus_window_notify_id;
  gulong overview_visible_notify_id;
  PerWindowState overview_state;
  gboolean disable_ibus;
  gboolean reloading;

  ShellInputSourcePopup *switcher_popup;  /* weak */
  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellInputSourceManager, shell_input_source_manager, G_TYPE_OBJECT)

static ShellInputSourceManager *default_manager = NULL;

static void manager_input_sources_changed (ShellInputSourceManager *self);

static GQuark
per_window_quark (void)
{
  return g_quark_from_static_string ("shell-input-source-per-window");
}

static void
per_window_state_free (PerWindowState *state)
{
  g_clear_object (&state->current);
  g_free (state);
}

static void
ptr_array_unref_clear (GPtrArray **array)
{
  g_clear_pointer (array, g_ptr_array_unref);
}

static GPtrArray *
source_array_new (void)
{
  return g_ptr_array_new_with_free_func (g_object_unref);
}

static GPtrArray *
source_array_copy (GPtrArray *sources)
{
  GPtrArray *copy = source_array_new ();
  guint i;

  for (i = 0; i < sources->len; i++)
    g_ptr_array_add (copy, g_object_ref (g_ptr_array_index (sources, i)));

  return copy;
}

/* A new list of the sources that are not @source (`filter (s => s !== source)`),
 * with @source in front of them when it is not %NULL */
static GPtrArray *
source_array_with_first (ShellInputSource *first,
                         GPtrArray        *sources,
                         gboolean          match_type_and_id)
{
  GPtrArray *result = source_array_new ();
  guint i;

  if (first != NULL)
    g_ptr_array_add (result, g_object_ref (first));

  for (i = 0; i < sources->len; i++)
    {
      ShellInputSource *source = g_ptr_array_index (sources, i);

      if (first != NULL &&
          (match_type_and_id ? input_source_matches (source, first) : source == first))
        continue;

      g_ptr_array_add (result, g_object_ref (source));
    }

  return result;
}

static void
manager_notify_current_source (ShellInputSourceManager *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), manager_props[MANAGER_PROP_CURRENT_SOURCE]);
}

/* ---- the key bindings and the accelerator that switch ----------------- */

/* _modifiersSwitcher () */
static void
manager_modifiers_switcher (ShellInputSourceManager *self)
{
  ShellInputSource *is;
  guint next_index;

  if (self->input_sources->len == 0)
    return;

  is = self->current_source;
  if (is == NULL)
    is = g_ptr_array_index (self->input_sources, 0);

  next_index = is->index + 1;
  if (next_index >= self->input_sources->len)
    next_index = 0;

  shell_input_source_activate (g_ptr_array_index (self->input_sources, next_index), TRUE);
}

static gboolean
manager_modifiers_accelerator_activated_cb (MetaDisplay             *display,
                                            ShellInputSourceManager *self)
{
  if (!self->shutting_down)
    manager_modifiers_switcher (self);

  return FALSE;
}

static void
manager_switcher_popup_destroy_cb (ClutterActor            *popup,
                                   ShellInputSourceManager *self)
{
  g_clear_weak_pointer (&self->switcher_popup);
}

/* _switchInputSource () */
static void
manager_switch_input_source_cb (MetaDisplay        *display,
                                MetaWindow         *window,
                                const ClutterEvent *event,
                                MetaKeyBinding     *binding,
                                gpointer            user_data)
{
  ShellInputSourceManager *self = user_data;
  ShellInputSourcePopup *popup;

  if (self->shutting_down)
    return;

  if (shell_keyboard_manager_is_locked (self->keyboard_manager))
    return;

  if (self->mru_sources->len < 2)
    return;

  /* HACK: Fall back on simple input source switching since we
   * can't show a popup switcher while a GrabHelper grab is in
   * effect without considerable work to consolidate the usage
   * of pushModal/popModal and grabHelper. See
   * https://bugzilla.gnome.org/show_bug.cgi?id=695143 .
   */
  if (shell_main_get_action_mode (shell_main_get_default ()) == SHELL_ACTION_MODE_POPUP)
    {
      manager_modifiers_switcher (self);
      return;
    }

  popup = input_source_popup_new (self->mru_sources,
                                  self->keybinding_action,
                                  self->keybinding_action_backward);
  g_set_weak_pointer (&self->switcher_popup, popup);
  g_signal_connect (popup, "destroy",
                    G_CALLBACK (manager_switcher_popup_destroy_cb), self);

  if (!shell_switcher_popup_show_switcher (SHELL_SWITCHER_POPUP (popup),
                                           meta_key_binding_is_reversed (binding),
                                           meta_key_binding_get_name (binding),
                                           meta_key_binding_get_mask (binding)))
    shell_switcher_popup_fade_and_destroy (SHELL_SWITCHER_POPUP (popup));
}

/* ---- the settings ----------------------------------------------------- */

static void
manager_apply_keyboard_options (ShellInputSourceManager *self)
{
  g_auto (GStrv) options = settings_get_keyboard_options (self->settings);

  shell_keyboard_manager_set_keyboard_options (self->keyboard_manager,
                                               (const char * const *) options);
}

static void
manager_apply_keyboard_model (ShellInputSourceManager *self)
{
  g_autofree char *model = settings_get_keyboard_model (self->settings);

  shell_keyboard_manager_set_keyboard_model (self->keyboard_manager, model);
}

/* _keyboardOptionsChanged () */
static void
manager_keyboard_options_changed_cb (GObject                 *settings,
                                     ShellInputSourceManager *self)
{
  manager_apply_keyboard_options (self);
  shell_keyboard_manager_reapply (self->keyboard_manager);
}

/* _keyboardModelChanged () */
static void
manager_keyboard_model_changed_cb (GObject                 *settings,
                                   ShellInputSourceManager *self)
{
  manager_apply_keyboard_model (self);
  shell_keyboard_manager_reapply (self->keyboard_manager);
}

static void
manager_input_sources_changed_cb (GObject                 *settings,
                                  ShellInputSourceManager *self)
{
  manager_input_sources_changed (self);
}

/* _keymapChanged () */
static void
manager_keymap_changed_cb (ShellKeyboardManager    *keyboard_manager,
                           ShellInputSourceManager *self)
{
  g_signal_emit (self, manager_signals[MANAGER_KEYMAP_CHANGED], 0);
}

/* ---- the most recently used sources ----------------------------------- */

/* _updateMruSettings () */
static void
manager_update_mru_settings (ShellInputSourceManager *self)
{
  g_autoptr (GPtrArray) sources_list = NULL;
  guint i;

  /* If IBus is not ready we don't have a full picture of all
   * the available sources, so don't update the setting */
  if (!self->ibus_ready)
    return;

  /* If IBus is temporarily disabled, don't update the setting */
  if (self->disable_ibus)
    return;

  sources_list = source_desc_array_new ();
  for (i = 0; i < self->mru_sources->len; ++i)
    {
      ShellInputSource *source = g_ptr_array_index (self->mru_sources, i);

      g_ptr_array_add (sources_list, source_desc_new (source->source_type, source->id));
    }

  settings_set_mru_sources (self->settings, sources_list);
}

/* _changePerWindowSource () needs the state of the current window */
static PerWindowState *
manager_get_current_window_state (ShellInputSourceManager *self)
{
  MetaWindow *window;
  PerWindowState *state;

  /* Main.overview.visible */
  if (self->window_manager != NULL &&
      shell_window_manager_get_overview_visible (self->window_manager))
    return &self->overview_state;

  if (self->display == NULL)
    return NULL;

  window = meta_display_get_focus_window (self->display);
  if (window == NULL)
    return NULL;

  state = g_object_get_qdata (G_OBJECT (window), per_window_quark ());
  if (state == NULL)
    {
      state = g_new0 (PerWindowState, 1);
      g_object_set_qdata_full (G_OBJECT (window), per_window_quark (), state,
                               (GDestroyNotify) per_window_state_free);
    }

  return state;
}

/* _changePerWindowSource () */
static void
manager_change_per_window_source (ShellInputSourceManager *self)
{
  PerWindowState *state;

  if (!self->sources_per_window)
    return;

  state = manager_get_current_window_state (self);
  if (state == NULL)
    return;

  state->generation = self->sources_generation;
  g_set_object (&state->current, self->current_source);
}

/* _currentInputSourceChanged () */
static void
manager_current_input_source_changed (ShellInputSourceManager *self,
                                      ShellInputSource        *new_source,
                                      gboolean                 interactive)
{
  g_autoptr (ShellInputSource) old_source = self->current_source;
  GPtrArray *mru;

  self->current_source = new_source != NULL ? g_object_ref (new_source) : NULL;

  if (self->input_method != NULL)
    shell_input_method_set_has_input_source (self->input_method, new_source != NULL);
  manager_notify_current_source (self);
  g_signal_emit (self, manager_signals[MANAGER_CURRENT_SOURCE_CHANGED], 0, old_source);

  mru = source_array_with_first (new_source, self->mru_sources, FALSE);
  ptr_array_unref_clear (&self->mru_sources);
  self->mru_sources = mru;

  /* Only track user-initiated switches in backup MRU.
   * Internal (non-interactive) activations during reload/reapply must not
   * affect restore order. */
  if (interactive && self->disable_ibus && self->mru_sources_backup != NULL &&
      new_source != NULL)
    {
      mru = source_array_with_first (new_source, self->mru_sources_backup, TRUE);
      ptr_array_unref_clear (&self->mru_sources_backup);
      self->mru_sources_backup = mru;
    }

  manager_change_per_window_source (self);
}

/**
 * shell_input_source_manager_activate_input_source:
 * @self: a #ShellInputSourceManager
 * @source: the input source to switch to
 * @interactive: whether the user asked for the switch
 *
 * Applies the keyboard layout of @source, selects its IBus engine and makes
 * it the current input source (`activateInputSource ()`). The input sources
 * do this themselves when they are activated with
 * shell_input_source_activate ().
 */
void
shell_input_source_manager_activate_input_source (ShellInputSourceManager *self,
                                                  ShellInputSource        *source,
                                                  gboolean                 interactive)
{
  g_autoptr (ShellInputSource) is = NULL;
  g_autofree char *engine = NULL;

  g_return_if_fail (SHELL_IS_INPUT_SOURCE_MANAGER (self));
  g_return_if_fail (SHELL_IS_INPUT_SOURCE (source));

  is = g_object_ref (source);

  shell_keyboard_manager_apply (self->keyboard_manager, is->xkb_id);

  if (g_strcmp0 (is->source_type, SHELL_INPUT_SOURCE_TYPE_IBUS) == 0)
    {
      engine = g_strdup (is->id);
    }
  else
    {
      g_auto (GStrv) parts = g_strsplit (is->id, "+", 3);
      g_autoptr (GList) languages = gnome_xkb_info_get_languages_for_layout (self->xkb_info, is->id);
      const char *name = parts[0] != NULL ? parts[0] : "";
      const char *variant = parts[0] != NULL && parts[1] != NULL ? parts[1] : "";
      const char *lang = languages != NULL ? languages->data : "eng";

      engine = g_strdup_printf ("xkb:%s:%s:%s", name, variant, lang);
    }

  shell_ibus_manager_set_engine (self->ibus_manager, engine);

  manager_current_input_source_changed (self, is, interactive);

  if (interactive)
    manager_update_mru_settings (self);
}

static void
manager_source_activate_cb (ShellInputSource        *source,
                            gboolean                 interactive,
                            ShellInputSourceManager *self)
{
  shell_input_source_manager_activate_input_source (self, source, interactive);
}

static gboolean
mru_contains_desc (GPtrArray  *sources,
                   SourceDesc *desc,
                   guint      *position)
{
  guint i;

  for (i = 0; i < sources->len; i++)
    {
      ShellInputSource *source = g_ptr_array_index (sources, i);

      if (g_strcmp0 (source->source_type, desc->source_type) == 0 &&
          g_strcmp0 (source->id, desc->id) == 0)
        {
          if (position != NULL)
            *position = i;
          return TRUE;
        }
    }

  return FALSE;
}

/* _updateMruSources () */
static void
manager_update_mru_sources (ShellInputSourceManager *self)
{
  g_autoptr (GPtrArray) sources_list = source_array_new ();
  g_autoptr (GPtrArray) ids = g_ptr_array_new ();
  g_autoptr (GPtrArray) mru_sources = source_array_new ();
  guint i, j;

  for (i = 0; i < self->input_sources->len; i++)
    {
      ShellInputSource *source = g_ptr_array_index (self->input_sources, i);

      g_ptr_array_add (sources_list, g_object_ref (source));
      g_ptr_array_add (ids, source->xkb_id);
    }
  g_ptr_array_add (ids, NULL);

  shell_keyboard_manager_set_user_layouts (self->keyboard_manager,
                                           (const char * const *) ids->pdata);

  if (!self->disable_ibus && self->mru_sources_backup != NULL)
    {
      ptr_array_unref_clear (&self->mru_sources);
      self->mru_sources = g_steal_pointer (&self->mru_sources_backup);
    }

  /* Initialize from settings when we have no MRU sources list */
  if (self->mru_sources->len == 0)
    {
      g_autoptr (GPtrArray) mru_settings = settings_get_mru_sources (self->settings);

      for (i = 0; i < mru_settings->len; i++)
        {
          SourceDesc *desc = g_ptr_array_index (mru_settings, i);
          guint position;

          if (mru_contains_desc (sources_list, desc, &position))
            g_ptr_array_add (self->mru_sources,
                             g_object_ref (g_ptr_array_index (sources_list, position)));
        }
    }

  if (self->mru_sources->len > 1)
    {
      for (i = 0; i < self->mru_sources->len; i++)
        {
          ShellInputSource *mru = g_ptr_array_index (self->mru_sources, i);

          for (j = 0; j < sources_list->len; j++)
            {
              ShellInputSource *source = g_ptr_array_index (sources_list, j);

              if (input_source_matches (mru, source))
                {
                  g_ptr_array_add (mru_sources, g_object_ref (source));
                  g_ptr_array_remove_index (sources_list, j);
                  break;
                }
            }
        }
    }

  for (i = 0; i < sources_list->len; i++)
    g_ptr_array_add (mru_sources, g_object_ref (g_ptr_array_index (sources_list, i)));

  ptr_array_unref_clear (&self->mru_sources);
  self->mru_sources = g_steal_pointer (&mru_sources);
}

/* ---- the list of input sources ---------------------------------------- */

/* _makeEngineShortName () */
static char *
make_engine_short_name (IBusEngineDesc *engine_desc)
{
  const char *symbol = ibus_engine_desc_get_symbol (engine_desc);
  const char *language = ibus_engine_desc_get_language (engine_desc);
  g_autofree char *lang_code = NULL;
  const char *underscore;
  gsize length;

  if (symbol != NULL && symbol[0] != '\0')
    return g_strdup (symbol);

  if (language == NULL)
    language = "";
  underscore = strchr (language, '_');
  length = underscore != NULL ? (gsize) (underscore - language) : strlen (language);
  if (length == 2 || length == 3)
    {
      lang_code = g_ascii_strdown (language, length);
      return g_steal_pointer (&lang_code);
    }

  return g_strdup (KEYBOARD_GLYPH); /* keyboard glyph */
}

typedef struct
{
  char *source_type;
  char *id;
  char *display_name;
  char *short_name;
} SourceInfo;

static void
source_info_free (SourceInfo *info)
{
  g_free (info->source_type);
  g_free (info->id);
  g_free (info->display_name);
  g_free (info->short_name);
  g_free (info);
}

static SourceInfo *
source_info_new (const char *source_type,
                 const char *id,
                 const char *display_name,
                 const char *short_name)
{
  SourceInfo *info = g_new0 (SourceInfo, 1);

  info->source_type = g_strdup (source_type);
  info->id = g_strdup (id);
  info->display_name = g_strdup (display_name);
  info->short_name = g_strdup (short_name);
  return info;
}

/* _inputSourcesChanged () */
static void
manager_input_sources_changed (ShellInputSourceManager *self)
{
  g_autoptr (GPtrArray) sources = settings_get_input_sources (self->settings);
  g_autoptr (GPtrArray) infos_list = g_ptr_array_new_with_free_func ((GDestroyNotify) source_info_free);
  g_autoptr (GHashTable) by_short_name = NULL;
  g_autoptr (GPtrArray) new_sources = source_array_new ();
  g_autoptr (GPtrArray) engine_ids = g_ptr_array_new ();
  GHashTableIter iter;
  gpointer key, value;
  guint i;

  g_clear_object (&self->current_source);
  manager_notify_current_source (self);

  for (i = 0; i < sources->len; i++)
    {
      SourceDesc *desc = g_ptr_array_index (sources, i);
      const char *display_name = NULL;
      const char *short_name = NULL;
      g_autofree char *ibus_display_name = NULL;
      g_autofree char *ibus_short_name = NULL;
      gboolean exists = FALSE;

      if (g_strcmp0 (desc->source_type, SHELL_INPUT_SOURCE_TYPE_XKB) == 0)
        {
          exists = gnome_xkb_info_get_layout_info (self->xkb_info, desc->id,
                                                   &display_name, &short_name,
                                                   NULL, NULL);
        }
      else if (g_strcmp0 (desc->source_type, SHELL_INPUT_SOURCE_TYPE_IBUS) == 0)
        {
          IBusEngineDesc *engine_desc;

          if (self->disable_ibus)
            continue;

          engine_desc = shell_ibus_manager_get_engine_desc (self->ibus_manager, desc->id);
          if (engine_desc != NULL)
            {
              g_autofree char *language = ibus_get_language_name (ibus_engine_desc_get_language (engine_desc));
              const char *long_name = ibus_engine_desc_get_longname (engine_desc);
              const char *textdomain = ibus_engine_desc_get_textdomain (engine_desc);

              if (textdomain != NULL && textdomain[0] != '\0')
                long_name = g_dgettext (textdomain, long_name);

              exists = TRUE;
              ibus_display_name = g_strdup_printf ("%s (%s)", language, long_name);
              display_name = ibus_display_name;
              ibus_short_name = make_engine_short_name (engine_desc);
              short_name = ibus_short_name;
            }
        }

      if (exists)
        g_ptr_array_add (infos_list,
                         source_info_new (desc->source_type, desc->id, display_name, short_name));
    }

  if (infos_list->len == 0)
    {
      const char *display_name = NULL;
      const char *short_name = NULL;

      gnome_xkb_info_get_layout_info (self->xkb_info, SHELL_KEYBOARD_MANAGER_DEFAULT_LAYOUT,
                                      &display_name, &short_name, NULL, NULL);
      g_ptr_array_add (infos_list,
                       source_info_new (SHELL_INPUT_SOURCE_TYPE_XKB,
                                        SHELL_KEYBOARD_MANAGER_DEFAULT_LAYOUT,
                                        display_name != NULL ? display_name : SHELL_KEYBOARD_MANAGER_DEFAULT_LAYOUT,
                                        short_name != NULL ? short_name : SHELL_KEYBOARD_MANAGER_DEFAULT_LAYOUT));
    }

  /* The sources by their short name, in the order of the list */
  by_short_name = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify) g_ptr_array_unref);
  for (i = 0; i < infos_list->len; i++)
    {
      SourceInfo *info = g_ptr_array_index (infos_list, i);
      ShellInputSource *is = shell_input_source_new (info->source_type, info->id,
                                                     info->display_name, info->short_name,
                                                     i);
      GPtrArray *same_name = g_hash_table_lookup (by_short_name, info->short_name);

      g_signal_connect_object (is, "activate",
                               G_CALLBACK (manager_source_activate_cb), self, 0);

      if (same_name == NULL)
        {
          same_name = g_ptr_array_new ();
          g_hash_table_insert (by_short_name, g_strdup (info->short_name), same_name);
        }
      g_ptr_array_add (same_name, is);

      g_ptr_array_add (new_sources, is);
    }

  g_hash_table_remove_all (self->ibus_sources);
  for (i = 0; i < new_sources->len; i++)
    {
      ShellInputSource *is = g_ptr_array_index (new_sources, i);

      if (g_strcmp0 (is->source_type, SHELL_INPUT_SOURCE_TYPE_IBUS) == 0)
        g_hash_table_replace (self->ibus_sources, g_strdup (is->id), g_object_ref (is));
    }

  g_hash_table_iter_init (&iter, by_short_name);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      GPtrArray *same_name = value;

      if (same_name->len > 1)
        {
          for (i = 0; i < same_name->len; i++)
            {
              ShellInputSource *is = g_ptr_array_index (same_name, i);
              g_autofree char *sub_name = NULL;
              char subscript[8] = { 0, };

              g_unichar_to_utf8 (0x2080 + i + 1, subscript);
              sub_name = g_strconcat (is->short_name, subscript, NULL);
              shell_input_source_set_short_name (is, sub_name);
            }
        }
    }

  ptr_array_unref_clear (&self->input_sources);
  self->input_sources = g_steal_pointer (&new_sources);
  self->sources_generation++;

  g_signal_emit (self, manager_signals[MANAGER_SOURCES_CHANGED], 0);

  manager_update_mru_sources (self);

  if (self->mru_sources->len > 0)
    {
      g_autoptr (ShellInputSource) first = g_object_ref (g_ptr_array_index (self->mru_sources, 0));

      shell_input_source_activate (first, FALSE);
    }

  /* All ibus engines are preloaded here to reduce the launching time
   * when users switch the input sources. */
  g_hash_table_iter_init (&iter, self->ibus_sources);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_ptr_array_add (engine_ids, key);
  g_ptr_array_add (engine_ids, NULL);
  shell_ibus_manager_preload_engines (self->ibus_manager, (const char * const *) engine_ids->pdata);
}

/**
 * shell_input_source_manager_reload:
 * @self: a #ShellInputSourceManager
 *
 * Applies the keyboard options and model of the settings and builds the
 * list of input sources again (`reload ()`).
 */
void
shell_input_source_manager_reload (ShellInputSourceManager *self)
{
  g_return_if_fail (SHELL_IS_INPUT_SOURCE_MANAGER (self));

  self->reloading = TRUE;
  manager_apply_keyboard_options (self);
  manager_apply_keyboard_model (self);
  manager_input_sources_changed (self);
  self->reloading = FALSE;
}

/* ---- IBus ------------------------------------------------------------- */

/* _ibusReadyCallback () */
static void
manager_ibus_ready_cb (ShellIBusManager        *ibus_manager,
                       gboolean                 ready,
                       ShellInputSourceManager *self)
{
  if (self->ibus_ready == ready)
    return;

  self->ibus_ready = ready;
  ptr_array_unref_clear (&self->mru_sources);
  self->mru_sources = source_array_new ();
  manager_input_sources_changed (self);
}

/* _ibusPropertiesRegistered () */
static void
manager_ibus_properties_registered_cb (ShellIBusManager        *ibus_manager,
                                       const char              *engine_name,
                                       IBusPropList            *props,
                                       ShellInputSourceManager *self)
{
  ShellInputSource *source = g_hash_table_lookup (self->ibus_sources, engine_name);

  if (source == NULL)
    return;

  shell_input_source_set_properties (source, props);

  if (source == self->current_source)
    g_signal_emit (self, manager_signals[MANAGER_CURRENT_SOURCE_CHANGED], 0, NULL);
}

/* _updateSubProperty () */
static gboolean
update_sub_property (IBusPropList *props,
                     IBusProperty *prop)
{
  IBusProperty *p;
  guint i;

  if (props == NULL)
    return FALSE;

  for (i = 0; (p = ibus_prop_list_get (props, i)) != NULL; ++i)
    {
      if (g_strcmp0 (ibus_property_get_key (p), ibus_property_get_key (prop)) == 0 &&
          ibus_property_get_prop_type (p) == ibus_property_get_prop_type (prop))
        {
          ibus_property_update (p, prop);
          return TRUE;
        }
      else if (ibus_property_get_prop_type (p) == PROP_TYPE_MENU)
        {
          if (update_sub_property (ibus_property_get_sub_props (p), prop))
            return TRUE;
        }
    }

  return FALSE;
}

/* _ibusPropertyUpdated () */
static void
manager_ibus_property_updated_cb (ShellIBusManager        *ibus_manager,
                                  const char              *engine_name,
                                  IBusProperty            *prop,
                                  ShellInputSourceManager *self)
{
  ShellInputSource *source = engine_name != NULL ? g_hash_table_lookup (self->ibus_sources, engine_name) : NULL;

  if (source == NULL)
    return;

  if (update_sub_property (source->properties, prop) &&
      source == self->current_source)
    g_signal_emit (self, manager_signals[MANAGER_CURRENT_SOURCE_CHANGED], 0, NULL);
}

/* _ibusSetContentType () */
static void
manager_ibus_set_content_type_cb (ShellIBusManager        *ibus_manager,
                                  guint                    purpose,
                                  guint                    hints,
                                  ShellInputSourceManager *self)
{
  /* Avoid purpose changes while the switcher popup is shown, likely due to
   * the focus change caused by the switcher popup causing this purpose change. */
  if (self->switcher_popup != NULL)
    return;

  if (purpose == IBUS_INPUT_PURPOSE_PASSWORD)
    {
      if (self->input_sources->len == g_hash_table_size (self->ibus_sources))
        return;

      if (self->disable_ibus)
        return;
      self->disable_ibus = TRUE;
      ptr_array_unref_clear (&self->mru_sources_backup);
      self->mru_sources_backup = source_array_copy (self->mru_sources);
    }
  else
    {
      if (!self->disable_ibus)
        return;
      self->disable_ibus = FALSE;
    }

  shell_input_source_manager_reload (self);
}

static void
manager_ibus_panel_service_cb (ShellIBusManager        *ibus_manager,
                               GParamSpec              *pspec,
                               ShellInputSourceManager *self)
{
  if (self->candidate_popup != NULL)
    shell_ibus_candidate_popup_set_panel_service (self->candidate_popup,
                                                  shell_ibus_manager_get_panel_service (ibus_manager));
}

static void
manager_ibus_engine_changed_cb (ShellIBusManager        *ibus_manager,
                                const char              *engine_name,
                                ShellInputSourceManager *self)
{
  if (self->candidate_popup != NULL)
    shell_box_pointer_close (SHELL_BOX_POINTER (self->candidate_popup),
                             SHELL_POPUP_ANIMATION_NONE, NULL, NULL, NULL);
}

/* ---- one set of input sources per window ------------------------------ */

/* _getNewInputSource () */
static ShellInputSource *
manager_get_new_input_source (ShellInputSourceManager *self,
                              ShellInputSource        *current)
{
  guint i;

  if (self->input_sources->len == 0)
    return NULL;

  if (current != NULL)
    {
      for (i = 0; i < self->input_sources->len; i++)
        {
          ShellInputSource *is = g_ptr_array_index (self->input_sources, i);

          if (input_source_matches (is, current))
            return is;
        }
    }

  return g_ptr_array_index (self->input_sources, 0);
}

/* _setPerWindowInputSource () */
static void
manager_set_per_window_input_source (ShellInputSourceManager *self)
{
  PerWindowState *state = manager_get_current_window_state (self);
  g_autoptr (ShellInputSource) current = NULL;

  if (state == NULL)
    return;

  if (state->generation != self->sources_generation)
    {
      ShellInputSource *new_source = manager_get_new_input_source (self, state->current);

      state->generation = self->sources_generation;
      g_set_object (&state->current, new_source);
    }

  if (state->current != NULL)
    {
      current = g_object_ref (state->current);
      shell_input_source_activate (current, FALSE);
    }
}

static void
manager_focus_window_notify_cb (MetaDisplay             *display,
                                GParamSpec              *pspec,
                                ShellInputSourceManager *self)
{
  manager_set_per_window_input_source (self);
}

static void
manager_overview_visible_notify_cb (ShellWindowManager      *window_manager,
                                    GParamSpec              *pspec,
                                    ShellInputSourceManager *self)
{
  manager_set_per_window_input_source (self);
}

/* _sourcesPerWindowChanged () */
static void
manager_sources_per_window_changed (ShellInputSourceManager *self)
{
  self->sources_per_window = settings_get_per_window (self->settings);

  if (self->sources_per_window && self->focus_window_notify_id == 0)
    {
      if (self->display != NULL)
        self->focus_window_notify_id =
          g_signal_connect (self->display, "notify::focus-window",
                            G_CALLBACK (manager_focus_window_notify_cb), self);
      if (self->window_manager != NULL)
        self->overview_visible_notify_id =
          g_signal_connect (self->window_manager, "notify::overview-visible",
                            G_CALLBACK (manager_overview_visible_notify_cb), self);
    }
  else if (!self->sources_per_window && self->focus_window_notify_id != 0)
    {
      g_autoptr (GList) window_actors = NULL;
      GList *l;

      g_clear_signal_handler (&self->focus_window_notify_id, self->display);
      if (self->window_manager != NULL)
        g_clear_signal_handler (&self->overview_visible_notify_id, self->window_manager);
      self->overview_visible_notify_id = 0;

      window_actors = shell_global_get_window_actors (shell_global_get ());
      for (l = window_actors; l != NULL; l = l->next)
        {
          MetaWindow *window = meta_window_actor_get_meta_window (META_WINDOW_ACTOR (l->data));

          g_object_set_qdata (G_OBJECT (window), per_window_quark (), NULL);
        }

      g_clear_object (&self->overview_state.current);
      self->overview_state.generation = 0;
    }
}

static void
manager_per_window_changed_cb (GObject                 *settings,
                               ShellInputSourceManager *self)
{
  manager_sources_per_window_changed (self);
}

/* ---- construction ----------------------------------------------------- */

static void
manager_global_shutdown_cb (ShellGlobal             *global,
                            ShellInputSourceManager *self)
{
  self->shutting_down = TRUE;
}

static void
shell_input_source_manager_constructed (GObject *object)
{
  ShellInputSourceManager *self = SHELL_INPUT_SOURCE_MANAGER (object);
  ShellMain *shell_main = shell_main_get_default ();
  ShellGlobal *global = shell_global_get ();
  ShellSessionMode *session_mode = shell_main_get_session_mode (shell_main);
  g_autoptr (GSettings) keybindings_settings = NULL;

  G_OBJECT_CLASS (shell_input_source_manager_parent_class)->constructed (object);

  self->input_sources = source_array_new ();
  self->ibus_sources = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  self->mru_sources = source_array_new ();

  g_set_weak_pointer (&self->window_manager, shell_main_get_window_manager (shell_main));
  g_set_weak_pointer (&self->input_method, shell_main_get_input_method (shell_main));
  g_set_weak_pointer (&self->display, shell_global_get_display (global));

  g_signal_connect_object (global, "shutdown",
                           G_CALLBACK (manager_global_shutdown_cb), self, 0);

  keybindings_settings = shell_status_settings_new (WM_KEYBINDINGS_SCHEMA);
  if (self->window_manager != NULL && keybindings_settings != NULL)
    {
      self->keybinding_action =
        shell_window_manager_add_keybinding (self->window_manager, "switch-input-source",
                                             keybindings_settings,
                                             META_KEY_BINDING_NONE,
                                             SHELL_ACTION_MODE_ALL,
                                             manager_switch_input_source_cb,
                                             self, NULL);
      self->keybinding_action_backward =
        shell_window_manager_add_keybinding (self->window_manager, "switch-input-source-backward",
                                             keybindings_settings,
                                             META_KEY_BINDING_IS_REVERSED,
                                             SHELL_ACTION_MODE_ALL,
                                             manager_switch_input_source_cb,
                                             self, NULL);
    }

  if (session_mode != NULL && shell_session_mode_is_greeter (session_mode))
    self->settings = g_object_new (SHELL_TYPE_INPUT_SOURCE_SYSTEM_SETTINGS, NULL);
  else
    self->settings = g_object_new (SHELL_TYPE_INPUT_SOURCE_SESSION_SETTINGS, NULL);
  g_signal_connect_object (self->settings, "input-sources-changed",
                           G_CALLBACK (manager_input_sources_changed_cb), self, 0);
  g_signal_connect_object (self->settings, "keyboard-options-changed",
                           G_CALLBACK (manager_keyboard_options_changed_cb), self, 0);
  g_signal_connect_object (self->settings, "keyboard-model-changed",
                           G_CALLBACK (manager_keyboard_model_changed_cb), self, 0);

  self->keyboard_manager = shell_keyboard_manager_get_default ();
  self->xkb_info = shell_keyboard_manager_get_xkb_info (self->keyboard_manager);
  g_signal_connect_object (self->keyboard_manager, "keymap-changed",
                           G_CALLBACK (manager_keymap_changed_cb), self, 0);

  self->ibus_ready = FALSE;
  self->ibus_manager = shell_ibus_manager_get_default ();
  g_set_weak_pointer (&self->candidate_popup, shell_ibus_candidate_popup_new ());
  g_signal_connect_object (self->ibus_manager, "notify::panel-service",
                           G_CALLBACK (manager_ibus_panel_service_cb), self, 0);
  g_signal_connect_object (self->ibus_manager, "engine-changed",
                           G_CALLBACK (manager_ibus_engine_changed_cb), self, 0);
  if (shell_ibus_manager_get_panel_service (self->ibus_manager) != NULL)
    shell_ibus_candidate_popup_set_panel_service (self->candidate_popup,
                                                  shell_ibus_manager_get_panel_service (self->ibus_manager));
  g_signal_connect_object (self->ibus_manager, "ready",
                           G_CALLBACK (manager_ibus_ready_cb), self, 0);
  g_signal_connect_object (self->ibus_manager, "properties-registered",
                           G_CALLBACK (manager_ibus_properties_registered_cb), self, 0);
  g_signal_connect_object (self->ibus_manager, "property-updated",
                           G_CALLBACK (manager_ibus_property_updated_cb), self, 0);
  g_signal_connect_object (self->ibus_manager, "set-content-type",
                           G_CALLBACK (manager_ibus_set_content_type_cb), self, 0);

  self->modifiers_accelerator_id =
    g_signal_connect (self->display, "modifiers-accelerator-activated",
                      G_CALLBACK (manager_modifiers_accelerator_activated_cb), self);

  self->sources_per_window = FALSE;
  self->focus_window_notify_id = 0;
  g_signal_connect_object (self->settings, "per-window-changed",
                           G_CALLBACK (manager_per_window_changed_cb), self, 0);
  manager_sources_per_window_changed (self);
  self->disable_ibus = FALSE;
  self->reloading = FALSE;
}

static void
manager_get_property (GObject    *object,
                      guint       prop_id,
                      GValue     *value,
                      GParamSpec *pspec)
{
  ShellInputSourceManager *self = SHELL_INPUT_SOURCE_MANAGER (object);

  switch (prop_id)
    {
    case MANAGER_PROP_CURRENT_SOURCE:
      g_value_set_object (value, self->current_source);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_input_source_manager_dispose (GObject *object)
{
  ShellInputSourceManager *self = SHELL_INPUT_SOURCE_MANAGER (object);

  if (self->display != NULL)
    {
      g_clear_signal_handler (&self->modifiers_accelerator_id, self->display);
      g_clear_signal_handler (&self->focus_window_notify_id, self->display);
    }
  if (self->window_manager != NULL)
    g_clear_signal_handler (&self->overview_visible_notify_id, self->window_manager);

  /* The popup can outlive the manager: its "destroy" handler has @self as data */
  if (self->switcher_popup != NULL)
    g_signal_handlers_disconnect_by_func (self->switcher_popup,
                                          manager_switcher_popup_destroy_cb, self);
  g_clear_weak_pointer (&self->switcher_popup);
  g_clear_weak_pointer (&self->candidate_popup);
  g_clear_weak_pointer (&self->window_manager);
  g_clear_weak_pointer (&self->input_method);
  g_clear_weak_pointer (&self->display);

  g_clear_object (&self->settings);
  g_clear_object (&self->current_source);
  g_clear_object (&self->overview_state.current);
  g_clear_pointer (&self->ibus_sources, g_hash_table_unref);
  ptr_array_unref_clear (&self->input_sources);
  ptr_array_unref_clear (&self->mru_sources);
  ptr_array_unref_clear (&self->mru_sources_backup);

  G_OBJECT_CLASS (shell_input_source_manager_parent_class)->dispose (object);
}

static void
shell_input_source_manager_class_init (ShellInputSourceManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_input_source_manager_constructed;
  object_class->get_property = manager_get_property;
  object_class->dispose = shell_input_source_manager_dispose;

  /**
   * ShellInputSourceManager:current-source:
   *
   * The input source in use (`currentSource`), %NULL while the list of
   * input sources is rebuilt.
   */
  manager_props[MANAGER_PROP_CURRENT_SOURCE] =
    g_param_spec_object ("current-source", NULL, NULL,
                         SHELL_TYPE_INPUT_SOURCE,
                         G_PARAM_READABLE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, MANAGER_N_PROPS, manager_props);

  /**
   * ShellInputSourceManager::sources-changed:
   * @self: the #ShellInputSourceManager
   *
   * Emitted when the list of input sources was built again, before the
   * first of them is activated.
   */
  manager_signals[MANAGER_SOURCES_CHANGED] =
    g_signal_new ("sources-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellInputSourceManager::current-source-changed:
   * @self: the #ShellInputSourceManager
   * @old_source: (nullable): the input source that was current, or %NULL if
   *   the current input source is the same but its properties changed
   *
   * Emitted when another input source became the current one, and when the
   * properties of the current IBus engine changed.
   */
  manager_signals[MANAGER_CURRENT_SOURCE_CHANGED] =
    g_signal_new ("current-source-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_INPUT_SOURCE);

  /**
   * ShellInputSourceManager::keymap-changed:
   * @self: the #ShellInputSourceManager
   *
   * Emitted when the keymap of the compositor changed.
   */
  manager_signals[MANAGER_KEYMAP_CHANGED] =
    g_signal_new ("keymap-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_input_source_manager_init (ShellInputSourceManager *self)
{
}

/**
 * shell_input_source_manager_get_default:
 *
 * Gets the input source manager, which is created on the first call
 * (`getInputSourceManager ()`). The window manager, the input method and the
 * session mode have to be set on #ShellMain by then.
 *
 * Returns: (transfer none): the #ShellInputSourceManager
 */
ShellInputSourceManager *
shell_input_source_manager_get_default (void)
{
  if (default_manager == NULL)
    default_manager = g_object_new (SHELL_TYPE_INPUT_SOURCE_MANAGER, NULL);

  return default_manager;
}

/**
 * shell_input_source_manager_get_current_source:
 * @self: a #ShellInputSourceManager
 *
 * Returns: (transfer none) (nullable): #ShellInputSourceManager:current-source
 */
ShellInputSource *
shell_input_source_manager_get_current_source (ShellInputSourceManager *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_SOURCE_MANAGER (self), NULL);

  return self->current_source;
}

/**
 * shell_input_source_manager_get_input_sources:
 * @self: a #ShellInputSourceManager
 *
 * Gets the input sources of the settings, in the order of the list
 * (`inputSources`). The list is replaced when the manager emits
 * #ShellInputSourceManager::sources-changed.
 *
 * Returns: (transfer none) (element-type ShellInputSource): the input sources
 */
GPtrArray *
shell_input_source_manager_get_input_sources (ShellInputSourceManager *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_SOURCE_MANAGER (self), NULL);

  return self->input_sources;
}

/**
 * shell_input_source_manager_get_keyboard_manager:
 * @self: a #ShellInputSourceManager
 *
 * Returns: (transfer none): the keyboard manager that applies the layouts
 */
ShellKeyboardManager *
shell_input_source_manager_get_keyboard_manager (ShellInputSourceManager *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_SOURCE_MANAGER (self), NULL);

  return self->keyboard_manager;
}

/* ======================================================================== */
/* ShellInputSourceLayoutMenuItem                                           */
/* ======================================================================== */

#define SHELL_TYPE_INPUT_SOURCE_LAYOUT_MENU_ITEM (shell_input_source_layout_menu_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellInputSourceLayoutMenuItem, shell_input_source_layout_menu_item,
                      SHELL, INPUT_SOURCE_LAYOUT_MENU_ITEM, ShellPopupBaseMenuItem)

struct _ShellInputSourceLayoutMenuItem
{
  ShellPopupBaseMenuItem parent;

  StLabel *label;               /* weak */
  StLabel *indicator;           /* weak */
  StLabel *top_bar_label;       /* weak, the label of the same source in the top bar */
  ShellInputSource *source;     /* the source the item activates, if it can */
};

G_DEFINE_FINAL_TYPE (ShellInputSourceLayoutMenuItem, shell_input_source_layout_menu_item,
                     SHELL_TYPE_POPUP_BASE_MENU_ITEM)

static void
layout_menu_item_dispose (GObject *object)
{
  ShellInputSourceLayoutMenuItem *self = SHELL_INPUT_SOURCE_LAYOUT_MENU_ITEM (object);

  g_clear_weak_pointer (&self->label);
  g_clear_weak_pointer (&self->indicator);
  g_clear_weak_pointer (&self->top_bar_label);
  g_clear_object (&self->source);

  G_OBJECT_CLASS (shell_input_source_layout_menu_item_parent_class)->dispose (object);
}

static void
shell_input_source_layout_menu_item_class_init (ShellInputSourceLayoutMenuItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = layout_menu_item_dispose;
}

static void
shell_input_source_layout_menu_item_init (ShellInputSourceLayoutMenuItem *self)
{
}

static ShellInputSourceLayoutMenuItem *
layout_menu_item_new (const char *display_name,
                      const char *short_name)
{
  ShellInputSourceLayoutMenuItem *self = g_object_new (SHELL_TYPE_INPUT_SOURCE_LAYOUT_MENU_ITEM, NULL);
  ClutterActor *label, *indicator;

  label = g_object_new (ST_TYPE_LABEL,
                        "text", display_name,
                        "x-expand", TRUE,
                        NULL);
  indicator = g_object_new (ST_TYPE_LABEL,
                            "text", short_name,
                            NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), label);
  clutter_actor_add_child (CLUTTER_ACTOR (self), indicator);
  st_widget_set_label_actor (ST_WIDGET (self), label);

  g_set_weak_pointer (&self->label, ST_LABEL (label));
  g_set_weak_pointer (&self->indicator, ST_LABEL (indicator));

  return self;
}

/* ======================================================================== */
/* ShellInputSourceIndicatorContainer                                       */
/* ======================================================================== */

#define SHELL_TYPE_INPUT_SOURCE_INDICATOR_CONTAINER (shell_input_source_indicator_container_get_type ())
G_DECLARE_FINAL_TYPE (ShellInputSourceIndicatorContainer, shell_input_source_indicator_container,
                      SHELL, INPUT_SOURCE_INDICATOR_CONTAINER, StWidget)

struct _ShellInputSourceIndicatorContainer
{
  StWidget parent;
};

G_DEFINE_FINAL_TYPE (ShellInputSourceIndicatorContainer, shell_input_source_indicator_container,
                     ST_TYPE_WIDGET)

static void
container_get_preferred_width (ClutterActor *actor,
                               float         for_height,
                               float        *min_width_p,
                               float        *natural_width_p)
{
  ClutterActor *child;
  float min = 0, natural = 0;

  /* Here, and in get_preferred_height, we need to query
   * for the height of all children, but we ignore the results
   * for those we don't actually display. */
  for (child = clutter_actor_get_first_child (actor);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      float child_min, child_natural;

      clutter_actor_get_preferred_width (child, for_height, &child_min, &child_natural);
      min = MAX (min, child_min);
      natural = MAX (natural, child_natural);
    }

  if (min_width_p)
    *min_width_p = min;
  if (natural_width_p)
    *natural_width_p = natural;
}

static void
container_get_preferred_height (ClutterActor *actor,
                                float         for_width,
                                float        *min_height_p,
                                float        *natural_height_p)
{
  ClutterActor *child;
  float min = 0, natural = 0;

  for (child = clutter_actor_get_first_child (actor);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      float child_min, child_natural;

      clutter_actor_get_preferred_height (child, for_width, &child_min, &child_natural);
      min = MAX (min, child_min);
      natural = MAX (natural, child_natural);
    }

  if (min_height_p)
    *min_height_p = min;
  if (natural_height_p)
    *natural_height_p = natural;
}

static void
container_allocate (ClutterActor          *actor,
                    const ClutterActorBox *box)
{
  ClutterActorBox child_box;
  ClutterActor *child;

  clutter_actor_set_allocation (actor, box);

  /* translate box to (0, 0) */
  child_box.x1 = 0;
  child_box.x2 = box->x2 - box->x1;
  child_box.y1 = 0;
  child_box.y2 = box->y2 - box->y1;

  for (child = clutter_actor_get_first_child (actor);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    clutter_actor_allocate_align_fill (child, &child_box, 0.5, 0.5, FALSE, FALSE);
}

static void
shell_input_source_indicator_container_class_init (ShellInputSourceIndicatorContainerClass *klass)
{
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  actor_class->get_preferred_width = container_get_preferred_width;
  actor_class->get_preferred_height = container_get_preferred_height;
  actor_class->allocate = container_allocate;
}

static void
shell_input_source_indicator_container_init (ShellInputSourceIndicatorContainer *self)
{
}

/* ======================================================================== */
/* ShellInputSourceIndicator                                                */
/* ======================================================================== */

enum
{
  INDICATOR_OVERVIEW_HIDE_REQUESTED,
  INDICATOR_LAST_SIGNAL
};

static guint indicator_signals[INDICATOR_LAST_SIGNAL] = { 0, };

struct _ShellInputSourceIndicator
{
  ShellPanelMenuButton parent;

  ShellInputSourceManager *input_source_manager;
  GPtrArray *input_sources;         /* of ShellInputSource; the sources shown */
  GHashTable *menu_items;           /* indicator index -> ShellInputSourceLayoutMenuItem */
  GHashTable *indicator_labels;     /* indicator index -> StLabel */

  ClutterActor *container;          /* weak */
  ShellPopupSeparatorMenuItem *prop_separator;  /* weak */
  ShellPopupMenuSection *prop_section;          /* weak */
  ShellPopupBaseMenuItem *show_layout_item;     /* weak */
};

G_DEFINE_FINAL_TYPE (ShellInputSourceIndicator, shell_input_source_indicator,
                     SHELL_TYPE_PANEL_MENU_BUTTON)

static ShellPopupMenuBase *
indicator_get_menu (ShellInputSourceIndicator *self)
{
  return SHELL_POPUP_MENU_BASE (shell_panel_menu_button_get_menu (SHELL_PANEL_MENU_BUTTON (self)));
}

/* _sessionUpdated () */
static void
indicator_session_updated (ShellInputSourceIndicator *self)
{
  ShellSessionMode *session_mode = shell_status_get_session_mode ();
  gboolean allow_settings = TRUE;

  if (self->show_layout_item == NULL)
    return;

  /* re-using "allowSettings" for the keyboard layout is a bit shady,
   * but at least for now it is used as "allow popping up windows
   * from shell menus"; we can always add a separate sessionMode
   * option if need arises. */
  if (session_mode != NULL)
    g_object_get (session_mode, "allow-settings", &allow_settings, NULL);

  shell_status_set_visible (self->show_layout_item, allow_settings);
}

/* _createExternalSource () */
static ShellInputSource *
indicator_create_external_source (ShellKeyboardManager *keyboard_manager)
{
  ShellInputSource *is = shell_input_source_new (SHELL_INPUT_SOURCE_TYPE_EXTERNAL,
                                                 SHELL_INPUT_SOURCE_TYPE_EXTERNAL,
                                                 shell_keyboard_manager_get_display_name (keyboard_manager),
                                                 shell_keyboard_manager_get_short_name (keyboard_manager),
                                                 -1);

  shell_input_source_set_locked (is, shell_keyboard_manager_is_locked (keyboard_manager));
  return is;
}

/* _getCurrentSource () */
static ShellInputSource *
indicator_get_current_source (ShellInputSourceIndicator *self)
{
  ShellKeyboardManager *keyboard_manager =
    shell_input_source_manager_get_keyboard_manager (self->input_source_manager);

  if (shell_keyboard_manager_is_external (keyboard_manager))
    return self->input_sources->len > 0 ? g_ptr_array_index (self->input_sources, 0) : NULL;

  return shell_input_source_manager_get_current_source (self->input_source_manager);
}

/* _updateInputSources () */
static void
indicator_update_input_sources (ShellInputSourceIndicator *self)
{
  ShellKeyboardManager *keyboard_manager =
    shell_input_source_manager_get_keyboard_manager (self->input_source_manager);
  GPtrArray *internal_sources;
  guint i;

  g_ptr_array_set_size (self->input_sources, 0);

  if (shell_keyboard_manager_is_locked (keyboard_manager))
    {
      g_ptr_array_add (self->input_sources, indicator_create_external_source (keyboard_manager));
      return;
    }

  if (shell_keyboard_manager_is_external (keyboard_manager))
    g_ptr_array_add (self->input_sources, indicator_create_external_source (keyboard_manager));

  internal_sources = shell_input_source_manager_get_input_sources (self->input_source_manager);
  for (i = 0; i < internal_sources->len; i++)
    g_ptr_array_add (self->input_sources, g_object_ref (g_ptr_array_index (internal_sources, i)));
}

/* _calculateSourceIndicatorIndex () */
static int
indicator_calculate_source_indicator_index (ShellInputSourceIndicator *self,
                                            ShellInputSource          *source)
{
  ShellKeyboardManager *keyboard_manager =
    shell_input_source_manager_get_keyboard_manager (self->input_source_manager);

  return (shell_keyboard_manager_is_external (keyboard_manager) ? 1 : 0) + source->index;
}

static void
layout_menu_item_activate_cb (ShellInputSourceLayoutMenuItem *item,
                              const ClutterEvent             *event,
                              gpointer                        user_data)
{
  if (item->source != NULL)
    shell_input_source_activate (item->source, TRUE);
}

/* The 'changed' handler of the source of an item */
static void
layout_source_changed_cb (ShellInputSource               *source,
                          ShellInputSourceLayoutMenuItem *item)
{
  if (item->indicator != NULL)
    st_label_set_text (item->indicator, source->short_name);
  if (item->top_bar_label != NULL)
    st_label_set_text (item->top_bar_label, source->short_name);
}

/* _addSourceIndicators () */
static void
indicator_add_source_indicators (ShellInputSourceIndicator *self)
{
  ShellPopupMenuBase *menu = indicator_get_menu (self);
  guint i;

  for (i = 0; i < self->input_sources->len; i++)
    {
      ShellInputSource *is = g_ptr_array_index (self->input_sources, i);
      ShellInputSourceLayoutMenuItem *menu_item;
      ClutterActor *indicator_label;
      int indicator_index;

      menu_item = layout_menu_item_new (is->display_name, is->short_name);
      if (g_strcmp0 (is->source_type, SHELL_INPUT_SOURCE_TYPE_EXTERNAL) != 0)
        {
          menu_item->source = g_object_ref (is);
          g_signal_connect (menu_item, "activate",
                            G_CALLBACK (layout_menu_item_activate_cb), NULL);
        }
      else
        {
          shell_popup_base_menu_item_set_sensitive (SHELL_POPUP_BASE_MENU_ITEM (menu_item), FALSE);
        }

      if (is->locked)
        shell_popup_base_menu_item_set_ornament (SHELL_POPUP_BASE_MENU_ITEM (menu_item),
                                                 SHELL_ORNAMENT_HIDDEN);
      else
        shell_popup_base_menu_item_set_ornament (SHELL_POPUP_BASE_MENU_ITEM (menu_item),
                                                 SHELL_ORNAMENT_NO_DOT);

      indicator_label = g_object_new (ST_TYPE_LABEL,
                                      "text", is->short_name,
                                      "visible", FALSE,
                                      NULL);
      g_set_weak_pointer (&menu_item->top_bar_label, ST_LABEL (indicator_label));

      indicator_index = indicator_calculate_source_indicator_index (self, is);

      g_hash_table_replace (self->menu_items, GINT_TO_POINTER (indicator_index),
                            g_object_ref_sink (menu_item));
      g_hash_table_replace (self->indicator_labels, GINT_TO_POINTER (indicator_index),
                            g_object_ref_sink (indicator_label));
      g_signal_connect_object (is, "changed",
                               G_CALLBACK (layout_source_changed_cb), menu_item, 0);

      shell_popup_menu_base_insert_menu_item (menu, G_OBJECT (menu_item), indicator_index);
      clutter_actor_add_child (self->container, indicator_label);
    }
}

static void
destroy_actor_value (gpointer actor)
{
  clutter_actor_destroy (CLUTTER_ACTOR (actor));
  g_object_unref (actor);
}

/* _sourcesChanged () */
static void
indicator_sources_changed (ShellInputSourceIndicator *self)
{
  indicator_update_input_sources (self);

  g_hash_table_remove_all (self->menu_items);
  g_hash_table_remove_all (self->indicator_labels);

  indicator_add_source_indicators (self);
}

/* _setSourceAsActive () */
static void
indicator_set_source_as_active (ShellInputSourceIndicator *self,
                                ShellInputSource          *source)
{
  int index;
  ShellInputSourceLayoutMenuItem *item;
  ClutterActor *label;

  if (source == NULL)
    return;

  index = indicator_calculate_source_indicator_index (self, source);
  item = g_hash_table_lookup (self->menu_items, GINT_TO_POINTER (index));
  label = g_hash_table_lookup (self->indicator_labels, GINT_TO_POINTER (index));

  if (!source->locked && item != NULL)
    shell_popup_base_menu_item_set_ornament (SHELL_POPUP_BASE_MENU_ITEM (item), SHELL_ORNAMENT_DOT);
  if (label != NULL)
    clutter_actor_show (label);
}

/* _setSourceAsInactive () */
static void
indicator_set_source_as_inactive (ShellInputSourceIndicator *self,
                                  ShellInputSource          *source)
{
  int index = indicator_calculate_source_indicator_index (self, source);
  ShellInputSourceLayoutMenuItem *item = g_hash_table_lookup (self->menu_items, GINT_TO_POINTER (index));
  ClutterActor *label = g_hash_table_lookup (self->indicator_labels, GINT_TO_POINTER (index));

  if (!source->locked && item != NULL)
    shell_popup_base_menu_item_set_ornament (SHELL_POPUP_BASE_MENU_ITEM (item), SHELL_ORNAMENT_NO_DOT);
  if (label != NULL)
    clutter_actor_hide (label);
}

/* The number of grapheme clusters of @text (`[...segmenter.segment (text)].length`) */
static guint
count_grapheme_clusters (const char *text)
{
  glong n_chars = g_utf8_strlen (text, -1);
  g_autofree PangoLogAttr *attrs = NULL;
  guint count = 0;
  glong i;

  if (n_chars == 0)
    return 0;

  attrs = g_new0 (PangoLogAttr, n_chars + 1);
  pango_get_log_attrs (text, -1, -1, pango_language_get_default (), attrs, n_chars + 1);

  /* Every cursor position after the first one ends a cluster */
  for (i = 1; i <= n_chars; i++)
    {
      if (attrs[i].is_cursor_position)
        count++;
    }

  return count;
}

static void
prop_menu_item_set_prop (gpointer      item,
                         IBusProperty *prop)
{
  g_object_set_data_full (G_OBJECT (item), "shell-ibus-property",
                          g_object_ref (prop), g_object_unref);
}

static IBusProperty *
prop_menu_item_get_prop (gpointer item)
{
  return g_object_get_data (G_OBJECT (item), "shell-ibus-property");
}

static void
radio_item_activate_cb (ShellPopupBaseMenuItem *item,
                        const ClutterEvent     *event,
                        gpointer                user_data)
{
  IBusProperty *prop = prop_menu_item_get_prop (item);
  ShellIBusManager *ibus_manager = shell_ibus_manager_get_default ();
  ShellPopupMenuBase *parent_menu;
  g_autoptr (GList) items = NULL;
  GList *l;

  if (ibus_property_get_state (prop) == PROP_STATE_CHECKED)
    return;

  /* The radio group is every radio item of the menu the item is in */
  parent_menu = shell_popup_base_menu_item_get_parent_menu (item);
  if (parent_menu == NULL)
    return;

  items = shell_popup_menu_base_get_menu_items (parent_menu);
  for (l = items; l != NULL; l = l->next)
    {
      IBusProperty *group_prop;

      if (!SHELL_IS_POPUP_BASE_MENU_ITEM (l->data))
        continue;

      group_prop = prop_menu_item_get_prop (l->data);
      if (group_prop == NULL || ibus_property_get_prop_type (group_prop) != PROP_TYPE_RADIO)
        continue;

      if (l->data == (gpointer) item)
        {
          shell_popup_base_menu_item_set_ornament (item, SHELL_ORNAMENT_DOT);
          ibus_property_set_state (prop, PROP_STATE_CHECKED);
          shell_ibus_manager_activate_property (ibus_manager,
                                                ibus_property_get_key (prop),
                                                PROP_STATE_CHECKED);
        }
      else
        {
          shell_popup_base_menu_item_set_ornament (SHELL_POPUP_BASE_MENU_ITEM (l->data),
                                                   SHELL_ORNAMENT_NO_DOT);
          ibus_property_set_state (group_prop, PROP_STATE_UNCHECKED);
          shell_ibus_manager_activate_property (ibus_manager,
                                                ibus_property_get_key (group_prop),
                                                PROP_STATE_UNCHECKED);
        }
    }
}

static void
toggle_item_toggled_cb (ShellPopupSwitchMenuItem *item,
                        gboolean                  state,
                        gpointer                  user_data)
{
  IBusProperty *prop = prop_menu_item_get_prop (item);
  ShellIBusManager *ibus_manager = shell_ibus_manager_get_default ();

  if (shell_popup_switch_menu_item_get_state (item))
    {
      ibus_property_set_state (prop, PROP_STATE_CHECKED);
      shell_ibus_manager_activate_property (ibus_manager, ibus_property_get_key (prop),
                                            PROP_STATE_CHECKED);
    }
  else
    {
      ibus_property_set_state (prop, PROP_STATE_UNCHECKED);
      shell_ibus_manager_activate_property (ibus_manager, ibus_property_get_key (prop),
                                            PROP_STATE_UNCHECKED);
    }
}

static void
normal_item_activate_cb (ShellPopupBaseMenuItem *item,
                         const ClutterEvent     *event,
                         gpointer                user_data)
{
  IBusProperty *prop = prop_menu_item_get_prop (item);

  shell_ibus_manager_activate_property (shell_ibus_manager_get_default (),
                                        ibus_property_get_key (prop),
                                        ibus_property_get_state (prop));
}

static const char *
ibus_text_or_empty (IBusText *text)
{
  return text != NULL ? ibus_text_get_text (text) : "";
}

/* _buildPropSubMenu () */
static void
indicator_build_prop_sub_menu (ShellInputSourceIndicator *self,
                               ShellPopupMenuBase        *menu,
                               IBusPropList              *props)
{
  IBusProperty *prop;
  guint i;

  if (props == NULL)
    return;

  for (i = 0; (prop = ibus_prop_list_get (props, i)) != NULL; ++i)
    {
      ShellPopupBaseMenuItem *item;
      IBusPropType type;

      if (!ibus_property_get_visible (prop))
        continue;

      if (g_strcmp0 (ibus_property_get_key (prop), "InputMode") == 0)
        {
          IBusText *symbol = ibus_property_get_symbol (prop);
          const char *text = symbol != NULL ? ibus_text_get_text (symbol)
                                            : ibus_text_or_empty (ibus_property_get_label (prop));
          ShellInputSource *current_source = indicator_get_current_source (self);

          if (current_source != NULL)
            {
              int index = indicator_calculate_source_indicator_index (self, current_source);
              StLabel *indicator_label = g_hash_table_lookup (self->indicator_labels,
                                                              GINT_TO_POINTER (index));
              guint n_clusters = count_grapheme_clusters (text);

              if (indicator_label != NULL && n_clusters > 0 && n_clusters < 3)
                st_label_set_text (indicator_label, text);
            }
        }

      type = ibus_property_get_prop_type (prop);
      switch (type)
        {
        case PROP_TYPE_MENU:
          item = g_object_new (SHELL_TYPE_POPUP_SUB_MENU_MENU_ITEM,
                               "text", ibus_text_or_empty (ibus_property_get_label (prop)),
                               NULL);
          indicator_build_prop_sub_menu (self,
                                         SHELL_POPUP_MENU_BASE (shell_popup_sub_menu_menu_item_get_menu (SHELL_POPUP_SUB_MENU_MENU_ITEM (item))),
                                         ibus_property_get_sub_props (prop));
          break;

        case PROP_TYPE_RADIO:
          item = g_object_new (SHELL_TYPE_POPUP_MENU_ITEM,
                               "text", ibus_text_or_empty (ibus_property_get_label (prop)),
                               NULL);
          prop_menu_item_set_prop (item, prop);
          shell_popup_base_menu_item_set_ornament (item,
                                                   ibus_property_get_state (prop) == PROP_STATE_CHECKED
                                                   ? SHELL_ORNAMENT_DOT : SHELL_ORNAMENT_NO_DOT);
          g_signal_connect (item, "activate", G_CALLBACK (radio_item_activate_cb), NULL);
          break;

        case PROP_TYPE_TOGGLE:
          item = g_object_new (SHELL_TYPE_POPUP_SWITCH_MENU_ITEM,
                               "text", ibus_text_or_empty (ibus_property_get_label (prop)),
                               "state", ibus_property_get_state (prop) == PROP_STATE_CHECKED,
                               NULL);
          prop_menu_item_set_prop (item, prop);
          g_signal_connect (item, "toggled", G_CALLBACK (toggle_item_toggled_cb), NULL);
          break;

        case PROP_TYPE_NORMAL:
          item = g_object_new (SHELL_TYPE_POPUP_MENU_ITEM,
                               "text", ibus_text_or_empty (ibus_property_get_label (prop)),
                               NULL);
          prop_menu_item_set_prop (item, prop);
          g_signal_connect (item, "activate", G_CALLBACK (normal_item_activate_cb), NULL);
          break;

        case PROP_TYPE_SEPARATOR:
          item = g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL);
          break;

        default:
          g_message ("IBus property %s has invalid type %d",
                     ibus_property_get_key (prop), (int) type);
          continue;
        }

      shell_popup_base_menu_item_set_sensitive (item, ibus_property_get_sensitive (prop));
      shell_popup_menu_base_add_menu_item (menu, G_OBJECT (item));
    }
}

/* _buildPropSection () */
static void
indicator_build_prop_section (ShellInputSourceIndicator *self,
                              IBusPropList              *properties)
{
  ShellPopupMenuBase *section = SHELL_POPUP_MENU_BASE (self->prop_section);

  clutter_actor_hide (CLUTTER_ACTOR (self->prop_separator));
  clutter_actor_hide (shell_popup_menu_base_get_actor (section));
  shell_popup_menu_base_remove_all (section);

  indicator_build_prop_sub_menu (self, section, properties);

  if (!shell_popup_menu_base_is_empty (section))
    {
      clutter_actor_show (shell_popup_menu_base_get_actor (section));
      clutter_actor_show (CLUTTER_ACTOR (self->prop_separator));
    }
}

/* _currentSourceChanged () */
static void
indicator_current_source_changed_cb (ShellInputSourceManager   *manager,
                                     ShellInputSource          *old_source,
                                     ShellInputSourceIndicator *self)
{
  guint n_visible_sources = self->input_sources->len;
  ShellInputSource *new_source = indicator_get_current_source (self);

  if (old_source != NULL)
    indicator_set_source_as_inactive (self, old_source);

  if (new_source == NULL || (n_visible_sources < 2 && new_source->properties == NULL))
    {
      /* This source index might be invalid if we weren't able
       * to build a menu item for it, so we hide ourselves since
       * we can't fix it here. *shrug*
       *
       * We also hide if we have only one visible source unless
       * it's an IBus source with properties. */
      shell_popup_menu_base_close (indicator_get_menu (self), SHELL_POPUP_ANIMATION_NONE);
      clutter_actor_hide (CLUTTER_ACTOR (self));
      return;
    }

  clutter_actor_show (CLUTTER_ACTOR (self));

  indicator_build_prop_section (self, new_source->properties);

  indicator_set_source_as_active (self, new_source);
}

static void
indicator_sources_changed_cb (ShellInputSourceManager   *manager,
                              ShellInputSourceIndicator *self)
{
  indicator_sources_changed (self);
}

/* _keymapChanged () */
static void
indicator_keymap_changed_cb (ShellInputSourceManager   *manager,
                             ShellInputSourceIndicator *self)
{
  indicator_sources_changed (self);
  indicator_set_source_as_active (self, indicator_get_current_source (self));
}

static void
indicator_show_layout_cb (const ClutterEvent *event,
                          gpointer            user_data)
{
  GWeakRef *indicator_ref = user_data;
  g_autoptr (ShellInputSourceIndicator) self = g_weak_ref_get (indicator_ref);
  ShellApp *app;

  if (self == NULL)
    return;

  app = shell_app_system_lookup_app (shell_app_system_get_default (), "org.gnome.Tecla.desktop");

  g_signal_emit (self, indicator_signals[INDICATOR_OVERVIEW_HIDE_REQUESTED], 0);
  if (app != NULL)
    shell_app_activate (app);
}

static void
weak_ref_free (GWeakRef *ref)
{
  g_weak_ref_clear (ref);
  g_free (ref);
}

static GObject *
shell_input_source_indicator_constructor (GType                  type,
                                          guint                  n_construct_properties,
                                          GObjectConstructParam *construct_properties)
{
  guint i;

  /* `super._init ({menu_alignment: 0.5, ...})`: the alignment is forced here,
   * before the button creates its menu */
  for (i = 0; i < n_construct_properties; i++)
    {
      if (g_strcmp0 (g_param_spec_get_name (construct_properties[i].pspec),
                     "menu-alignment") == 0)
        g_value_set_double (construct_properties[i].value, 0.5);
    }

  return G_OBJECT_CLASS (shell_input_source_indicator_parent_class)->constructor (type,
                                                                                  n_construct_properties,
                                                                                  construct_properties);
}

static void
shell_input_source_indicator_constructed (GObject *object)
{
  ShellInputSourceIndicator *self = SHELL_INPUT_SOURCE_INDICATOR (object);
  ShellPopupMenuBase *menu;
  ShellSessionMode *session_mode;
  GWeakRef *indicator_ref;
  ClutterActor *container;
  ShellPopupSeparatorMenuItem *separator;
  ShellPopupMenuSection *section;

  G_OBJECT_CLASS (shell_input_source_indicator_parent_class)->constructed (object);

  clutter_actor_set_accessible_name (CLUTTER_ACTOR (self), _("Keyboard"));

  self->input_sources = g_ptr_array_new_with_free_func (g_object_unref);
  self->menu_items = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                            destroy_actor_value);
  self->indicator_labels = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                                  destroy_actor_value);

  container = g_object_new (SHELL_TYPE_INPUT_SOURCE_INDICATOR_CONTAINER,
                            "style-class", "system-status-icon",
                            NULL);
  g_set_weak_pointer (&self->container, container);
  clutter_actor_add_child (CLUTTER_ACTOR (self), container);

  menu = indicator_get_menu (self);

  separator = g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL);
  g_set_weak_pointer (&self->prop_separator, separator);
  shell_popup_menu_base_add_menu_item (menu, G_OBJECT (separator));

  section = shell_popup_menu_section_new ();
  g_set_weak_pointer (&self->prop_section, section);
  shell_popup_menu_base_add_menu_item (menu, G_OBJECT (section));
  clutter_actor_hide (shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (section)));

  shell_popup_menu_base_add_menu_item (menu,
                                       g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL));
  indicator_ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (indicator_ref, self);
  g_set_weak_pointer (&self->show_layout_item,
                      shell_popup_menu_base_add_action (menu, _("Show Keyboard Layout"),
                                                        indicator_show_layout_cb,
                                                        indicator_ref,
                                                        (GDestroyNotify) weak_ref_free));
  shell_popup_menu_base_add_settings_action (menu, _("Keyboard Settings"),
                                             "gnome-keyboard-panel.desktop");

  session_mode = shell_status_get_session_mode ();
  if (session_mode != NULL)
    g_signal_connect_object (session_mode, "updated",
                             G_CALLBACK (indicator_session_updated), self, G_CONNECT_SWAPPED);
  indicator_session_updated (self);

  self->input_source_manager = shell_input_source_manager_get_default ();
  g_signal_connect_object (self->input_source_manager, "sources-changed",
                           G_CALLBACK (indicator_sources_changed_cb), self, 0);
  g_signal_connect_object (self->input_source_manager, "current-source-changed",
                           G_CALLBACK (indicator_current_source_changed_cb), self, 0);
  g_signal_connect_object (self->input_source_manager, "keymap-changed",
                           G_CALLBACK (indicator_keymap_changed_cb), self, 0);
  shell_input_source_manager_reload (self->input_source_manager);
}

static void
shell_input_source_indicator_dispose (GObject *object)
{
  ShellInputSourceIndicator *self = SHELL_INPUT_SOURCE_INDICATOR (object);

  /* _onDestroy () */
  self->input_source_manager = NULL;

  g_clear_pointer (&self->menu_items, g_hash_table_unref);
  g_clear_pointer (&self->indicator_labels, g_hash_table_unref);
  g_clear_pointer (&self->input_sources, g_ptr_array_unref);

  g_clear_weak_pointer (&self->container);
  g_clear_weak_pointer (&self->prop_separator);
  g_clear_weak_pointer (&self->prop_section);
  g_clear_weak_pointer (&self->show_layout_item);

  G_OBJECT_CLASS (shell_input_source_indicator_parent_class)->dispose (object);
}

static void
shell_input_source_indicator_class_init (ShellInputSourceIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructor = shell_input_source_indicator_constructor;
  object_class->constructed = shell_input_source_indicator_constructed;
  object_class->dispose = shell_input_source_indicator_dispose;

  /**
   * ShellInputSourceIndicator::overview-hide-requested:
   * @self: the #ShellInputSourceIndicator
   *
   * Emitted when the indicator launches the keyboard layout viewer, so that
   * the overview is hidden (`Main.overview.hide ()`).
   */
  indicator_signals[INDICATOR_OVERVIEW_HIDE_REQUESTED] =
    g_signal_new ("overview-hide-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_input_source_indicator_init (ShellInputSourceIndicator *self)
{
}

/**
 * shell_input_source_indicator_new:
 *
 * Creates the keyboard menu button of the top bar
 * (`new InputSourceIndicator ()`).
 *
 * Returns: (transfer floating): a new #ShellInputSourceIndicator
 */
ShellInputSourceIndicator *
shell_input_source_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_INPUT_SOURCE_INDICATOR, NULL);
}
