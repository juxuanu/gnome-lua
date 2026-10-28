/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Helpers shared by the test programs. Everything is static inline so that
 * each program only pays for what it uses.
 */
#pragma once

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>

#include "shell-lua.h"
#include "shell-lua-api-private.h"

/* ------------------------------------------------------------------------ */
/* Running Lua                                                               */
/* ------------------------------------------------------------------------ */

static inline ShellLuaState *
tu_state_new (const char *id, ShellLuaStateKind kind)
{
  ShellLuaState *state = shell_lua_state_new (id, kind, NULL);

  g_assert_true (shell_lua_state_is_alive (state));
  return state;
}

/* Run a chunk, which must succeed. */
static inline void
tu_do (ShellLuaState *state, const char *code)
{
  g_autoptr (GError) error = NULL;

  if (!shell_lua_state_do_string (state, code, "=(test)", &error))
    g_error ("Lua chunk failed: %s\n--- chunk:\n%s", error->message, code);
}

/* Registers the warning that a failed pcall in a sandboxed state logs
 * (privileged states only log at debug level). */
static inline void
tu_expect_state_warning (ShellLuaState *state, const char *substr)
{
  g_autofree char *pattern = NULL;

  if (shell_lua_state_get_kind (state) != SHELL_LUA_STATE_SANDBOXED)
    return;

  /* g_test_expect_message() patterns are globs: '[' is a literal. */
  pattern = g_strdup_printf ("[%s] *%s*", shell_lua_state_get_id (state),
                             substr != NULL ? substr : "");
  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, pattern);
}

/* Run a chunk that must fail with @code and a message containing @substr.
 * Returns the error for further inspection. */
static inline GError *
tu_do_fails (ShellLuaState *state, const char *code, ShellLuaError expected,
             const char *substr)
{
  GError *error = NULL;
  gboolean ok;

  /* A syntax error is reported without logging; every other failure of a
   * sandboxed state logs a warning. */
  if (expected != SHELL_LUA_ERROR_SYNTAX && expected != SHELL_LUA_ERROR_DEAD)
    tu_expect_state_warning (state, substr);

  ok = shell_lua_state_do_string (state, code, "=(test)", &error);
  if (ok)
    g_error ("Lua chunk unexpectedly succeeded:\n%s", code);
  if (!g_error_matches (error, SHELL_LUA_ERROR, (int) expected))
    g_error ("Lua chunk failed with the wrong error (wanted code %d): %s\n--- chunk:\n%s",
             (int) expected, error->message, code);
  if (substr != NULL && strstr (error->message, substr) == NULL)
    g_error ("error message lacks '%s': %s\n--- chunk:\n%s",
             substr, error->message, code);
  if (expected != SHELL_LUA_ERROR_SYNTAX && expected != SHELL_LUA_ERROR_DEAD)
    g_test_assert_expected_messages ();
  return error;
}

/* Like tu_do_fails() but drops the error. */
static inline void
tu_assert_fails (ShellLuaState *state, const char *code, ShellLuaError expected,
                 const char *substr)
{
  g_autoptr (GError) error = tu_do_fails (state, code, expected, substr);
}

/* Evaluate the expression @expr and return tostring() of it. */
static inline char *
tu_eval_str (ShellLuaState *state, const char *expr)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *code = g_strdup_printf ("return tostring(%s)", expr);
  lua_State *L = shell_lua_state_get_L (state);
  int top = lua_gettop (L);
  char *result;

  if (!shell_lua_state_load_string (state, code, "=(eval)", &error) ||
      !shell_lua_state_pcall (state, 0, 1, &error))
    g_error ("evaluating '%s' failed: %s", expr, error->message);

  result = g_strdup (lua_tostring (L, -1));
  lua_settop (L, top);
  return result;
}

static inline gboolean
tu_eval_bool (ShellLuaState *state, const char *expr)
{
  g_autofree char *s = tu_eval_str (state, expr);

  g_assert_true (strcmp (s, "true") == 0 || strcmp (s, "false") == 0);
  return strcmp (s, "true") == 0;
}

static inline gint64
tu_eval_int (ShellLuaState *state, const char *expr)
{
  g_autofree char *s = tu_eval_str (state, expr);

  return g_ascii_strtoll (s, NULL, 10);
}

#define tu_assert_eval_str(state, expr, expected) G_STMT_START { \
    g_autofree char *_s = tu_eval_str ((state), (expr)); \
    if (g_strcmp0 (_s, (expected)) != 0) \
      g_error ("%s:%d: '%s' evaluated to '%s', expected '%s'", \
               __FILE__, __LINE__, (expr), _s, (expected)); \
  } G_STMT_END

#define tu_assert_eval_true(state, expr) G_STMT_START { \
    if (!tu_eval_bool ((state), (expr))) \
      g_error ("%s:%d: '%s' is not true", __FILE__, __LINE__, (expr)); \
  } G_STMT_END

/* Make @func callable from Lua as global @name. */
static inline void
tu_set_cfunction (ShellLuaState *state, const char *name, lua_CFunction func)
{
  lua_State *L = shell_lua_state_get_L (state);

  lua_pushglobaltable (L);
  lua_pushcfunction (L, func);
  lua_setfield (L, -2, name);
  lua_pop (L, 1);
}

/* Same, with one lightuserdata upvalue (read it with lua_upvalueindex (1)). */
static inline void
tu_set_cfunction_with_data (ShellLuaState *state, const char *name,
                            lua_CFunction func, gpointer data)
{
  lua_State *L = shell_lua_state_get_L (state);

  lua_pushglobaltable (L);
  lua_pushlightuserdata (L, data);
  lua_pushcclosure (L, func, 1);
  lua_setfield (L, -2, name);
  lua_pop (L, 1);
}

/* ------------------------------------------------------------------------ */
/* Main loop                                                                 */
/* ------------------------------------------------------------------------ */

static gboolean
tu_quit_cb (gpointer data)
{
  g_main_loop_quit (data);
  return G_SOURCE_REMOVE;
}

/* Iterate the default main context for @ms milliseconds. */
static inline void
tu_spin (guint ms)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);

  /* the timeout removes itself after quitting the loop */
  g_timeout_add (ms, tu_quit_cb, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
}

typedef gboolean (*TuPredicate) (gpointer data);

typedef struct {
  GMainLoop   *loop;
  TuPredicate  pred;
  gpointer     data;
  gboolean     timed_out;
} TuWait;

static gboolean
tu_wait_poll (gpointer user_data)
{
  TuWait *wait = user_data;

  if (wait->pred (wait->data))
    g_main_loop_quit (wait->loop);
  return G_SOURCE_CONTINUE;
}

static gboolean
tu_wait_timeout (gpointer user_data)
{
  TuWait *wait = user_data;

  wait->timed_out = TRUE;
  g_main_loop_quit (wait->loop);
  return G_SOURCE_REMOVE;
}

/* Iterate the main loop until @pred returns TRUE; FALSE on timeout. A hang
 * therefore fails the test instead of blocking it. */
static inline gboolean
tu_wait_for (TuPredicate pred, gpointer data, guint timeout_ms)
{
  TuWait wait = { g_main_loop_new (NULL, FALSE), pred, data, FALSE };
  guint poll, timeout;
  gboolean ok;

  if (pred (data))
    {
      g_main_loop_unref (wait.loop);
      return TRUE;
    }

  poll = g_timeout_add (2, tu_wait_poll, &wait);
  timeout = g_timeout_add (timeout_ms, tu_wait_timeout, &wait);
  g_main_loop_run (wait.loop);
  ok = !wait.timed_out;
  if (!wait.timed_out)
    g_source_remove (timeout);
  g_source_remove (poll);
  g_main_loop_unref (wait.loop);
  return ok;
}

static inline gboolean
tu_pred_int_at_least_3 (gpointer data)
{
  return *(int *) data >= 3;
}

/* ------------------------------------------------------------------------ */
/* Files                                                                     */
/* ------------------------------------------------------------------------ */

static inline char *
tu_tmpdir_new (void)
{
  g_autoptr (GError) error = NULL;
  char *dir = g_dir_make_tmp ("shell-lua-test-XXXXXX", &error);

  g_assert_no_error (error);
  return dir;
}

static inline void
tu_rm_rf (const char *path)
{
  GDir *dir = g_dir_open (path, 0, NULL);
  const char *name;

  if (dir != NULL)
    {
      while ((name = g_dir_read_name (dir)) != NULL)
        {
          g_autofree char *child = g_build_filename (path, name, NULL);

          if (g_file_test (child, G_FILE_TEST_IS_DIR) &&
              !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
            tu_rm_rf (child);
          else
            g_unlink (child);
        }
      g_dir_close (dir);
    }
  g_rmdir (path);
}

/* Write @dir/@relpath, creating parent directories. */
static inline void
tu_write_file (const char *dir, const char *relpath, const char *contents)
{
  g_autofree char *path = g_build_filename (dir, relpath, NULL);
  g_autofree char *parent = g_path_get_dirname (path);
  g_autoptr (GError) error = NULL;

  g_assert_cmpint (g_mkdir_with_parents (parent, 0700), ==, 0);
  g_file_set_contents (path, contents, -1, &error);
  g_assert_no_error (error);
}
