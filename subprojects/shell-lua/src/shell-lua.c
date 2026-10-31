/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaState: one embedded Lua 5.5 state. See shell-lua.h for the contract
 * and subprojects/shell-lua/docs/SANDBOX.md for the sandbox rules.
 *
 * Implementation notes
 *
 *  - Each state owns its lua_State, created with a counting allocator that
 *    enforces `memory_limit` (returns NULL; Lua then raises a memory error).
 *  - SANDBOXED states never open os/io/debug/package. The "globals" table in
 *    the registry (LUA_RIDX_GLOBALS) is replaced by the sandbox environment,
 *    so lua_load, lua_pushglobaltable() in API modules and the explicit
 *    _ENV upvalue we set on loaded chunks all agree.
 *  - All C code that can raise a Lua error outside of a protected call
 *    (and would therefore reach the panic handler, which aborts) is run
 *    through lua_pcall of a light C function.
 *  - shell_lua_state_pcall() is the only place that arms the watchdog.
 *    The destroy of a state from inside one of its own callbacks is deferred
 *    until the outermost pcall returns.
 *
 * Registry layout (string keys, all prefixed "shell.lua."):
 *    state     lightuserdata, the ShellLuaState* (used by shell-lua-api.c)
 *    shell     the `shell` API table
 *    env       the environment chunks run in (sandbox env, or the globals)
 *    loaded    private module cache of our require()
 *    orig_require (privileged only) the stock require, used as fallback
 *    killed    weak-keyed set of the threads in which the watchdog raised its
 *              error (see "coroutine.wrap() and coroutine.close()" below)
 */

#ifndef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ShellLua"
#endif

#include "shell-lua.h"
#include "shell-lua-api-private.h"

#include <lualib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_HOOK_INTERVAL     10000
#define DEFAULT_SANDBOX_BUDGET_US (500 * 1000)
#define DEFAULT_SANDBOX_MEMORY    ((gsize) 64 * 1024 * 1024)
#define MAX_PCALL_DEPTH           50
#define MAX_MODULE_NAME_LEN       255

#define KEY_STATE        "shell.lua.state"
#define KEY_SHELL        "shell.lua.shell"
#define KEY_ENV          "shell.lua.env"
#define KEY_LOADED       "shell.lua.loaded"
#define KEY_ORIG_REQUIRE "shell.lua.orig_require"
#define KEY_KILLED       "shell.lua.killed"

/* Slow calls are reported at most this often per state */
#define SLOW_REPORT_INTERVAL_US (10 * G_USEC_PER_SEC)

#define WATCHDOG_PREFIX "watchdog: "
#define SANDBOX_PREFIX  "sandbox: "

typedef struct {
  guint           id;
  GDestroyNotify  func;
  gpointer        data;
} Teardown;

struct _ShellLuaState
{
  GObject parent;

  char *id;
  ShellLuaStateKind kind;
  ShellLuaLimits limits;     /* resolved: no zero fields */

  lua_State *L;
  GPtrArray *roots;          /* GFile */
  GArray *teardowns;         /* Teardown */
  guint next_teardown_id;

  gpointer owner;
  guint error_count;

  /* Memory accounting */
  gsize mem_used;
  gsize memory_limit;        /* G_MAXSIZE = unlimited */
  gboolean mem_refused;      /* allocator refused something since the outermost pcall began */

  /* pcall/watchdog bookkeeping */
  int depth;
  gint64 deadlines[MAX_PCALL_DEPTH + 1];
  guint watchdog_trips;

  gboolean destroy_pending;  /* destroy() requested during a pcall */
  gboolean destroying;

  /* Time accounting (shell_lua_state_get_stats ()) */
  ShellLuaStats stats;       /* stats.longest_where is longest_where */
  char *longest_where;
  gint64 outer_nested_us;    /* other states' calls inside the outermost one */
  gint64 last_slow_report;
  guint slow_unreported;
};

enum {
  SIGNAL_ERROR,
  SIGNAL_DESTROYED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

static gint n_states = 0;
static gssize total_memory = 0;
static GPtrArray *current_states = NULL;   /* innermost pcall last; not owned */
static const char loading_sentinel = 0;    /* address used as require() "in progress" marker */
static const char callee_key = 0;          /* registry: the function of the outermost call, or false */

G_DEFINE_FINAL_TYPE (ShellLuaState, shell_lua_state, G_TYPE_OBJECT)

G_DEFINE_QUARK (shell-lua-error-quark, shell_lua_error)

static void destroy_now (ShellLuaState *self);

/* ------------------------------------------------------------------------ */
/* Allocator, warnings, panic                                                */
/* ------------------------------------------------------------------------ */

static void *
lua_alloc (void   *ud,
           void   *ptr,
           size_t  osize,
           size_t  nsize)
{
  ShellLuaState *self = ud;
  size_t old = ptr != NULL ? osize : 0;
  void *newp;

  if (nsize == 0)
    {
      if (ptr != NULL)
        {
          free (ptr);
          self->mem_used -= old;
          g_atomic_pointer_add (&total_memory, -(gssize) old);
        }
      return NULL;
    }

  if (nsize > old && self->memory_limit != G_MAXSIZE)
    {
      size_t growth = nsize - old;

      if (growth > self->memory_limit ||
          self->mem_used > self->memory_limit - growth)
        {
          self->mem_refused = TRUE;
          return NULL;
        }
    }

  newp = realloc (ptr, nsize);
  if (newp == NULL)
    {
      self->mem_refused = TRUE;
      return NULL;
    }

  self->mem_used += nsize;
  self->mem_used -= old;
  g_atomic_pointer_add (&total_memory, (gssize) nsize - (gssize) old);
  return newp;
}

static int
lua_panic (lua_State *L)
{
  ShellLuaState *self = shell_lua_state_from_L (L);
  const char *msg = lua_tostring (L, -1);

  g_error ("Unprotected error in Lua state '%s': %s",
           self != NULL ? self->id : "?",
           msg != NULL ? msg : "(error object is not a string)");
  return 0;
}

static void
lua_warn (void       *ud,
          const char *msg,
          int         tocont)
{
  static GString *pending = NULL;
  ShellLuaState *self = ud;

  if (pending == NULL)
    pending = g_string_new (NULL);

  g_string_append (pending, msg);
  if (tocont)
    return;

  /* "@on" / "@off" control messages are accepted and ignored */
  if (pending->str[0] != '@')
    g_warning ("[%s] Lua warning: %s", self->id, pending->str);
  g_string_truncate (pending, 0);
}

/* ------------------------------------------------------------------------ */
/* Watchdog                                                                  */
/* ------------------------------------------------------------------------ */

static void watchdog_hook (lua_State *L, lua_Debug *ar);

/* True once any deadline on the state's pcall stack has passed. */
static gboolean
watchdog_expired (ShellLuaState *self)
{
  gint64 earliest = G_MAXINT64;
  int i;

  if (self == NULL || self->depth == 0)
    return FALSE;

  for (i = 0; i < self->depth; i++)
    earliest = MIN (earliest, self->deadlines[i]);
  return g_get_monotonic_time () >= earliest;
}

/* Remembers that the watchdog raised its error in thread @L. */
static void
mark_thread_killed (lua_State *L)
{
  if (!lua_checkstack (L, 3))
    return;

  if (lua_getfield (L, LUA_REGISTRYINDEX, KEY_KILLED) == LUA_TTABLE)
    {
      lua_pushthread (L);
      lua_pushboolean (L, 1);
      lua_rawset (L, -3);
    }
  lua_pop (L, 1);
}

/* Raises the watchdog error in @L. From now on the hook fires at every
 * instruction so that a script that catches the error with its own pcall()
 * cannot keep running. */
static int
watchdog_raise (lua_State     *L,
                ShellLuaState *self)
{
  self->watchdog_trips++;
  mark_thread_killed (L);
  lua_sethook (L, watchdog_hook, LUA_MASKCOUNT, 1);
  lua_pushfstring (L, WATCHDOG_PREFIX "callback exceeded its time budget (%I ms)",
                   (lua_Integer) (self->limits.callback_budget_us / 1000));
  return lua_error (L);
}

static void
watchdog_hook (lua_State *L,
               lua_Debug *ar)
{
  ShellLuaState *self = shell_lua_state_from_L (L);

  if (self == NULL)
    return;

  if (!watchdog_expired (self))
    {
      /* Undo an escalation left over from a previous expiry (coroutine
       * threads keep their own count). */
      if (lua_gethookcount (L) != (int) self->limits.hook_interval)
        lua_sethook (L, watchdog_hook, LUA_MASKCOUNT, self->limits.hook_interval);
      return;
    }

  watchdog_raise (L, self);
}

/* ------------------------------------------------------------------------ */
/* Message handler and error formatting                                      */
/* ------------------------------------------------------------------------ */

/* Runs at the raise point. Strings get a traceback appended; anything else
 * is wrapped as {object, traceback} and rendered after the fact by
 * format_error() once the stack has unwound (it may call __tostring, which
 * must not run here: when the error came from the watchdog hook, debug hooks
 * are disabled for as long as the handler runs). */
static int
msg_handler (lua_State *L)
{
  int t = lua_type (L, 1);

  if (t == LUA_TSTRING || t == LUA_TNUMBER)
    {
      luaL_traceback (L, L, lua_tostring (L, 1), 1);
    }
  else
    {
      lua_settop (L, 1);
      lua_createtable (L, 2, 0);
      lua_pushvalue (L, 1);
      lua_rawseti (L, -2, 1);
      luaL_traceback (L, L, NULL, 1);
      lua_rawseti (L, -2, 2);
    }

  return 1;
}

/* arg 1: what the handler returned. Returns a string. */
static int
format_error (lua_State *L)
{
  if (lua_type (L, 1) == LUA_TSTRING)
    {
      lua_settop (L, 1);
      return 1;
    }

  if (lua_type (L, 1) == LUA_TTABLE)
    {
      lua_rawgeti (L, 1, 1);   /* 2: object */
      lua_rawgeti (L, 1, 2);   /* 3: traceback */
      if (luaL_getmetafield (L, 2, "__tostring") != LUA_TNIL)
        {
          lua_pop (L, 1);
          luaL_tolstring (L, 2, NULL);
        }
      else
        {
          lua_pushfstring (L, "(error object is a %s value)", luaL_typename (L, 2));
        }
      lua_pushliteral (L, "\n");
      lua_pushvalue (L, 3);
      lua_concat (L, 3);
      return 1;
    }

  luaL_tolstring (L, 1, NULL);
  return 1;
}

/* Pops the error object from the top of the stack; returns a newly
 * allocated message. */
static char *
error_to_message (lua_State *L)
{
  char *msg;

  if (lua_type (L, -1) == LUA_TSTRING)
    {
      size_t len;
      const char *s = lua_tolstring (L, -1, &len);

      msg = g_strndup (s, len);
      lua_pop (L, 1);
      return msg;
    }

  lua_pushcfunction (L, format_error);
  lua_insert (L, -2);
  if (lua_pcall (L, 1, 1, 0) == LUA_OK && lua_type (L, -1) == LUA_TSTRING)
    {
      size_t len;
      const char *s = lua_tolstring (L, -1, &len);
      msg = g_strndup (s, len);
    }
  else
    {
      msg = g_strdup ("(error object could not be converted to a string)");
    }
  lua_pop (L, 1);
  return msg;
}

/* ------------------------------------------------------------------------ */
/* Frames: current-state stack and deadlines                                  */
/* ------------------------------------------------------------------------ */

static void
push_frame (ShellLuaState *self)
{
  gint64 budget = self->limits.callback_budget_us;
  gint64 now = g_get_monotonic_time ();

  if (current_states == NULL)
    current_states = g_ptr_array_new ();

  if (budget >= G_MAXINT64 || budget > G_MAXINT64 - now)
    self->deadlines[self->depth] = G_MAXINT64;
  else
    self->deadlines[self->depth] = now + budget;

  self->depth++;
  g_ptr_array_add (current_states, self);
}

static void
pop_frame (ShellLuaState *self)
{
  self->depth--;
  g_ptr_array_set_size (current_states, current_states->len - 1);
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static char *
file_display_name (GFile *file)
{
  char *path = g_file_get_path (file);

  if (path != NULL)
    return path;
  return g_file_get_uri (file);
}

/* After lua_load: the function is on top. Give it the sandbox environment. */
static void
apply_env (lua_State *L)
{
  lua_getfield (L, LUA_REGISTRYINDEX, KEY_ENV);
  if (lua_setupvalue (L, -2, 1) == NULL)
    lua_pop (L, 1);
}

static gboolean
is_sandboxed (ShellLuaState *self)
{
  return self->kind == SHELL_LUA_STATE_SANDBOXED;
}

/* ------------------------------------------------------------------------ */
/* require()                                                                 */
/* ------------------------------------------------------------------------ */

static gboolean
module_name_is_valid (const char *name)
{
  gboolean prev_dot = TRUE;
  const char *p;

  if (name[0] == '\0' || strlen (name) > MAX_MODULE_NAME_LEN)
    return FALSE;

  for (p = name; *p != '\0'; p++)
    {
      if (*p == '.')
        {
          if (prev_dot)
            return FALSE;   /* leading dot or empty component ("..") */
          prev_dot = TRUE;
        }
      else if (g_ascii_isalnum (*p) || *p == '_')
        {
          prev_dot = FALSE;
        }
      else
        {
          return FALSE;
        }
    }

  return !prev_dot;
}

typedef enum {
  FIND_FOUND,
  FIND_NOT_FOUND,
  FIND_ERROR
} FindResult;

static FindResult
find_module (ShellLuaState  *self,
             const char     *name,
             GString        *tried,
             char          **chunkname,
             char          **contents,
             gsize          *length,
             char          **errmsg)
{
  g_autofree char *rel = g_strdup (name);
  guint i;
  int k;

  g_strdelimit (rel, ".", '/');

  for (i = 0; i < self->roots->len; i++)
    {
      GFile *root = g_ptr_array_index (self->roots, i);
      for (k = 0; k < 2; k++)
        {
          g_autofree char *relfile = k == 0 ? g_strconcat (rel, ".lua", NULL)
                                            : g_strconcat (rel, "/init.lua", NULL);
          g_autoptr(GFile) child = g_file_resolve_relative_path (root, relfile);
          g_autofree char *display = file_display_name (child);
          g_autoptr(GError) error = NULL;
          char *data = NULL;
          gsize len = 0;

          if (g_file_load_contents (child, NULL, &data, &len, NULL, &error))
            {
              *chunkname = g_strconcat ("@", display, NULL);
              *contents = data;
              *length = len;
              return FIND_FOUND;
            }

          if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
              g_error_matches (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY) ||
              g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY) ||
              g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE))
            {
              g_string_append_printf (tried, "\n\tno file '%s'", display);
              continue;
            }

          *errmsg = g_strdup_printf ("cannot read '%s': %s", display, error->message);
          return FIND_ERROR;
        }
    }

  return FIND_NOT_FOUND;
}

static int
l_require (lua_State *L)
{
  ShellLuaState *self = shell_lua_state_from_L (L);
  const char *name = luaL_checkstring (L, 1);
  GString *tried;
  char *chunkname = NULL;
  char *contents = NULL;
  char *errmsg = NULL;
  gsize len = 0;
  FindResult res;
  int status;

  lua_settop (L, 1);   /* 1: name */

  if (!module_name_is_valid (name))
    {
      if (!is_sandboxed (self) && lua_getfield (L, LUA_REGISTRYINDEX, KEY_ORIG_REQUIRE) == LUA_TFUNCTION)
        {
          lua_pushvalue (L, 1);
          lua_call (L, 1, LUA_MULTRET);
          return lua_gettop (L) - 1;
        }
      /* no luaL_error: it would put a source position before the prefix */
      lua_pushfstring (L, SANDBOX_PREFIX "invalid module name '%s'", name);
      return lua_error (L);
    }

  lua_getfield (L, LUA_REGISTRYINDEX, KEY_LOADED);   /* 2: loaded */
  lua_getfield (L, 2, name);                         /* 3 */
  if (lua_touserdata (L, 3) == &loading_sentinel)
    return luaL_error (L, "loop or previous error loading module '%s'", name);
  if (!lua_isnil (L, 3))
    return 1;
  lua_pop (L, 1);

  if (strcmp (name, "shell") == 0)
    {
      lua_getfield (L, LUA_REGISTRYINDEX, KEY_SHELL);
      return 1;
    }

  tried = g_string_new (NULL);
  res = find_module (self, name, tried, &chunkname, &contents, &len, &errmsg);

  if (res == FIND_ERROR)
    {
      lua_pushstring (L, errmsg);
      g_free (errmsg);
      g_string_free (tried, TRUE);
      return luaL_error (L, "error loading module '%s': %s", name, lua_tostring (L, -1));
    }

  if (res == FIND_NOT_FOUND)
    {
      lua_pushstring (L, tried->str);       /* 3: list of tried paths */
      g_string_free (tried, TRUE);

      if (!is_sandboxed (self) && lua_getfield (L, LUA_REGISTRYINDEX, KEY_ORIG_REQUIRE) == LUA_TFUNCTION)
        {
          lua_pushvalue (L, 1);
          lua_call (L, 1, LUA_MULTRET);
          return lua_gettop (L) - 3;
        }
      return luaL_error (L, "module '%s' not found:%s", name, lua_tostring (L, 3));
    }

  g_string_free (tried, TRUE);

  /* FIND_FOUND */
  lua_pushstring (L, chunkname + 1);                        /* 3: path */
  status = luaL_loadbufferx (L, contents, len, chunkname, "t");   /* 4: chunk or message */
  g_free (contents);
  g_free (chunkname);

  if (status != LUA_OK)
    return luaL_error (L, "error loading module '%s' from file '%s':\n\t%s",
                       name, lua_tostring (L, 3), lua_tostring (L, 4));

  if (is_sandboxed (self))
    apply_env (L);

  lua_pushlightuserdata (L, (void *) &loading_sentinel);
  lua_setfield (L, 2, name);

  lua_pushvalue (L, 1);
  lua_pushvalue (L, 3);
  lua_call (L, 2, 1);                                       /* 4: result */

  if (!lua_isnil (L, 4))
    {
      lua_pushvalue (L, 4);
      lua_setfield (L, 2, name);
    }
  else
    {
      lua_getfield (L, 2, name);
      if (lua_touserdata (L, -1) == &loading_sentinel)
        {
          lua_pushboolean (L, 1);
          lua_setfield (L, 2, name);
        }
      lua_pop (L, 1);
    }

  lua_getfield (L, 2, name);
  lua_pushvalue (L, 3);
  return 2;
}

/* ------------------------------------------------------------------------ */
/* Sandbox construction                                                      */
/* ------------------------------------------------------------------------ */

static int
l_print (lua_State *L)
{
  ShellLuaState *self = shell_lua_state_from_L (L);
  int n = lua_gettop (L);
  luaL_Buffer b;
  int i;

  luaL_buffinit (L, &b);
  for (i = 1; i <= n; i++)
    {
      if (i > 1)
        luaL_addchar (&b, '\t');
      luaL_tolstring (L, i, NULL);
      luaL_addvalue (&b);
    }
  luaL_pushresult (&b);

  g_message ("[%s] %s", self->id, lua_tostring (L, -1));
  return 0;
}

static int
l_collectgarbage (lua_State *L)
{
  const char *opt = luaL_optstring (L, 1, "collect");

  if (strcmp (opt, "count") != 0)
    return luaL_error (L, "collectgarbage: option '%s' is not permitted (only \"count\")", opt);

  lua_pushnumber (L, (lua_Number) lua_gc (L, LUA_GCCOUNT) +
                     (lua_Number) lua_gc (L, LUA_GCCOUNTB) / 1024.0);
  return 1;
}

/* xpcall without a live message handler.
 *
 * Lua runs message handlers (and __gc metamethods) with debug hooks disabled
 * once an error has been raised from a hook, which would let a script defeat
 * the watchdog with `xpcall(f, function() while true do end end)`. So the
 * handler is called after the stack has unwound, under a normal protected
 * call where the watchdog is live. The sandbox has no debug library, so the
 * only thing lost is the ability to inspect the stack from the handler. */
static int
finish_xpcall (lua_State    *L,
               int           status,
               lua_KContext  extra)
{
  if (status != LUA_OK && status != LUA_YIELD)
    {
      /* stack: f, handler, true, err */
      lua_pushvalue (L, 2);
      lua_pushvalue (L, -2);
      lua_pcall (L, 1, 1, 0);   /* on failure the handler's error replaces its result */
      lua_pushboolean (L, 0);
      lua_insert (L, -2);
      return 2;
    }
  return lua_gettop (L) - (int) extra;
}

static int
l_xpcall (lua_State *L)
{
  int n = lua_gettop (L);
  int status;

  luaL_checktype (L, 2, LUA_TFUNCTION);
  lua_pushboolean (L, 1);
  lua_pushvalue (L, 1);
  lua_rotate (L, 3, 2);
  status = lua_pcallk (L, n - 2, LUA_MULTRET, 0, 2, finish_xpcall);
  return finish_xpcall (L, status, 2);
}

/* Same as the stock function, but refuses metatables with __gc: finalizers
 * run with debug hooks disabled, so the watchdog could not interrupt them. */
static int
l_setmetatable (lua_State *L)
{
  int t = lua_type (L, 2);

  luaL_checktype (L, 1, LUA_TTABLE);
  luaL_argexpected (L, t == LUA_TNIL || t == LUA_TTABLE, 2, "nil or table");
  if (luaL_getmetafield (L, 1, "__metatable") != LUA_TNIL)
    return luaL_error (L, "cannot change a protected metatable");

  if (t == LUA_TTABLE)
    {
      lua_pushliteral (L, "__gc");
      if (lua_rawget (L, 2) != LUA_TNIL)
        return luaL_error (L, "__gc metamethods are not permitted in the sandbox");
      lua_pop (L, 1);
    }

  lua_settop (L, 2);
  lua_setmetatable (L, 1);
  return 1;
}

/* Strings and userdata have no script-visible metatable in the sandbox. */
static int
l_getmetatable (lua_State *L)
{
  int t;

  luaL_checkany (L, 1);
  t = lua_type (L, 1);
  if (t == LUA_TSTRING || t == LUA_TUSERDATA || t == LUA_TLIGHTUSERDATA)
    {
      lua_pushnil (L);
      return 1;
    }
  if (!lua_getmetatable (L, 1))
    {
      lua_pushnil (L);
      return 1;
    }
  lua_pop (L, 1);
  if (luaL_getmetafield (L, 1, "__metatable") != LUA_TNIL)
    return 1;               /* honour __metatable like the stock function */
  lua_getmetatable (L, 1);
  return 1;
}

static int
l_readonly_newindex (lua_State *L)
{
  return luaL_error (L, "attempt to modify a read-only table");
}

static int
l_readonly_pairs (lua_State *L)
{
  lua_pushvalue (L, lua_upvalueindex (2));   /* next */
  lua_pushvalue (L, lua_upvalueindex (1));   /* the real table */
  lua_pushnil (L);
  return 3;
}

/* Shallow raw copy of the table at @idx into a new table pushed on top. */
static void
copy_table (lua_State *L,
            int        idx)
{
  idx = lua_absindex (L, idx);
  lua_newtable (L);
  lua_pushnil (L);
  while (lua_next (L, idx) != 0)
    {
      lua_pushvalue (L, -2);
      lua_insert (L, -2);
      lua_rawset (L, -4);
    }
}

/* coroutine.wrap() and coroutine.close() of the sandbox.
 *
 * Lua runs with debug hooks disabled from the moment an error is raised by a
 * hook until the protected call that catches it has restored them. The
 * watchdog raises its error from a hook; in the main thread that is the
 * pcall of shell_lua_state_pcall() or of the script, which restore the hooks
 * before they run any pending __close metamethods. A coroutine that dies of
 * the watchdog error, however, stays dead with hooks disabled, and both the
 * stock wrap() (which closes the to-be-closed variables of a coroutine that
 * failed) and close() (on a coroutine that died of an error) would run their
 * __close metamethods in that thread, where the watchdog cannot interrupt
 * them: `local x <close> = setmetatable({}, {__close = function()
 * while true do end end})` inside a coroutine would hang the shell. The
 * threads in which the watchdog raised its error are therefore recorded
 * (mark_thread_killed()), and these two never close such a thread (wrap()
 * just propagates the error, close() reports a failure). Once the budget
 * has expired, close() raises the watchdog error and wrap() does not close
 * anything. Everything else is the stock code. */

/* Is the thread at @idx one in which the watchdog raised its error? */
static gboolean
thread_was_killed (lua_State *L,
                   int        idx)
{
  gboolean killed = FALSE;

  idx = lua_absindex (L, idx);
  if (lua_getfield (L, LUA_REGISTRYINDEX, KEY_KILLED) == LUA_TTABLE)
    {
      lua_pushvalue (L, idx);
      killed = lua_rawget (L, -2) != LUA_TNIL;
      lua_pop (L, 1);
    }
  lua_pop (L, 1);
  return killed;
}

static int
co_auxresume (lua_State *L,
              lua_State *co,
              int        narg)
{
  int status, nres;

  if (!lua_checkstack (co, narg))
    {
      lua_pushliteral (L, "too many arguments to resume");
      return -1;
    }
  lua_xmove (L, co, narg);
  status = lua_resume (co, L, narg, &nres);
  if (status == LUA_OK || status == LUA_YIELD)
    {
      if (!lua_checkstack (L, nres + 1))
        {
          lua_pop (co, nres);
          lua_pushliteral (L, "too many results to resume");
          return -1;
        }
      lua_xmove (co, L, nres);
      return nres;
    }

  lua_xmove (co, L, 1);   /* the error message */
  return -1;
}

static int
l_cowrap_call (lua_State *L)
{
  ShellLuaState *self = shell_lua_state_from_L (L);
  lua_State *co = lua_tothread (L, lua_upvalueindex (1));
  int r = co_auxresume (L, co, lua_gettop (L));

  if (r < 0)
    {
      int stat = lua_status (co);

      /* The message is on top of L already. Closing the dead thread also
       * replaces it by the final error object, after running __close. */
      if (stat != LUA_OK && stat != LUA_YIELD &&
          !watchdog_expired (self) &&
          !thread_was_killed (L, lua_upvalueindex (1)))
        {
          stat = lua_closethread (co, L);
          lua_xmove (co, L, 1);
        }
      if (stat != LUA_ERRMEM && lua_type (L, -1) == LUA_TSTRING)
        {
          luaL_where (L, 1);
          lua_insert (L, -2);
          lua_concat (L, 2);
        }
      return lua_error (L);
    }
  return r;
}

static int
l_cowrap (lua_State *L)
{
  lua_State *NL;

  luaL_checktype (L, 1, LUA_TFUNCTION);
  NL = lua_newthread (L);
  lua_pushvalue (L, 1);
  lua_xmove (L, NL, 1);
  lua_pushcclosure (L, l_cowrap_call, 1);
  return 1;
}

/* upvalue 1: the stock coroutine.close */
static int
l_coclose (lua_State *L)
{
  ShellLuaState *self = shell_lua_state_from_L (L);

  if (watchdog_expired (self))
    return watchdog_raise (L, self);
  if (lua_isthread (L, 1) && thread_was_killed (L, 1))
    {
      lua_pushboolean (L, 0);
      lua_pushliteral (L, WATCHDOG_PREFIX "the coroutine was stopped by the watchdog");
      return 2;
    }

  lua_pushvalue (L, lua_upvalueindex (1));
  lua_insert (L, 1);
  lua_call (L, lua_gettop (L) - 1, LUA_MULTRET);
  return lua_gettop (L);
}

static void
build_sandbox_env (lua_State *L)
{
  static const char *const base_names[] = {
    "assert", "error", "ipairs", "next", "pairs", "pcall",
    "select", "tonumber", "tostring", "type", "rawequal", "rawget",
    "rawset", "rawlen", "_VERSION", NULL
  };
  static const char *const lib_names[] = {
    LUA_STRLIBNAME, LUA_TABLIBNAME, LUA_MATHLIBNAME, LUA_UTF8LIBNAME,
    LUA_COLIBNAME, NULL
  };
  static const lua_CFunction lib_openers[] = {
    luaopen_string, luaopen_table, luaopen_math, luaopen_utf8, luaopen_coroutine
  };
  int top = lua_gettop (L);
  int base_idx, table_idx, env_idx, i;

  luaL_requiref (L, LUA_GNAME, luaopen_base, 1);
  base_idx = lua_gettop (L);

  lua_newtable (L);
  env_idx = lua_gettop (L);

  for (i = 0; base_names[i] != NULL; i++)
    {
      lua_getfield (L, base_idx, base_names[i]);
      lua_setfield (L, env_idx, base_names[i]);
    }

  lua_pushcfunction (L, l_print);
  lua_setfield (L, env_idx, "print");
  lua_pushcfunction (L, l_collectgarbage);
  lua_setfield (L, env_idx, "collectgarbage");
  lua_pushcfunction (L, l_getmetatable);
  lua_setfield (L, env_idx, "getmetatable");
  lua_pushcfunction (L, l_setmetatable);
  lua_setfield (L, env_idx, "setmetatable");
  lua_pushcfunction (L, l_xpcall);
  lua_setfield (L, env_idx, "xpcall");
  lua_pushcfunction (L, l_require);
  lua_setfield (L, env_idx, "require");
  lua_pushvalue (L, env_idx);
  lua_setfield (L, env_idx, "_G");

  table_idx = 0;
  for (i = 0; lib_names[i] != NULL; i++)
    {
      luaL_requiref (L, lib_names[i], lib_openers[i], 0);
      if (strcmp (lib_names[i], LUA_TABLIBNAME) == 0)
        table_idx = lua_gettop (L);

      copy_table (L, -1);

      if (strcmp (lib_names[i], LUA_COLIBNAME) == 0)
        {
          lua_pushcfunction (L, l_cowrap);
          lua_setfield (L, -2, "wrap");
          lua_getfield (L, -2, "close");       /* the stock one, from the original table */
          lua_pushcclosure (L, l_coclose, 1);
          lua_setfield (L, -2, "close");
        }

      if (strcmp (lib_names[i], LUA_STRLIBNAME) == 0)
        {
          lua_pushnil (L);
          lua_setfield (L, -2, "dump");

          /* Make "x":method() resolve in the sandbox copy of `string`. */
          lua_pushliteral (L, "");
          lua_getmetatable (L, -1);
          lua_pushvalue (L, -3);
          lua_setfield (L, -2, "__index");
          lua_pop (L, 2);
        }

      lua_setfield (L, env_idx, lib_names[i]);
    }

  lua_getfield (L, table_idx, "unpack");
  lua_setfield (L, env_idx, "unpack");

  /* Swap in as the globals table, drop everything else we pushed. */
  lua_pushvalue (L, env_idx);
  lua_rawseti (L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
  lua_pushvalue (L, env_idx);
  lua_setfield (L, LUA_REGISTRYINDEX, KEY_ENV);
  lua_settop (L, top);
}

static void
push_api_version (lua_State *L)
{
  /* Proxy: an empty table whose metatable reads from the real one. */
  lua_newtable (L);            /* proxy */
  lua_newtable (L);            /* metatable */
  lua_newtable (L);            /* real */
  lua_pushinteger (L, SHELL_LUA_API_MAJOR);
  lua_setfield (L, -2, "major");
  lua_pushinteger (L, SHELL_LUA_API_MINOR);
  lua_setfield (L, -2, "minor");
  lua_pushvalue (L, -1);
  lua_setfield (L, -3, "__index");

  lua_pushglobaltable (L);
  lua_getfield (L, -1, "next");
  lua_remove (L, -2);
  lua_pushcclosure (L, l_readonly_pairs, 2);   /* upvalues: real, next */
  lua_setfield (L, -2, "__pairs");

  lua_pushcfunction (L, l_readonly_newindex);
  lua_setfield (L, -2, "__newindex");
  lua_pushboolean (L, 0);
  lua_setfield (L, -2, "__metatable");
  lua_setmetatable (L, -2);
}

/* Runs in protected mode from shell_lua_state_new(). arg 1: the state. */
static int
setup_state (lua_State *L)
{
  ShellLuaState *self = lua_touserdata (L, 1);
  int top;

  lua_settop (L, 0);

  lua_pushlightuserdata (L, self);
  lua_setfield (L, LUA_REGISTRYINDEX, KEY_STATE);

  /* Made now so that setting it later never allocates (it is set outside
   * the protected call); it is never nil */
  lua_pushboolean (L, 0);
  lua_rawsetp (L, LUA_REGISTRYINDEX, &callee_key);

  lua_newtable (L);
  lua_setfield (L, LUA_REGISTRYINDEX, KEY_LOADED);

  lua_newtable (L);
  lua_createtable (L, 0, 1);
  lua_pushliteral (L, "k");
  lua_setfield (L, -2, "__mode");
  lua_setmetatable (L, -2);
  lua_setfield (L, LUA_REGISTRYINDEX, KEY_KILLED);

  if (is_sandboxed (self))
    {
      build_sandbox_env (L);
    }
  else
    {
      luaL_openlibs (L);
      lua_pushglobaltable (L);
      lua_setfield (L, LUA_REGISTRYINDEX, KEY_ENV);
    }

  /* The shell table */
  lua_newtable (L);
  push_api_version (L);
  lua_setfield (L, -2, "api_version");
  lua_pushvalue (L, -1);
  lua_setfield (L, LUA_REGISTRYINDEX, KEY_SHELL);

  top = lua_gettop (L);
  shell_lua_api_populate (self, L);
  g_assert (lua_gettop (L) == top);

  /* Expose it (and, for privileged states, install our require) */
  lua_pushglobaltable (L);
  lua_pushvalue (L, 1);
  lua_setfield (L, -2, "shell");

  if (!is_sandboxed (self))
    {
      lua_getfield (L, -1, "require");
      lua_setfield (L, LUA_REGISTRYINDEX, KEY_ORIG_REQUIRE);
      lua_pushcfunction (L, l_require);
      lua_setfield (L, -2, "require");

      /* so that the stock require("shell") also works */
      lua_getfield (L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
      lua_pushvalue (L, 1);
      lua_setfield (L, -2, "shell");
    }

  lua_settop (L, 0);
  return 0;
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                   */
/* ------------------------------------------------------------------------ */

static void
shell_lua_state_dispose (GObject *object)
{
  ShellLuaState *self = SHELL_LUA_STATE (object);

  shell_lua_state_destroy (self);
  g_clear_pointer (&self->roots, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_lua_state_parent_class)->dispose (object);
}

static void
shell_lua_state_finalize (GObject *object)
{
  ShellLuaState *self = SHELL_LUA_STATE (object);

  g_clear_pointer (&self->teardowns, g_array_unref);
  g_free (self->longest_where);
  g_free (self->id);

  G_OBJECT_CLASS (shell_lua_state_parent_class)->finalize (object);
}

static void
shell_lua_state_class_init (ShellLuaStateClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_lua_state_dispose;
  object_class->finalize = shell_lua_state_finalize;

  signals[SIGNAL_ERROR] = g_signal_new ("error",
                                        SHELL_TYPE_LUA_STATE,
                                        G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 1,
                                        G_TYPE_ERROR);
  signals[SIGNAL_DESTROYED] = g_signal_new ("destroyed",
                                            SHELL_TYPE_LUA_STATE,
                                            G_SIGNAL_RUN_LAST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 0);
}

static void
shell_lua_state_init (ShellLuaState *self)
{
  self->roots = g_ptr_array_new_with_free_func (g_object_unref);
  self->teardowns = g_array_new (FALSE, FALSE, sizeof (Teardown));
  self->next_teardown_id = 1;
  self->memory_limit = G_MAXSIZE;
}

/* ------------------------------------------------------------------------ */
/* Public API: lifecycle                                                     */
/* ------------------------------------------------------------------------ */

/**
 * shell_lua_state_new:
 * @id: identifier used in log messages
 * @kind: sandboxed or privileged
 * @limits: (nullable): limits, or %NULL for the defaults of @kind
 *
 * Returns: (transfer full): a new state
 *
 * Never returns NULL for valid arguments. If the lua_State cannot be created
 * or the API modules fail to populate it (a programming error, logged as
 * critical), the returned state is already destroyed and
 * shell_lua_state_is_alive() is FALSE.
 */
ShellLuaState *
shell_lua_state_new (const char           *id,
                     ShellLuaStateKind     kind,
                     const ShellLuaLimits *limits)
{
  g_autoptr(ShellLuaState) self = NULL;
  gboolean sandboxed = kind == SHELL_LUA_STATE_SANDBOXED;
  lua_State *L;

  g_return_val_if_fail (id != NULL, NULL);
  g_return_val_if_fail (kind == SHELL_LUA_STATE_SANDBOXED ||
                        kind == SHELL_LUA_STATE_PRIVILEGED, NULL);

  self = g_object_new (SHELL_TYPE_LUA_STATE, NULL);
  self->id = g_strdup (id);
  self->kind = kind;

  self->limits.hook_interval = (limits != NULL && limits->hook_interval > 0)
    ? limits->hook_interval : DEFAULT_HOOK_INTERVAL;
  self->limits.callback_budget_us = (limits != NULL && limits->callback_budget_us > 0)
    ? limits->callback_budget_us
    : (sandboxed ? DEFAULT_SANDBOX_BUDGET_US : G_MAXINT64);
  self->limits.memory_limit = (limits != NULL && limits->memory_limit > 0)
    ? limits->memory_limit
    : (sandboxed ? DEFAULT_SANDBOX_MEMORY : G_MAXSIZE);
  self->memory_limit = self->limits.memory_limit;

  L = lua_newstate (lua_alloc, self, luaL_makeseed (NULL) ^ g_random_int ());
  if (L == NULL)
    {
      g_critical ("Could not create Lua state '%s'", id);
      return g_steal_pointer (&self);
    }

  self->L = L;
  g_atomic_int_inc (&n_states);

  lua_atpanic (L, lua_panic);
  lua_setwarnf (L, lua_warn, self);
  lua_sethook (L, watchdog_hook, LUA_MASKCOUNT, (int) self->limits.hook_interval);

  lua_pushcfunction (L, setup_state);
  lua_pushlightuserdata (L, self);
  if (lua_pcall (L, 1, 0, 0) != LUA_OK)
    {
      g_critical ("Could not initialise Lua state '%s': %s", id,
                  lua_tostring (L, -1) != NULL ? lua_tostring (L, -1) : "?");
      lua_pop (L, 1);
      shell_lua_state_destroy (self);
    }

  return g_steal_pointer (&self);
}

const char *
shell_lua_state_get_id (ShellLuaState *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_STATE (self), NULL);

  return self->id;
}

ShellLuaStateKind
shell_lua_state_get_kind (ShellLuaState *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_STATE (self), SHELL_LUA_STATE_SANDBOXED);

  return self->kind;
}

gboolean
shell_lua_state_is_alive (ShellLuaState *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_STATE (self), FALSE);

  return self->L != NULL && !self->destroy_pending;
}

lua_State *
shell_lua_state_get_L (ShellLuaState *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_STATE (self), NULL);

  return self->L;
}

void
shell_lua_state_set_owner (ShellLuaState *self,
                           gpointer       owner)
{
  g_return_if_fail (SHELL_IS_LUA_STATE (self));

  self->owner = owner;
}

gpointer
shell_lua_state_get_owner (ShellLuaState *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_STATE (self), NULL);

  return self->owner;
}

void
shell_lua_state_add_search_root (ShellLuaState *self,
                                 GFile         *root)
{
  g_return_if_fail (SHELL_IS_LUA_STATE (self));
  g_return_if_fail (G_IS_FILE (root));
  g_return_if_fail (self->roots != NULL);

  g_ptr_array_add (self->roots, g_object_ref (root));
}

/* ------------------------------------------------------------------------ */
/* Loading                                                                   */
/* ------------------------------------------------------------------------ */

static gboolean
load_buffer (ShellLuaState  *self,
             const char     *code,
             gsize           len,
             const char     *chunkname,
             GError        **error)
{
  lua_State *L = self->L;
  int status;

  if (!lua_checkstack (L, 2))
    {
      g_set_error_literal (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_MEMORY,
                           "cannot grow the Lua stack");
      return FALSE;
    }

  /* "t": text only. Precompiled chunks are never accepted. */
  status = luaL_loadbufferx (L, code, len, chunkname, "t");
  if (status != LUA_OK)
    {
      const char *msg = lua_tostring (L, -1);

      g_set_error (error, SHELL_LUA_ERROR,
                   status == LUA_ERRMEM ? SHELL_LUA_ERROR_MEMORY : SHELL_LUA_ERROR_SYNTAX,
                   "%s", msg != NULL ? msg : "syntax error");
      lua_pop (L, 1);
      return FALSE;
    }

  if (is_sandboxed (self))
    apply_env (L);

  return TRUE;
}

gboolean
shell_lua_state_load_string (ShellLuaState  *self,
                             const char     *code,
                             const char     *chunkname,
                             GError        **error)
{
  g_return_val_if_fail (SHELL_IS_LUA_STATE (self), FALSE);
  g_return_val_if_fail (code != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->L == NULL)
    {
      g_set_error_literal (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_DEAD,
                           "Lua state is dead");
      return FALSE;
    }

  return load_buffer (self, code, strlen (code),
                      chunkname != NULL ? chunkname : "=(string)", error);
}

gboolean
shell_lua_state_load_file (ShellLuaState  *self,
                           GFile          *file,
                           GError        **error)
{
  g_autofree char *contents = NULL;
  g_autofree char *display = NULL;
  g_autofree char *chunkname = NULL;
  gsize len = 0;

  g_return_val_if_fail (SHELL_IS_LUA_STATE (self), FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->L == NULL)
    {
      g_set_error_literal (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_DEAD,
                           "Lua state is dead");
      return FALSE;
    }

  if (!g_file_load_contents (file, NULL, &contents, &len, NULL, error))
    return FALSE;

  display = file_display_name (file);
  chunkname = g_strconcat ("@", display, NULL);
  return load_buffer (self, contents, len, chunkname, error);
}

/* ------------------------------------------------------------------------ */
/* pcall                                                                     */
/* ------------------------------------------------------------------------ */

static void
report_failure (ShellLuaState  *self,
                ShellLuaError   code,
                const char     *message,
                GError        **error)
{
  g_autoptr(GError) gerror = g_error_new_literal (SHELL_LUA_ERROR, code, message);

  self->error_count++;

  if (self->kind == SHELL_LUA_STATE_SANDBOXED)
    g_warning ("[%s] %s", self->id, message);
  else
    g_debug ("[%s] %s", self->id, message);

  g_signal_emit (self, signals[SIGNAL_ERROR], 0, gerror);

  if (error != NULL)
    *error = g_steal_pointer (&gerror);
}

/* True if @marker occurs in the first line of @msg. coroutine.wrap() prepends
 * a source position to string errors, so a strict prefix test is not enough. */
static gboolean
message_has_marker (const char *msg,
                    const char *marker)
{
  const char *hit = strstr (msg, marker);
  const char *nl = strchr (msg, '\n');

  return hit != NULL && (nl == NULL || hit < nl);
}

static ShellLuaError
classify_error (ShellLuaState *self,
                int            status,
                const char    *msg,
                guint          trips_before)
{
  if (status == LUA_ERRMEM)
    return SHELL_LUA_ERROR_MEMORY;
  if (status == LUA_ERRERR && self->mem_refused)
    return SHELL_LUA_ERROR_MEMORY;
  if (self->watchdog_trips != trips_before && message_has_marker (msg, WATCHDOG_PREFIX))
    return SHELL_LUA_ERROR_WATCHDOG;
  if (g_str_has_prefix (msg, "not enough memory"))
    return SHELL_LUA_ERROR_MEMORY;
  if (message_has_marker (msg, SANDBOX_PREFIX))
    return SHELL_LUA_ERROR_SANDBOX;
  return SHELL_LUA_ERROR_RUNTIME;
}

/* ------------------------------------------------------------------------ */
/* Time accounting                                                           */
/* ------------------------------------------------------------------------ */

void
shell_lua_state_note_callee (lua_State *L,
                             int        idx)
{
  ShellLuaState *self = shell_lua_state_from_L (L);

  if (self == NULL || self->depth != 1)
    return;

  lua_pushvalue (L, idx);
  lua_rawsetp (L, LUA_REGISTRYINDEX, &callee_key);
}

/* "extension.lua:57" for the function of the outermost call: its file
 * relative to a root of the state, and the line where it is defined. */
static char *
callee_where (ShellLuaState *self)
{
  lua_State *L = self->L;
  lua_Debug ar;
  guint i;

  if (!lua_checkstack (L, 2))
    return g_strdup ("?");
  if (lua_rawgetp (L, LUA_REGISTRYINDEX, &callee_key) != LUA_TFUNCTION)
    {
      lua_pop (L, 1);
      return g_strdup ("?");
    }
  if (!lua_getinfo (L, ">S", &ar))
    return g_strdup ("?");
  if (strcmp (ar.what, "C") == 0)
    return g_strdup ("[C]");

  if (ar.source[0] == '@')
    {
      g_autoptr (GFile) file = g_file_new_for_path (ar.source + 1);

      for (i = 0; i < self->roots->len; i++)
        {
          g_autofree char *rel = g_file_get_relative_path (g_ptr_array_index (self->roots, i), file);

          if (rel != NULL)
            return g_strdup_printf ("%s:%d", rel, ar.linedefined);
        }
    }
  return g_strdup_printf ("%s:%d", ar.short_src, ar.linedefined);
}

static void
report_slow_call (ShellLuaState *self,
                  gint64         us,
                  const char    *where)
{
  gint64 now = g_get_monotonic_time ();

  if (self->last_slow_report != 0 && now - self->last_slow_report < SLOW_REPORT_INTERVAL_US)
    {
      self->slow_unreported++;
      return;
    }

  if (self->slow_unreported > 0)
    g_message ("[%s] a call took %" G_GINT64_FORMAT " ms (%s), and %u more took over %d ms "
               "since the last report", self->id, us / 1000, where, self->slow_unreported,
               SHELL_LUA_SLOW_CALL_US / 1000);
  else
    g_message ("[%s] a call took %" G_GINT64_FORMAT " ms (%s)", self->id, us / 1000, where);

  self->last_slow_report = now;
  self->slow_unreported = 0;
}

/* After the outermost call that began at @start, its frame popped. Its own
 * time excludes the calls into other states made from inside it, which
 * count for those states; the state around it, if any, gets the same
 * treatment. */
static void
account_call (ShellLuaState *self,
              gint64         start)
{
  gint64 elapsed = g_get_monotonic_time () - start;
  gint64 own = MAX (elapsed - self->outer_nested_us, 0);
  g_autofree char *where = NULL;

  if (current_states != NULL && current_states->len > 0)
    {
      ShellLuaState *outer = g_ptr_array_index (current_states, current_states->len - 1);

      outer->outer_nested_us += elapsed;
    }

  self->stats.calls++;
  self->stats.total_us += own;

  if (own > self->stats.longest_us)
    {
      where = callee_where (self);
      self->stats.longest_us = own;
      g_free (self->longest_where);
      self->longest_where = g_strdup (where);
      self->stats.longest_where = self->longest_where;
    }

  if (own >= SHELL_LUA_SLOW_CALL_US)
    {
      self->stats.slow_calls++;
      if (is_sandboxed (self))
        {
          if (where == NULL)
            where = callee_where (self);
          report_slow_call (self, own, where);
        }
    }

  lua_pushboolean (self->L, 0);
  lua_rawsetp (self->L, LUA_REGISTRYINDEX, &callee_key);
}

gboolean
shell_lua_state_pcall (ShellLuaState  *self,
                       int             nargs,
                       int             nresults,
                       GError        **error)
{
  lua_State *L;
  int base, status;
  guint trips_before;
  gboolean ok, outermost;
  gint64 start = 0;

  g_return_val_if_fail (SHELL_IS_LUA_STATE (self), FALSE);
  g_return_val_if_fail (nargs >= 0, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  L = self->L;
  if (L == NULL)
    {
      g_set_error_literal (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_DEAD,
                           "Lua state is dead");
      return FALSE;
    }

  g_return_val_if_fail (lua_gettop (L) >= nargs + 1, FALSE);

  g_object_ref (self);

  if (self->depth >= MAX_PCALL_DEPTH)
    {
      lua_pop (L, nargs + 1);
      report_failure (self, SHELL_LUA_ERROR_RUNTIME,
                      "maximum nesting of protected calls exceeded", error);
      g_object_unref (self);
      return FALSE;
    }

  if (!lua_checkstack (L, 3))
    {
      lua_pop (L, nargs + 1);
      report_failure (self, SHELL_LUA_ERROR_MEMORY, "cannot grow the Lua stack", error);
      g_object_unref (self);
      return FALSE;
    }

  if (self->depth == 0)
    self->mem_refused = FALSE;

  base = lua_gettop (L) - nargs;

  /* A Lua function called directly is the callee; a C trampoline names the
   * function it runs with shell_lua_state_note_callee () */
  outermost = self->depth == 0;
  if (outermost)
    {
      start = g_get_monotonic_time ();
      self->outer_nested_us = 0;
      if (lua_isfunction (L, base) && !lua_iscfunction (L, base))
        lua_pushvalue (L, base);
      else
        lua_pushboolean (L, 0);
      lua_rawsetp (L, LUA_REGISTRYINDEX, &callee_key);
    }

  lua_pushcfunction (L, msg_handler);
  lua_insert (L, base);

  trips_before = self->watchdog_trips;
  push_frame (self);
  status = lua_pcall (L, nargs, nresults, base);

  if (status == LUA_OK)
    {
      pop_frame (self);
      lua_remove (L, base);
      ok = TRUE;
    }
  else
    {
      g_autofree char *msg = NULL;
      ShellLuaError code;

      /* The frame is still armed here: a hostile __tostring is bounded too. */
      msg = error_to_message (L);
      pop_frame (self);
      lua_settop (L, base - 1);

      code = classify_error (self, status, msg, trips_before);
      if (code == SHELL_LUA_ERROR_MEMORY)
        lua_gc (L, LUA_GCCOLLECT);

      report_failure (self, code, msg, error);
      ok = FALSE;
    }

  if (outermost && self->L != NULL)
    account_call (self, start);

  if (self->depth == 0 && self->destroy_pending)
    {
      destroy_now (self);
      if (ok)
        {
          g_set_error_literal (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_DEAD,
                               "Lua state was destroyed during the call");
          ok = FALSE;
        }
    }

  g_object_unref (self);
  return ok;
}

gboolean
shell_lua_state_do_string (ShellLuaState  *self,
                           const char     *code,
                           const char     *chunkname,
                           GError        **error)
{
  g_return_val_if_fail (SHELL_IS_LUA_STATE (self), FALSE);
  g_return_val_if_fail (code != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!shell_lua_state_load_string (self, code, chunkname, error))
    return FALSE;
  return shell_lua_state_pcall (self, 0, 0, error);
}

gboolean
shell_lua_state_do_file (ShellLuaState  *self,
                         GFile          *file,
                         GError        **error)
{
  g_return_val_if_fail (SHELL_IS_LUA_STATE (self), FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!shell_lua_state_load_file (self, file, error))
    return FALSE;
  return shell_lua_state_pcall (self, 0, 0, error);
}

/* ------------------------------------------------------------------------ */
/* Registry references                                                       */
/* ------------------------------------------------------------------------ */

static int
ref_thunk (lua_State *L)
{
  /* stack: 1 = value, 2 = lightuserdata(int *result) */
  int *result = lua_touserdata (L, 2);

  lua_settop (L, 1);
  *result = luaL_ref (L, LUA_REGISTRYINDEX);
  return 0;
}

int
shell_lua_state_ref (ShellLuaState *self)
{
  lua_State *L;
  int result = LUA_NOREF;

  g_return_val_if_fail (SHELL_IS_LUA_STATE (self), LUA_NOREF);
  g_return_val_if_fail (self->L != NULL, LUA_NOREF);

  L = self->L;
  g_return_val_if_fail (lua_gettop (L) >= 1, LUA_NOREF);

  /* stack: ..., value  ->  ..., f, value, ud  ->  call(2 args) */
  if (!lua_checkstack (L, 3))
    {
      lua_pop (L, 1);
      return LUA_NOREF;
    }
  lua_pushcfunction (L, ref_thunk);
  lua_insert (L, -2);
  lua_pushlightuserdata (L, &result);
  if (lua_pcall (L, 2, 0, 0) != LUA_OK)
    {
      lua_pop (L, 1);
      return LUA_NOREF;
    }

  return result;
}

/* Late unrefs (a callback holder finalized after its state was destroyed) are
 * expected and silently ignored. */
void
shell_lua_state_unref (ShellLuaState *self,
                       int            ref)
{
  g_return_if_fail (SHELL_IS_LUA_STATE (self));

  if (self->L == NULL || ref == LUA_NOREF || ref == LUA_REFNIL)
    return;

  luaL_unref (self->L, LUA_REGISTRYINDEX, ref);
}

void
shell_lua_state_push_ref (ShellLuaState *self,
                          int            ref)
{
  g_return_if_fail (SHELL_IS_LUA_STATE (self));
  g_return_if_fail (self->L != NULL);

  if (!lua_checkstack (self->L, 1))
    {
      g_critical ("cannot grow the Lua stack to push ref %d", ref);
      return;
    }

  lua_rawgeti (self->L, LUA_REGISTRYINDEX, ref);
}

/* ------------------------------------------------------------------------ */
/* Teardown and destruction                                                  */
/* ------------------------------------------------------------------------ */

guint
shell_lua_state_add_teardown (ShellLuaState  *self,
                              GDestroyNotify  func,
                              gpointer        data)
{
  Teardown t;

  g_return_val_if_fail (SHELL_IS_LUA_STATE (self), 0);
  g_return_val_if_fail (func != NULL, 0);
  g_return_val_if_fail (self->L != NULL, 0);

  t.id = self->next_teardown_id++;
  t.func = func;
  t.data = data;
  g_array_append_val (self->teardowns, t);
  return t.id;
}

/* Removes the registration without calling the function: the caller released
 * the resource itself. Unknown ids and dead states are ignored. */
void
shell_lua_state_remove_teardown (ShellLuaState *self,
                                 guint          id)
{
  guint i;

  g_return_if_fail (SHELL_IS_LUA_STATE (self));

  for (i = 0; i < self->teardowns->len; i++)
    {
      if (g_array_index (self->teardowns, Teardown, i).id == id)
        {
          g_array_remove_index (self->teardowns, i);
          return;
        }
    }
}

static void
destroy_now (ShellLuaState *self)
{
  lua_State *L = self->L;

  if (L == NULL || self->destroying)
    return;

  self->destroying = TRUE;
  self->destroy_pending = FALSE;

  while (self->teardowns->len > 0)
    {
      Teardown t = g_array_index (self->teardowns, Teardown, self->teardowns->len - 1);

      g_array_set_size (self->teardowns, self->teardowns->len - 1);
      t.func (t.data);
    }

  lua_sethook (L, NULL, 0, 0);
  lua_close (L);
  self->L = NULL;
  self->destroying = FALSE;

  if (self->mem_used != 0)
    {
      g_debug ("[%s] %" G_GSIZE_FORMAT " bytes still accounted after close", self->id, self->mem_used);
      g_atomic_pointer_add (&total_memory, -(gssize) self->mem_used);
      self->mem_used = 0;
    }
  g_atomic_int_dec_and_test (&n_states);

  g_signal_emit (self, signals[SIGNAL_DESTROYED], 0);
}

/* Destroying a state from inside one of its own callbacks is deferred until
 * the outermost shell_lua_state_pcall() returns. */
void
shell_lua_state_destroy (ShellLuaState *self)
{
  g_return_if_fail (SHELL_IS_LUA_STATE (self));

  if (self->L == NULL)
    return;

  if (self->depth > 0)
    {
      self->destroy_pending = TRUE;
      return;
    }

  g_object_ref (self);
  destroy_now (self);
  g_object_unref (self);
}

/* ------------------------------------------------------------------------ */
/* Statistics and introspection                                              */
/* ------------------------------------------------------------------------ */

/**
 * shell_lua_state_get_stats: (skip)
 * @self: a #ShellLuaState
 * @stats: (out caller-allocates): where to store the statistics
 *
 * The time spent in the code of the state since it was made.
 * @stats->longest_where belongs to the state and changes with the next call.
 */
void
shell_lua_state_get_stats (ShellLuaState *self,
                           ShellLuaStats *stats)
{
  g_return_if_fail (SHELL_IS_LUA_STATE (self));
  g_return_if_fail (stats != NULL);

  *stats = self->stats;
}

gsize
shell_lua_state_get_memory_usage (ShellLuaState *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_STATE (self), 0);

  return self->mem_used;
}

guint
shell_lua_state_get_error_count (ShellLuaState *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_STATE (self), 0);

  return self->error_count;
}

void
shell_lua_state_reset_error_count (ShellLuaState *self)
{
  g_return_if_fail (SHELL_IS_LUA_STATE (self));

  self->error_count = 0;
}

void
shell_lua_state_gc (ShellLuaState *self)
{
  g_return_if_fail (SHELL_IS_LUA_STATE (self));

  if (self->L == NULL)
    return;

  lua_gc (self->L, LUA_GCCOLLECT);
}

ShellLuaState *
shell_lua_state_get_current (void)
{
  if (current_states == NULL || current_states->len == 0)
    return NULL;

  return g_ptr_array_index (current_states, current_states->len - 1);
}

/* Called from the fatal signal handler. This is "async-signal-safe-ish", not
 * strictly async-signal-safe: luaL_traceback allocates in the Lua heap (and
 * pushes onto the Lua stack of a state that may be mid-operation). GJS makes
 * the same trade-off when it dumps JS stacks from the handler. We only use
 * write(2) for output and never touch GLib. The traceback covers the state's
 * main thread; if the fault happened inside a coroutine the frames up to the
 * resume are shown. */
void
shell_lua_dump_stack (void)
{
  static const char header[] = "== Lua stack (innermost state) ==\n";
  ShellLuaState *self = shell_lua_state_get_current ();
  const char *tb;
  ssize_t ignored;

  if (self == NULL || self->L == NULL)
    return;

  ignored = write (STDERR_FILENO, header, sizeof header - 1);
  luaL_traceback (self->L, self->L, NULL, 0);
  tb = lua_tostring (self->L, -1);
  if (tb != NULL)
    {
      ignored = write (STDERR_FILENO, tb, strlen (tb));
      ignored = write (STDERR_FILENO, "\n", 1);
    }
  (void) ignored;
}

guint
shell_lua_get_state_count (void)
{
  return (guint) g_atomic_int_get (&n_states);
}

gsize
shell_lua_get_total_memory (void)
{
  return (gsize) g_atomic_pointer_get (&total_memory);
}
