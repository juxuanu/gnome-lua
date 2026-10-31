/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.log, shell.extension, shell.timer, shell.settings, shell.gettext
 * (docs/api/core.md).
 *
 * Notes for maintainers:
 *  - Everything C hands back into Lua from a GLib callback goes through
 *    shell_lua_api_call_protected(): an error raised outside a protected call
 *    would hit the state's panic handler.
 *  - Objects created for Lua (timers, connections, opened settings) are
 *    owned by the extension (shell_lua_extension_own) or, for states without
 *    one, by the state's teardown list. Their handles are weak.
 *  - The owner of a state is set after shell_lua_state_new() has populated
 *    the API, so anything that depends on it (shell.extension, the gettext
 *    domain) is resolved lazily.
 */

#include <libintl.h>
#include <string.h>
#include <stdlib.h>

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "shell-lua-api-private.h"
#include "shell-lua-connection-private.h"
#include "shell-lua-extension.h"

static void __attribute__ ((constructor))
shell_lua_api_core_init (void)
{
  shell_lua_api_register ("core", shell_lua_api_core_register);
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

#define check_fields shell_lua_check_fields

static ShellLuaExtension *
extension_of (ShellLuaState *state)
{
  gpointer owner = shell_lua_state_get_owner (state);

  return SHELL_IS_LUA_EXTENSION (owner) ? SHELL_LUA_EXTENSION (owner) : NULL;
}

/* ------------------------------------------------------------------------ */
/* shell.log                                                                 */
/* ------------------------------------------------------------------------ */

/* Writes one message of an extension to the GLib log, domain "shell-lua/<id>".
 *
 * G_DEBUG=fatal-warnings (CI, developers) makes GLib abort on any warning or
 * critical, in every domain. What an extension logs on purpose must not take
 * the shell down, so for those two levels the global fatal mask is lowered
 * for the duration of the call; the log of the shell itself stays fatal (the
 * window is one call on the main thread). g_log () rather than
 * g_log_structured (): the message must reach the handlers of
 * g_log_set_handler () and g_test_expect_message (), which the structured
 * writer path bypasses; the default handler turns it into a structured entry
 * (MESSAGE, GLIB_DOMAIN, PRIORITY) for the journal anyway. */
static void
write_extension_log (const char     *id,
                     GLogLevelFlags  level,
                     const char     *message)
{
  g_autofree char *domain = g_strconcat ("shell-lua/", id, NULL);
  GLogLevelFlags fatal = 0;
  gboolean relax = (level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL)) != 0;

  if (relax)
    fatal = g_log_set_always_fatal (G_LOG_FATAL_MASK);

  g_log (domain, level, "%s", message);

  if (relax)
    g_log_set_always_fatal (fatal);
}

/* upvalues: 1 = GLogLevelFlags, 2 = string.format (or nil),
 *           3 = number of leading arguments to drop (1 for the __call form),
 *           4 = true for shell.log.fatal: raise the message after logging it */
static int
l_log (lua_State *L)
{
  GLogLevelFlags level = (GLogLevelFlags) lua_tointeger (L, lua_upvalueindex (1));
  ShellLuaState *state = shell_lua_state_from_L (L);
  const char *message;
  int n;

  if (lua_tointeger (L, lua_upvalueindex (3)) > 0)
    {
      luaL_checkany (L, 1);
      lua_remove (L, 1);
    }

  luaL_checktype (L, 1, LUA_TSTRING);
  n = lua_gettop (L);

  if (!lua_isnil (L, lua_upvalueindex (2)))
    {
      lua_pushvalue (L, lua_upvalueindex (2));
      lua_insert (L, 1);
      lua_call (L, n, 1);
    }
  else
    {
      lua_settop (L, 1);
    }

  message = luaL_checkstring (L, -1);
  write_extension_log (state != NULL ? shell_lua_state_get_id (state) : "?", level, message);

  if (lua_toboolean (L, lua_upvalueindex (4)))
    {
      lua_settop (L, 1);
      return lua_error (L);   /* the formatted message, as is */
    }
  return 0;
}

static void
push_log_function (lua_State     *L,
                   GLogLevelFlags level,
                   int            format_idx,
                   int            skip,
                   gboolean       raise)
{
  lua_pushinteger (L, level);
  lua_pushvalue (L, format_idx);
  lua_pushinteger (L, skip);
  lua_pushboolean (L, raise);
  lua_pushcclosure (L, l_log, 4);
}

/* Leaves string.format of the sandbox's own `string` table (or nil) on the
 * stack. Captured at registration so that the extension cannot redirect or
 * break logging by replacing it later. */
static void
push_format_function (lua_State *L)
{
  lua_pushglobaltable (L);
  if (lua_getfield (L, -1, "string") == LUA_TTABLE)
    {
      lua_getfield (L, -1, "format");
      if (!lua_isfunction (L, -1))
        {
          lua_pop (L, 1);
          lua_pushnil (L);
        }
      lua_replace (L, -3);   /* globals -> format */
      lua_pop (L, 1);        /* string */
    }
  else
    {
      lua_pop (L, 2);
      lua_pushnil (L);
    }
}

/* Pushes the shell.log table. */
static void
push_log_table (lua_State *L)
{
  int format, log;

  push_format_function (L);
  format = lua_gettop (L);

  lua_newtable (L);
  log = lua_gettop (L);

  push_log_function (L, G_LOG_LEVEL_DEBUG, format, 0, FALSE);
  lua_setfield (L, log, "debug");
  push_log_function (L, G_LOG_LEVEL_INFO, format, 0, FALSE);
  lua_setfield (L, log, "info");
  push_log_function (L, G_LOG_LEVEL_WARNING, format, 0, FALSE);
  lua_setfield (L, log, "warning");
  /* G_LOG_LEVEL_ERROR aborts in GLib: "error" maps to CRITICAL (which does
   * not take the shell down either, see write_extension_log ()). */
  push_log_function (L, G_LOG_LEVEL_CRITICAL, format, 0, FALSE);
  lua_setfield (L, log, "error");
  /* Since 1.1: like error, then raises the message */
  push_log_function (L, G_LOG_LEVEL_CRITICAL, format, 0, TRUE);
  lua_setfield (L, log, "fatal");

  /* shell.log(fmt, ...) == shell.log.info(fmt, ...) */
  lua_newtable (L);
  push_log_function (L, G_LOG_LEVEL_INFO, format, 1, FALSE);
  lua_setfield (L, -2, "__call");
  lua_pushboolean (L, 0);
  lua_setfield (L, -2, "__metatable");
  lua_setmetatable (L, log);

  lua_remove (L, format);   /* leaves the log table on top */
}

/* ------------------------------------------------------------------------ */
/* shell.extension                                                           */
/* ------------------------------------------------------------------------ */

#define MAX_JSON_DEPTH 32

static int
ro_newindex (lua_State *L)
{
  return luaL_error (L, "attempt to modify a read-only table");
}

static int
ro_len (lua_State *L)
{
  lua_pushinteger (L, (lua_Integer) lua_rawlen (L, lua_upvalueindex (1)));
  return 1;
}

static int
ro_next (lua_State *L)
{
  lua_settop (L, 2);
  if (lua_next (L, 1))
    return 2;
  lua_pushnil (L);
  return 1;
}

static int
ro_pairs (lua_State *L)
{
  lua_pushcfunction (L, ro_next);
  lua_pushvalue (L, lua_upvalueindex (1));
  lua_pushnil (L);
  return 3;
}

/* Replace the table on top of the stack by a read-only proxy of it. */
static void
make_readonly (lua_State *L)
{
  luaL_checkstack (L, 5, "make_readonly");

  lua_newtable (L);                 /* real, proxy */
  lua_newtable (L);                 /* real, proxy, mt */
  lua_pushvalue (L, -3);
  lua_setfield (L, -2, "__index");
  lua_pushvalue (L, -3);
  lua_pushcclosure (L, ro_len, 1);
  lua_setfield (L, -2, "__len");
  lua_pushvalue (L, -3);
  lua_pushcclosure (L, ro_pairs, 1);
  lua_setfield (L, -2, "__pairs");
  lua_pushcfunction (L, ro_newindex);
  lua_setfield (L, -2, "__newindex");
  lua_pushboolean (L, 0);
  lua_setfield (L, -2, "__metatable");
  lua_setmetatable (L, -2);         /* real, proxy */
  lua_remove (L, -2);
}

static void
push_json_node (lua_State *L,
                JsonNode  *node,
                int        depth)
{
  luaL_checkstack (L, 6, "metadata");
  if (depth > MAX_JSON_DEPTH)
    luaL_error (L, "metadata is nested too deeply");

  switch (json_node_get_node_type (node))
    {
    case JSON_NODE_OBJECT:
      {
        JsonObject *object = json_node_get_object (node);
        g_autoptr(GList) members = json_object_get_members (object);
        GList *l;

        lua_createtable (L, 0, json_object_get_size (object));
        for (l = members; l != NULL; l = l->next)
          {
            JsonNode *member = json_object_get_member (object, l->data);

            if (JSON_NODE_HOLDS_NULL (member))
              continue;
            push_json_node (L, member, depth + 1);
            lua_setfield (L, -2, l->data);
          }
        make_readonly (L);
        break;
      }
    case JSON_NODE_ARRAY:
      {
        JsonArray *array = json_node_get_array (node);
        guint i, n = json_array_get_length (array);

        lua_createtable (L, n, 0);
        for (i = 0; i < n; i++)
          {
            push_json_node (L, json_array_get_element (array, i), depth + 1);
            lua_rawseti (L, -2, i + 1);
          }
        make_readonly (L);
        break;
      }
    case JSON_NODE_VALUE:
      switch (json_node_get_value_type (node))
        {
        case G_TYPE_BOOLEAN:
          lua_pushboolean (L, json_node_get_boolean (node));
          break;
        case G_TYPE_INT64:
          lua_pushinteger (L, (lua_Integer) json_node_get_int (node));
          break;
        case G_TYPE_DOUBLE:
          lua_pushnumber (L, json_node_get_double (node));
          break;
        case G_TYPE_STRING:
          lua_pushstring (L, json_node_get_string (node));
          break;
        default:
          lua_pushnil (L);
          break;
        }
      break;
    case JSON_NODE_NULL:
    default:
      lua_pushnil (L);
      break;
    }
}

static void
push_extension_table (lua_State        *L,
                      ShellLuaExtension *extension)
{
  JsonObject *metadata = shell_lua_extension_get_metadata (extension);
  const char * const *capabilities = shell_lua_extension_get_capabilities (extension);
  g_autofree char *path = g_file_get_path (shell_lua_extension_get_dir (extension));
  JsonNode *node;
  int i;

  luaL_checkstack (L, 6, "shell.extension");

  lua_createtable (L, 0, 4);

  lua_pushstring (L, shell_lua_extension_get_uuid (extension));
  lua_setfield (L, -2, "uuid");

  lua_pushstring (L, path != NULL ? path : "");
  lua_setfield (L, -2, "dir");

  node = json_node_new (JSON_NODE_OBJECT);
  json_node_set_object (node, metadata);
  push_json_node (L, node, 0);
  json_node_unref (node);
  lua_setfield (L, -2, "metadata");

  lua_newtable (L);
  for (i = 0; capabilities != NULL && capabilities[i] != NULL; i++)
    {
      lua_pushstring (L, capabilities[i]);
      lua_rawseti (L, -2, i + 1);
    }
  make_readonly (L);
  lua_setfield (L, -2, "capabilities");

  /* data_dir (1.1): resolved on first read, shell-lua-api-fs.c */
  shell_lua_fs_install_data_dir (L);

  make_readonly (L);
}

/* __index of the shell table: builds shell.extension on first access. */
static int
shell_table_index (lua_State *L)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  ShellLuaExtension *extension;

  if (lua_type (L, 2) != LUA_TSTRING || strcmp (lua_tostring (L, 2), "extension") != 0 ||
      state == NULL)
    {
      lua_pushnil (L);
      return 1;
    }

  extension = extension_of (state);
  if (extension == NULL)
    {
      lua_pushnil (L);
      return 1;
    }

  push_extension_table (L, extension);
  lua_pushliteral (L, "extension");
  lua_pushvalue (L, -2);
  lua_rawset (L, 1);
  return 1;
}

/* ------------------------------------------------------------------------ */
/* shell.gettext                                                             */
/* ------------------------------------------------------------------------ */

static char domain_key;

/* The gettext domain of the running state, bound to <ext dir>/locale on first
 * use; NULL (default domain) for states without an extension. The string is
 * kept alive in the registry. */
static const char *
current_domain (lua_State *L)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  ShellLuaExtension *extension;
  const char *domain = NULL;
  JsonNode *node;

  switch (lua_rawgetp (L, LUA_REGISTRYINDEX, &domain_key))
    {
    case LUA_TSTRING:
      domain = lua_tostring (L, -1);
      lua_pop (L, 1);
      return domain;
    case LUA_TBOOLEAN:
      lua_pop (L, 1);
      return NULL;
    default:
      lua_pop (L, 1);
      break;
    }

  extension = state != NULL ? extension_of (state) : NULL;
  if (extension == NULL)
    {
      lua_pushboolean (L, 0);
      lua_rawsetp (L, LUA_REGISTRYINDEX, &domain_key);
      return NULL;
    }

  domain = shell_lua_extension_get_uuid (extension);
  node = json_object_get_member (shell_lua_extension_get_metadata (extension), "gettext-domain");
  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING &&
      json_node_get_string (node)[0] != '\0')
    domain = json_node_get_string (node);

  {
    g_autofree char *dir = g_file_get_path (shell_lua_extension_get_dir (extension));
    g_autofree char *locale_dir = dir != NULL ? g_build_filename (dir, "locale", NULL) : NULL;

    if (locale_dir != NULL)
      {
        bindtextdomain (domain, locale_dir);
        bind_textdomain_codeset (domain, "UTF-8");
      }
  }

  lua_pushstring (L, domain);
  lua_pushvalue (L, -1);
  lua_rawsetp (L, LUA_REGISTRYINDEX, &domain_key);
  domain = lua_tostring (L, -1);
  lua_pop (L, 1);
  return domain;
}

static int
l_gettext (lua_State *L)
{
  const char *msgid = luaL_checkstring (L, 1);

  if (msgid[0] == '\0')
    lua_pushvalue (L, 1);
  else
    lua_pushstring (L, g_dgettext (current_domain (L), msgid));
  return 1;
}

static int
l_ngettext (lua_State *L)
{
  const char *msgid = luaL_checkstring (L, 1);
  const char *plural = luaL_checkstring (L, 2);
  lua_Integer n = luaL_checkinteger (L, 3);

  lua_pushstring (L, g_dngettext (current_domain (L), msgid, plural,
                                  (gulong) (n < 0 ? -n : n)));
  return 1;
}

static int
l_pgettext (lua_State *L)
{
  const char *context = luaL_checkstring (L, 1);
  const char *msgid = luaL_checkstring (L, 2);

  if (msgid[0] == '\0')
    lua_pushvalue (L, 2);
  else
    lua_pushstring (L, g_dpgettext2 (current_domain (L), context, msgid));
  return 1;
}

/* ------------------------------------------------------------------------ */
/* shell.Timer                                                               */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LUA_TIMER (shell_lua_timer_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaTimer, shell_lua_timer, SHELL, LUA_TIMER, GObject)

struct _ShellLuaTimer
{
  GObject parent_instance;

  ShellLuaState   *state;     /* unowned; NULL once the state is gone */
  int              callback_ref;
  guint            source_id; /* 0 = not scheduled */
  gboolean         repeat;
  gboolean         disposing;
  ShellLuaOwnToken own;
};

G_DEFINE_FINAL_TYPE (ShellLuaTimer, shell_lua_timer, G_TYPE_OBJECT)

enum { TIMER_PROP_0, TIMER_PROP_ACTIVE, TIMER_N_PROPS };
static GParamSpec *timer_props[TIMER_N_PROPS];

typedef enum { TIMER_AFTER, TIMER_EVERY, TIMER_IDLE } TimerKind;

static void
timer_release_callback (ShellLuaTimer *self)
{
  if (self->callback_ref != LUA_NOREF)
    {
      int ref = self->callback_ref;

      self->callback_ref = LUA_NOREF;
      if (self->state != NULL && shell_lua_state_is_alive (self->state))
        shell_lua_state_unref (self->state, ref);
    }
}

/* Stop the source (if any) and drop the callback. */
static void
timer_cancel (ShellLuaTimer *self)
{
  if (self->source_id != 0)
    {
      guint id = self->source_id;

      self->source_id = 0;
      g_source_remove (id);
      if (!self->disposing)
        g_object_notify_by_pspec (G_OBJECT (self), timer_props[TIMER_PROP_ACTIVE]);
    }
  timer_release_callback (self);
}

/* Cancel and hand the object back to its owner. Not for use from dispose. */
static void
timer_finish (ShellLuaTimer *self)
{
  g_object_ref (self);
  timer_cancel (self);
  if (!self->disposing)
    shell_lua_api_disown (&self->own);
  g_object_unref (self);
}

static int
timer_trampoline (lua_State *L,
                  gpointer   data)
{
  ShellLuaTimer *self = data;

  shell_lua_state_push_ref (self->state, self->callback_ref);
  shell_lua_state_note_callee (L, -1);
  lua_call (L, 0, 1);
  return 1;
}

static gboolean
timer_fire (gpointer data)
{
  ShellLuaTimer *self = data;
  gboolean keep = FALSE;

  g_object_ref (self);

  if (self->callback_ref != LUA_NOREF && self->state != NULL &&
      shell_lua_state_is_alive (self->state))
    {
      ShellLuaState *state = self->state;

      if (shell_lua_api_call_protected (state, 1, timer_trampoline, self))
        {
          lua_State *L = shell_lua_state_get_L (state);
          gboolean stop = lua_isboolean (L, -1) && !lua_toboolean (L, -1);

          lua_pop (L, 1);
          /* source_id == 0: the callback cancelled the timer itself. A
           * callback that raised never repeats (the error is already
           * counted; repeating would flood the log). */
          keep = self->repeat && !stop && self->source_id != 0;
        }
    }

  if (!keep)
    {
      /* Returning G_SOURCE_REMOVE destroys the source; do not remove it again. */
      if (self->source_id != 0)
        {
          self->source_id = 0;
          g_object_notify_by_pspec (G_OBJECT (self), timer_props[TIMER_PROP_ACTIVE]);
        }
      timer_finish (self);
    }

  g_object_unref (self);
  return keep ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void
timer_state_gone (gpointer data,
                  GObject *where_the_object_was)
{
  ShellLuaTimer *self = data;

  self->state = NULL;
  self->callback_ref = LUA_NOREF;
  self->own.state = NULL;
  timer_cancel (self);
}

static void
shell_lua_timer_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  ShellLuaTimer *self = SHELL_LUA_TIMER (object);

  switch (prop_id)
    {
    case TIMER_PROP_ACTIVE:
      g_value_set_boolean (value, self->source_id != 0);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_lua_timer_dispose (GObject *object)
{
  ShellLuaTimer *self = SHELL_LUA_TIMER (object);

  self->disposing = TRUE;
  timer_cancel (self);
  if (self->state != NULL)
    {
      g_object_weak_unref (G_OBJECT (self->state), timer_state_gone, self);
      self->state = NULL;
    }

  G_OBJECT_CLASS (shell_lua_timer_parent_class)->dispose (object);
}

static void
shell_lua_timer_class_init (ShellLuaTimerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_lua_timer_get_property;
  object_class->dispose = shell_lua_timer_dispose;

  timer_props[TIMER_PROP_ACTIVE] =
    g_param_spec_boolean ("active", NULL, NULL, FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties (object_class, TIMER_N_PROPS, timer_props);
}

static void
shell_lua_timer_init (ShellLuaTimer *self)
{
  self->callback_ref = LUA_NOREF;
}

static int
l_timer_cancel (lua_State *L)
{
  GObject *object = shell_lua_handle_check (L, 1, "shell.Timer");

  timer_finish (SHELL_LUA_TIMER (object));
  return 0;
}

static const luaL_Reg timer_methods[] = {
  { "cancel", l_timer_cancel },
  { NULL, NULL }
};

static const ShellLuaHandleProperty timer_properties[] = {
  { "active", "active", TRUE, FALSE },
  { NULL }
};

static ShellLuaHandleType timer_type = {
  .name = "shell.Timer",
  .methods = timer_methods,
  .properties = timer_properties,
};

static const char * const timer_fields[] = { "ms", "callback", NULL };
static const char * const idle_fields[] = { "callback", NULL };

static int
timer_create_common (lua_State *L,
                     TimerKind  kind)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  ShellLuaTimer *timer;
  lua_Integer ms = 0;
  int ref;

  if (lua_type (L, 1) == LUA_TTABLE)
    {
      check_fields (L, 1, kind == TIMER_IDLE ? idle_fields : timer_fields);
      if (kind != TIMER_IDLE)
        {
          ms = shell_lua_opt_integer (L, 1, "ms", -1);
          if (ms < 0)
            return luaL_error (L, "missing required field 'ms' (a non-negative integer)");
        }
      if (ms > G_MAXINT)
        return luaL_error (L, "field 'ms' is too large");
      if (kind == TIMER_EVERY && ms < 1)
        return luaL_error (L, "field 'ms' must be at least 1 for shell.timer.every");
      ref = shell_lua_opt_callback (L, 1, "callback", TRUE);
    }
  else
    {
      int fn = 1;

      if (kind != TIMER_IDLE)
        {
          ms = luaL_checkinteger (L, 1);
          if (ms < 0 || ms > G_MAXINT)
            return luaL_argerror (L, 1, "milliseconds out of range");
          if (kind == TIMER_EVERY && ms < 1)
            return luaL_argerror (L, 1, "must be at least 1 for shell.timer.every");
          fn = 2;
        }
      luaL_checktype (L, fn, LUA_TFUNCTION);
      lua_pushvalue (L, fn);
      ref = luaL_ref (L, LUA_REGISTRYINDEX);
    }

  timer = g_object_new (SHELL_TYPE_LUA_TIMER, NULL);
  timer->state = state;
  timer->callback_ref = ref;
  timer->repeat = (kind == TIMER_EVERY);
  g_object_weak_ref (G_OBJECT (state), timer_state_gone, timer);

  if (kind == TIMER_IDLE)
    timer->source_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, timer_fire, timer, NULL);
  else
    timer->source_id = g_timeout_add_full (G_PRIORITY_DEFAULT, (guint) ms, timer_fire, timer, NULL);

  shell_lua_api_own (state, G_OBJECT (timer), &timer->own);
  g_object_unref (timer);   /* the owner holds it now */

  shell_lua_handle_push (L, "shell.Timer", G_OBJECT (timer));
  shell_lua_api_anchor (L, -1, G_OBJECT (timer));
  return 1;
}

static int
l_timer_after (lua_State *L)
{
  return timer_create_common (L, TIMER_AFTER);
}

static int
l_timer_every (lua_State *L)
{
  return timer_create_common (L, TIMER_EVERY);
}

static int
l_timer_idle (lua_State *L)
{
  return timer_create_common (L, TIMER_IDLE);
}

static const luaL_Reg timer_funcs[] = {
  { "after", l_timer_after },
  { "every", l_timer_every },
  { "idle",  l_timer_idle },
  { NULL, NULL }
};

/* ------------------------------------------------------------------------ */
/* shell.Settings                                                            */
/* ------------------------------------------------------------------------ */

/* GVariantTypes handed to shell_lua_to_variant() must outlive any Lua error
 * that interrupts the call, so they are interned and never freed. */
static const GVariantType *
intern_type (const GVariantType *type)
{
  static GHashTable *table = NULL;
  char *string;
  GVariantType *interned;

  if (table == NULL)
    table = g_hash_table_new (g_str_hash, g_str_equal);

  string = g_variant_type_dup_string (type);
  interned = g_hash_table_lookup (table, string);
  if (interned == NULL)
    {
      interned = g_variant_type_copy (type);
      g_hash_table_insert (table, string, interned);
    }
  else
    {
      g_free (string);
    }
  return interned;
}

static GSettings *
check_settings (lua_State *L)
{
  return G_SETTINGS (shell_lua_handle_check (L, 1, "shell.Settings"));
}

/* Type of @key, raising a Lua error for an unknown key (GSettings would
 * abort the process). */
static const GVariantType *
settings_key_type (lua_State  *L,
                   GSettings  *settings,
                   const char *key)
{
  GSettingsSchema *schema = NULL;
  const GVariantType *type = NULL;

  g_object_get (settings, "settings-schema", &schema, NULL);
  if (schema != NULL && g_settings_schema_has_key (schema, key))
    {
      GSettingsSchemaKey *schema_key = g_settings_schema_get_key (schema, key);

      type = intern_type (g_settings_schema_key_get_value_type (schema_key));
      g_settings_schema_key_unref (schema_key);
    }

  if (type == NULL)
    {
      lua_pushfstring (L, "no key '%s' in settings schema '%s'", key,
                       schema != NULL ? g_settings_schema_get_id (schema) : "?");
      g_clear_pointer (&schema, g_settings_schema_unref);
      luaL_where (L, 1);
      lua_insert (L, -2);
      lua_concat (L, 2);
      lua_error (L);
    }

  g_settings_schema_unref (schema);
  return type;
}

/* Is the type string of @type one of the space separated @accepted ones? */
static gboolean
type_accepted (const GVariantType *type,
               const char         *accepted)
{
  gsize len = g_variant_type_get_string_length (type);
  const char *string = g_variant_type_peek_string (type);
  const char *p = accepted;

  while (*p != '\0')
    {
      const char *end = strchr (p, ' ');
      gsize n = end != NULL ? (gsize) (end - p) : strlen (p);

      if (n == len && strncmp (p, string, n) == 0)
        return TRUE;
      p += n;
      while (*p == ' ')
        p++;
    }
  return FALSE;
}

static const GVariantType *
typed_key (lua_State  *L,
           GSettings  *settings,
           const char *key,
           const char *accepted,
           const char *what)
{
  const GVariantType *type = settings_key_type (L, settings, key);

  if (!type_accepted (type, accepted))
    luaL_error (L, "key '%s' has type '%s', not %s", key, g_variant_type_peek_string (type), what);
  return type;
}

static int
l_settings_get (lua_State *L)
{
  GSettings *settings = check_settings (L);
  const char *key = luaL_checkstring (L, 2);
  GVariant *value;

  settings_key_type (L, settings, key);
  value = g_settings_get_value (settings, key);
  shell_lua_push_variant (L, value);
  g_variant_unref (value);
  return 1;
}

/* Does the schema's range / choices / enum for @key accept @value? */
static gboolean
settings_value_acceptable (GSettings  *settings,
                           const char *key,
                           GVariant   *value)
{
  GSettingsSchema *schema = NULL;
  GSettingsSchemaKey *schema_key;
  gboolean ok = FALSE;

  g_object_get (settings, "settings-schema", &schema, NULL);
  if (schema == NULL)
    return FALSE;

  schema_key = g_settings_schema_get_key (schema, key);
  ok = g_settings_schema_key_range_check (schema_key, value);
  g_settings_schema_key_unref (schema_key);
  g_settings_schema_unref (schema);
  return ok;
}

static void
settings_store (lua_State          *L,
                GSettings          *settings,
                const char         *key,
                const GVariantType *type,
                int                 idx)
{
  GVariant *value;

  if (!g_settings_is_writable (settings, key))
    luaL_error (L, "key '%s' is not writable", key);

  value = shell_lua_to_variant (L, idx, type);

  /* g_settings_set_value() logs a g_warning for a value outside the key's
   * range, which is fatal under G_DEBUG=fatal-warnings: check beforehand so
   * that Lua code can only ever cause a Lua error. */
  if (!settings_value_acceptable (settings, key, value) ||
      !g_settings_set_value (settings, key, value))
    {
      g_variant_unref (value);
      luaL_error (L, "value rejected for key '%s' (out of range or not allowed)", key);
    }
  g_variant_unref (value);
}

static int
l_settings_set (lua_State *L)
{
  GSettings *settings = check_settings (L);
  const char *key = luaL_checkstring (L, 2);
  const GVariantType *type;

  luaL_checkany (L, 3);
  type = settings_key_type (L, settings, key);
  settings_store (L, settings, key, type, 3);
  return 0;
}

static int
settings_get_typed (lua_State  *L,
                    const char *accepted,
                    const char *what)
{
  GSettings *settings = check_settings (L);
  const char *key = luaL_checkstring (L, 2);
  GVariant *value;

  typed_key (L, settings, key, accepted, what);
  value = g_settings_get_value (settings, key);
  shell_lua_push_variant (L, value);
  g_variant_unref (value);
  return 1;
}

static int
settings_set_typed (lua_State  *L,
                    const char *accepted,
                    const char *what,
                    int         lua_type_expected)
{
  GSettings *settings = check_settings (L);
  const char *key = luaL_checkstring (L, 2);
  const GVariantType *type;

  if (lua_type (L, 3) != lua_type_expected)
    return luaL_typeerror (L, 3, what);
  type = typed_key (L, settings, key, accepted, what);
  settings_store (L, settings, key, type, 3);
  return 0;
}

#define INT_TYPES "y i u x t n q h"

static int l_settings_get_boolean (lua_State *L) { return settings_get_typed (L, "b", "boolean"); }
static int l_settings_get_int (lua_State *L) { return settings_get_typed (L, INT_TYPES, "integer"); }
static int l_settings_get_double (lua_State *L) { return settings_get_typed (L, "d", "double"); }
static int l_settings_get_string (lua_State *L) { return settings_get_typed (L, "s o g", "string"); }
static int l_settings_get_strv (lua_State *L) { return settings_get_typed (L, "as ao", "string array"); }
static int l_settings_set_boolean (lua_State *L) { return settings_set_typed (L, "b", "boolean", LUA_TBOOLEAN); }
static int l_settings_set_int (lua_State *L) { return settings_set_typed (L, INT_TYPES, "integer", LUA_TNUMBER); }
static int l_settings_set_double (lua_State *L) { return settings_set_typed (L, "d", "double", LUA_TNUMBER); }
static int l_settings_set_string (lua_State *L) { return settings_set_typed (L, "s o g", "string", LUA_TSTRING); }
static int l_settings_set_strv (lua_State *L) { return settings_set_typed (L, "as ao", "string array", LUA_TTABLE); }

static int
l_settings_reset (lua_State *L)
{
  GSettings *settings = check_settings (L);
  const char *key = luaL_checkstring (L, 2);

  settings_key_type (L, settings, key);
  g_settings_reset (settings, key);
  return 0;
}

static int
compare_strings (const void *a,
                 const void *b)
{
  return strcmp (*(const char * const *) a, *(const char * const *) b);
}

static int
l_settings_keys (lua_State *L)
{
  GSettings *settings = check_settings (L);
  GSettingsSchema *schema = NULL;
  char **keys;
  int i, n;

  g_object_get (settings, "settings-schema", &schema, NULL);
  keys = g_settings_schema_list_keys (schema);
  g_settings_schema_unref (schema);

  n = g_strv_length (keys);
  qsort (keys, n, sizeof (char *), compare_strings);
  luaL_checkstack (L, 3, "keys");
  lua_createtable (L, n, 0);
  for (i = 0; i < n; i++)
    {
      lua_pushstring (L, keys[i]);
      lua_rawseti (L, -2, i + 1);
    }
  g_strfreev (keys);
  return 1;
}

/* --- on_changed --------------------------------------------------------- */

typedef struct {
  GWeakRef            settings;
  gulong              handler;
  ShellLuaState      *state;
  ShellLuaConnection *connection;   /* unowned; @watch is freed on disconnect */
} SettingsWatch;

typedef struct {
  ShellLuaState *state;
  int            ref;
  GSettings     *settings;
  const char    *key;
} ChangedCall;

static int
changed_trampoline (lua_State *L,
                    gpointer   data)
{
  ChangedCall *call = data;
  GSettingsSchema *schema = NULL;
  gboolean has_key;

  shell_lua_state_push_ref (call->state, call->ref);
  shell_lua_state_note_callee (L, -1);
  lua_pushstring (L, call->key);

  g_object_get (call->settings, "settings-schema", &schema, NULL);
  has_key = schema != NULL && g_settings_schema_has_key (schema, call->key);
  g_clear_pointer (&schema, g_settings_schema_unref);
  if (has_key)
    {
      GVariant *value = g_settings_get_value (call->settings, call->key);

      shell_lua_push_variant (L, value);
      g_variant_unref (value);
      lua_call (L, 2, 0);
    }
  else
    {
      lua_call (L, 1, 0);
    }
  return 0;
}

static void
settings_changed_cb (GSettings  *settings,
                     const char *key,
                     gpointer    user_data)
{
  SettingsWatch *watch = user_data;
  g_autoptr(ShellLuaConnection) connection = NULL;
  ChangedCall call;

  /* The callback may disconnect (and thereby free @watch). */
  connection = g_object_ref (watch->connection);
  call.state = watch->state;
  call.ref = shell_lua_connection_get_callback_ref (connection);
  call.settings = settings;
  call.key = key;
  if (call.ref == LUA_NOREF || call.ref == LUA_REFNIL)
    return;

  shell_lua_api_call_protected (call.state, 0, changed_trampoline, &call);
}

static void
settings_watch_disconnect (gpointer data)
{
  SettingsWatch *watch = data;
  g_autoptr(GSettings) settings = g_weak_ref_get (&watch->settings);

  if (settings != NULL && watch->handler != 0)
    g_signal_handler_disconnect (settings, watch->handler);
  g_weak_ref_clear (&watch->settings);
  g_free (watch);
}

static int
l_settings_on_changed (lua_State *L)
{
  GSettings *settings = check_settings (L);
  ShellLuaState *state = shell_lua_state_from_L (L);
  const char *key = NULL;
  int fn = 3;
  g_autofree char *signal_name = NULL;
  SettingsWatch *watch;
  ShellLuaConnection *connection;
  int ref;

  if (lua_isfunction (L, 2) && lua_isnone (L, 3))
    fn = 2;                          /* on_changed(fn) */
  else if (!lua_isnil (L, 2))
    key = luaL_checkstring (L, 2);   /* on_changed(key, fn) */
  luaL_checktype (L, fn, LUA_TFUNCTION);

  if (key != NULL)
    settings_key_type (L, settings, key);

  shell_lua_connection_register_handle_type (L);

  lua_pushvalue (L, fn);
  ref = luaL_ref (L, LUA_REGISTRYINDEX);

  signal_name = key != NULL ? g_strconcat ("changed::", key, NULL) : g_strdup ("changed");

  watch = g_new0 (SettingsWatch, 1);
  g_weak_ref_init (&watch->settings, settings);
  watch->state = state;
  connection = shell_lua_connection_new (state, ref, settings_watch_disconnect, watch);
  watch->connection = connection;
  watch->handler = g_signal_connect (settings, signal_name, G_CALLBACK (settings_changed_cb), watch);

  shell_lua_connection_own (connection);
  return shell_lua_connection_push_take (connection, L);
}

static const luaL_Reg settings_methods[] = {
  { "get",         l_settings_get },
  { "set",         l_settings_set },
  { "get_boolean", l_settings_get_boolean },
  { "get_int",     l_settings_get_int },
  { "get_double",  l_settings_get_double },
  { "get_string",  l_settings_get_string },
  { "get_strv",    l_settings_get_strv },
  { "set_boolean", l_settings_set_boolean },
  { "set_int",     l_settings_set_int },
  { "set_double",  l_settings_set_double },
  { "set_string",  l_settings_set_string },
  { "set_strv",    l_settings_set_strv },
  { "reset",       l_settings_reset },
  { "keys",        l_settings_keys },
  { "on_changed",  l_settings_on_changed },
  { NULL, NULL }
};

static const ShellLuaHandleProperty settings_properties[] = {
  { NULL }
};

static ShellLuaHandleType settings_type = {
  .name = "shell.Settings",
  .methods = settings_methods,
  .properties = settings_properties,
};

static void
register_settings_type (lua_State *L)
{
  settings_type.gtype = G_TYPE_SETTINGS;
  shell_lua_handle_register_type (L, &settings_type);
}

/* --- shell.settings.extension / open ------------------------------------ */

static char settings_cache_key;

static const char *
metadata_settings_schema (ShellLuaExtension *extension)
{
  JsonNode *node = json_object_get_member (shell_lua_extension_get_metadata (extension),
                                           "settings-schema");

  if (node != NULL && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING)
    return json_node_get_string (node);
  return NULL;
}

static int
l_settings_extension (lua_State *L)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  ShellLuaExtension *extension = extension_of (state);
  GSettings *settings;

  if (extension == NULL)
    return luaL_error (L, "shell.settings.extension() is only available to extensions");
  if (metadata_settings_schema (extension) == NULL)
    return luaL_error (L, "this extension has no \"settings-schema\" in its metadata");

  settings = shell_lua_extension_get_settings (extension);
  if (settings == NULL)
    return luaL_error (L, "cannot open the settings schema '%s'", metadata_settings_schema (extension));

  register_settings_type (L);
  shell_lua_handle_push (L, "shell.Settings", G_OBJECT (settings));
  return 1;
}

static int
l_settings_open (lua_State *L)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  ShellLuaExtension *extension = extension_of (state);
  const char *schema_id = luaL_checkstring (L, 1);
  GSettingsSchemaSource *source;
  GSettingsSchema *schema;
  GSettings *settings;

  /* An extension may always open its own schema; anything else needs the
   * "settings" capability. Other sandboxed states (tests) get nothing. */
  if (extension != NULL)
    {
      const char *own = metadata_settings_schema (extension);

      if (own != NULL && strcmp (own, schema_id) == 0)
        {
          lua_settop (L, 0);
          return l_settings_extension (L);
        }
      if (!shell_lua_extension_has_capability (extension, "settings"))
        return luaL_error (L, "shell.settings.open() requires the \"settings\" capability");
    }
  else if (shell_lua_state_get_kind (state) != SHELL_LUA_STATE_PRIVILEGED)
    {
      return luaL_error (L, "shell.settings.open() requires the \"settings\" capability");
    }

  register_settings_type (L);

  /* One GSettings per schema and state. */
  luaL_checkstack (L, 6, "shell.settings.open");
  if (lua_rawgetp (L, LUA_REGISTRYINDEX, &settings_cache_key) != LUA_TTABLE)
    {
      lua_pop (L, 1);
      lua_newtable (L);
      lua_pushvalue (L, -1);
      lua_rawsetp (L, LUA_REGISTRYINDEX, &settings_cache_key);
    }
  if (lua_getfield (L, -1, schema_id) != LUA_TNIL && shell_lua_handle_is_alive (L, -1))
    {
      lua_remove (L, -2);
      return 1;
    }
  lua_pop (L, 1);

  source = g_settings_schema_source_get_default ();
  schema = source != NULL ? g_settings_schema_source_lookup (source, schema_id, TRUE) : NULL;
  if (schema == NULL)
    return luaL_error (L, "settings schema '%s' is not installed", schema_id);
  if (g_settings_schema_get_path (schema) == NULL)
    {
      g_settings_schema_unref (schema);
      return luaL_error (L, "settings schema '%s' is relocatable and cannot be opened without a path",
                         schema_id);
    }

  settings = g_settings_new_full (schema, NULL, NULL);
  g_settings_schema_unref (schema);

  {
    ShellLuaOwnToken token = { 0 };

    /* The owner keeps the only strong reference; the token needs no
     * early release (settings live as long as the extension). */
    shell_lua_api_own (state, G_OBJECT (settings), &token);
    g_object_unref (settings);
  }

  shell_lua_handle_push (L, "shell.Settings", G_OBJECT (settings));
  lua_pushvalue (L, -1);
  lua_setfield (L, -3, schema_id);
  lua_remove (L, -2);
  return 1;
}

static const luaL_Reg settings_funcs[] = {
  { "extension", l_settings_extension },
  { "open",      l_settings_open },
  { NULL, NULL }
};

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

static const luaL_Reg gettext_funcs[] = {
  { "gettext",  l_gettext },
  { "ngettext", l_ngettext },
  { "pgettext", l_pgettext },
  { NULL, NULL }
};

void
shell_lua_api_core_register (ShellLuaState *state,
                             lua_State     *L)
{
  int shell = lua_absindex (L, -1);

  luaL_checkstack (L, 10, "shell core api");

  timer_type.gtype = SHELL_TYPE_LUA_TIMER;
  shell_lua_handle_register_type (L, &timer_type);
  register_settings_type (L);
  shell_lua_connection_register_handle_type (L);

  push_log_table (L);
  lua_setfield (L, shell, "log");

  lua_newtable (L);
  luaL_setfuncs (L, timer_funcs, 0);
  lua_setfield (L, shell, "timer");

  lua_newtable (L);
  luaL_setfuncs (L, settings_funcs, 0);
  lua_setfield (L, shell, "settings");

  /* shell.gettext & co (the shell table is on top of the stack again), and
   * the `_`, `ngettext`, `pgettext` globals of the state (the sandbox
   * environment). */
  luaL_setfuncs (L, gettext_funcs, 0);
  lua_pushglobaltable (L);
  lua_getfield (L, shell, "gettext");
  lua_setfield (L, -2, "_");
  lua_getfield (L, shell, "ngettext");
  lua_setfield (L, -2, "ngettext");
  lua_getfield (L, shell, "pgettext");
  lua_setfield (L, -2, "pgettext");
  lua_pop (L, 1);

  /* shell.extension: resolved on first access, once the owner is known. */
  if (!lua_getmetatable (L, shell))
    {
      lua_newtable (L);
      lua_pushcfunction (L, shell_table_index);
      lua_setfield (L, -2, "__index");
      lua_setmetatable (L, shell);
    }
  else
    {
      lua_pop (L, 1);
    }
}
