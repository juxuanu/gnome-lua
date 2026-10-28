/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * extension-prefs-schema.c: Loads and validates the declarative preferences
 * of a Lua extension. The format and the validation rules are those of
 * docs/api/prefs.md; the numbers in the comments below are those of its
 * "Validation" list.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

#include <json-glib/json-glib.h>

#include "extension-prefs-schema.h"

#define MAX_PROBLEMS 40
#define MAX_PAGES 8
#define MAX_GROUPS 16
#define MAX_ROWS 64
#define MAX_CHOICES 256
#define MAX_STRING 200
#define MAX_LONG_STRING 500
#define MAX_DEPTH 12
#define MAX_NODES 20000

typedef struct
{
  GPtrArray       *problems;
  GSettingsSchema *schema;
  GHashTable      *bound_keys;   /* key -> path of the row (owned) */
} Context;

/* ------------------------------------------------------------------------ */
/* Problems and paths                                                        */
/* ------------------------------------------------------------------------ */

static void add_problem (Context    *ctx,
                         const char *path,
                         const char *format,
                         ...) G_GNUC_PRINTF (3, 4);

static void
add_problem (Context    *ctx,
             const char *path,
             const char *format,
             ...)
{
  g_autofree char *message = NULL;
  va_list args;

  if (ctx->problems->len >= MAX_PROBLEMS)
    return;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);

  if (ctx->problems->len == MAX_PROBLEMS - 1)
    g_ptr_array_add (ctx->problems, g_strdup ("too many problems, the others are not listed"));
  else if (path != NULL && *path != '\0')
    g_ptr_array_add (ctx->problems, g_strdup_printf ("%s: %s", path, message));
  else
    g_ptr_array_add (ctx->problems, g_steal_pointer (&message));
}

static char *
path_field (const char *parent,
            const char *name)
{
  if (parent == NULL || *parent == '\0')
    return g_strdup (name);

  return g_strdup_printf ("%s.%s", parent, name);
}

/* 1-based, as in the Lua file */
static char *
path_index (const char *parent,
            const char *name,
            guint       index)
{
  g_autofree char *field = path_field (parent, name);

  return g_strdup_printf ("%s[%u]", field, index + 1);
}

/* ------------------------------------------------------------------------ */
/* Lua value to data tree                                                    */
/* ------------------------------------------------------------------------ */

typedef struct
{
  Context    *ctx;
  GHashTable *in_progress;   /* lua_topointer () of the tables being converted */
  guint       n_nodes;
  gboolean    failed;
} Converter;

static JsonNode *convert_value (Converter  *cv,
                                lua_State  *L,
                                int         idx,
                                const char *path,
                                guint       depth);

static char *
convert_key_path (const char *path,
                  lua_State  *L,
                  int         key)
{
  if (lua_type (L, key) == LUA_TSTRING)
    return path_field (path, lua_tostring (L, key));

  return g_strdup_printf ("%s[%lld]", path, (long long) lua_tointeger (L, key));
}

static JsonNode *
convert_table (Converter  *cv,
               lua_State  *L,
               int         idx,
               const char *path,
               guint       depth)
{
  gconstpointer id = lua_topointer (L, idx);
  g_autoptr (GPtrArray) seq = g_ptr_array_new_with_free_func ((GDestroyNotify) json_node_unref);
  g_autoptr (JsonObject) object = json_object_new ();
  gboolean has_string_keys = FALSE;
  gboolean has_int_keys = FALSE;
  lua_Integer max_index = 0;
  lua_Integer n_int_keys = 0;
  JsonNode *result;

  if (depth > MAX_DEPTH)
    {
      add_problem (cv->ctx, path, "nested too deeply");
      cv->failed = TRUE;
      return NULL;
    }

  if (g_hash_table_contains (cv->in_progress, id))
    {
      add_problem (cv->ctx, path, "a table that contains itself cannot be used");
      cv->failed = TRUE;
      return NULL;
    }
  g_hash_table_add (cv->in_progress, (gpointer) id);

  /* First pass: what kind of keys */
  lua_pushnil (L);
  while (lua_next (L, idx) != 0)
    {
      int key_type = lua_type (L, -2);

      if (key_type == LUA_TSTRING)
        {
          has_string_keys = TRUE;
        }
      else if (key_type == LUA_TNUMBER && lua_isinteger (L, -2) &&
               lua_tointeger (L, -2) > 0)
        {
          has_int_keys = TRUE;
          n_int_keys++;
          if (lua_tointeger (L, -2) > max_index)
            max_index = lua_tointeger (L, -2);
        }
      else
        {
          add_problem (cv->ctx, path, "a table key of type %s cannot be used",
                       lua_typename (L, key_type));
          cv->failed = TRUE;
        }
      lua_pop (L, 1);
    }

  if (has_string_keys && has_int_keys)
    {
      add_problem (cv->ctx, path, "a table must be either a list or a set of named fields");
      cv->failed = TRUE;
    }
  else if (has_int_keys && max_index != n_int_keys)
    {
      add_problem (cv->ctx, path, "a list must not have holes");
      cv->failed = TRUE;
    }

  if (cv->failed)
    {
      g_hash_table_remove (cv->in_progress, id);
      return NULL;
    }

  if (has_int_keys)
    {
      lua_Integer i;

      for (i = 1; i <= max_index; i++)
        {
          g_autofree char *child_path = g_strdup_printf ("%s[%lld]", path, (long long) i);
          JsonNode *child;

          lua_rawgeti (L, idx, i);
          child = convert_value (cv, L, lua_gettop (L), child_path, depth + 1);
          lua_pop (L, 1);
          if (child == NULL)
            break;
          g_ptr_array_add (seq, child);
        }
    }
  else
    {
      lua_pushnil (L);
      while (lua_next (L, idx) != 0)
        {
          g_autofree char *child_path = convert_key_path (path, L, lua_gettop (L) - 1);
          JsonNode *child = convert_value (cv, L, lua_gettop (L), child_path, depth + 1);

          if (child == NULL)
            {
              lua_pop (L, 2);   /* key and value: abandon the traversal */
              break;
            }
          json_object_set_member (object, lua_tostring (L, -2), child);
          lua_pop (L, 1);
        }
    }

  g_hash_table_remove (cv->in_progress, id);

  if (cv->failed)
    return NULL;

  if (has_string_keys)
    {
      result = json_node_new (JSON_NODE_OBJECT);
      json_node_set_object (result, object);
    }
  else
    {
      JsonArray *array = json_array_sized_new (seq->len);
      guint i;

      for (i = 0; i < seq->len; i++)
        json_array_add_element (array, json_node_copy (g_ptr_array_index (seq, i)));

      result = json_node_new (JSON_NODE_ARRAY);
      json_node_take_array (result, array);
    }

  return result;
}

static JsonNode *
convert_value (Converter  *cv,
               lua_State  *L,
               int         idx,
               const char *path,
               guint       depth)
{
  JsonNode *node;

  idx = lua_absindex (L, idx);

  if (++cv->n_nodes > MAX_NODES)
    {
      add_problem (cv->ctx, path, "the file is too large");
      cv->failed = TRUE;
      return NULL;
    }

  switch (lua_type (L, idx))
    {
    case LUA_TBOOLEAN:
      node = json_node_new (JSON_NODE_VALUE);
      json_node_set_boolean (node, lua_toboolean (L, idx));
      return node;

    case LUA_TNUMBER:
      node = json_node_new (JSON_NODE_VALUE);
      if (lua_isinteger (L, idx))
        {
          json_node_set_int (node, lua_tointeger (L, idx));
        }
      else
        {
          double d = lua_tonumber (L, idx);

          if (isnan (d) || isinf (d))
            {
              json_node_unref (node);
              add_problem (cv->ctx, path, "a number must be finite");
              cv->failed = TRUE;
              return NULL;
            }
          json_node_set_double (node, d);
        }
      return node;

    case LUA_TSTRING:
      {
        size_t length;
        const char *s = lua_tolstring (L, idx, &length);

        if (strlen (s) != length || !g_utf8_validate (s, length, NULL))
          {
            add_problem (cv->ctx, path, "a string must be valid UTF-8 without NUL bytes");
            cv->failed = TRUE;
            return NULL;
          }
        node = json_node_new (JSON_NODE_VALUE);
        json_node_set_string (node, s);
        return node;
      }

    case LUA_TTABLE:
      return convert_table (cv, L, idx, path, depth);

    default:
      add_problem (cv->ctx, path, "a %s value cannot be used, the file must return data only",
                   luaL_typename (L, idx));
      cv->failed = TRUE;
      return NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* Running prefs.lua                                                         */
/* ------------------------------------------------------------------------ */

/* What the sandbox keeps of `shell` for a prefs file (docs/api/prefs.md,
 * "Loading"): everything that is not data about the extension or a way to
 * log and translate is removed. */
static const char strip_shell_script[] =
  "for name in pairs(shell) do\n"
  "  if name ~= 'api_version' and name ~= 'log' and name ~= 'gettext' and\n"
  "     name ~= 'ngettext' and name ~= 'pgettext' then\n"
  "    shell[name] = nil\n"
  "  end\n"
  "end\n";

static JsonNode *
run_prefs_lua (Context           *ctx,
               ShellLuaExtension *extension)
{
  g_autoptr (ShellLuaState) state = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GHashTable) in_progress = NULL;
  Converter cv = { 0, };
  JsonNode *root = NULL;
  lua_State *L;

  file = g_file_get_child (shell_lua_extension_get_dir (extension), "prefs.lua");
  if (!g_file_query_exists (file, NULL))
    {
      add_problem (ctx, NULL, "prefs.lua not found");
      return NULL;
    }

  state = shell_lua_state_new (shell_lua_extension_get_uuid (extension),
                               SHELL_LUA_STATE_SANDBOXED, NULL);
  shell_lua_state_set_owner (state, extension);
  shell_lua_state_add_search_root (state, shell_lua_extension_get_dir (extension));

  if (!shell_lua_state_do_string (state, strip_shell_script, "=(prefs setup)", &error))
    {
      add_problem (ctx, NULL, "prefs.lua: could not set up the sandbox: %s", error->message);
      goto out;
    }

  if (!shell_lua_state_load_file (state, file, &error) ||
      !shell_lua_state_pcall (state, 0, 1, &error))
    {
      add_problem (ctx, "prefs.lua", "%s", error->message);
      goto out;
    }

  L = shell_lua_state_get_L (state);
  if (!lua_istable (L, -1))
    {
      add_problem (ctx, "prefs.lua", "the file must return a table, not a %s",
                   luaL_typename (L, -1));
      lua_pop (L, 1);
      goto out;
    }

  in_progress = g_hash_table_new (g_direct_hash, g_direct_equal);
  cv.ctx = ctx;
  cv.in_progress = in_progress;
  root = convert_value (&cv, L, lua_gettop (L), "", 0);
  lua_pop (L, 1);

out:
  shell_lua_state_set_owner (state, NULL);
  shell_lua_state_destroy (state);
  return root;
}

/* ------------------------------------------------------------------------ */
/* Typed access to the data tree                                             */
/* ------------------------------------------------------------------------ */

/* A table without entries converts to an empty list: it is also an empty set
 * of fields */
static JsonObject *
node_get_object (JsonNode *node)
{
  static JsonObject *empty = NULL;

  if (node == NULL)
    return NULL;
  if (JSON_NODE_HOLDS_OBJECT (node))
    return json_node_get_object (node);
  if (JSON_NODE_HOLDS_ARRAY (node) && json_array_get_length (json_node_get_array (node)) == 0)
    {
      if (empty == NULL)
        empty = json_object_new ();
      return empty;
    }

  return NULL;
}

static JsonArray *
node_get_array (JsonNode *node)
{
  static JsonArray *empty = NULL;

  if (node == NULL)
    return NULL;
  if (JSON_NODE_HOLDS_ARRAY (node))
    return json_node_get_array (node);
  if (JSON_NODE_HOLDS_OBJECT (node) && json_object_get_size (json_node_get_object (node)) == 0)
    {
      if (empty == NULL)
        empty = json_array_new ();
      return empty;
    }

  return NULL;
}

static void
check_fields (Context            *ctx,
              const char         *path,
              JsonObject         *object,
              const char * const *allowed)
{
  g_autoptr (GList) members = json_object_get_members (object);
  GList *l;

  for (l = members; l != NULL; l = l->next)
    {
      if (!g_strv_contains (allowed, l->data))
        {
          g_autofree char *field = path_field (path, l->data);

          add_problem (ctx, field, "unknown field");
        }
    }
}

static gboolean
get_string (Context     *ctx,
            const char  *path,
            JsonObject  *object,
            const char  *name,
            gboolean     required,
            gsize        max_length,
            const char **out)
{
  g_autofree char *field = path_field (path, name);
  JsonNode *node = json_object_get_member (object, name);
  const char *s;

  *out = NULL;

  if (node == NULL)
    {
      if (required)
        add_problem (ctx, field, "required field is missing");
      return !required;
    }

  if (!JSON_NODE_HOLDS_VALUE (node) || json_node_get_value_type (node) != G_TYPE_STRING)
    {
      add_problem (ctx, field, "must be a string");
      return FALSE;
    }

  s = json_node_get_string (node);
  if (*s == '\0')
    {
      add_problem (ctx, field, "must not be empty");
      return FALSE;
    }

  if (strlen (s) > max_length)
    {
      add_problem (ctx, field, "must not be longer than %" G_GSIZE_FORMAT " bytes", max_length);
      return FALSE;
    }

  *out = s;
  return TRUE;
}

static gboolean
get_boolean (Context    *ctx,
             const char *path,
             JsonObject *object,
             const char *name,
             gboolean    default_value,
             gboolean   *out)
{
  g_autofree char *field = path_field (path, name);
  JsonNode *node = json_object_get_member (object, name);

  *out = default_value;
  if (node == NULL)
    return TRUE;

  if (!JSON_NODE_HOLDS_VALUE (node) || json_node_get_value_type (node) != G_TYPE_BOOLEAN)
    {
      add_problem (ctx, field, "must be a boolean");
      return FALSE;
    }

  *out = json_node_get_boolean (node);
  return TRUE;
}

static gboolean
node_get_number (JsonNode *node,
                 double   *out)
{
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
    return FALSE;

  switch (json_node_get_value_type (node))
    {
    case G_TYPE_INT64:
      *out = (double) json_node_get_int (node);
      return TRUE;
    case G_TYPE_DOUBLE:
      *out = json_node_get_double (node);
      return TRUE;
    default:
      return FALSE;
    }
}

static gboolean
is_integral (double d)
{
  return d == floor (d) && fabs (d) <= 9007199254740992.0;
}

/* Present or absent; @present says which. */
static gboolean
get_number (Context    *ctx,
            const char *path,
            JsonObject *object,
            const char *name,
            double     *out,
            gboolean   *present)
{
  g_autofree char *field = path_field (path, name);
  JsonNode *node = json_object_get_member (object, name);

  *present = node != NULL;
  if (node == NULL)
    return TRUE;

  if (!node_get_number (node, out))
    {
      add_problem (ctx, field, "must be a number");
      return FALSE;
    }

  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Keys of the GSettings schema                                              */
/* ------------------------------------------------------------------------ */

typedef struct
{
  GSettingsSchemaKey *key;
  char                type;        /* first character of the type string */
  const char         *type_string;
  gboolean            is_integer;
  gboolean            has_range;   /* a <range> */
  double              range_min;
  double              range_max;
  char              **enum_values; /* nullable: <choices> or <enum> */
  gboolean            is_flags;
} KeyInfo;

static void
key_info_clear (KeyInfo *info)
{
  g_clear_pointer (&info->key, g_settings_schema_key_unref);
  g_clear_pointer (&info->enum_values, g_strfreev);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (KeyInfo, key_info_clear)

static gboolean
variant_to_double (GVariant *value,
                   double   *out)
{
  switch (g_variant_classify (value))
    {
    case G_VARIANT_CLASS_BYTE:
      *out = g_variant_get_byte (value);
      return TRUE;
    case G_VARIANT_CLASS_INT16:
      *out = g_variant_get_int16 (value);
      return TRUE;
    case G_VARIANT_CLASS_UINT16:
      *out = g_variant_get_uint16 (value);
      return TRUE;
    case G_VARIANT_CLASS_INT32:
      *out = g_variant_get_int32 (value);
      return TRUE;
    case G_VARIANT_CLASS_UINT32:
      *out = g_variant_get_uint32 (value);
      return TRUE;
    case G_VARIANT_CLASS_INT64:
      *out = (double) g_variant_get_int64 (value);
      return TRUE;
    case G_VARIANT_CLASS_UINT64:
      *out = (double) g_variant_get_uint64 (value);
      return TRUE;
    case G_VARIANT_CLASS_DOUBLE:
      *out = g_variant_get_double (value);
      return TRUE;
    default:
      return FALSE;
    }
}

/* The limits of the type of a key: what a spin row may span when the schema
 * has no <range> (the bounds are explicit then) */
static void
type_limits (char    type,
             double *lo,
             double *hi)
{
  switch (type)
    {
    case 'y':
      *lo = 0;
      *hi = 255;
      break;
    case 'n':
      *lo = -32768.0;
      *hi = 32767.0;
      break;
    case 'q':
      *lo = 0;
      *hi = 65535.0;
      break;
    case 'i':
      *lo = -2147483648.0;
      *hi = 2147483647.0;
      break;
    case 'u':
      *lo = 0;
      *hi = 4294967295.0;
      break;
    case 'x':
      *lo = -9223372036854775808.0;
      *hi = 9223372036854775807.0;
      break;
    case 't':
      *lo = 0;
      *hi = 18446744073709551615.0;
      break;
    default:
      *lo = -DBL_MAX;
      *hi = DBL_MAX;
      break;
    }
}

static gboolean
key_info_init (KeyInfo         *info,
               GSettingsSchema *schema,
               const char      *name)
{
  const GVariantType *vtype;
  g_autoptr (GVariant) range = NULL;
  g_autoptr (GVariant) detail = NULL;
  const char *kind = NULL;

  memset (info, 0, sizeof (KeyInfo));

  if (!g_settings_schema_has_key (schema, name))
    return FALSE;

  info->key = g_settings_schema_get_key (schema, name);
  vtype = g_settings_schema_key_get_value_type (info->key);
  info->type_string = g_variant_type_peek_string (vtype);
  info->type = info->type_string[0];
  info->is_integer = strchr ("nqiuxty", info->type) != NULL && info->type_string[1] == '\0';

  range = g_settings_schema_key_get_range (info->key);
  g_variant_get (range, "(&sv)", &kind, &detail);

  if (strcmp (kind, "range") == 0)
    {
      g_autoptr (GVariant) lo = g_variant_get_child_value (detail, 0);
      g_autoptr (GVariant) hi = g_variant_get_child_value (detail, 1);

      info->has_range = variant_to_double (lo, &info->range_min) &&
                        variant_to_double (hi, &info->range_max);
    }
  else if (strcmp (kind, "enum") == 0)
    {
      info->enum_values = g_variant_dup_strv (detail, NULL);
    }
  else if (strcmp (kind, "flags") == 0)
    {
      info->is_flags = TRUE;
    }

  return TRUE;
}

/* Converts a value of the data tree to a GVariant of @type; NULL (and a
 * problem) if it does not fit. Floating. */
static GVariant *
node_to_variant (Context    *ctx,
                 const char *path,
                 JsonNode   *node,
                 KeyInfo    *info)
{
  double d;

  if (node == NULL)
    return NULL;

  switch (info->type)
    {
    case 'b':
      if (JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_BOOLEAN)
        return g_variant_new_boolean (json_node_get_boolean (node));
      add_problem (ctx, path, "must be a boolean");
      return NULL;

    case 's':
      if (JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING &&
          info->type_string[1] == '\0')
        return g_variant_new_string (json_node_get_string (node));
      add_problem (ctx, path, "must be a string");
      return NULL;

    case 'd':
      if (node_get_number (node, &d))
        return g_variant_new_double (d);
      add_problem (ctx, path, "must be a number");
      return NULL;

    case 'n':
    case 'q':
    case 'i':
    case 'u':
    case 'x':
    case 't':
    case 'y':
      {
        double lo, hi;

        if (!info->is_integer || !node_get_number (node, &d) || !is_integral (d))
          {
            add_problem (ctx, path, "must be an integer");
            return NULL;
          }

        type_limits (info->type, &lo, &hi);
        if (d < lo || d > hi)
          {
            add_problem (ctx, path, "does not fit the type '%s' of the key", info->type_string);
            return NULL;
          }

        switch (info->type)
          {
          case 'n':
            return g_variant_new_int16 ((gint16) d);
          case 'q':
            return g_variant_new_uint16 ((guint16) d);
          case 'i':
            return g_variant_new_int32 ((gint32) d);
          case 'u':
            return g_variant_new_uint32 ((guint32) d);
          case 'x':
            return g_variant_new_int64 ((gint64) d);
          case 't':
            return g_variant_new_uint64 ((guint64) d);
          default:
            return g_variant_new_byte ((guchar) d);
          }
      }

    default:
      add_problem (ctx, path, "keys of type '%s' cannot be used here", info->type_string);
      return NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* Model                                                                     */
/* ------------------------------------------------------------------------ */

static void
choice_free (ExtPrefsChoice *choice)
{
  g_free (choice->label);
  g_clear_pointer (&choice->value, g_variant_unref);
  g_free (choice);
}

static void
depends_free (ExtPrefsDepends *depends)
{
  if (depends == NULL)
    return;

  g_free (depends->key);
  g_clear_pointer (&depends->value, g_variant_unref);
  g_free (depends);
}

static void
row_free (ExtPrefsRow *row)
{
  g_free (row->key);
  g_free (row->title);
  g_free (row->subtitle);
  depends_free (row->depends_on);
  g_clear_pointer (&row->choices, g_ptr_array_unref);
  g_free (row);
}

static void
group_free (ExtPrefsGroup *group)
{
  g_free (group->title);
  g_free (group->description);
  g_clear_pointer (&group->rows, g_ptr_array_unref);
  g_free (group);
}

static void
page_free (ExtPrefsPage *page)
{
  g_free (page->title);
  g_free (page->icon_name);
  g_clear_pointer (&page->groups, g_ptr_array_unref);
  g_free (page);
}

/**
 * ext_prefs_schema_free:
 * @schema: (nullable): a schema returned by ext_prefs_schema_load ()
 *
 * Frees @schema.
 */
void
ext_prefs_schema_free (ExtPrefsSchema *schema)
{
  if (schema == NULL)
    return;

  g_free (schema->title);
  g_clear_pointer (&schema->pages, g_ptr_array_unref);
  g_clear_object (&schema->settings);
  g_free (schema);
}

/* ------------------------------------------------------------------------ */
/* Validation                                                                */
/* ------------------------------------------------------------------------ */

static const char * const page_fields[] = { "title", "icon_name", "groups", NULL };
static const char * const group_fields[] = { "title", "description", "rows", NULL };
static const char * const switch_fields[] = { "type", "key", "title", "subtitle", "depends_on", NULL };
static const char * const spin_fields[] = { "type", "key", "title", "subtitle", "depends_on",
                                            "min", "max", "step", "digits", NULL };
static const char * const combo_fields[] = { "type", "key", "title", "subtitle", "depends_on",
                                             "choices", NULL };
static const char * const entry_fields[] = { "type", "key", "title", "subtitle", "depends_on",
                                             "max_length", NULL };
static const char * const color_fields[] = { "type", "key", "title", "subtitle", "depends_on",
                                             "alpha", NULL };
static const char * const keybinding_fields[] = { "type", "key", "title", "subtitle", "depends_on",
                                                  "max", "allow_empty", NULL };

static const struct
{
  const char          *name;
  ExtPrefsRowType      type;
  const char * const  *fields;
  const char          *key_types;   /* space separated type strings */
} row_types[] = {
  { "switch",     EXT_PREFS_ROW_SWITCH,     switch_fields,     "b" },
  { "spin",       EXT_PREFS_ROW_SPIN,       spin_fields,       "n q i u x t y d" },
  { "combo",      EXT_PREFS_ROW_COMBO,      combo_fields,      "s n q i u x t y" },
  { "entry",      EXT_PREFS_ROW_ENTRY,      entry_fields,      "s" },
  { "color",      EXT_PREFS_ROW_COLOR,      color_fields,      "s" },
  { "keybinding", EXT_PREFS_ROW_KEYBINDING, keybinding_fields, "as" },
};

static gboolean
key_type_allowed (const char *key_types,
                  const char *type_string)
{
  g_auto (GStrv) types = g_strsplit (key_types, " ", -1);

  return g_strv_contains ((const char * const *) types, type_string);
}

static void
validate_spin (Context     *ctx,
               const char  *path,
               JsonObject  *object,
               KeyInfo     *info,
               ExtPrefsRow *row)
{
  gboolean ok = TRUE;
  gboolean has_min, has_max, has_step, has_digits;
  double min = 0, max = 0, step = 0, digits = 0;
  double type_lo, type_hi;
  g_autofree char *min_path = path_field (path, "min");
  g_autofree char *max_path = path_field (path, "max");
  g_autofree char *step_path = path_field (path, "step");
  g_autofree char *digits_path = path_field (path, "digits");

  type_limits (info->type, &type_lo, &type_hi);

  ok &= get_number (ctx, path, object, "min", &min, &has_min);
  ok &= get_number (ctx, path, object, "max", &max, &has_max);
  ok &= get_number (ctx, path, object, "step", &step, &has_step);
  ok &= get_number (ctx, path, object, "digits", &digits, &has_digits);
  if (!ok)
    return;

  /* 4: min and max default to the <range> and are required without one */
  if (!has_min)
    {
      if (info->has_range)
        min = info->range_min;
      else
        {
          add_problem (ctx, min_path, "required field is missing, the key has no <range>");
          ok = FALSE;
        }
    }
  if (!has_max)
    {
      if (info->has_range)
        max = info->range_max;
      else
        {
          add_problem (ctx, max_path, "required field is missing, the key has no <range>");
          ok = FALSE;
        }
    }
  if (!ok)
    return;

  if (info->is_integer)
    {
      if (!is_integral (min))
        {
          add_problem (ctx, min_path, "must be an integer for the key of type '%s'", info->type_string);
          ok = FALSE;
        }
      if (!is_integral (max))
        {
          add_problem (ctx, max_path, "must be an integer for the key of type '%s'", info->type_string);
          ok = FALSE;
        }
      if (has_step && !is_integral (step))
        {
          add_problem (ctx, step_path, "must be an integer for the key of type '%s'", info->type_string);
          ok = FALSE;
        }
    }

  if (min > max)
    {
      add_problem (ctx, min_path, "must not be greater than max");
      ok = FALSE;
    }

  if (info->has_range)
    {
      if (min < info->range_min || min > info->range_max)
        {
          add_problem (ctx, min_path, "lies outside the range %g..%g of the key",
                       info->range_min, info->range_max);
          ok = FALSE;
        }
      if (max < info->range_min || max > info->range_max)
        {
          add_problem (ctx, max_path, "lies outside the range %g..%g of the key",
                       info->range_min, info->range_max);
          ok = FALSE;
        }
    }
  else
    {
      if (min < type_lo || min > type_hi)
        {
          add_problem (ctx, min_path, "does not fit the type '%s' of the key", info->type_string);
          ok = FALSE;
        }
      if (max < type_lo || max > type_hi)
        {
          add_problem (ctx, max_path, "does not fit the type '%s' of the key", info->type_string);
          ok = FALSE;
        }
    }

  if (!has_step)
    step = info->type == 'd' ? 0.1 : 1;
  if (step <= 0)
    {
      add_problem (ctx, step_path, "must be greater than 0");
      ok = FALSE;
    }

  if (!has_digits)
    digits = info->type == 'd' ? 2 : 0;
  if (!is_integral (digits) || digits < 0 || digits > 10)
    {
      add_problem (ctx, digits_path, "must be an integer between 0 and 10");
      ok = FALSE;
    }

  if (!ok)
    return;

  row->min = min;
  row->max = max;
  row->step = step;
  row->digits = (unsigned int) digits;
}

static void
validate_combo (Context     *ctx,
                const char  *path,
                JsonObject  *object,
                KeyInfo     *info,
                ExtPrefsRow *row)
{
  g_autofree char *choices_path = path_field (path, "choices");
  JsonNode *node = json_object_get_member (object, "choices");
  JsonArray *array;
  guint i, n;

  if (node == NULL)
    {
      add_problem (ctx, choices_path, "required field is missing");
      return;
    }

  array = node_get_array (node);
  if (array == NULL)
    {
      add_problem (ctx, choices_path, "must be a list of {value = ..., label = ...}");
      return;
    }

  n = json_array_get_length (array);
  if (n == 0)
    {
      add_problem (ctx, choices_path, "needs at least one choice");
      return;
    }
  if (n > MAX_CHOICES)
    {
      add_problem (ctx, choices_path, "has more than %d choices", MAX_CHOICES);
      return;
    }

  if (info->is_flags)
    {
      add_problem (ctx, path, "keys that hold flags cannot be shown in a combo row");
      return;
    }

  row->choices = g_ptr_array_new_with_free_func ((GDestroyNotify) choice_free);

  for (i = 0; i < n; i++)
    {
      g_autofree char *choice_path = path_index (path, "choices", i);
      g_autofree char *value_path = path_field (choice_path, "value");
      JsonObject *choice = node_get_object (json_array_get_element (array, i));
      JsonNode *value_node;
      const char *label;
      GVariant *value;
      ExtPrefsChoice *entry;
      guint j;

      if (choice == NULL)
        {
          add_problem (ctx, choice_path, "must be {value = ..., label = ...}");
          continue;
        }

      check_fields (ctx, choice_path, choice, (const char * const[]) { "value", "label", NULL });

      if (!get_string (ctx, choice_path, choice, "label", TRUE, MAX_STRING, &label))
        continue;

      value_node = json_object_get_member (choice, "value");
      if (value_node == NULL)
        {
          add_problem (ctx, value_path, "required field is missing");
          continue;
        }

      value = node_to_variant (ctx, value_path, value_node, info);
      if (value == NULL)
        continue;
      g_variant_ref_sink (value);

      /* 5: unique, and one of the allowed values of an enumerated key */
      for (j = 0; j < row->choices->len; j++)
        {
          ExtPrefsChoice *other = g_ptr_array_index (row->choices, j);

          if (g_variant_equal (other->value, value))
            {
              add_problem (ctx, value_path, "is already used by choices[%u]", j + 1);
              g_variant_unref (value);
              value = NULL;
              break;
            }
        }
      if (value == NULL)
        continue;

      if (info->enum_values != NULL &&
          !g_strv_contains ((const char * const *) info->enum_values, g_variant_get_string (value, NULL)))
        {
          g_autofree char *allowed = g_strjoinv (", ", info->enum_values);

          add_problem (ctx, value_path, "is not one of the values of the key (%s)", allowed);
          g_variant_unref (value);
          continue;
        }

      entry = g_new0 (ExtPrefsChoice, 1);
      entry->label = g_strdup (label);
      entry->value = value;
      g_ptr_array_add (row->choices, entry);
    }
}

/* 7 */
static ExtPrefsDepends *
validate_depends_on (Context    *ctx,
                     const char *path,
                     JsonObject *object)
{
  g_autofree char *field = path_field (path, "depends_on");
  JsonNode *node = json_object_get_member (object, "depends_on");
  g_auto (KeyInfo) info = { 0, };
  ExtPrefsDepends *depends;
  const char *key_name = NULL;
  GVariant *value = NULL;

  if (node == NULL)
    return NULL;

  if (JSON_NODE_HOLDS_VALUE (node))
    {
      const char *s;
      gboolean negate = FALSE;

      if (json_node_get_value_type (node) != G_TYPE_STRING)
        {
          add_problem (ctx, field, "must be a key name or {key = ..., value = ...}");
          return NULL;
        }

      s = json_node_get_string (node);
      if (*s == '!')
        {
          negate = TRUE;
          s++;
        }
      if (*s == '\0' || strlen (s) > MAX_STRING)
        {
          add_problem (ctx, field, "is not a key name");
          return NULL;
        }

      if (!key_info_init (&info, ctx->schema, s))
        {
          add_problem (ctx, field, "the schema has no key '%s'", s);
          return NULL;
        }
      if (info.type != 'b' || info.type_string[1] != '\0')
        {
          add_problem (ctx, field, "the key '%s' is not a boolean", s);
          return NULL;
        }

      key_name = s;
      value = g_variant_ref_sink (g_variant_new_boolean (!negate));
    }
  else
    {
      JsonObject *table = node_get_object (node);
      g_autofree char *value_path = path_field (field, "value");
      JsonNode *value_node;
      const char *s;

      if (table == NULL)
        {
          add_problem (ctx, field, "must be a key name or {key = ..., value = ...}");
          return NULL;
        }

      check_fields (ctx, field, table, (const char * const[]) { "key", "value", NULL });

      if (!get_string (ctx, field, table, "key", TRUE, MAX_STRING, &s))
        return NULL;

      if (!key_info_init (&info, ctx->schema, s))
        {
          g_autofree char *key_path = path_field (field, "key");

          add_problem (ctx, key_path, "the schema has no key '%s'", s);
          return NULL;
        }

      value_node = json_object_get_member (table, "value");
      if (value_node == NULL)
        {
          add_problem (ctx, value_path, "required field is missing");
          return NULL;
        }

      value = node_to_variant (ctx, value_path, value_node, &info);
      if (value == NULL)
        return NULL;
      g_variant_ref_sink (value);
      key_name = s;
    }

  depends = g_new0 (ExtPrefsDepends, 1);
  depends->key = g_strdup (key_name);
  depends->value = value;
  return depends;
}

static ExtPrefsRow *
validate_row (Context    *ctx,
              const char *path,
              JsonNode   *node)
{
  JsonObject *object = node_get_object (node);
  g_auto (KeyInfo) info = { 0, };
  g_autofree char *type_path = path_field (path, "type");
  g_autofree char *key_path = path_field (path, "key");
  const char *type_name, *key_name, *title, *subtitle;
  ExtPrefsRow *row;
  gboolean ok = TRUE;
  gsize i, type_index = G_N_ELEMENTS (row_types);

  if (object == NULL)
    {
      add_problem (ctx, path, "must be a table");
      return NULL;
    }

  if (!get_string (ctx, path, object, "type", TRUE, MAX_STRING, &type_name))
    return NULL;

  for (i = 0; i < G_N_ELEMENTS (row_types); i++)
    {
      if (strcmp (row_types[i].name, type_name) == 0)
        type_index = i;
    }
  if (type_index == G_N_ELEMENTS (row_types))
    {
      add_problem (ctx, type_path,
                   "unknown row type '%s', expected switch, spin, combo, entry, color or keybinding",
                   type_name);
      return NULL;
    }

  check_fields (ctx, path, object, row_types[type_index].fields);

  ok &= get_string (ctx, path, object, "key", TRUE, MAX_STRING, &key_name);
  ok &= get_string (ctx, path, object, "title", TRUE, MAX_STRING, &title);
  ok &= get_string (ctx, path, object, "subtitle", FALSE, MAX_LONG_STRING, &subtitle);

  row = g_new0 (ExtPrefsRow, 1);
  row->type = row_types[type_index].type;
  row->title = g_strdup (title);
  row->subtitle = g_strdup (subtitle);
  row->depends_on = validate_depends_on (ctx, path, object);
  if (json_object_has_member (object, "depends_on") && row->depends_on == NULL)
    ok = FALSE;

  if (key_name == NULL)
    goto out;

  /* 3: the key exists, has a type the row can show, and is bound once */
  if (!key_info_init (&info, ctx->schema, key_name))
    {
      add_problem (ctx, key_path, "the schema has no key '%s'", key_name);
      ok = FALSE;
      goto out;
    }

  if (!key_type_allowed (row_types[type_index].key_types, info.type_string))
    {
      add_problem (ctx, key_path, "a %s row cannot show the key '%s' of type '%s'",
                   type_name, key_name, info.type_string);
      ok = FALSE;
      goto out;
    }

  if (g_hash_table_contains (ctx->bound_keys, key_name))
    {
      add_problem (ctx, key_path, "the key '%s' is already bound by %s", key_name,
                   (const char *) g_hash_table_lookup (ctx->bound_keys, key_name));
      ok = FALSE;
      goto out;
    }
  g_hash_table_insert (ctx->bound_keys, g_strdup (key_name), g_strdup (path));
  row->key = g_strdup (key_name);

  switch (row->type)
    {
    case EXT_PREFS_ROW_SWITCH:
      break;

    case EXT_PREFS_ROW_SPIN:
      validate_spin (ctx, path, object, &info, row);
      /* validate_spin () leaves step at 0 when it found a problem */
      if (row->step <= 0)
        ok = FALSE;
      break;

    case EXT_PREFS_ROW_COMBO:
      validate_combo (ctx, path, object, &info, row);
      if (row->choices == NULL || row->choices->len == 0)
        ok = FALSE;
      break;

    case EXT_PREFS_ROW_ENTRY:
      {
        g_autofree char *max_length_path = path_field (path, "max_length");
        double max_length;
        gboolean present;

        if (!get_number (ctx, path, object, "max_length", &max_length, &present))
          {
            ok = FALSE;
          }
        else if (present)
          {
            if (!is_integral (max_length) || max_length < 1 || max_length > 65535)
              {
                add_problem (ctx, max_length_path, "must be an integer between 1 and 65535");
                ok = FALSE;
              }
            else
              {
                row->max_length = (unsigned int) max_length;
              }
          }
        break;
      }

    case EXT_PREFS_ROW_COLOR:
      if (!get_boolean (ctx, path, object, "alpha", FALSE, &row->alpha))
        ok = FALSE;
      break;

    case EXT_PREFS_ROW_KEYBINDING:
      {
        g_autofree char *max_path = path_field (path, "max");
        double max_accels;
        gboolean present;

        row->max_accels = 1;
        if (!get_boolean (ctx, path, object, "allow_empty", TRUE, &row->allow_empty))
          ok = FALSE;

        /* 6 */
        if (!get_number (ctx, path, object, "max", &max_accels, &present))
          {
            ok = FALSE;
          }
        else if (present)
          {
            if (!is_integral (max_accels) || max_accels < 1 || max_accels > 4)
              {
                add_problem (ctx, max_path, "must be an integer between 1 and 4");
                ok = FALSE;
              }
            else
              {
                row->max_accels = (unsigned int) max_accels;
              }
          }
        break;
      }

    default:
      g_assert_not_reached ();
    }

out:
  if (!ok)
    {
      row_free (row);
      return NULL;
    }

  return row;
}

static ExtPrefsGroup *
validate_group (Context    *ctx,
                const char *path,
                JsonNode   *node)
{
  JsonObject *object = node_get_object (node);
  g_autofree char *rows_path = path_field (path, "rows");
  const char *title, *description;
  JsonArray *rows;
  ExtPrefsGroup *group;
  gboolean ok = TRUE;
  guint i, n;

  if (object == NULL)
    {
      add_problem (ctx, path, "must be a table");
      return NULL;
    }

  check_fields (ctx, path, object, group_fields);

  ok &= get_string (ctx, path, object, "title", FALSE, MAX_STRING, &title);
  ok &= get_string (ctx, path, object, "description", FALSE, MAX_LONG_STRING, &description);

  if (json_object_get_member (object, "rows") == NULL)
    {
      add_problem (ctx, rows_path, "required field is missing");
      return NULL;
    }

  rows = node_get_array (json_object_get_member (object, "rows"));
  if (rows == NULL)
    {
      add_problem (ctx, rows_path, "must be a list of rows");
      return NULL;
    }

  n = json_array_get_length (rows);
  if (n == 0)
    {
      add_problem (ctx, rows_path, "needs at least one row");
      return NULL;
    }
  if (n > MAX_ROWS)
    {
      add_problem (ctx, rows_path, "has more than %d rows", MAX_ROWS);
      return NULL;
    }

  group = g_new0 (ExtPrefsGroup, 1);
  group->title = g_strdup (title);
  group->description = g_strdup (description);
  group->rows = g_ptr_array_new_with_free_func ((GDestroyNotify) row_free);

  for (i = 0; i < n; i++)
    {
      g_autofree char *row_path = path_index (path, "rows", i);
      ExtPrefsRow *row = validate_row (ctx, row_path, json_array_get_element (rows, i));

      if (row == NULL)
        ok = FALSE;
      else
        g_ptr_array_add (group->rows, row);
    }

  if (!ok)
    {
      group_free (group);
      return NULL;
    }

  return group;
}

static GPtrArray *
validate_groups (Context    *ctx,
                 const char *path,
                 JsonNode   *node)
{
  g_autofree char *field = path_field (path, "groups");
  g_autoptr (GPtrArray) groups = NULL;
  JsonArray *array = node_get_array (node);
  gboolean ok = TRUE;
  guint i, n;

  if (array == NULL)
    {
      add_problem (ctx, field, "must be a list of groups");
      return NULL;
    }

  n = json_array_get_length (array);
  if (n == 0)
    {
      add_problem (ctx, field, "needs at least one group");
      return NULL;
    }
  if (n > MAX_GROUPS)
    {
      add_problem (ctx, field, "has more than %d groups", MAX_GROUPS);
      return NULL;
    }

  groups = g_ptr_array_new_with_free_func ((GDestroyNotify) group_free);
  for (i = 0; i < n; i++)
    {
      g_autofree char *group_path = path_index (path, "groups", i);
      ExtPrefsGroup *group = validate_group (ctx, group_path, json_array_get_element (array, i));

      if (group == NULL)
        ok = FALSE;
      else
        g_ptr_array_add (groups, group);
    }

  return ok ? g_steal_pointer (&groups) : NULL;
}

static ExtPrefsSchema *
validate_root (Context   *ctx,
               JsonNode  *root)
{
  JsonObject *object = node_get_object (root);
  g_autoptr (GPtrArray) pages = NULL;
  const char *title;
  JsonNode *pages_node, *groups_node;
  gboolean ok = TRUE;
  ExtPrefsSchema *schema;

  if (object == NULL)
    {
      add_problem (ctx, NULL, "the file must return a table of fields, not a list");
      return NULL;
    }

  check_fields (ctx, NULL, object, (const char * const[]) { "title", "pages", "groups", NULL });
  ok &= get_string (ctx, NULL, object, "title", FALSE, MAX_STRING, &title);

  pages_node = json_object_get_member (object, "pages");
  groups_node = json_object_get_member (object, "groups");
  if ((pages_node == NULL) == (groups_node == NULL))
    {
      add_problem (ctx, NULL, "exactly one of 'pages' and 'groups' is required");
      return NULL;
    }

  pages = g_ptr_array_new_with_free_func ((GDestroyNotify) page_free);

  if (groups_node != NULL)
    {
      GPtrArray *groups = validate_groups (ctx, NULL, groups_node);
      ExtPrefsPage *page;

      if (groups == NULL)
        return NULL;

      page = g_new0 (ExtPrefsPage, 1);
      page->groups = groups;
      g_ptr_array_add (pages, page);
    }
  else
    {
      JsonArray *array = node_get_array (pages_node);
      guint i, n;

      if (array == NULL)
        {
          add_problem (ctx, "pages", "must be a list of pages");
          return NULL;
        }

      n = json_array_get_length (array);
      if (n == 0)
        {
          add_problem (ctx, "pages", "needs at least one page");
          return NULL;
        }
      if (n > MAX_PAGES)
        {
          add_problem (ctx, "pages", "has more than %d pages", MAX_PAGES);
          return NULL;
        }

      for (i = 0; i < n; i++)
        {
          g_autofree char *page_path = path_index (NULL, "pages", i);
          JsonObject *page_object = node_get_object (json_array_get_element (array, i));
          const char *page_title, *icon_name;
          GPtrArray *groups;
          ExtPrefsPage *page;
          gboolean page_ok = TRUE;

          if (page_object == NULL)
            {
              add_problem (ctx, page_path, "must be a table");
              ok = FALSE;
              continue;
            }

          check_fields (ctx, page_path, page_object, page_fields);
          page_ok &= get_string (ctx, page_path, page_object, "title", n > 1, MAX_STRING, &page_title);
          page_ok &= get_string (ctx, page_path, page_object, "icon_name", FALSE, MAX_STRING, &icon_name);

          if (json_object_get_member (page_object, "groups") == NULL)
            {
              g_autofree char *groups_path = path_field (page_path, "groups");

              add_problem (ctx, groups_path, "required field is missing");
              ok = FALSE;
              continue;
            }

          groups = validate_groups (ctx, page_path, json_object_get_member (page_object, "groups"));
          if (groups == NULL || !page_ok)
            {
              g_clear_pointer (&groups, g_ptr_array_unref);
              ok = FALSE;
              continue;
            }

          page = g_new0 (ExtPrefsPage, 1);
          page->title = g_strdup (page_title);
          page->icon_name = g_strdup (icon_name);
          page->groups = groups;
          g_ptr_array_add (pages, page);
        }
    }

  if (!ok)
    return NULL;

  schema = g_new0 (ExtPrefsSchema, 1);
  schema->title = g_strdup (title);
  schema->pages = g_steal_pointer (&pages);
  return schema;
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                               */
/* ------------------------------------------------------------------------ */

/* 2: the schema of the extension, the same way
 * shell_lua_extension_get_settings () finds it, but with a message instead of
 * a warning */
static GSettings *
open_extension_settings (Context           *ctx,
                         ShellLuaExtension *extension)
{
  JsonObject *metadata = shell_lua_extension_get_metadata (extension);
  g_autoptr (GSettingsSchemaSource) source = NULL;
  g_autoptr (GSettingsSchema) schema = NULL;
  g_autoptr (GFile) schema_dir = NULL;
  g_autoptr (GError) error = NULL;
  GSettingsSchemaSource *default_source = g_settings_schema_source_get_default ();
  const char *schema_id = NULL;

  if (json_object_has_member (metadata, "settings-schema"))
    schema_id = json_object_get_string_member (metadata, "settings-schema");

  if (schema_id == NULL || *schema_id == '\0')
    {
      add_problem (ctx, "metadata.json", "prefs.lua needs a \"settings-schema\"");
      return NULL;
    }

  schema_dir = g_file_get_child (shell_lua_extension_get_dir (extension), "schemas");
  if (g_file_query_exists (schema_dir, NULL))
    {
      g_autofree char *path = g_file_get_path (schema_dir);

      source = g_settings_schema_source_new_from_directory (path, default_source, FALSE, &error);
      if (source == NULL)
        {
          add_problem (ctx, "schemas", "cannot load the compiled schemas: %s", error->message);
          return NULL;
        }
    }
  else if (default_source != NULL)
    {
      source = g_settings_schema_source_ref (default_source);
    }

  if (source != NULL)
    schema = g_settings_schema_source_lookup (source, schema_id, TRUE);
  if (schema == NULL)
    {
      add_problem (ctx, "schemas", "the schema '%s' is not installed (is it compiled into schemas/?)",
                   schema_id);
      return NULL;
    }

  ctx->schema = g_settings_schema_ref (schema);
  return g_settings_new_full (schema, NULL, NULL);
}

/**
 * ext_prefs_schema_load:
 * @extension: the extension, as the Lua extension manager describes it
 * @problems: (out) (transfer full) (optional): a %NULL-terminated array of
 *   messages, set when the function returns %NULL
 *
 * Runs `prefs.lua` of @extension and validates what it returns (the list in
 * docs/api/prefs.md). Every problem is collected, with the path of the field
 * in the file, such as `pages[2].groups[1].rows[3].key: ...`.
 *
 * Returns: (transfer full) (nullable): the preferences, or %NULL if the file
 *   is missing or invalid
 */
ExtPrefsSchema *
ext_prefs_schema_load (ShellLuaExtension  *extension,
                       char             ***problems)
{
  Context ctx = { 0, };
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (JsonNode) root = NULL;
  ExtPrefsSchema *schema = NULL;

  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (extension), NULL);

  ctx.problems = g_ptr_array_new_with_free_func (g_free);
  ctx.bound_keys = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  settings = open_extension_settings (&ctx, extension);
  if (settings != NULL)
    root = run_prefs_lua (&ctx, extension);

  if (root != NULL && ctx.problems->len == 0)
    schema = validate_root (&ctx, root);

  if (schema != NULL && ctx.problems->len == 0)
    {
      schema->settings = g_steal_pointer (&settings);
    }
  else
    {
      ext_prefs_schema_free (schema);
      schema = NULL;

      if (ctx.problems->len == 0)
        add_problem (&ctx, NULL, "the preferences are not valid");
    }

  if (schema == NULL && problems != NULL)
    {
      g_ptr_array_add (ctx.problems, NULL);
      *problems = (char **) g_ptr_array_free (g_steal_pointer (&ctx.problems), FALSE);
    }

  g_clear_pointer (&ctx.problems, g_ptr_array_unref);
  g_clear_pointer (&ctx.bound_keys, g_hash_table_unref);
  g_clear_pointer (&ctx.schema, g_settings_schema_unref);

  return schema;
}
