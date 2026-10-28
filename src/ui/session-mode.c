/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * session-mode.c: The session mode stack (user, gdm, unlock-dialog, ...) and
 * the capabilities of the mode on top of it. Port of js/ui/sessionMode.js.
 *
 * The mode table keeps the JavaScript shape: every mode is a dictionary from
 * the camelCase key used by the JSON mode files (`hasOverview`, `panel`, ...)
 * to a #GVariant holding the raw value, where a key mapped to %NULL stands
 * for the JavaScript `undefined` ("no change / inherit from the previous
 * mode") and a JSON `null` is the unit tuple. Syncing merges
 * restrictive + parent + current exactly as the JS did, copies every defined
 * value into the `applied` dictionary (which keeps the values of previously
 * active modes for the keys a mode leaves undefined) and derives the typed
 * properties from it.
 */

#include "config.h"

#include <math.h>
#include <string.h>

#include <json-glib/json-glib.h>

#include "shell-global.h"
#include "shell-hooks.h"
#include "ui/session-mode.h"

#define DEFAULT_MODE "restrictive"

typedef enum
{
  KIND_BOOL,
  KIND_STRING,
  KIND_STRV,
  KIND_PANEL
} FieldKind;

/* The order is that of the keys of the JS `restrictive` mode, with `panel`
 * split in its three arrays. */
typedef enum
{
  FIELD_PARENT_MODE,
  FIELD_STYLESHEET_NAME,
  FIELD_COLOR_SCHEME,
  FIELD_THEME_RESOURCE_NAME,
  FIELD_HAS_OVERVIEW,
  FIELD_SHOW_CALENDAR_EVENTS,
  FIELD_SHOW_WELCOME_DIALOG,
  FIELD_ALLOW_SETTINGS,
  FIELD_ALLOW_SCREENCAST,
  FIELD_ENABLED_EXTENSIONS,
  FIELD_HAS_RUN_DIALOG,
  FIELD_HAS_WORKSPACES,
  FIELD_HAS_WINDOWS,
  FIELD_HAS_NOTIFICATIONS,
  FIELD_HAS_WM_MENUS,
  FIELD_IS_LOCKED,
  FIELD_IS_GREETER,
  FIELD_IS_PRIMARY,
  FIELD_UNLOCK_DIALOG,
  FIELD_COMPONENTS,
  FIELD_PANEL_LEFT,
  FIELD_PANEL_CENTER,
  FIELD_PANEL_RIGHT,
  FIELD_PANEL_STYLE,

  N_FIELDS
} Field;

typedef struct
{
  const char *key;       /* key in the mode tables and the JSON files */
  const char *prop_name;
  FieldKind kind;
  const char *panel_key; /* KIND_PANEL: member of the `panel` object */
  gboolean from_json;    /* copied from JSON mode files */
} FieldInfo;

static const FieldInfo fields[N_FIELDS] = {
  [FIELD_PARENT_MODE]         = { "parentMode",        "parent-mode",         KIND_STRING, NULL, TRUE },
  [FIELD_STYLESHEET_NAME]     = { "stylesheetName",    "stylesheet-name",     KIND_STRING, NULL, TRUE },
  [FIELD_COLOR_SCHEME]        = { "colorScheme",       "color-scheme",        KIND_STRING, NULL, TRUE },
  [FIELD_THEME_RESOURCE_NAME] = { "themeResourceName", "theme-resource-name", KIND_STRING, NULL, TRUE },
  [FIELD_HAS_OVERVIEW]        = { "hasOverview",       "has-overview",        KIND_BOOL,   NULL, TRUE },
  [FIELD_SHOW_CALENDAR_EVENTS] = { "showCalendarEvents", "show-calendar-events", KIND_BOOL, NULL, TRUE },
  [FIELD_SHOW_WELCOME_DIALOG] = { "showWelcomeDialog", "show-welcome-dialog", KIND_BOOL,   NULL, TRUE },
  [FIELD_ALLOW_SETTINGS]      = { "allowSettings",     "allow-settings",      KIND_BOOL,   NULL, TRUE },
  [FIELD_ALLOW_SCREENCAST]    = { "allowScreencast",   "allow-screencast",    KIND_BOOL,   NULL, TRUE },
  [FIELD_ENABLED_EXTENSIONS]  = { "enabledExtensions", "enabled-extensions",  KIND_STRV,   NULL, TRUE },
  [FIELD_HAS_RUN_DIALOG]      = { "hasRunDialog",      "has-run-dialog",      KIND_BOOL,   NULL, TRUE },
  [FIELD_HAS_WORKSPACES]      = { "hasWorkspaces",     "has-workspaces",      KIND_BOOL,   NULL, TRUE },
  [FIELD_HAS_WINDOWS]         = { "hasWindows",        "has-windows",         KIND_BOOL,   NULL, TRUE },
  [FIELD_HAS_NOTIFICATIONS]   = { "hasNotifications",  "has-notifications",   KIND_BOOL,   NULL, TRUE },
  [FIELD_HAS_WM_MENUS]        = { "hasWmMenus",        "has-wm-menus",        KIND_BOOL,   NULL, TRUE },
  [FIELD_IS_LOCKED]           = { "isLocked",          "is-locked",           KIND_BOOL,   NULL, TRUE },
  [FIELD_IS_GREETER]          = { "isGreeter",         "is-greeter",          KIND_BOOL,   NULL, TRUE },
  [FIELD_IS_PRIMARY]          = { "isPrimary",         "is-primary",          KIND_BOOL,   NULL, TRUE },
  [FIELD_UNLOCK_DIALOG]       = { "unlockDialog",      "unlock-dialog",       KIND_STRING, NULL, FALSE },
  [FIELD_COMPONENTS]          = { "components",        "components",          KIND_STRV,   NULL, TRUE },
  [FIELD_PANEL_LEFT]          = { "panel",             "panel-left",          KIND_PANEL,  "left", TRUE },
  [FIELD_PANEL_CENTER]        = { "panel",             "panel-center",        KIND_PANEL,  "center", TRUE },
  [FIELD_PANEL_RIGHT]         = { "panel",             "panel-right",         KIND_PANEL,  "right", TRUE },
  [FIELD_PANEL_STYLE]         = { "panelStyle",        "panel-style",         KIND_STRING, NULL, TRUE },
};

enum
{
  PROP_0,

  /* PROP_0 + 1 + field: one property per field */
  PROP_CURRENT_MODE = N_FIELDS + 1,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  UPDATED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

typedef struct
{
  gboolean b;
  char *s;
  char **strv;
} FieldValue;

struct _ShellSessionMode
{
  GObject parent;

  GPtrArray *mode_stack;   /* (element-type char *) */
  char *notified_mode;     /* what ::notify::current-mode last announced */

  /* Every value a mode ever defined: key -> GVariant */
  GHashTable *applied;
  FieldValue values[N_FIELDS];
};

G_DEFINE_FINAL_TYPE (ShellSessionMode, shell_session_mode, G_TYPE_OBJECT)

/* ======================================================================== */
/* The mode table                                                           */
/* ======================================================================== */

typedef struct
{
  char *name;
  GHashTable *entries; /* char * -> GVariant * (NULL: undefined) */
} Mode;

static GHashTable *modes = NULL;     /* name -> Mode */
static GPtrArray *mode_names = NULL; /* insertion order, like Object.getOwnPropertyNames */

static void
mode_free (Mode *mode)
{
  g_free (mode->name);
  g_hash_table_unref (mode->entries);
  g_free (mode);
}

static Mode *
lookup_mode (const char *name)
{
  if (modes == NULL || name == NULL)
    return NULL;

  return g_hash_table_lookup (modes, name);
}

static Mode *
mode_new (const char *name)
{
  Mode *mode = g_new0 (Mode, 1);

  mode->name = g_strdup (name);
  mode->entries = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify) g_variant_unref);

  g_hash_table_replace (modes, mode->name, mode);
  g_ptr_array_add (mode_names, mode->name);

  return mode;
}

/* @value is floating or owned, %NULL for `undefined` */
static void
mode_set (Mode       *mode,
          const char *key,
          GVariant   *value)
{
  g_hash_table_replace (mode->entries, g_strdup (key),
                        value != NULL ? g_variant_ref_sink (value) : NULL);
}

static void
mode_set_bool (Mode       *mode,
               const char *key,
               gboolean    value)
{
  mode_set (mode, key, g_variant_new_boolean (value));
}

static void
mode_set_string (Mode       *mode,
                 const char *key,
                 const char *value)
{
  mode_set (mode, key, g_variant_new_string (value));
}

static void
mode_set_null (Mode       *mode,
               const char *key)
{
  mode_set (mode, key, g_variant_new_tuple (NULL, 0));
}

static void
mode_set_strv (Mode               *mode,
               const char         *key,
               const char * const *value)
{
  mode_set (mode, key, g_variant_new_strv (value, -1));
}

static void
mode_set_panel (Mode               *mode,
                const char * const *left,
                const char * const *center,
                const char * const *right)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "left", g_variant_new_strv (left, -1));
  g_variant_builder_add (&builder, "{sv}", "center", g_variant_new_strv (center, -1));
  g_variant_builder_add (&builder, "{sv}", "right", g_variant_new_strv (right, -1));
  mode_set (mode, "panel", g_variant_builder_end (&builder));
}

static void
init_builtin_modes (void)
{
  static const char * const none[] = { NULL };
  static const char * const gdm_components[] = {
#ifdef HAVE_NETWORKMANAGER
    "networkAgent",
#endif
    "polkitAgent", NULL
  };
  static const char * const user_components[] = {
    "polkitAgent", "keyring", "autorunManager", "automountManager",
#ifdef HAVE_NETWORKMANAGER
    "networkAgent",
#endif
    NULL
  };
  static const char * const date_menu[] = { "dateMenu", NULL };
  static const char * const gdm_right[] = { "dwellClick", "keyboard", "quickSettings", NULL };
  static const char * const unlock_right[] = { "dwellClick", "a11y", "keyboard", "quickSettings", NULL };
  static const char * const activities[] = { "activities", NULL };
  static const char * const user_right[] = {
    "screenRecording", "screenSharing", "dwellClick", "a11y", "keyboard", "quickSettings", NULL
  };
  Mode *mode;

  if (modes != NULL)
    return;

  modes = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) mode_free);
  mode_names = g_ptr_array_new ();

  mode = mode_new (DEFAULT_MODE);
  mode_set_null (mode, "parentMode");
  mode_set_string (mode, "stylesheetName", "gnome-shell.css");
  mode_set_string (mode, "colorScheme", "prefer-dark");
  mode_set_string (mode, "themeResourceName", "gnome-shell-theme.gresource");
  mode_set_bool (mode, "hasOverview", FALSE);
  mode_set_bool (mode, "showCalendarEvents", FALSE);
  mode_set_bool (mode, "showWelcomeDialog", FALSE);
  mode_set_bool (mode, "allowSettings", FALSE);
  mode_set_bool (mode, "allowScreencast", FALSE);
  mode_set_strv (mode, "enabledExtensions", none);
  mode_set_bool (mode, "hasRunDialog", FALSE);
  mode_set_bool (mode, "hasWorkspaces", FALSE);
  mode_set_bool (mode, "hasWindows", FALSE);
  mode_set_bool (mode, "hasNotifications", FALSE);
  mode_set_bool (mode, "hasWmMenus", FALSE);
  mode_set_bool (mode, "isLocked", FALSE);
  mode_set_bool (mode, "isGreeter", FALSE);
  mode_set_bool (mode, "isPrimary", FALSE);
  mode_set_null (mode, "unlockDialog");
  mode_set_strv (mode, "components", none);
  mode_set_panel (mode, none, none, none);
  mode_set_null (mode, "panelStyle");

  mode = mode_new ("gdm");
  mode_set_bool (mode, "hasNotifications", TRUE);
  mode_set_bool (mode, "isGreeter", TRUE);
  mode_set_bool (mode, "isPrimary", TRUE);
  /* Names the dialog src/gdm/login-dialog.c implements (see src/ui/screen-shield.c) */
  mode_set_string (mode, "unlockDialog", "login-dialog");
  mode_set_strv (mode, "components", gdm_components);
  mode_set_panel (mode, none, date_menu, gdm_right);
  mode_set_string (mode, "panelStyle", "login-screen");

  mode = mode_new ("unlock-dialog");
  mode_set_bool (mode, "isLocked", TRUE);
  mode_set (mode, "unlockDialog", NULL); /* undefined: keep the previous mode's */
  mode_set_strv (mode, "components", gdm_components);
  mode_set_panel (mode, none, none, unlock_right);
  mode_set_string (mode, "panelStyle", "unlock-screen");

  mode = mode_new ("user");
  mode_set_bool (mode, "hasOverview", TRUE);
  mode_set_bool (mode, "showCalendarEvents", TRUE);
  mode_set_bool (mode, "showWelcomeDialog", TRUE);
  mode_set_bool (mode, "allowSettings", TRUE);
  mode_set_bool (mode, "allowScreencast", TRUE);
  mode_set_bool (mode, "hasRunDialog", TRUE);
  mode_set_bool (mode, "hasWorkspaces", TRUE);
  mode_set_bool (mode, "hasWindows", TRUE);
  mode_set_bool (mode, "hasWmMenus", TRUE);
  mode_set_bool (mode, "hasNotifications", TRUE);
  mode_set_bool (mode, "isLocked", FALSE);
  mode_set_bool (mode, "isPrimary", TRUE);
  /* Names the dialog src/ui/unlock-dialog.c implements */
  mode_set_string (mode, "unlockDialog", "unlock-dialog");
  mode_set_strv (mode, "components", user_components);
  mode_set_panel (mode, activities, date_menu, user_right);
}

/* ======================================================================== */
/* JSON mode files                                                          */
/* ======================================================================== */

/* Returns a floating variant. Without a signature to follow, the mapping is:
 * null -> (), boolean -> b, integer -> x, number -> d, string -> s,
 * array -> as when every element is a string, av otherwise, and
 * object -> a{sv}. */
static GVariant *
node_to_variant (JsonNode *node)
{
  switch (json_node_get_node_type (node))
    {
    case JSON_NODE_VALUE:
      switch (json_node_get_value_type (node))
        {
        case G_TYPE_BOOLEAN:
          return g_variant_new_boolean (json_node_get_boolean (node));

        case G_TYPE_INT64:
          return g_variant_new_int64 (json_node_get_int (node));

        case G_TYPE_DOUBLE:
          return g_variant_new_double (json_node_get_double (node));

        case G_TYPE_STRING:
          return g_variant_new_string (json_node_get_string (node));

        default:
          return g_variant_new_tuple (NULL, 0);
        }

    case JSON_NODE_ARRAY:
      {
        JsonArray *array = json_node_get_array (node);
        guint length = json_array_get_length (array);
        g_autoptr (GPtrArray) children = g_ptr_array_new ();
        gboolean all_strings = TRUE;

        for (guint i = 0; i < length; i++)
          {
            GVariant *child = node_to_variant (json_array_get_element (array, i));

            if (!g_variant_is_of_type (child, G_VARIANT_TYPE_STRING))
              all_strings = FALSE;
            g_ptr_array_add (children, child);
          }

        if (!all_strings)
          {
            for (guint i = 0; i < length; i++)
              children->pdata[i] = g_variant_new_variant (children->pdata[i]);
          }

        return g_variant_new_array (all_strings ? G_VARIANT_TYPE_STRING
                                                : G_VARIANT_TYPE_VARIANT,
                                    (GVariant * const *) children->pdata,
                                    length);
      }

    case JSON_NODE_OBJECT:
      {
        JsonObject *object = json_node_get_object (node);
        g_autoptr (GList) members = json_object_get_members (object);
        GVariantBuilder builder;

        g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
        for (GList *l = members; l != NULL; l = l->next)
          {
            JsonNode *member = json_object_get_member (object, l->data);

            g_variant_builder_add (&builder, "{sv}", l->data, node_to_variant (member));
          }
        return g_variant_builder_end (&builder);
      }

    case JSON_NODE_NULL:
    default:
      return g_variant_new_tuple (NULL, 0);
    }
}

static void
load_mode (GFile     *file,
           GFileInfo *info)
{
  const char *name = g_file_info_get_name (info);
  const char *suffix = strstr (name, ".json");
  g_autofree char *mode_name = NULL;
  g_autofree char *contents = NULL;
  g_autoptr (JsonParser) parser = NULL;
  gsize length;
  JsonNode *root;
  Mode *mode;

  mode_name = suffix != NULL ? g_strndup (name, suffix - name) : g_strdup (name);

  if (lookup_mode (mode_name) != NULL)
    return;

  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, NULL))
    return;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, contents, length, NULL))
    return;

  root = json_parser_get_root (parser);

  /* JSON.parse ('null') succeeds, but the JS then throws on the first
   * property access; skip such a file instead */
  if (root == NULL || json_node_get_node_type (root) == JSON_NODE_NULL)
    return;

  mode = mode_new (mode_name);

  if (json_node_get_node_type (root) == JSON_NODE_OBJECT)
    {
      JsonObject *object = json_node_get_object (root);

      for (int i = 0; i < N_FIELDS; i++)
        {
          const FieldInfo *field = &fields[i];

          if (!field->from_json)
            continue;

          /* `panel` stands for three fields */
          if (field->kind == KIND_PANEL && i != FIELD_PANEL_LEFT)
            continue;

          if (json_object_has_member (object, field->key))
            mode_set (mode, field->key,
                      node_to_variant (json_object_get_member (object, field->key)));
        }
    }

  mode_set_bool (mode, "isPrimary", TRUE);
}

/* Loads external session modes from the system data directories. */
static void
load_modes (void)
{
  const char * const *data_dirs = g_get_system_data_dirs ();

  init_builtin_modes ();

  for (int i = 0; data_dirs[i] != NULL; i++)
    {
      g_autofree char *path = g_build_filename (data_dirs[i], "gnome-shell", "modes", NULL);
      g_autoptr (GFile) dir = g_file_new_for_path (path);
      g_autoptr (GFileEnumerator) enumerator = NULL;
      GFileInfo *info;

      enumerator = g_file_enumerate_children (dir, "standard::name,standard::type",
                                              G_FILE_QUERY_INFO_NONE, NULL, NULL);
      if (enumerator == NULL)
        continue;

      while ((info = g_file_enumerator_next_file (enumerator, NULL, NULL)) != NULL)
        {
          g_autoptr (GFile) file = g_file_enumerator_get_child (enumerator, info);

          load_mode (file, info);
          g_object_unref (info);
        }
    }
}

/* ======================================================================== */
/* GVariant -> typed values (JavaScript semantics where they apply)         */
/* ======================================================================== */

static gboolean
variant_truthy (GVariant *value)
{
  double number;

  if (value == NULL)
    return FALSE;

  switch (g_variant_classify (value))
    {
    case G_VARIANT_CLASS_BOOLEAN:
      return g_variant_get_boolean (value);

    case G_VARIANT_CLASS_INT64:
      return g_variant_get_int64 (value) != 0;

    case G_VARIANT_CLASS_DOUBLE:
      number = g_variant_get_double (value);
      return number != 0.0 && !isnan (number);

    case G_VARIANT_CLASS_STRING:
      return *g_variant_get_string (value, NULL) != '\0';

    case G_VARIANT_CLASS_TUPLE: /* null */
      return FALSE;

    default: /* arrays and objects */
      return TRUE;
    }
}

static char *
variant_dup_string (GVariant *value)
{
  if (value == NULL || !g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
    return NULL;

  return g_variant_dup_string (value, NULL);
}

/* Never %NULL; anything that is not an array of strings is skipped */
static char **
variant_dup_strv (GVariant *value)
{
  g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();

  if (value != NULL && g_variant_is_of_type (value, G_VARIANT_TYPE ("as")))
    {
      GVariantIter iter;
      const char *item;

      g_variant_iter_init (&iter, value);
      while (g_variant_iter_next (&iter, "&s", &item))
        g_strv_builder_add (builder, item);
    }
  else if (value != NULL && g_variant_is_of_type (value, G_VARIANT_TYPE ("av")))
    {
      GVariantIter iter;
      GVariant *item;

      g_variant_iter_init (&iter, value);
      while ((item = g_variant_iter_next_value (&iter)) != NULL)
        {
          GVariant *inner = g_variant_get_variant (item);

          if (g_variant_is_of_type (inner, G_VARIANT_TYPE_STRING))
            g_strv_builder_add (builder, g_variant_get_string (inner, NULL));
          g_variant_unref (inner);
          g_variant_unref (item);
        }
    }

  return g_strv_builder_end (builder);
}

static char **
variant_dup_panel (GVariant   *panel,
                   const char *panel_key)
{
  g_autoptr (GVariant) member = NULL;

  if (panel != NULL && g_variant_is_of_type (panel, G_VARIANT_TYPE_VARDICT))
    member = g_variant_lookup_value (panel, panel_key, NULL);

  return variant_dup_strv (member);
}

/* ======================================================================== */
/* ShellSessionMode                                                         */
/* ======================================================================== */

static const char *
get_current_mode (ShellSessionMode *self)
{
  return g_ptr_array_index (self->mode_stack, self->mode_stack->len - 1);
}

static void
update_field (ShellSessionMode *self,
              int               index)
{
  const FieldInfo *field = &fields[index];
  FieldValue *current = &self->values[index];
  GVariant *value = g_hash_table_lookup (self->applied, field->key);
  gboolean changed = FALSE;

  /* Not defined by any mode so far; only possible before the first sync */
  if (value == NULL)
    return;

  switch (field->kind)
    {
    case KIND_BOOL:
      {
        gboolean b = variant_truthy (value);

        changed = b != current->b;
        current->b = b;
      }
      break;

    case KIND_STRING:
      {
        char *s = variant_dup_string (value);

        changed = g_strcmp0 (s, current->s) != 0;
        g_free (current->s);
        current->s = s;
      }
      break;

    case KIND_STRV:
    case KIND_PANEL:
      {
        char **strv = field->kind == KIND_STRV ? variant_dup_strv (value)
                                               : variant_dup_panel (value, field->panel_key);

        changed = current->strv == NULL || !g_strv_equal ((const char * const *) strv,
                                                          (const char * const *) current->strv);
        g_strfreev (current->strv);
        current->strv = strv;
      }
      break;

    default:
      g_assert_not_reached ();
    }

  if (changed)
    g_object_notify_by_pspec (G_OBJECT (self), props[index + 1]);
}

static void
sync_mode (ShellSessionMode *self)
{
  g_autofree char *mode_name = g_strdup (get_current_mode (self));
  Mode *current = lookup_mode (mode_name);
  Mode *default_mode = lookup_mode (DEFAULT_MODE);
  Mode *parent = NULL;
  g_autoptr (GHashTable) params = NULL;
  GVariant *parent_name;
  GHashTableIter iter;
  gpointer key, value;
  Mode *layers[3];
  g_autofree char *hook_mode = NULL;
  ShellHookRegistry *registry;

  g_assert (current != NULL && default_mode != NULL);

  /* Only a named parent mode is looked up; the JS spread of a missing mode
   * contributes nothing, just like a parent of another type here. */
  parent_name = g_hash_table_lookup (current->entries, "parentMode");
  if (parent_name != NULL && g_variant_is_of_type (parent_name, G_VARIANT_TYPE_STRING))
    parent = lookup_mode (g_variant_get_string (parent_name, NULL));

  /* {...DEFAULT, ...parent, ...current}: later layers override, also with
   * `undefined` */
  params = g_hash_table_new (g_str_hash, g_str_equal);
  layers[0] = default_mode;
  layers[1] = parent;
  layers[2] = current;
  for (int i = 0; i < 3; i++)
    {
      if (layers[i] == NULL)
        continue;

      g_hash_table_iter_init (&iter, layers[i]->entries);
      while (g_hash_table_iter_next (&iter, &key, &value))
        g_hash_table_replace (params, key, value);
    }

  g_object_freeze_notify (G_OBJECT (self));

  /* A simplified version of Lang.copyProperties, handles undefined as a
   * special case for "no change / inherit from previous mode" */
  g_hash_table_iter_init (&iter, params);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (value != NULL)
        g_hash_table_replace (self->applied, g_strdup (key), g_variant_ref (value));
    }

  for (int i = 0; i < N_FIELDS; i++)
    update_field (self, i);

  if (g_strcmp0 (self->notified_mode, mode_name) != 0)
    {
      g_free (self->notified_mode);
      self->notified_mode = g_strdup (mode_name);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CURRENT_MODE]);
    }

  g_object_thaw_notify (G_OBJECT (self));

  g_signal_emit (self, signals[UPDATED], 0);

  /* Lua extensions (gnome-lua): the "session.mode-changed" hook. A handler
   * of ::updated may have changed the mode again. */
  registry = shell_hook_registry_get_default ();
  if (shell_hook_registry_is_declared (registry, "session.mode-changed"))
    {
      GVariantBuilder builder;

      hook_mode = g_strdup (get_current_mode (self));
      g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&builder, "{sv}", "mode", g_variant_new_string (hook_mode));
      shell_hook_registry_emit_after (registry, "session.mode-changed",
                                      g_variant_builder_end (&builder));
    }
}

static gboolean
check_mode_exists (const char  *mode,
                   GError     **error)
{
  if (lookup_mode (mode) != NULL)
    return TRUE;

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               "Unknown session mode '%s'", mode != NULL ? mode : "(null)");
  return FALSE;
}

/**
 * shell_session_mode_new:
 *
 * Creates the session mode stack. The initial mode is the one the shell was
 * started with (`GNOME_SHELL_SESSION_MODE`, see #ShellGlobal:session-mode)
 * when it is a primary mode, `user` otherwise. External modes are loaded from
 * the `gnome-shell/modes` directory of the system data directories.
 *
 * Returns: (transfer full): a new #ShellSessionMode
 */
ShellSessionMode *
shell_session_mode_new (void)
{
  return g_object_new (SHELL_TYPE_SESSION_MODE, NULL);
}

/**
 * shell_session_mode_push_mode:
 * @self: a #ShellSessionMode
 * @mode: the name of the mode to push
 * @error: return location for a #GError
 *
 * Makes @mode the current mode, keeping the previous one on the stack until
 * the matching shell_session_mode_pop_mode().
 *
 * Returns: %TRUE on success, %FALSE if @mode is not a known mode
 */
gboolean
shell_session_mode_push_mode (ShellSessionMode  *self,
                              const char        *mode,
                              GError           **error)
{
  g_return_val_if_fail (SHELL_IS_SESSION_MODE (self), FALSE);
  g_return_val_if_fail (mode != NULL, FALSE);

  if (!check_mode_exists (mode, error))
    return FALSE;

  g_debug ("sessionMode: Pushing mode %s", mode);
  g_ptr_array_add (self->mode_stack, g_strdup (mode));
  sync_mode (self);

  return TRUE;
}

/**
 * shell_session_mode_pop_mode:
 * @self: a #ShellSessionMode
 * @mode: the name of the current mode
 * @error: return location for a #GError
 *
 * Leaves @mode, which must be the current mode, and goes back to the one
 * below it.
 *
 * Returns: %TRUE on success, %FALSE if @mode is not the current mode or it is
 *   the only one left on the stack
 */
gboolean
shell_session_mode_pop_mode (ShellSessionMode  *self,
                             const char        *mode,
                             GError           **error)
{
  g_return_val_if_fail (SHELL_IS_SESSION_MODE (self), FALSE);
  g_return_val_if_fail (mode != NULL, FALSE);

  if (g_strcmp0 (get_current_mode (self), mode) != 0 || self->mode_stack->len == 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Invalid SessionMode.popMode");
      return FALSE;
    }

  g_debug ("sessionMode: Popping mode %s", mode);
  g_ptr_array_remove_index (self->mode_stack, self->mode_stack->len - 1);
  sync_mode (self);

  return TRUE;
}

/**
 * shell_session_mode_switch_mode:
 * @self: a #ShellSessionMode
 * @to: the name of the mode to switch to
 * @error: return location for a #GError
 *
 * Replaces the current mode with @to, without touching the modes below it.
 * Does nothing if @to is already the current mode.
 *
 * Returns: %TRUE on success, %FALSE if @to is not a known mode
 */
gboolean
shell_session_mode_switch_mode (ShellSessionMode  *self,
                                const char        *to,
                                GError           **error)
{
  g_return_val_if_fail (SHELL_IS_SESSION_MODE (self), FALSE);
  g_return_val_if_fail (to != NULL, FALSE);

  if (g_strcmp0 (get_current_mode (self), to) == 0)
    return TRUE;

  if (!check_mode_exists (to, error))
    return FALSE;

  g_free (self->mode_stack->pdata[self->mode_stack->len - 1]);
  self->mode_stack->pdata[self->mode_stack->len - 1] = g_strdup (to);
  sync_mode (self);

  return TRUE;
}

/**
 * shell_session_mode_get_current_mode:
 * @self: a #ShellSessionMode
 *
 * Returns: (transfer none): the name of the mode on top of the stack
 */
const char *
shell_session_mode_get_current_mode (ShellSessionMode *self)
{
  g_return_val_if_fail (SHELL_IS_SESSION_MODE (self), NULL);

  return get_current_mode (self);
}

/**
 * shell_session_mode_get_parent_mode:
 * @self: a #ShellSessionMode
 *
 * Returns: (transfer none) (nullable): the `parentMode` of the current mode
 */
const char *
shell_session_mode_get_parent_mode (ShellSessionMode *self)
{
  g_return_val_if_fail (SHELL_IS_SESSION_MODE (self), NULL);

  return self->values[FIELD_PARENT_MODE].s;
}

/**
 * shell_session_mode_is_locked:
 * @self: a #ShellSessionMode
 *
 * Returns: whether the current mode is a locked one (the lock screen)
 */
gboolean
shell_session_mode_is_locked (ShellSessionMode *self)
{
  g_return_val_if_fail (SHELL_IS_SESSION_MODE (self), FALSE);

  return self->values[FIELD_IS_LOCKED].b;
}

/**
 * shell_session_mode_is_greeter:
 * @self: a #ShellSessionMode
 *
 * Returns: whether the current mode is the login screen
 */
gboolean
shell_session_mode_is_greeter (ShellSessionMode *self)
{
  g_return_val_if_fail (SHELL_IS_SESSION_MODE (self), FALSE);

  return self->values[FIELD_IS_GREETER].b;
}

/**
 * shell_session_mode_is_primary:
 * @self: a #ShellSessionMode
 *
 * Returns: whether the current mode can be the mode the shell starts in
 */
gboolean
shell_session_mode_is_primary (ShellSessionMode *self)
{
  g_return_val_if_fail (SHELL_IS_SESSION_MODE (self), FALSE);

  return self->values[FIELD_IS_PRIMARY].b;
}

/**
 * shell_session_mode_get_field:
 * @self: a #ShellSessionMode
 * @name: a key of the mode files, e.g. `showCalendarEvents`; kebab-case
 *   (`show-calendar-events`) is accepted too
 *
 * Gets the raw value of any field of the current mode, as defined in the
 * built-in table or its JSON file: booleans are `b`, integers `x`, other
 * numbers `d`, strings `s`, arrays of strings `as`, other arrays `av`
 * and objects `a{sv}`. The value is the one in effect, i.e. a field the
 * current mode leaves undefined keeps the value of the mode before it.
 *
 * Returns: (transfer full) (nullable): the value, or %NULL if the field is
 *   null or was never defined
 */
GVariant *
shell_session_mode_get_field (ShellSessionMode *self,
                              const char       *name)
{
  GVariant *value;

  g_return_val_if_fail (SHELL_IS_SESSION_MODE (self), NULL);
  g_return_val_if_fail (name != NULL, NULL);

  value = g_hash_table_lookup (self->applied, name);

  if (value == NULL && strchr (name, '-') != NULL)
    {
      g_autoptr (GString) camel = g_string_new (NULL);
      gboolean upper = FALSE;

      for (const char *p = name; *p != '\0'; p++)
        {
          if (*p == '-')
            upper = TRUE;
          else
            {
              g_string_append_c (camel, upper ? g_ascii_toupper (*p) : *p);
              upper = FALSE;
            }
        }

      value = g_hash_table_lookup (self->applied, camel->str);
    }

  if (value == NULL || g_variant_is_of_type (value, G_VARIANT_TYPE_UNIT))
    return NULL;

  return g_variant_ref (value);
}

/**
 * shell_session_mode_list_modes:
 *
 * Loads the external modes and lists the primary ones (those the shell can be
 * started in), built-in modes first. This is what `gnome-shell --list-modes`
 * prints.
 *
 * Returns: (transfer full) (array zero-terminated=1): the mode names
 */
char **
shell_session_mode_list_modes (void)
{
  g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();

  load_modes ();

  for (guint i = 0; i < mode_names->len; i++)
    {
      Mode *mode = lookup_mode (g_ptr_array_index (mode_names, i));

      if (variant_truthy (g_hash_table_lookup (mode->entries, "isPrimary")))
        g_strv_builder_add (builder, mode->name);
    }

  return g_strv_builder_end (builder);
}

static void
shell_session_mode_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellSessionMode *self = SHELL_SESSION_MODE (object);

  if (prop_id == PROP_CURRENT_MODE)
    {
      g_value_set_string (value, get_current_mode (self));
      return;
    }

  if (prop_id >= 1 && prop_id <= N_FIELDS)
    {
      int index = prop_id - 1;
      FieldValue *field_value = &self->values[index];

      switch (fields[index].kind)
        {
        case KIND_BOOL:
          g_value_set_boolean (value, field_value->b);
          break;

        case KIND_STRING:
          g_value_set_string (value, field_value->s);
          break;

        case KIND_STRV:
        case KIND_PANEL:
          g_value_set_boxed (value, field_value->strv);
          break;

        default:
          g_assert_not_reached ();
        }
      return;
    }

  G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
shell_session_mode_constructed (GObject *object)
{
  ShellSessionMode *self = SHELL_SESSION_MODE (object);
  ShellGlobal *global = NULL;
  const char *session_mode = NULL;
  Mode *requested;
  const char *mode = "user";

  G_OBJECT_CLASS (shell_session_mode_parent_class)->constructed (object);

  load_modes ();

  /* GNOME_SHELL_SESSION_MODE, lower-cased, or gdm/user (see main.c) */
  global = g_type_class_peek (SHELL_TYPE_GLOBAL) != NULL ? shell_global_get () : NULL;
  if (global != NULL)
    session_mode = shell_global_get_session_mode (global);

  requested = lookup_mode (session_mode);
  if (requested != NULL &&
      variant_truthy (g_hash_table_lookup (requested->entries, "isPrimary")))
    mode = requested->name;

  g_ptr_array_add (self->mode_stack, g_strdup (mode));
  sync_mode (self);
}

static void
shell_session_mode_finalize (GObject *object)
{
  ShellSessionMode *self = SHELL_SESSION_MODE (object);

  g_clear_pointer (&self->mode_stack, g_ptr_array_unref);
  g_clear_pointer (&self->applied, g_hash_table_unref);
  g_clear_pointer (&self->notified_mode, g_free);

  for (int i = 0; i < N_FIELDS; i++)
    {
      g_clear_pointer (&self->values[i].s, g_free);
      g_clear_pointer (&self->values[i].strv, g_strfreev);
    }

  G_OBJECT_CLASS (shell_session_mode_parent_class)->finalize (object);
}

static void
shell_session_mode_class_init (ShellSessionModeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamFlags flags = G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;

  object_class->constructed = shell_session_mode_constructed;
  object_class->finalize = shell_session_mode_finalize;
  object_class->get_property = shell_session_mode_get_property;

  for (int i = 0; i < N_FIELDS; i++)
    {
      switch (fields[i].kind)
        {
        case KIND_BOOL:
          props[i + 1] = g_param_spec_boolean (fields[i].prop_name, NULL, NULL,
                                               FALSE, flags);
          break;

        case KIND_STRING:
          props[i + 1] = g_param_spec_string (fields[i].prop_name, NULL, NULL,
                                              NULL, flags);
          break;

        case KIND_STRV:
        case KIND_PANEL:
          props[i + 1] = g_param_spec_boxed (fields[i].prop_name, NULL, NULL,
                                             G_TYPE_STRV, flags);
          break;

        default:
          g_assert_not_reached ();
        }
    }

  /**
   * ShellSessionMode:current-mode:
   *
   * The name of the mode on top of the stack.
   */
  props[PROP_CURRENT_MODE] =
    g_param_spec_string ("current-mode", NULL, NULL, NULL, flags);

  /**
   * ShellSessionMode:parent-mode:
   *
   * The `parentMode` of the current mode: the mode whose fields it inherits
   * before its own, or %NULL.
   */

  /**
   * ShellSessionMode:stylesheet-name:
   *
   * Name of the theme stylesheet to load, e.g. `gnome-shell.css`.
   */

  /**
   * ShellSessionMode:color-scheme:
   *
   * `prefer-dark` or `default`: the colour scheme the stylesheet is for.
   */

  /**
   * ShellSessionMode:theme-resource-name:
   *
   * Name of the theme gresource in the data directory.
   */

  /**
   * ShellSessionMode:has-overview:
   *
   * Whether the overview is available.
   */

  /**
   * ShellSessionMode:show-calendar-events:
   *
   * Whether the date menu lists calendar events.
   */

  /**
   * ShellSessionMode:show-welcome-dialog:
   *
   * Whether the welcome dialog may be shown at startup.
   */

  /**
   * ShellSessionMode:allow-settings:
   *
   * Whether the settings entries of menus are available.
   */

  /**
   * ShellSessionMode:allow-screencast:
   *
   * Whether screen casting and remote access are allowed.
   */

  /**
   * ShellSessionMode:enabled-extensions:
   *
   * UUIDs of the extensions the mode enables on its own.
   */

  /**
   * ShellSessionMode:has-run-dialog:
   *
   * Whether the run dialog (Alt+F2) is available.
   */

  /**
   * ShellSessionMode:has-workspaces:
   *
   * Whether workspaces can be used.
   */

  /**
   * ShellSessionMode:has-windows:
   *
   * Whether windows are shown and can be managed.
   */

  /**
   * ShellSessionMode:has-notifications:
   *
   * Whether notifications are shown.
   */

  /**
   * ShellSessionMode:has-wm-menus:
   *
   * Whether window menus are available.
   */

  /**
   * ShellSessionMode:is-locked:
   *
   * Whether the session is locked.
   */

  /**
   * ShellSessionMode:is-greeter:
   *
   * Whether this is the login screen.
   */

  /**
   * ShellSessionMode:is-primary:
   *
   * Whether the mode can be the mode the shell is started in.
   */

  /**
   * ShellSessionMode:unlock-dialog:
   *
   * Which dialog authenticates the user in this mode: `login-dialog`
   * (src/gdm/login-dialog.c) or `unlock-dialog` (src/ui/unlock-dialog.c);
   * %NULL if the mode cannot be locked. A mode that leaves it undefined
   * (`unlock-dialog`) keeps the value of the mode below it.
   */

  /**
   * ShellSessionMode:components:
   *
   * Names of the components (js/ui/components/) to enable.
   */

  /**
   * ShellSessionMode:panel-left:
   *
   * Roles of the indicators in the left box of the top bar.
   */

  /**
   * ShellSessionMode:panel-center:
   *
   * Roles of the indicators in the center box of the top bar.
   */

  /**
   * ShellSessionMode:panel-right:
   *
   * Roles of the indicators in the right box of the top bar.
   */

  /**
   * ShellSessionMode:panel-style:
   *
   * Style class added to the top bar, or %NULL.
   */

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellSessionMode::updated:
   * @self: the #ShellSessionMode
   *
   * Emitted after the mode changed and the properties were updated.
   */
  signals[UPDATED] =
    g_signal_new ("updated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_session_mode_init (ShellSessionMode *self)
{
  self->mode_stack = g_ptr_array_new_with_free_func (g_free);
  self->applied = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify) g_variant_unref);

  for (int i = 0; i < N_FIELDS; i++)
    {
      if (fields[i].kind == KIND_STRV || fields[i].kind == KIND_PANEL)
        self->values[i].strv = g_new0 (char *, 1);
    }
}
