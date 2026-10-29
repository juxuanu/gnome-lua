/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.spawn (docs/api/spawn.md).
 *
 * Only /bin/sh and coreutils are used; the extensions are installed in a
 * temporary directory and enabled through the extension manager, because the
 * capability is what the module checks first.
 */

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include "test-utils.h"
#include "shell-lua-extension-private.h"

static int bumps = 0;

static int
cf_bump (lua_State *L)
{
  bumps++;
  return 0;
}

G_GNUC_PRINTF (2, 3)
static void
do_f (ShellLuaState *s, const char *format, ...)
{
  va_list args;
  char *code;

  va_start (args, format);
  code = g_strdup_vprintf (format, args);
  va_end (args);
  tu_do (s, code);
  g_free (code);
}

G_GNUC_PRINTF (3, 4)
static void
fails_f (ShellLuaState *s, const char *substr, const char *format, ...)
{
  va_list args;
  char *code;

  va_start (args, format);
  code = g_strdup_vprintf (format, args);
  va_end (args);
  tu_assert_fails (s, code, SHELL_LUA_ERROR_RUNTIME, substr);
  g_free (code);
}

typedef struct {
  ShellLuaState *state;
  const char    *expr;
} WaitLua;

static gboolean
pred_lua (gpointer data)
{
  WaitLua *wait = data;

  return tu_eval_bool (wait->state, wait->expr);
}

static void
wait_lua_for (ShellLuaState *s, const char *expr, guint ms)
{
  WaitLua wait = { s, expr };

  if (!tu_wait_for (pred_lua, &wait, ms))
    g_error ("timed out waiting for: %s", expr);
}

static void
wait_lua (ShellLuaState *s, const char *expr)
{
  wait_lua_for (s, expr, 8000);
}

typedef struct {
  char                     *root;
  char                     *user;
  ShellLuaExtensionManager *manager;
} Env;

static Env *
env_new (void)
{
  Env *env = g_new0 (Env, 1);
  g_autoptr (GFile) dir = NULL;
  GFile *dirs[1];

  env->root = g_dir_make_tmp ("t-XXXXXX", NULL);
  g_assert_nonnull (env->root);
  env->user = g_build_filename (env->root, "extensions", NULL);
  g_assert_cmpint (g_mkdir_with_parents (env->user, 0700), ==, 0);

  env->manager = g_object_new (SHELL_TYPE_LUA_EXTENSION_MANAGER, NULL);
  /* the tests make errors on purpose, many of them */
  shell_lua_extension_manager_set_error_limit (env->manager, G_MAXUINT);
  dir = g_file_new_for_path (env->user);
  dirs[0] = dir;
  shell_lua_extension_manager_set_search_dirs (env->manager, dirs, 1);
  return env;
}

static void
env_free (Env *env)
{
  g_object_unref (env->manager);
  tu_rm_rf (env->root);
  g_free (env->root);
  g_free (env->user);
  g_free (env);
}

static ShellLuaState *
env_enable (Env *env, const char *uuid, const char *caps)
{
  g_autofree char *dir = g_build_filename (env->user, uuid, NULL);
  g_autofree char *metadata = g_strdup_printf ("{\"uuid\": \"%s\", \"name\": \"Test\", \"shell-api\": \"1.1\"%s%s}",
                                               uuid, caps != NULL ? ", \"capabilities\": " : "",
                                               caps != NULL ? caps : "");
  g_autoptr (GError) error = NULL;
  ShellLuaExtension *extension;
  ShellLuaState *state;

  tu_write_file (dir, "metadata.json", metadata);
  tu_write_file (dir, "extension.lua", "return { enable = function() end }");
  shell_lua_extension_manager_scan (env->manager);
  g_assert_true (shell_lua_extension_manager_enable (env->manager, uuid, &error));
  g_assert_no_error (error);
  extension = shell_lua_extension_manager_lookup (env->manager, uuid);
  g_assert_nonnull (extension);
  state = shell_lua_extension_get_lua_state (extension);
  g_assert_nonnull (state);
  tu_set_cfunction (state, "bump", cf_bump);
  bumps = 0;
  return state;
}

static ShellLuaState *
spawn_state (Env *env, const char *uuid)
{
  return env_enable (env, uuid, "[\"spawn\"]");
}

/* r / n are written by the on_exit of the snippets */
#define RESET "r = nil; n = 0\n"
#define ON_EXIT "on_exit = function(x) r = x; n = n + 1 end"

static gboolean
process_exists (pid_t pid)
{
  return kill (pid, 0) == 0;
}

/* ------------------------------------------------------------------------ */
/* Capability                                                                */
/* ------------------------------------------------------------------------ */

static void
test_capability_gate (void)
{
  ShellLuaState *sbx = tu_state_new ("sandbox", SHELL_LUA_STATE_SANDBOXED);
  ShellLuaState *priv = tu_state_new ("priv", SHELL_LUA_STATE_PRIVILEGED);
  Env *env = env_new ();
  g_autofree char *marker = g_build_filename (env->root, "marker", NULL);
  ShellLuaState *plain = env_enable (env, "spawnplain@test", NULL);
  ShellLuaState *other = env_enable (env, "spawnother@test", "[\"fs\", \"dbus\"]");
  g_autofree char *code = g_strdup_printf ("shell.spawn{ argv = { '/bin/sh', '-c', 'echo > %s' } }", marker);

  tu_assert_eval_str (sbx, "type(shell.spawn)", "function");

  /* the capability is checked first: before the options, and before anything runs */
  tu_assert_fails (sbx, "shell.spawn{ argv = { '/bin/true' } }", SHELL_LUA_ERROR_RUNTIME,
                   "shell.spawn() requires the \"spawn\" capability");
  tu_assert_fails (sbx, "shell.spawn()", SHELL_LUA_ERROR_RUNTIME,
                   "shell.spawn() requires the \"spawn\" capability");
  tu_assert_fails (sbx, "shell.spawn{ bogus = 1 }", SHELL_LUA_ERROR_RUNTIME,
                   "shell.spawn() requires the \"spawn\" capability");
  tu_assert_fails (plain, code, SHELL_LUA_ERROR_RUNTIME, "shell.spawn() requires the \"spawn\" capability");
  tu_assert_fails (other, code, SHELL_LUA_ERROR_RUNTIME, "shell.spawn() requires the \"spawn\" capability");
  tu_spin (200);
  g_assert_false (g_file_test (marker, G_FILE_TEST_EXISTS));

  /* a privileged state is not gated */
  tu_do (priv, "p = shell.spawn{ argv = { '/bin/true' } }");
  tu_assert_eval_true (priv, "p.alive");

  g_object_unref (sbx);
  g_object_unref (priv);
  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* Results                                                                   */
/* ------------------------------------------------------------------------ */

static void
test_basic_results (void)
{
  Env *env = env_new ();
  ShellLuaState *s = spawn_state (env, "spawnbasic@test");

  /* never synchronous; the handle describes a running process */
  do_f (s, RESET "p = shell.spawn{ argv = { '/bin/true' }, " ON_EXIT " }");
  tu_assert_eval_true (s, "r == nil and p.alive == true");
  tu_assert_eval_true (s, "math.type(p.pid) == 'integer' and p.pid > 0");
  tu_assert_eval_true (s, "tostring(p):find('^<shell%.Process ') ~= nil");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.status == 0 and r.signal == nil and r.stdout == '' and r.stderr == ''");
  tu_assert_eval_true (s, "r.timed_out == false and r.truncated == false");
  tu_assert_eval_str (s, "math.type(r.status)", "integer");
  tu_assert_eval_true (s, "p.alive == false");
  tu_spin (50);
  tu_assert_eval_str (s, "n", "1");

  /* exit code, both streams, in order */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'echo out; echo err >&2; exit 3' }, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.status == 3 and r.signal == nil and r.stdout == 'out\\n' and r.stderr == 'err\\n'");

  /* the handle is already dead inside on_exit, and pid raises like any property */
  do_f (s, RESET "alive_in_exit = 'unset'\n"
        "p = shell.spawn{ argv = { '/bin/true' }, on_exit = function(x) alive_in_exit = p.alive; n = n + 1 end }");
  wait_lua (s, "n > 0");
  tu_assert_eval_str (s, "alive_in_exit", "false");
  tu_assert_eval_true (s, "not pcall(function() return p.pid end)");
  tu_do (s, "p:kill(); p:destroy()");   /* harmless on a finished process */

  /* killed by a signal: no status, the name of the signal */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'kill -TERM $$' }, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.status == nil and r.signal == 'SIGTERM' and r.timed_out == false");
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'kill -KILL $$' }, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.status == nil and r.signal == 'SIGKILL'");
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'kill -USR1 $$' }, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.signal == 'SIGUSR1'");

  /* the program is looked up in PATH */
  do_f (s, RESET "shell.spawn{ argv = { 'sh', '-c', 'exit 7' }, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_str (s, "r.status", "7");

  /* binary output */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'printf \"a\\\\000b\\\\377\"' }, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.stdout == 'a\\0b\\xff'");

  /* several at once */
  tu_do (s, "res = {}; done = 0\n"
            "for i = 1, 12 do shell.spawn{ argv = { '/bin/sh', '-c', 'echo ' .. i }, on_exit = function(x)\n"
            "  res[i] = x.stdout; done = done + 1 end } end");
  wait_lua (s, "done == 12");
  tu_assert_eval_true (s, "res[1] == '1\\n' and res[12] == '12\\n' and res[7] == '7\\n'");

  env_free (env);
}

static void
test_input_environment_cwd (void)
{
  Env *env = env_new ();
  ShellLuaState *s = spawn_state (env, "spawninput@test");
  g_autofree char *real_root = realpath (env->root, NULL);

  /* standard input */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/cat' }, stdin = 'hello', " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.status == 0 and r.stdout == 'hello' and r.stderr == ''");

  do_f (s, RESET "shell.spawn{ argv = { '/bin/cat' }, stdin = '', " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.status == 0 and r.stdout == ''");

  /* without it standard input is /dev/null: cat ends at once */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/cat' }, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.status == 0 and r.stdout == ''");

  /* more than a pipe holds, both ways, without a deadlock */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/cat' }, stdin = ('0123456789abcdef'):rep(32768), " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.status == 0 and #r.stdout == 524288 and r.truncated == false");
  tu_assert_eval_true (s, "r.stdout == ('0123456789abcdef'):rep(32768)");

  /* a process that does not read its input */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/true' }, stdin = ('x'):rep(1000000), " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.status == 0");

  /* env: added to, and replacing, the environment of the shell */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'echo \"$FOO-$BAR-$SPAWN_TEST_KEPT-$SPAWN_TEST_REPLACED-$EMPTY.\"' },\n"
        "  env = { FOO = 'a', BAR = 'b c', SPAWN_TEST_REPLACED = 'new', EMPTY = '' }, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_str (s, "r.stdout", "a-b c-kept-new-.\n");
  /* the environment of the shell itself is untouched */
  g_assert_null (g_getenv ("FOO"));

  /* cwd */
  {
    g_autofree char *code = g_strdup_printf (RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'pwd -P' }, cwd = '%s', " ON_EXIT " }",
                                             env->root);

    tu_do (s, code);
  }
  wait_lua (s, "n > 0");
  {
    g_autofree char *expr = g_strdup_printf ("r.stdout == '%s\\n'", real_root);

    tu_assert_eval_true (s, expr);
  }

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* Failing to start                                                          */
/* ------------------------------------------------------------------------ */

static void
test_spawn_failure (void)
{
  Env *env = env_new ();
  ShellLuaState *s = spawn_state (env, "spawnfail@test");

  /* environmental: nil and a message, no callback, nothing owned */
  do_f (s, RESET "a, b = shell.spawn{ argv = { '/nonexistent/program' }, " ON_EXIT " }");
  tu_assert_eval_true (s, "a == nil and type(b) == 'string' and b:find('/nonexistent/program', 1, true) ~= nil");
  do_f (s, RESET "a, b = shell.spawn{ argv = { 'gnome-lua-no-such-program-xyz' }, " ON_EXIT " }");
  tu_assert_eval_true (s, "a == nil and type(b) == 'string'");
  do_f (s, RESET "a, b = shell.spawn{ argv = { '/bin/true' }, cwd = '/nonexistent/dir', " ON_EXIT " }");
  tu_assert_eval_true (s, "a == nil and type(b) == 'string'");
  do_f (s, RESET "a, b = shell.spawn{ argv = { '/etc/passwd' }, " ON_EXIT " }");   /* not executable */
  tu_assert_eval_true (s, "a == nil and type(b) == 'string'");
  tu_spin (100);
  tu_assert_eval_str (s, "n", "0");

  /* a process that starts and fails is a result, not an error */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'exit 127' }, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_str (s, "r.status", "127");

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* Output limits                                                             */
/* ------------------------------------------------------------------------ */

static void
test_max_output (void)
{
  Env *env = env_new ();
  ShellLuaState *s = spawn_state (env, "spawnmax@test");

  /* default: 1 MiB per stream, then truncated */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'head -c 3000000 /dev/zero; echo e >&2' }, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.status == 0 and #r.stdout == 1048576 and r.truncated == true and r.stderr == 'e\\n'");

  /* exactly the limit is not truncated */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'head -c 1000 /dev/zero' }, max_output = 1000, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "#r.stdout == 1000 and r.truncated == false");
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'head -c 1001 /dev/zero' }, max_output = 1000, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "#r.stdout == 1000 and r.truncated == true and r.status == 0");

  /* the limit is per stream, and the head is kept */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'printf 0123456789; printf abcdefghij >&2' }, max_output = 4, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.stdout == '0123' and r.stderr == 'abcd' and r.truncated == true");

  /* zero keeps nothing */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/echo', 'x' }, max_output = 0, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.stdout == '' and r.truncated == true");
  do_f (s, RESET "shell.spawn{ argv = { '/bin/true' }, max_output = 0, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.truncated == false");

  /* an endless producer is drained, not stored: the timeout ends it */
  do_f (s, RESET "shell.spawn{ argv = { 'yes' }, max_output = 100, timeout_ms = 300, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "#r.stdout == 100 and r.truncated == true and r.timed_out == true and r.signal == 'SIGTERM'");

  /* without on_exit nothing is kept and nothing blocks */
  {
    g_autofree char *marker = g_build_filename (env->root, "done", NULL);

    do_f (s, "q = shell.spawn{ argv = { '/bin/sh', '-c', 'head -c 3000000 /dev/zero; echo > %s' } }", marker);
    tu_assert_eval_true (s, "q.alive");
    wait_lua (s, "not q.alive");
    g_assert_true (g_file_test (marker, G_FILE_TEST_EXISTS));
  }

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* Timeout, kill, destroy                                                    */
/* ------------------------------------------------------------------------ */

static void
test_timeout (void)
{
  Env *env = env_new ();
  ShellLuaState *s = spawn_state (env, "spawntimeout@test");
  gint64 start;

  start = g_get_monotonic_time ();
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sleep', '30' }, timeout_ms = 100, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.timed_out == true and r.status == nil and r.signal == 'SIGTERM' and r.truncated == false");
  g_assert_cmpint (g_get_monotonic_time () - start, <, 5 * G_USEC_PER_SEC);
  g_assert_cmpint (g_get_monotonic_time () - start, >=, 90 * 1000);

  /* a process that finishes in time is not timed out, and the timer is gone */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'exit 2' }, timeout_ms = 60000, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.timed_out == false and r.status == 2");

  /* SIGTERM ignored: SIGKILL follows after two seconds */
  start = g_get_monotonic_time ();
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'trap \"\" TERM; exec sleep 30' }, timeout_ms = 200, " ON_EXIT " }");
  wait_lua_for (s, "n > 0", 10000);
  tu_assert_eval_true (s, "r.timed_out == true and r.status == nil and r.signal == 'SIGKILL'");
  g_assert_cmpint (g_get_monotonic_time () - start, >=, 1800 * 1000);
  g_assert_cmpint (g_get_monotonic_time () - start, <, 8 * G_USEC_PER_SEC);

  env_free (env);
}

static void
test_kill_and_destroy (void)
{
  Env *env = env_new ();
  ShellLuaState *s = spawn_state (env, "spawnkill@test");
  pid_t pid;
  gint64 deadline;

  /* kill () is SIGTERM; the result still comes through on_exit */
  do_f (s, RESET "p = shell.spawn{ argv = { '/bin/sleep', '30' }, " ON_EXIT " }");
  tu_assert_eval_true (s, "p.alive");
  tu_do (s, "p:kill()");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.signal == 'SIGTERM' and r.status == nil and r.timed_out == false");
  tu_assert_eval_true (s, "not p.alive");
  tu_do (s, "p:kill(); p:kill()");

  /* a process that ignores SIGTERM survives kill () */
  do_f (s, RESET "p = shell.spawn{ argv = { '/bin/sh', '-c', 'trap \"\" TERM; echo ready; exec sleep 2' }, " ON_EXIT " }");
  tu_spin (150);
  tu_do (s, "p:kill()");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.status == 0 and r.signal == nil");   /* sleep 2 ended by itself */

  /* destroy () kills and forgets: no on_exit */
  do_f (s, RESET "p = shell.spawn{ argv = { '/bin/sleep', '30' }, " ON_EXIT " }");
  pid = (pid_t) tu_eval_int (s, "p.pid");
  g_assert_cmpint (pid, >, 1);
  g_assert_true (process_exists (pid));
  tu_do (s, "p:destroy()");
  tu_assert_eval_true (s, "p.alive == false");
  tu_do (s, "p:destroy(); p:kill()");
  deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  while (process_exists (pid) && g_get_monotonic_time () < deadline)
    tu_spin (20);
  g_assert_false (process_exists (pid));
  tu_assert_eval_str (s, "n", "0");

  env_free (env);
}

static void
test_grace_for_descendants (void)
{
  Env *env = env_new ();
  ShellLuaState *s = spawn_state (env, "spawngrace@test");
  gint64 start = g_get_monotonic_time ();

  /* the process ends at once but a background job keeps its output open for
   * three seconds: the result comes after the grace period, not after the job */
  do_f (s, RESET "shell.spawn{ argv = { '/bin/sh', '-c', 'sleep 3 & echo done' }, " ON_EXIT " }");
  wait_lua (s, "n > 0");
  tu_assert_eval_true (s, "r.status == 0 and r.stdout == 'done\\n'");
  g_assert_cmpint (g_get_monotonic_time () - start, <, 2500 * 1000);

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* Teardown                                                                  */
/* ------------------------------------------------------------------------ */

static void
test_teardown (void)
{
  Env *env = env_new ();
  ShellLuaState *s = spawn_state (env, "spawnteardown@test");
  pid_t pid1, pid2;
  gint64 deadline;

  tu_do (s,
         "a = shell.spawn{ argv = { '/bin/sleep', '30' }, on_exit = function() bump() end }\n"
         "b = shell.spawn{ argv = { '/bin/sleep', '30' } }\n"
         "c = shell.spawn{ argv = { '/bin/cat' }, stdin = 'x', timeout_ms = 100, on_exit = function() bump() end }");
  pid1 = (pid_t) tu_eval_int (s, "a.pid");
  pid2 = (pid_t) tu_eval_int (s, "b.pid");
  g_assert_true (process_exists (pid1) && process_exists (pid2));

  g_assert_true (shell_lua_extension_manager_disable (env->manager, "spawnteardown@test", NULL));

  deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  while ((process_exists (pid1) || process_exists (pid2)) && g_get_monotonic_time () < deadline)
    tu_spin (20);
  g_assert_false (process_exists (pid1));
  g_assert_false (process_exists (pid2));
  tu_spin (300);
  g_assert_cmpint (bumps, ==, 0);

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* Options                                                                   */
/* ------------------------------------------------------------------------ */

static void
test_options (void)
{
  Env *env = env_new ();
  ShellLuaState *s = spawn_state (env, "spawnopts@test");

  fails_f (s, "options table expected", "shell.spawn()");
  fails_f (s, "options table expected", "shell.spawn('true')");
  fails_f (s, "unknown field 'shell'", "shell.spawn{ argv = { 'true' }, shell = true }");
  fails_f (s, "missing required field 'argv'", "shell.spawn{}");
  fails_f (s, "field 'argv' must be a sequence of strings, got string", "shell.spawn{ argv = 'true' }");
  fails_f (s, "field 'argv' must not be empty", "shell.spawn{ argv = {} }");
  fails_f (s, "field 'argv'[2] must be a string, got number", "shell.spawn{ argv = { 'echo', 5 } }");
  fails_f (s, "field 'argv'[1] must not contain a NUL byte", "shell.spawn{ argv = { 'tr\\0ue' } }");
  fails_f (s, "field 'cwd' must be a string, got number", "shell.spawn{ argv = { 'true' }, cwd = 5 }");
  fails_f (s, "field 'cwd' must be a non-empty path", "shell.spawn{ argv = { 'true' }, cwd = '' }");
  fails_f (s, "field 'env' must be a table, got string", "shell.spawn{ argv = { 'true' }, env = 'A=b' }");
  fails_f (s, "field 'env.A' must be a string, got number", "shell.spawn{ argv = { 'true' }, env = { A = 1 } }");
  fails_f (s, "is not a valid variable name", "shell.spawn{ argv = { 'true' }, env = { ['A=B'] = 'x' } }");
  fails_f (s, "is not a valid variable name", "shell.spawn{ argv = { 'true' }, env = { [''] = 'x' } }");
  fails_f (s, "field 'env' must have string keys", "shell.spawn{ argv = { 'true' }, env = { 'x' } }");
  fails_f (s, "field 'stdin' must be a string, got table", "shell.spawn{ argv = { 'cat' }, stdin = {} }");
  fails_f (s, "field 'stdin' is too large", "shell.spawn{ argv = { 'cat' }, stdin = ('x'):rep(17000000) }");
  fails_f (s, "field 'timeout_ms' must be between 1 and 86400000", "shell.spawn{ argv = { 'true' }, timeout_ms = 0 }");
  fails_f (s, "field 'timeout_ms' must be between 1 and 86400000", "shell.spawn{ argv = { 'true' }, timeout_ms = 86400001 }");
  fails_f (s, "field 'timeout_ms' must be an integer", "shell.spawn{ argv = { 'true' }, timeout_ms = 'x' }");
  fails_f (s, "field 'max_output' must be between 0 and 67108864", "shell.spawn{ argv = { 'true' }, max_output = -1 }");
  fails_f (s, "field 'max_output' must be between 0 and 67108864", "shell.spawn{ argv = { 'true' }, max_output = 67108865 }");
  fails_f (s, "field 'on_exit' must be a function, got string", "shell.spawn{ argv = { 'true' }, on_exit = 'x' }");

  /* nothing ran, nothing is held */
  tu_assert_eval_true (s, "true");

  env_free (env);
}


/* ------------------------------------------------------------------------ */
/* The fixtures of the conformance test (tests/api/fixtures/spawn@test, ...) */
/* ------------------------------------------------------------------------ */

/* A stand-in for the shell.test API of src/shell-test-runner.c */
typedef struct {
  char     *name;
  gboolean  ok;
  char     *message;
} Report;

static GPtrArray  *reports = NULL;
static GHashTable *done_labels = NULL;

static void
report_free (gpointer data)
{
  Report *r = data;

  g_free (r->name);
  g_free (r->message);
  g_free (r);
}

static int
cf_report (lua_State *L)
{
  Report *r = g_new0 (Report, 1);

  r->name = g_strdup (luaL_checkstring (L, 1));
  r->ok = lua_toboolean (L, 2);
  r->message = g_strdup (luaL_optstring (L, 3, NULL));
  g_ptr_array_add (reports, r);
  return 0;
}

static int
cf_done (lua_State *L)
{
  g_hash_table_add (done_labels, g_strdup (luaL_optstring (L, 1, "main")));
  return 0;
}

static void
test_api_register (ShellLuaState *state, lua_State *L)
{
  lua_newtable (L);
  lua_pushcfunction (L, cf_report);
  lua_setfield (L, -2, "report");
  lua_pushcfunction (L, cf_done);
  lua_setfield (L, -2, "done");
  lua_setfield (L, -2, "test");
}

static gboolean
pred_done (gpointer data)
{
  return g_hash_table_contains (done_labels, data);
}

static void
check_reports (const char *what,
               guint       minimum)
{
  GString *failures = g_string_new (NULL);
  guint i;

  for (i = 0; i < reports->len; i++)
    {
      Report *r = g_ptr_array_index (reports, i);

      if (!r->ok)
        g_string_append_printf (failures, "\n  %s: %s", r->name, r->message != NULL ? r->message : "failed");
    }
  if (failures->len > 0)
    g_error ("%s: checks failed (of %u):%s", what, reports->len, failures->str);
  g_assert_cmpuint (reports->len, >=, minimum);
  g_string_free (failures, TRUE);
}

static const char *
reported_message (const char *name)
{
  guint i;

  for (i = 0; i < reports->len; i++)
    {
      Report *r = g_ptr_array_index (reports, i);

      if (strcmp (r->name, name) == 0)
        return r->message;
    }
  return NULL;
}

static void
install_fixtures (const char *user, const char * const *uuids)
{
  g_autofree char *src = g_build_filename (g_getenv ("G_TEST_SRCDIR"), "..", "..", "..", "tests", "api",
                                           "fixtures", NULL);
  g_autoptr (GError) error = NULL;
  guint i;

  for (i = 0; uuids[i] != NULL; i++)
    {
      g_autofree char *metadata_path = g_build_filename (src, uuids[i], "metadata.json", NULL);
      g_autofree char *lua_path = g_build_filename (src, uuids[i], "extension.lua", NULL);
      g_autofree char *metadata = NULL;
      g_autofree char *lua = NULL;
      g_autofree char *dest = g_build_filename (user, uuids[i], NULL);

      g_assert_true (g_file_get_contents (metadata_path, &metadata, NULL, &error));
      g_assert_no_error (error);
      g_assert_true (g_file_get_contents (lua_path, &lua, NULL, &error));
      g_assert_no_error (error);
      tu_write_file (dest, "metadata.json", metadata);
      tu_write_file (dest, "extension.lua", lua);
    }
}

static void
test_fixtures (void)
{
  Env *env = env_new ();
  g_autoptr (GError) error = NULL;
  const char *uuids[] = { "spawn@test", "spawn-nocap@test", NULL };
  guint cycle;

  install_fixtures (env->user, uuids);
  shell_lua_extension_manager_scan (env->manager);
  g_assert_nonnull (shell_lua_extension_manager_lookup (env->manager, "spawn@test"));

  reports = g_ptr_array_new_with_free_func (report_free);
  done_labels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* without the capability, nothing runs */
  g_assert_true (shell_lua_extension_manager_enable (env->manager, "spawn-nocap@test", &error));
  g_assert_no_error (error);
  g_assert_true (tu_wait_for (pred_done, (gpointer) "main", 10000));
  check_reports ("spawn-nocap@test", 2);
  g_assert_true (shell_lua_extension_manager_disable (env->manager, "spawn-nocap@test", &error));
  g_ptr_array_set_size (reports, 0);
  g_hash_table_remove_all (done_labels);

  /* the conformance fixture, twice */
  for (cycle = 0; cycle < 2; cycle++)
    {
      ShellLuaExtension *ext = shell_lua_extension_manager_lookup (env->manager, "spawn@test");
      pid_t pid;
      gint64 deadline;
      guint n_before;

      g_assert_true (shell_lua_extension_manager_enable (env->manager, "spawn@test", &error));
      g_assert_no_error (error);
      g_assert_true (tu_wait_for (pred_done, (gpointer) "main", 30000));
      check_reports ("spawn@test", 12);
      g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 0);
      g_assert_nonnull (reported_message ("sleeper-pid"));
      pid = (pid_t) g_ascii_strtoll (reported_message ("sleeper-pid"), NULL, 10);
      g_assert_cmpint (pid, >, 1);
      g_assert_true (process_exists (pid));

      g_assert_true (shell_lua_extension_manager_disable (env->manager, "spawn@test", &error));
      g_assert_no_error (error);

      /* the process the extension left is killed, nothing runs any more */
      deadline = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
      while (process_exists (pid) && g_get_monotonic_time () < deadline)
        tu_spin (20);
      g_assert_false (process_exists (pid));
      n_before = reports->len;
      tu_spin (200);
      g_assert_cmpuint (reports->len, ==, n_before);

      g_ptr_array_set_size (reports, 0);
      g_hash_table_remove_all (done_labels);
    }

  g_clear_pointer (&reports, g_ptr_array_unref);
  g_clear_pointer (&done_labels, g_hash_table_unref);
  env_free (env);
}

int
main (int argc, char **argv)
{
  /* as the shell does (src/main.c): a process that does not read its input
   * must give a broken pipe, not a signal */
  signal (SIGPIPE, SIG_IGN);

  /* the environment of the shell, for the env tests (before any thread) */
  g_setenv ("SPAWN_TEST_KEPT", "kept", TRUE);
  g_setenv ("SPAWN_TEST_REPLACED", "old", TRUE);

  g_test_init (&argc, &argv, NULL);

  shell_lua_api_register ("zz-test", test_api_register);

  g_test_add_func ("/shell-lua/spawn/capability", test_capability_gate);
  g_test_add_func ("/shell-lua/spawn/results", test_basic_results);
  g_test_add_func ("/shell-lua/spawn/input-environment-cwd", test_input_environment_cwd);
  g_test_add_func ("/shell-lua/spawn/failure", test_spawn_failure);
  g_test_add_func ("/shell-lua/spawn/max-output", test_max_output);
  g_test_add_func ("/shell-lua/spawn/timeout", test_timeout);
  g_test_add_func ("/shell-lua/spawn/kill-destroy", test_kill_and_destroy);
  g_test_add_func ("/shell-lua/spawn/grace", test_grace_for_descendants);
  g_test_add_func ("/shell-lua/spawn/teardown", test_teardown);
  g_test_add_func ("/shell-lua/spawn/options", test_options);
  g_test_add_func ("/shell-lua/spawn/fixtures", test_fixtures);

  return g_test_run ();
}
