/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaExtension: one installed Lua extension and its runtime context.
 * Mirrors the per-extension parts of js/ui/extensionSystem.js and
 * js/misc/extensionUtils.js.
 */

#include <string.h>

#include "shell-lua-extension.h"
#include "shell-lua-extension-private.h"
#include "shell-lua-api-private.h"

static const char * const known_capabilities[] = {
  "dbus", "settings", "search", "fs", "spawn", "network", NULL
};

enum {
  SIGNAL_STATE_CHANGED,
  SIGNAL_LUA_ERROR,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

typedef struct
{
  guint          id;
  GObject       *object;   /* NULL for own_full() entries */
  gpointer       data;
  GDestroyNotify func;
} OwnedEntry;

struct _ShellLuaExtension
{
  GObject parent_instance;

  char *uuid;
  GFile *dir;
  ShellLuaExtensionType type;
  ShellLuaExtensionState state;
  char *error;

  JsonObject *metadata;
  char **capabilities;   /* NULL-terminated, never NULL */
  char **session_modes;  /* NULL-terminated, never NULL */
  gboolean has_prefs;
  gboolean enabled;
  gboolean can_change;

  GSettings *settings;
  gboolean settings_tried;

  ShellLuaLimits limits;
  gboolean has_limits;

  ShellLuaState *lua_state;
  gulong error_handler;
  int ext_ref;
  guint error_count;

  GQueue owned;
  guint next_owned_id;
};

G_DEFINE_FINAL_TYPE (ShellLuaExtension, shell_lua_extension, G_TYPE_OBJECT)

const char *
shell_lua_extension_state_to_string (ShellLuaExtensionState state)
{
  switch (state)
    {
    case SHELL_LUA_EXTENSION_STATE_ACTIVE:
      return "ACTIVE";
    case SHELL_LUA_EXTENSION_STATE_INACTIVE:
      return "INACTIVE";
    case SHELL_LUA_EXTENSION_STATE_ERROR:
      return "ERROR";
    case SHELL_LUA_EXTENSION_STATE_OUT_OF_DATE:
      return "OUT_OF_DATE";
    case SHELL_LUA_EXTENSION_STATE_DOWNLOADING:
      return "DOWNLOADING";
    case SHELL_LUA_EXTENSION_STATE_INITIALIZED:
      return "INITIALIZED";
    case SHELL_LUA_EXTENSION_STATE_DEACTIVATING:
      return "DEACTIVATING";
    case SHELL_LUA_EXTENSION_STATE_ACTIVATING:
      return "ACTIVATING";
    case SHELL_LUA_EXTENSION_STATE_UNINSTALLED:
      return "UNINSTALLED";
    default:
      return "UNKNOWN";
    }
}

static void
set_state (ShellLuaExtension      *self,
           ShellLuaExtensionState  state)
{
  g_debug ("Changing state of Lua extension %s to %s", self->uuid,
           shell_lua_extension_state_to_string (state));

  self->state = state;
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

static void
set_error_message (ShellLuaExtension *self,
                   const char        *message)
{
  g_free (self->error);
  self->error = g_strdup (message);
}

/* ------------------------------------------------------------------ */
/* Metadata                                                           */
/* ------------------------------------------------------------------ */

static const char *
metadata_get_string (JsonObject *obj,
                     const char *member)
{
  JsonNode *node = json_object_get_member (obj, member);

  if (node == NULL || json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;

  return json_node_get_string (node);
}

static gboolean
is_known_capability (const char *capability)
{
  return g_strv_contains (known_capabilities, capability);
}

/* Parses "N" or "N.M" (decimal digits only). Returns FALSE for anything
 * else. *minor is 0 when absent. */
static gboolean
parse_api_version (const char *api,
                   gint64     *major,
                   gint64     *minor)
{
  char *end = NULL;

  if (!g_ascii_isdigit (api[0]))
    return FALSE;

  *major = g_ascii_strtoll (api, &end, 10);
  *minor = 0;
  if (*end == '\0')
    return TRUE;
  if (*end != '.' || !g_ascii_isdigit (end[1]))
    return FALSE;

  *minor = g_ascii_strtoll (end + 1, &end, 10);
  return *end == '\0';
}

static char **
strv_from_json_array (JsonArray *array)
{
  g_autoptr (GPtrArray) out = g_ptr_array_new_with_free_func (g_free);
  unsigned int i, n = json_array_get_length (array);

  for (i = 0; i < n; i++)
    {
      JsonNode *node = json_array_get_element (array, i);

      if (json_node_get_value_type (node) == G_TYPE_STRING)
        g_ptr_array_add (out, g_strdup (json_node_get_string (node)));
    }

  g_ptr_array_add (out, NULL);
  return (char **) g_ptr_array_free (g_steal_pointer (&out), FALSE);
}

static gboolean
load_metadata (ShellLuaExtension  *self,
               GFile              *dir,
               GError            **error)
{
  g_autoptr (GFile) metadata_file = NULL;
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GError) local_error = NULL;
  g_autofree char *dir_name = NULL;
  g_autofree char *contents = NULL;
  g_autoptr (GPtrArray) caps = NULL;
  JsonNode *root;
  JsonObject *obj;
  JsonNode *node;
  const char *uuid;
  const char *name;
  const char *api;
  gint64 api_major = 0;
  gint64 api_minor = 0;
  gsize length = 0;

  dir_name = g_file_get_basename (dir);
  metadata_file = g_file_get_child (dir, "metadata.json");

  if (!g_file_load_contents (metadata_file, NULL, &contents, &length, NULL,
                             &local_error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             "Missing metadata.json");
      else
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Failed to load metadata.json: %s", local_error->message);
      return FALSE;
    }

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, contents, length, &local_error))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Failed to parse metadata.json: %s", local_error->message);
      return FALSE;
    }

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "metadata.json does not contain an object");
      return FALSE;
    }
  obj = json_node_get_object (root);

  /* Required string properties */
  uuid = metadata_get_string (obj, "uuid");
  if (uuid == NULL || *uuid == '\0')
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           json_object_has_member (obj, "uuid")
                           ? "property \"uuid\" is not of type string"
                           : "missing \"uuid\" property in metadata.json");
      return FALSE;
    }

  name = metadata_get_string (obj, "name");
  if (name == NULL || *name == '\0')
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           json_object_has_member (obj, "name")
                           ? "property \"name\" is not of type string"
                           : "missing \"name\" property in metadata.json");
      return FALSE;
    }

  api = metadata_get_string (obj, "shell-api");
  if (api == NULL || *api == '\0')
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           json_object_has_member (obj, "shell-api")
                           ? "property \"shell-api\" is not of type string"
                           : "missing \"shell-api\" property in metadata.json");
      return FALSE;
    }

  /* Same rule as loadExtensionMetadata() */
  if (strcmp (dir_name, uuid) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "UUID \"%s\" from metadata.json does not match "
                   "directory name \"%s\"", uuid, dir_name);
      return FALSE;
    }

  /* Optional properties with a type */
  if (json_object_has_member (obj, "description") &&
      metadata_get_string (obj, "description") == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "property \"description\" is not of type string");
      return FALSE;
    }

  if (json_object_has_member (obj, "settings-schema") &&
      metadata_get_string (obj, "settings-schema") == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "property \"settings-schema\" is not of type string");
      return FALSE;
    }

  if (json_object_has_member (obj, "gettext-domain") &&
      metadata_get_string (obj, "gettext-domain") == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "property \"gettext-domain\" is not of type string");
      return FALSE;
    }

  /* Capabilities: array of strings from the known set */
  caps = g_ptr_array_new_with_free_func (g_free);
  node = json_object_get_member (obj, "capabilities");
  if (node != NULL)
    {
      JsonArray *array;
      unsigned int i, n;

      if (!JSON_NODE_HOLDS_ARRAY (node))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "property \"capabilities\" is not of type "
                               "string array");
          return FALSE;
        }

      array = json_node_get_array (node);
      n = json_array_get_length (array);
      for (i = 0; i < n; i++)
        {
          JsonNode *element = json_array_get_element (array, i);
          const char *capability;

          if (json_node_get_value_type (element) != G_TYPE_STRING)
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                   "property \"capabilities\" is not of type "
                                   "string array");
              return FALSE;
            }

          capability = json_node_get_string (element);
          if (!is_known_capability (capability))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "unknown capability \"%s\" in metadata.json",
                           capability);
              return FALSE;
            }

          if (!g_ptr_array_find_with_equal_func (caps, capability,
                                                 g_str_equal, NULL))
            g_ptr_array_add (caps, g_strdup (capability));
        }
    }
  g_ptr_array_add (caps, NULL);

  /* Everything is valid: commit */
  self->uuid = g_strdup (uuid);
  self->metadata = json_object_ref (obj);
  self->capabilities =
    (char **) g_ptr_array_free (g_steal_pointer (&caps), FALSE);

  node = json_object_get_member (obj, "session-modes");
  if (node != NULL && JSON_NODE_HOLDS_ARRAY (node))
    self->session_modes = strv_from_json_array (json_node_get_array (node));
  if (self->session_modes == NULL || self->session_modes[0] == NULL)
    {
      g_strfreev (self->session_modes);
      self->session_modes = g_strsplit ("user", ",", -1);
    }

  /* "1" or "1.N": the major must match, and the running minor must be at
   * least N (additive minors: an extension written for 1.3 needs 1.3). */
  if (!parse_api_version (api, &api_major, &api_minor) ||
      api_major != SHELL_LUA_API_MAJOR ||
      api_minor > SHELL_LUA_API_MINOR)
    {
      self->state = SHELL_LUA_EXTENSION_STATE_OUT_OF_DATE;
      self->error = g_strdup_printf ("shell-api %s not supported", api);
    }

  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Object                                                             */
/* ------------------------------------------------------------------ */

ShellLuaExtension *
shell_lua_extension_new_from_dir (GFile                  *dir,
                                  ShellLuaExtensionType   type,
                                  GError                **error)
{
  g_autoptr (ShellLuaExtension) self = NULL;
  g_autoptr (GFile) prefs = NULL;

  g_return_val_if_fail (G_IS_FILE (dir), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  self = g_object_new (SHELL_TYPE_LUA_EXTENSION, NULL);
  self->dir = g_object_ref (dir);
  self->type = type;
  self->state = SHELL_LUA_EXTENSION_STATE_INITIALIZED;

  if (!load_metadata (self, dir, error))
    return NULL;

  prefs = g_file_get_child (dir, "prefs.lua");
  self->has_prefs = g_file_query_exists (prefs, NULL);

  return g_steal_pointer (&self);
}

static void
owned_entry_free (OwnedEntry *entry)
{
  g_free (entry);
}

static void
owned_entry_run (OwnedEntry *entry)
{
  if (entry->object != NULL)
    {
      if (entry->func != NULL)
        entry->func (entry->object);
      else
        g_object_run_dispose (entry->object);
      g_object_unref (entry->object);
    }
  else if (entry->func != NULL)
    {
      entry->func (entry->data);
    }

  owned_entry_free (entry);
}

static void
dispose_owned (ShellLuaExtension *self)
{
  OwnedEntry *entry;

  /* Pop one at a time: dispose callbacks may release other entries. */
  while ((entry = g_queue_pop_tail (&self->owned)) != NULL)
    owned_entry_run (entry);
}

static void
on_state_error (ShellLuaState     *state,
                GError            *error,
                ShellLuaExtension *self)
{
  self->error_count++;
  g_signal_emit (self, signals[SIGNAL_LUA_ERROR], 0, error);
}

/* Calls the optional extension.disable() of a live state; errors are logged
 * and otherwise ignored. */
static void
call_lua_disable (ShellLuaExtension *self)
{
  g_autoptr (GError) error = NULL;
  lua_State *L;
  int top;

  if (self->lua_state == NULL || self->ext_ref == LUA_NOREF ||
      !shell_lua_state_is_alive (self->lua_state))
    return;

  L = shell_lua_state_get_L (self->lua_state);
  top = lua_gettop (L);

  shell_lua_state_push_ref (self->lua_state, self->ext_ref);
  lua_pushliteral (L, "disable");
  lua_rawget (L, -2);

  if (lua_isfunction (L, -1))
    {
      if (!shell_lua_state_pcall (self->lua_state, 0, 0, &error))
        g_warning ("Error in disable() of Lua extension %s: %s",
                   self->uuid, error->message);
    }

  lua_settop (L, top);
}

/* Destroys everything created at enable(): owned objects (LIFO), the registry
 * reference and the Lua state. */
static void
teardown_runtime (ShellLuaExtension *self,
                  gboolean           call_disable)
{
  /* From here on no callback runs (README "Ownership and teardown"): not
   * during disable(), not while the objects are destroyed. */
  if (self->lua_state != NULL)
    shell_lua_objects_block (self->lua_state);

  if (call_disable)
    call_lua_disable (self);

  /* Everything created through shell.*, newest first. */
  if (self->lua_state != NULL)
    shell_lua_objects_destroy_all (self->lua_state);

  dispose_owned (self);

  if (self->lua_state != NULL)
    {
      if (self->ext_ref != LUA_NOREF && shell_lua_state_is_alive (self->lua_state))
        shell_lua_state_unref (self->lua_state, self->ext_ref);
      self->ext_ref = LUA_NOREF;

      if (self->error_handler != 0)
        g_signal_handler_disconnect (self->lua_state, self->error_handler);
      self->error_handler = 0;

      shell_lua_state_destroy (self->lua_state);
      shell_lua_state_set_owner (self->lua_state, NULL);
      g_clear_object (&self->lua_state);
    }

  self->ext_ref = LUA_NOREF;
}

static void
shell_lua_extension_dispose (GObject *object)
{
  ShellLuaExtension *self = SHELL_LUA_EXTENSION (object);

  teardown_runtime (self, FALSE);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (shell_lua_extension_parent_class)->dispose (object);
}

static void
shell_lua_extension_finalize (GObject *object)
{
  ShellLuaExtension *self = SHELL_LUA_EXTENSION (object);

  g_free (self->uuid);
  g_free (self->error);
  g_clear_object (&self->dir);
  g_clear_pointer (&self->metadata, json_object_unref);
  g_strfreev (self->capabilities);
  g_strfreev (self->session_modes);

  G_OBJECT_CLASS (shell_lua_extension_parent_class)->finalize (object);
}

static void
shell_lua_extension_class_init (ShellLuaExtensionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_lua_extension_dispose;
  object_class->finalize = shell_lua_extension_finalize;

  signals[SIGNAL_STATE_CHANGED] =
    g_signal_new ("state-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_LUA_ERROR] =
    g_signal_new ("lua-error",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_ERROR);
}

static void
shell_lua_extension_init (ShellLuaExtension *self)
{
  self->ext_ref = LUA_NOREF;
  self->next_owned_id = 1;
  g_queue_init (&self->owned);
}

/* ------------------------------------------------------------------ */
/* Accessors                                                          */
/* ------------------------------------------------------------------ */

const char *
shell_lua_extension_get_uuid (ShellLuaExtension *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), NULL);
  return self->uuid;
}

GFile *
shell_lua_extension_get_dir (ShellLuaExtension *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), NULL);
  return self->dir;
}

ShellLuaExtensionType
shell_lua_extension_get_extension_type (ShellLuaExtension *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), 0);
  return self->type;
}

ShellLuaExtensionState
shell_lua_extension_get_state (ShellLuaExtension *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), 0);
  return self->state;
}

const char *
shell_lua_extension_get_error (ShellLuaExtension *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), NULL);
  return self->error;
}

JsonObject *
shell_lua_extension_get_metadata (ShellLuaExtension *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), NULL);
  return self->metadata;
}

ShellLuaState *
shell_lua_extension_get_lua_state (ShellLuaExtension *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), NULL);
  return self->lua_state;
}

const char * const *
shell_lua_extension_get_capabilities (ShellLuaExtension *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), NULL);
  return (const char * const *) self->capabilities;
}

gboolean
shell_lua_extension_has_capability (ShellLuaExtension *self,
                                    const char        *capability)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), FALSE);
  g_return_val_if_fail (capability != NULL, FALSE);

  return g_strv_contains ((const char * const *) self->capabilities,
                          capability);
}

void
shell_lua_extension_set_limits (ShellLuaExtension    *self,
                                const ShellLuaLimits *limits)
{
  g_return_if_fail (SHELL_IS_LUA_EXTENSION (self));

  if (limits != NULL)
    {
      self->limits = *limits;
      self->has_limits = TRUE;
    }
  else
    {
      memset (&self->limits, 0, sizeof (self->limits));
      self->has_limits = FALSE;
    }
}

void
shell_lua_extension_set_enabled (ShellLuaExtension *self,
                                 gboolean           enabled)
{
  g_return_if_fail (SHELL_IS_LUA_EXTENSION (self));

  enabled = !!enabled;
  if (self->enabled == enabled)
    return;

  self->enabled = enabled;
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

gboolean
shell_lua_extension_get_enabled (ShellLuaExtension *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), FALSE);
  return self->enabled;
}

void
shell_lua_extension_set_can_change (ShellLuaExtension *self,
                                    gboolean           can_change)
{
  g_return_if_fail (SHELL_IS_LUA_EXTENSION (self));

  can_change = !!can_change;
  if (self->can_change == can_change)
    return;

  self->can_change = can_change;
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

guint
shell_lua_extension_get_error_count (ShellLuaExtension *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), 0);
  return self->error_count;
}

/* ------------------------------------------------------------------ */
/* Settings                                                           */
/* ------------------------------------------------------------------ */

GSettings *
shell_lua_extension_get_settings (ShellLuaExtension *self)
{
  g_autoptr (GSettingsSchemaSource) source = NULL;
  g_autoptr (GSettingsSchema) schema = NULL;
  g_autoptr (GFile) schema_dir = NULL;
  g_autoptr (GError) error = NULL;
  GSettingsSchemaSource *default_source;
  const char *schema_id;

  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), NULL);

  if (self->settings != NULL || self->settings_tried)
    return self->settings;

  self->settings_tried = TRUE;

  schema_id = metadata_get_string (self->metadata, "settings-schema");
  if (schema_id == NULL)
    {
      g_warning ("Lua extension %s has no \"settings-schema\" in its metadata",
                 self->uuid);
      return NULL;
    }

  default_source = g_settings_schema_source_get_default ();

  /* Like getSettings(): user extensions ship a schemas/ subfolder, otherwise
   * assume a system extension whose schema is installed system-wide. */
  schema_dir = g_file_get_child (self->dir, "schemas");
  if (g_file_query_exists (schema_dir, NULL))
    {
      g_autofree char *path = g_file_get_path (schema_dir);

      source = g_settings_schema_source_new_from_directory (path,
                                                            default_source,
                                                            FALSE, &error);
      if (source == NULL)
        {
          g_warning ("Could not load schemas of Lua extension %s: %s",
                     self->uuid, error->message);
          return NULL;
        }
    }
  else if (default_source != NULL)
    {
      source = g_settings_schema_source_ref (default_source);
    }
  else
    {
      g_warning ("Schema %s could not be found for Lua extension %s. "
                 "Please check your installation", schema_id, self->uuid);
      return NULL;
    }

  schema = g_settings_schema_source_lookup (source, schema_id, TRUE);
  if (schema == NULL)
    {
      g_warning ("Schema %s could not be found for Lua extension %s. "
                 "Please check your installation", schema_id, self->uuid);
      return NULL;
    }

  self->settings = g_settings_new_full (schema, NULL, NULL);
  return self->settings;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static gboolean
enable_failed (ShellLuaExtension  *self,
               GError             *failure,
               GError            **error)
{
  teardown_runtime (self, FALSE);
  set_error_message (self, failure->message);
  set_state (self, SHELL_LUA_EXTENSION_STATE_ERROR);

  g_propagate_error (error, failure);
  return FALSE;
}

gboolean
shell_lua_extension_enable (ShellLuaExtension  *self,
                            GError            **error)
{
  g_autoptr (GFile) main_file = NULL;
  GError *local_error = NULL;
  lua_State *L;
  int top;

  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  switch (self->state)
    {
    case SHELL_LUA_EXTENSION_STATE_ACTIVE:
      return TRUE;

    case SHELL_LUA_EXTENSION_STATE_OUT_OF_DATE:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Extension %s cannot be enabled: %s", self->uuid,
                   self->error ? self->error : "out of date");
      return FALSE;

    case SHELL_LUA_EXTENSION_STATE_ACTIVATING:
    case SHELL_LUA_EXTENSION_STATE_DEACTIVATING:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                   "Extension %s is changing state", self->uuid);
      return FALSE;

    case SHELL_LUA_EXTENSION_STATE_UNINSTALLED:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Extension %s is not installed", self->uuid);
      return FALSE;

    default:
      break;
    }

  /* A previous failure may have left a state behind */
  teardown_runtime (self, FALSE);

  set_error_message (self, NULL);
  self->error_count = 0;
  set_state (self, SHELL_LUA_EXTENSION_STATE_ACTIVATING);

  self->lua_state = shell_lua_state_new (self->uuid,
                                         SHELL_LUA_STATE_SANDBOXED,
                                         self->has_limits ? &self->limits
                                                          : NULL);
  shell_lua_state_set_owner (self->lua_state, self);
  shell_lua_state_add_search_root (self->lua_state, self->dir);
  self->error_handler = g_signal_connect (self->lua_state, "error",
                                          G_CALLBACK (on_state_error), self);

  L = shell_lua_state_get_L (self->lua_state);
  top = lua_gettop (L);

  main_file = g_file_get_child (self->dir, "extension.lua");
  if (!shell_lua_state_load_file (self->lua_state, main_file, &local_error))
    return enable_failed (self, local_error, error);

  if (!shell_lua_state_pcall (self->lua_state, 0, 1, &local_error))
    return enable_failed (self, local_error, error);

  if (!lua_istable (L, -1))
    {
      lua_settop (L, top);
      g_set_error_literal (&local_error, SHELL_LUA_ERROR,
                           SHELL_LUA_ERROR_RUNTIME,
                           "extension.lua must return a table");
      return enable_failed (self, local_error, error);
    }

  /* Keep the returned table; pops it */
  self->ext_ref = shell_lua_state_ref (self->lua_state);
  g_assert (lua_gettop (L) == top);

  /* enable() is optional. Raw access: the table may carry a metatable. */
  shell_lua_state_push_ref (self->lua_state, self->ext_ref);
  lua_pushliteral (L, "enable");
  lua_rawget (L, -2);
  if (lua_isfunction (L, -1))
    {
      if (!shell_lua_state_pcall (self->lua_state, 0, 0, &local_error))
        {
          /* enable() may have disabled this very extension (or destroyed
           * its state): the lua_State is closed then, leave it alone. */
          if (self->lua_state != NULL && shell_lua_state_get_L (self->lua_state) == L)
            lua_settop (L, top);
          return enable_failed (self, local_error, error);
        }
    }
  lua_settop (L, top);

  set_state (self, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  return TRUE;
}

gboolean
shell_lua_extension_disable (ShellLuaExtension  *self,
                             GError            **error)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->lua_state == NULL)
    {
      /* Nothing running: nothing to do, keep INITIALIZED/INACTIVE/ERROR. */
      return TRUE;
    }

  if (self->state == SHELL_LUA_EXTENSION_STATE_DEACTIVATING)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                   "Extension %s is already being disabled", self->uuid);
      return FALSE;
    }

  set_state (self, SHELL_LUA_EXTENSION_STATE_DEACTIVATING);
  teardown_runtime (self, TRUE);
  set_state (self, SHELL_LUA_EXTENSION_STATE_INACTIVE);

  return TRUE;
}

void
shell_lua_extension_fail (ShellLuaExtension *self,
                          const char        *message)
{
  g_return_if_fail (SHELL_IS_LUA_EXTENSION (self));

  teardown_runtime (self, TRUE);
  set_error_message (self, message);
  set_state (self, SHELL_LUA_EXTENSION_STATE_ERROR);
}

void
shell_lua_extension_mark_uninstalled (ShellLuaExtension *self)
{
  g_return_if_fail (SHELL_IS_LUA_EXTENSION (self));

  teardown_runtime (self, TRUE);
  set_state (self, SHELL_LUA_EXTENSION_STATE_UNINSTALLED);
}

/* ------------------------------------------------------------------ */
/* Ownership                                                          */
/* ------------------------------------------------------------------ */

guint
shell_lua_extension_own (ShellLuaExtension *self,
                         GObject           *object,
                         GDestroyNotify     dispose)
{
  OwnedEntry *entry;

  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), 0);
  g_return_val_if_fail (G_IS_OBJECT (object), 0);

  entry = g_new0 (OwnedEntry, 1);
  entry->id = self->next_owned_id++;
  entry->object = g_object_ref (object);
  entry->func = dispose;
  g_queue_push_tail (&self->owned, entry);

  return entry->id;
}

guint
shell_lua_extension_own_full (ShellLuaExtension *self,
                              gpointer           data,
                              GDestroyNotify     destroy)
{
  OwnedEntry *entry;

  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), 0);

  entry = g_new0 (OwnedEntry, 1);
  entry->id = self->next_owned_id++;
  entry->data = data;
  entry->func = destroy;
  g_queue_push_tail (&self->owned, entry);

  return entry->id;
}

/* Drops ownership without running the dispose/destroy callback: the caller
 * has already cleaned up (or is handing the resource elsewhere). The object
 * reference taken by own() is released. */
void
shell_lua_extension_release (ShellLuaExtension *self,
                             guint              id)
{
  GList *l;

  g_return_if_fail (SHELL_IS_LUA_EXTENSION (self));

  if (id == 0)
    return;

  for (l = self->owned.tail; l != NULL; l = l->prev)
    {
      OwnedEntry *entry = l->data;

      if (entry->id != id)
        continue;

      g_queue_delete_link (&self->owned, l);
      g_clear_object (&entry->object);
      owned_entry_free (entry);
      return;
    }
}

/* ------------------------------------------------------------------ */
/* Serialization                                                      */
/* ------------------------------------------------------------------ */

static GVariant *pack_node (JsonNode *node);

/* Same type mapping as packValue() in extensionUtils.js: numbers become 'd',
 * arrays 'av', objects 'a{sv}', null is dropped. */
static GVariant *
pack_node (JsonNode *node)
{
  switch (json_node_get_node_type (node))
    {
    case JSON_NODE_VALUE:
      switch (json_node_get_value_type (node))
        {
        case G_TYPE_STRING:
          return g_variant_new_string (json_node_get_string (node));
        case G_TYPE_BOOLEAN:
          return g_variant_new_boolean (json_node_get_boolean (node));
        case G_TYPE_INT64:
        case G_TYPE_DOUBLE:
          return g_variant_new_double (json_node_get_double (node));
        default:
          return NULL;
        }

    case JSON_NODE_ARRAY:
      {
        JsonArray *array = json_node_get_array (node);
        GVariantBuilder builder;
        unsigned int i, n = json_array_get_length (array);

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("av"));
        for (i = 0; i < n; i++)
          {
            GVariant *packed = pack_node (json_array_get_element (array, i));

            if (packed != NULL)
              g_variant_builder_add (&builder, "v", packed);
          }
        return g_variant_builder_end (&builder);
      }

    case JSON_NODE_OBJECT:
      {
        JsonObject *object = json_node_get_object (node);
        GVariantBuilder builder;
        GList *members, *l;

        g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
        members = json_object_get_members (object);
        for (l = members; l != NULL; l = l->next)
          {
            GVariant *packed =
              pack_node (json_object_get_member (object, l->data));

            if (packed != NULL)
              g_variant_builder_add (&builder, "{sv}", l->data, packed);
          }
        g_list_free (members);
        return g_variant_builder_end (&builder);
      }

    case JSON_NODE_NULL:
    default:
      return NULL;
    }
}

static void
dict_take (GHashTable *dict,
           const char *key,
           GVariant   *value)
{
  g_hash_table_replace (dict, g_strdup (key), g_variant_ref_sink (value));
}

/* The key names are those of org.gnome.Shell.Extensions' extension info
 * (SERIALIZED_PROPERTIES of extensionUtils.js plus the metadata fields). The
 * numeric fields 'type' and 'state' are doubles, as in the JS implementation,
 * so existing clients (gnome-extensions, the Extensions app) keep working. */
GVariant *
shell_lua_extension_serialize (ShellLuaExtension *self)
{
  g_autoptr (GHashTable) dict = NULL;
  g_autofree char *path = NULL;
  GVariantBuilder builder;
  GVariantBuilder modes;
  GHashTableIter iter;
  gpointer key, value;
  GList *members, *l;
  int i;

  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (self), NULL);

  dict = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                (GDestroyNotify) g_variant_unref);

  /* metadata first, so the fixed fields below win */
  members = json_object_get_members (self->metadata);
  for (l = members; l != NULL; l = l->next)
    {
      GVariant *packed =
        pack_node (json_object_get_member (self->metadata, l->data));

      if (packed != NULL)
        dict_take (dict, l->data, packed);
    }
  g_list_free (members);

  path = g_file_get_path (self->dir);

  g_variant_builder_init (&modes, G_VARIANT_TYPE ("av"));
  for (i = 0; self->session_modes[i] != NULL; i++)
    g_variant_builder_add (&modes, "v",
                           g_variant_new_string (self->session_modes[i]));

  dict_take (dict, "type", g_variant_new_double (self->type));
  dict_take (dict, "state", g_variant_new_double (self->state));
  dict_take (dict, "enabled", g_variant_new_boolean (self->enabled));
  dict_take (dict, "path", g_variant_new_string (path ? path : ""));
  dict_take (dict, "error", g_variant_new_string (self->error ? self->error : ""));
  dict_take (dict, "hasPrefs", g_variant_new_boolean (self->has_prefs));
  dict_take (dict, "hasUpdate", g_variant_new_boolean (FALSE));
  dict_take (dict, "canChange", g_variant_new_boolean (self->can_change));
  dict_take (dict, "sessionModes", g_variant_builder_end (&modes));

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_hash_table_iter_init (&iter, dict);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_variant_builder_add (&builder, "{sv}", key, (GVariant *) value);

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}
