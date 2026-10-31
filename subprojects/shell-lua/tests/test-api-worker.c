/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.worker (docs/api/worker.md).
 *
 * The extensions are installed in a temporary directory with their worker
 * files and enabled through the extension manager, because the capability is
 * what the module checks first. The main loop is iterated with tu_wait_for ()
 * while the worker threads run; the tests never block on a thread except
 * where they wait for the live count to drop, which is what the shell never
 * does and what the tests check happens on its own.
 */

#include <string.h>

#include "test-utils.h"
#include "shell-lua-extension-private.h"

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
  wait_lua_for (s, expr, 10000);
}

static gboolean
pred_no_live_workers (gpointer data)
{
  return shell_lua_worker_get_live_count () == 0;
}

/* The threads of every worker have exited (the main thread never waits for
 * that; a test does, to check that it happens on its own). */
static void
wait_threads_gone (void)
{
  if (!tu_wait_for (pred_no_live_workers, NULL, 10000))
    g_error ("worker threads are still there: %u", shell_lua_worker_get_live_count ());
}

/* ------------------------------------------------------------------------ */
/* Extensions with workers                                                   */
/* ------------------------------------------------------------------------ */

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
  wait_threads_gone ();
  tu_rm_rf (env->root);
  g_free (env->root);
  g_free (env->user);
  g_free (env);
}

/* The worker files every extension of these tests gets */
static const char *const ECHO_LUA =
  "local lib = require('lib')\n"
  "local count = 0\n"
  "return { on_message = function(v)\n"
  "  if type(v) == 'table' and v.op then\n"
  "    if v.op == 'progress' then shell.worker.post('p1'); shell.worker.post('p2'); return 'done' end\n"
  "    if v.op == 'log' then shell.log.info('hello from the worker %d', 42); print('printed', 7); return 'logged' end\n"
  "    if v.op == 'warn' then shell.log.warning('a warning of the worker'); return 'warned' end\n"
  "    if v.op == 'fatal' then shell.log.fatal('fatal in the worker') end\n"
  "    if v.op == 'env' then\n"
  "      return { timer = shell.timer == nil, fs = shell.fs == nil, settings = shell.settings == nil,\n"
  "               hooks = shell.hooks == nil, dbus = shell.dbus == nil, spawn = shell.spawn == nil,\n"
  "               network = shell.network == nil, ui = shell.ui == nil, test = shell.test == nil,\n"
  "               start = shell.worker.start == nil, post = type(shell.worker.post),\n"
  "               gettext = shell.gettext == nil and _ == nil, os = os == nil and io == nil and load == nil,\n"
  "               uuid = shell.extension.uuid, dir = shell.extension.dir, name = shell.extension.metadata.name,\n"
  "               caps = #shell.extension.capabilities, cap1 = shell.extension.capabilities[1],\n"
  "               data_dir = shell.extension.data_dir == nil, lib = lib.twice(21),\n"
  "               ro = not pcall(function() shell.extension.uuid = 'x' end),\n"
  "               time = type(shell.time.monotonic()) == 'number', minor = shell.api_version.minor }\n"
  "    end\n"
  "    if v.op == 'slow' then local t = shell.time.monotonic() while shell.time.monotonic() - t < v.s do end return 'slow-done' end\n"
  "    if v.op == 'fail' then error('boom') end\n"
  "    if v.op == 'bad' then return function() end end\n"
  "    if v.op == 'nothing' then return nil end\n"
  "    if v.op == 'count' then count = count + 1; return count end\n"
  "    if v.op == 'loop' then while true do end end\n"
  "    if v.op == 'flood' then for i = 1, v.n do shell.worker.post(i) end return 'flooded' end\n"
  "    if v.op == 'big' then return string.rep('x', v.n) end\n"
  "    if v.op == 'post_nil' then shell.worker.post(nil) end\n"
  "    if v.op == 'post_two' then shell.worker.post(1, 2) end\n"
  "  end\n"
  "  return v\n"
  "end }\n";

static void
write_workers (const char *dir)
{
  tu_write_file (dir, "lib.lua", "return { twice = function(n) return 2 * n end }");
  tu_write_file (dir, "workers/echo.lua", ECHO_LUA);
  tu_write_file (dir, "workers/ready.lua",
                 "shell.worker.post('ready')\n"
                 "return { on_message = function(v) return v end }");
  tu_write_file (dir, "workers/loop.lua", "return { on_message = function() while true do end end }");
  tu_write_file (dir, "workers/catcher.lua",
                 "return { on_message = function() while true do pcall(function() while true do end end) end end }");
  tu_write_file (dir, "workers/hog.lua",
                 "return { on_message = function() local t = {} while true do t[#t + 1] = string.rep('x', 4096) end end }");
  tu_write_file (dir, "workers/notable.lua", "return function() end");
  tu_write_file (dir, "workers/nohandler.lua", "return { on_massage = function() end }");
  tu_write_file (dir, "workers/syntax.lua", "return { on_message = function( }");
  tu_write_file (dir, "workers/raises.lua", "error('at load time')");
  tu_write_file (dir, "workers/loadloop.lua", "while true do end");
}

static ShellLuaState *
env_enable (Env *env, const char *uuid, const char *caps)
{
  g_autofree char *dir = g_build_filename (env->user, uuid, NULL);
  g_autofree char *metadata = g_strdup_printf ("{\"uuid\": \"%s\", \"name\": \"Worker test\", \"shell-api\": \"1.2\"%s%s}",
                                               uuid, caps != NULL ? ", \"capabilities\": " : "",
                                               caps != NULL ? caps : "");
  g_autoptr (GError) error = NULL;
  ShellLuaExtension *extension;
  ShellLuaState *state;

  tu_write_file (dir, "metadata.json", metadata);
  tu_write_file (dir, "extension.lua", "return { enable = function() end }");
  write_workers (dir);
  shell_lua_extension_manager_scan (env->manager);
  g_assert_true (shell_lua_extension_manager_enable (env->manager, uuid, &error));
  g_assert_no_error (error);
  extension = shell_lua_extension_manager_lookup (env->manager, uuid);
  g_assert_nonnull (extension);
  state = shell_lua_extension_get_lua_state (extension);
  g_assert_nonnull (state);
  return state;
}

static ShellLuaState *
worker_state (Env *env, const char *uuid)
{
  return env_enable (env, uuid, "[\"worker\"]");
}

static ShellLuaExtension *
extension_of (Env *env, const char *uuid)
{
  return shell_lua_extension_manager_lookup (env->manager, uuid);
}

/* The callbacks of the snippets: r is the last message, msgs all of them,
 * n their count, e the error */
#define RESET "r = nil; msgs = {}; n = 0; e = nil; alive_in_error = 'unset'\n"
#define CALLBACKS "on_message = function(v) r = v; n = n + 1; msgs[n] = v end, " \
                  "on_error = function(x) e = x; alive_in_error = w.alive end"
#define START(module, extra) RESET "w = shell.worker.start{ module = '" module "', " extra CALLBACKS " }"

/* ------------------------------------------------------------------------ */
/* Capability                                                                */
/* ------------------------------------------------------------------------ */

static void
test_capability_gate (void)
{
  ShellLuaState *sbx = tu_state_new ("sandbox", SHELL_LUA_STATE_SANDBOXED);
  ShellLuaState *priv = tu_state_new ("priv", SHELL_LUA_STATE_PRIVILEGED);
  Env *env = env_new ();
  ShellLuaState *plain = env_enable (env, "workerplain@test", NULL);
  ShellLuaState *other = env_enable (env, "workerother@test", "[\"fs\", \"spawn\", \"network\", \"dbus\"]");
  const char *msg = "shell.worker.start() requires the \"worker\" capability";

  tu_assert_eval_str (sbx, "type(shell.worker.start)", "function");
  tu_assert_eval_true (sbx, "math.type(shell.worker.limit) == 'integer' and shell.worker.limit >= 8");
  tu_assert_eval_true (sbx, "shell.worker.post == nil");

  /* checked first: before the options, before any thread */
  tu_assert_fails (sbx, "shell.worker.start{ module = 'workers.echo' }", SHELL_LUA_ERROR_RUNTIME, msg);
  tu_assert_fails (sbx, "shell.worker.start()", SHELL_LUA_ERROR_RUNTIME, msg);
  tu_assert_fails (sbx, "shell.worker.start{ bogus = 1 }", SHELL_LUA_ERROR_RUNTIME, msg);
  tu_assert_fails (plain, "shell.worker.start{ module = 'workers.echo' }", SHELL_LUA_ERROR_RUNTIME, msg);
  tu_assert_fails (other, "shell.worker.start{ module = 'workers.echo' }", SHELL_LUA_ERROR_RUNTIME, msg);
  tu_assert_fails (other, "shell.worker.start('workers.echo')", SHELL_LUA_ERROR_RUNTIME, msg);
  g_assert_cmpuint (shell_lua_worker_get_live_count (), ==, 0);

  /* a privileged state is not gated (no roots: the module is not found) */
  tu_do (priv, RESET "w = shell.worker.start{ module = 'workers.echo', " CALLBACKS " }");
  tu_assert_eval_true (priv, "w.alive == true");
  wait_lua (priv, "e ~= nil");
  tu_assert_eval_str (priv, "e.code", "load");
  tu_assert_eval_true (priv, "e.message:find('not found', 1, true) ~= nil");
  tu_assert_eval_true (priv, "w.alive == false and alive_in_error == false");

  g_object_unref (sbx);
  g_object_unref (priv);
  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* Messages                                                                  */
/* ------------------------------------------------------------------------ */

static void
test_echo (void)
{
  Env *env = env_new ();
  ShellLuaState *s = worker_state (env, "workerecho@test");

  tu_do (s, START ("workers.echo", "")
            "ok, err = pcall(function() w:post('hello') end)");
  tu_assert_eval_true (s, "ok == true and w.alive == true and n == 0");   /* never synchronous */
  tu_assert_eval_true (s, "tostring(w):find('^<shell%.Worker ') ~= nil");
  wait_lua (s, "n == 1");
  tu_assert_eval_str (s, "r", "hello");

  /* every kind of value, by value */
  tu_do (s, "local function deq(a, b)\n"
            "  if type(a) ~= type(b) then return false end\n"
            "  if type(a) ~= 'table' then return a == b and math.type(a) == math.type(b) end\n"
            "  for k, v in pairs(a) do if not deq(v, b[k]) then return false end end\n"
            "  for k in pairs(b) do if a[k] == nil then return false end end\n"
            "  return true\n"
            "end\n"
            "_G.deq = deq\n"
            "values = { true, false, 0, -7, 2^53, 1.5, -0.25, 'text', '', 'ünïcödé', 'a\\0b\\xff\\xfe',\n"
            "  { 1, 2, 3 }, { a = 1, b = 'x' }, { nested = { deep = { 1, { 2, { 3 } } } }, list = { 'a', { b = true } } }, {} }\n"
            RESET "for _, v in ipairs(values) do w:post(v) end");
  wait_lua (s, "n == #values");
  tu_assert_eval_true (s, "(function() for i, v in ipairs(values) do if not deq(msgs[i], v) then return false end end return true end)()");
  tu_assert_eval_str (s, "math.type(msgs[3])", "integer");
  tu_assert_eval_str (s, "math.type(msgs[6])", "float");
  tu_assert_eval_true (s, "msgs[11] == 'a\\0b\\xff\\xfe'");   /* bytes cross as ay */

  /* order, 300 messages both ways, fed as the queue drains (the cap is 64) */
  tu_do (s, RESET "i = 0; q = 0\n"
            "shell.timer.every{ ms = 1, callback = function()\n"
            "  while i < 300 and w.queued < 48 do i = i + 1; w:post(i); q = math.max(q, w.queued) end\n"
            "  return i < 300\n"
            "end }");
  wait_lua (s, "n == 300");
  tu_assert_eval_true (s, "q >= 1 and q <= 64");
  tu_assert_eval_true (s, "(function() for i = 1, 300 do if msgs[i] ~= i then return false end end return true end)()");
  tu_assert_eval_str (s, "w.queued", "0");

  /* the return value is posted; nil is not; progress posts arrive before it */
  tu_do (s, RESET "w:post{ op = 'progress' }");
  wait_lua (s, "n == 3");
  tu_assert_eval_true (s, "msgs[1] == 'p1' and msgs[2] == 'p2' and msgs[3] == 'done'");
  tu_do (s, RESET "w:post{ op = 'nothing' }; w:post('after')");
  wait_lua (s, "n == 1");
  tu_assert_eval_str (s, "r", "after");

  /* the worker keeps its globals between messages */
  tu_do (s, RESET "w:post{ op = 'count' }; w:post{ op = 'count' }");
  wait_lua (s, "n == 2");
  tu_assert_eval_true (s, "msgs[1] == 1 and msgs[2] == 2");

  /* the environment of the worker */
  tu_do (s, RESET "w:post{ op = 'env' }");
  wait_lua (s, "n == 1");
  tu_assert_eval_true (s, "r.timer and r.fs and r.settings and r.hooks and r.dbus and r.spawn and r.network and r.ui and r.test");
  tu_assert_eval_true (s, "r.start and r.post == 'function' and r.gettext and r.os and r.data_dir and r.time and r.ro");
  tu_assert_eval_str (s, "r.uuid", "workerecho@test");
  tu_assert_eval_str (s, "r.name", "Worker test");
  tu_assert_eval_true (s, "r.caps == 1 and r.cap1 == 'worker' and r.lib == 42 and r.minor == 2");
  tu_assert_eval_true (s, "r.dir == shell.extension.dir");

  /* a worker that posts while it loads */
  tu_do (s, RESET "w2 = shell.worker.start{ module = 'workers.ready', on_message = function(v) r = v; n = n + 1 end }");
  wait_lua (s, "n == 1");
  tu_assert_eval_str (s, "r", "ready");
  tu_do (s, "w2:destroy()");
  tu_assert_eval_true (s, "w2.alive == false");

  /* without on_message the replies are dropped, nothing breaks */
  tu_do (s, "w3 = shell.worker.start{ module = 'workers.echo' }; w3:post('x'); w3:post{ op = 'progress' }");
  tu_spin (300);
  tu_assert_eval_true (s, "w3.alive == true");
  tu_do (s, "w3:destroy()");

  tu_assert_eval_true (s, "w.alive == true");
  tu_do (s, "w:destroy()");
  env_free (env);
}

static void
test_log_relayed (void)
{
  Env *env = env_new ();
  ShellLuaState *s = worker_state (env, "workerlog@test");

  tu_do (s, START ("workers.echo", ""));

  g_test_expect_message ("shell-lua/workerlog@test", G_LOG_LEVEL_INFO, "hello from the worker 42");
  g_test_expect_message ("shell-lua/workerlog@test", G_LOG_LEVEL_MESSAGE, "printed\t7");
  tu_do (s, "w:post{ op = 'log' }");
  wait_lua (s, "n == 1");
  tu_assert_eval_str (s, "r", "logged");
  g_test_assert_expected_messages ();

  /* warnings of the worker are not fatal (G_DEBUG=fatal-warnings is on) */
  g_test_expect_message ("shell-lua/workerlog@test", G_LOG_LEVEL_WARNING, "a warning of the worker");
  tu_do (s, RESET "w:post{ op = 'warn' }");
  wait_lua (s, "n == 1");
  g_test_assert_expected_messages ();

  tu_do (s, "w:destroy()");
  env_free (env);
}

static void
test_post_misuse (void)
{
  Env *env = env_new ();
  ShellLuaState *s = worker_state (env, "workermisuse@test");

  tu_do (s, START ("workers.echo", ""));

  tu_assert_fails (s, "w:post(function() end)", SHELL_LUA_ERROR_RUNTIME, "cannot convert");
  tu_assert_fails (s, "w:post(nil)", SHELL_LUA_ERROR_RUNTIME, "nil");
  tu_assert_fails (s, "w:post()", SHELL_LUA_ERROR_RUNTIME, "exactly one value");
  tu_assert_fails (s, "w:post(1, 2)", SHELL_LUA_ERROR_RUNTIME, "exactly one value");
  tu_assert_fails (s, "w:post{ 1, x = 2 }", SHELL_LUA_ERROR_RUNTIME, "mixes integer and string keys");
  tu_assert_fails (s, "w:post{ 1, nil, 3 }", SHELL_LUA_ERROR_RUNTIME, "sparse");
  tu_assert_fails (s, "w:post{ [true] = 1 }", SHELL_LUA_ERROR_RUNTIME, "key");
  tu_assert_fails (s, "w:post(w)", SHELL_LUA_ERROR_RUNTIME, "cannot convert");
  tu_assert_fails (s, "w:post(coroutine.create(function() end))", SHELL_LUA_ERROR_RUNTIME, "cannot convert");
  tu_assert_fails (s, "local t = {} t[1] = t; w:post(t)", SHELL_LUA_ERROR_RUNTIME, "too deeply");
  tu_assert_fails (s, "w:post(string.rep('x', 16 * 1024 * 1024 + 1))", SHELL_LUA_ERROR_RUNTIME,
                   "message is too large (limit 16777216 bytes)");
  /* the cap on values, lowered: the conversion stops at the cap */
  shell_lua_worker_set_max_message_values (1000);
  tu_assert_fails (s, "local t = {} for i = 1, 10 do local u = {} for j = 1, 100 do u[j] = j end t[i] = u end w:post(t)",
                   SHELL_LUA_ERROR_RUNTIME, "too many values (limit 1000)");
  tu_do (s, RESET "local t = {} for i = 1, 9 do local u = {} for j = 1, 100 do u[j] = j end t[i] = u end w:post(t)");
  wait_lua (s, "n == 1");
  tu_assert_eval_true (s, "#r == 9 and #r[9] == 100");
  shell_lua_worker_set_max_message_values (0);

  /* the worker is unaffected */
  tu_do (s, RESET "w:post('still here')");
  wait_lua (s, "n == 1");
  tu_assert_eval_str (s, "r", "still here");

  /* the 65th queued message is refused and not queued; the others arrive */
  tu_do (s, RESET "w:post{ op = 'slow', s = 0.5 }");
  wait_lua (s, "w.queued == 0");   /* the worker took it and is busy with it */
  tu_do (s, "for i = 1, 64 do w:post(i) end; q = w.queued");
  tu_assert_eval_str (s, "q", "64");
  tu_assert_fails (s, "w:post(65)", SHELL_LUA_ERROR_RUNTIME, "too many queued messages (limit 64)");
  wait_lua (s, "n == 65");
  tu_assert_eval_true (s, "msgs[1] == 'slow-done' and msgs[2] == 1 and msgs[65] == 64");

  tu_do (s, "w:destroy()");
  tu_assert_fails (s, "w:post('dead')", SHELL_LUA_ERROR_RUNTIME, "no longer valid");
  tu_assert_fails (s, "return w.queued", SHELL_LUA_ERROR_RUNTIME, "no longer valid");
  tu_do (s, "w:terminate(); w:destroy()");   /* DEAD_OK */

  env_free (env);
}

static void
test_start_options (void)
{
  Env *env = env_new ();
  ShellLuaState *s = worker_state (env, "workeropts@test");

  tu_assert_fails (s, "shell.worker.start{}", SHELL_LUA_ERROR_RUNTIME, "missing required field 'module'");
  tu_assert_fails (s, "shell.worker.start{ module = 7 }", SHELL_LUA_ERROR_RUNTIME, "field 'module' must be a string");
  tu_assert_fails (s, "shell.worker.start{ module = 'workers/echo' }", SHELL_LUA_ERROR_RUNTIME, "invalid module name 'workers/echo'");
  tu_assert_fails (s, "shell.worker.start{ module = '../x' }", SHELL_LUA_ERROR_RUNTIME, "invalid module name");
  tu_assert_fails (s, "shell.worker.start{ module = 'workers.echo', bogus = 1 }", SHELL_LUA_ERROR_RUNTIME, "unknown field 'bogus'");
  tu_assert_fails (s, "shell.worker.start{ module = 'workers.echo', max_memory = 1000 }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'max_memory' must be between 1048576 and 134217728");
  tu_assert_fails (s, "shell.worker.start{ module = 'workers.echo', max_memory = 129 * 1024 * 1024 }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'max_memory' must be between");
  tu_assert_fails (s, "shell.worker.start{ module = 'workers.echo', max_memory = 'big' }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'max_memory' must be an integer");
  tu_assert_fails (s, "shell.worker.start{ module = 'workers.echo', on_message = 1 }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'on_message' must be a function");
  tu_assert_fails (s, "shell.worker.start{ module = 'workers.echo', on_error = 'x' }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'on_error' must be a function");
  tu_assert_fails (s, "shell.worker.start('workers.echo')", SHELL_LUA_ERROR_RUNTIME, "options table expected");
  g_assert_cmpuint (shell_lua_worker_get_live_count (), ==, 0);

  /* callable tables are callbacks too */
  tu_do (s, RESET "local cb = setmetatable({}, { __call = function(_, v) r = v; n = n + 1 end })\n"
            "w = shell.worker.start{ module = 'workers.echo', on_message = cb }; w:post('called')");
  wait_lua (s, "n == 1");
  tu_assert_eval_str (s, "r", "called");
  tu_do (s, "w:destroy()");

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* Errors                                                                    */
/* ------------------------------------------------------------------------ */

static void
test_handler_error (void)
{
  Env *env = env_new ();
  ShellLuaState *s = worker_state (env, "workererr@test");
  ShellLuaExtension *ext = extension_of (env, "workererr@test");

  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 0);

  /* the error is reported like the error of a callback, with the worker's
   * id, and counts; the worker ends; what was queued behind is dropped */
  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "[workererr@test] worker-1: *boom*");
  tu_do (s, START ("workers.echo", "") "w:post('first'); w:post{ op = 'fail' }; w:post('never')");
  wait_lua (s, "e ~= nil");
  g_test_assert_expected_messages ();
  tu_assert_eval_str (s, "e.code", "error");
  tu_assert_eval_true (s, "e.message:find('boom', 1, true) ~= nil and e.message:find('echo.lua', 1, true) ~= nil");
  tu_assert_eval_true (s, "alive_in_error == false and w.alive == false");
  tu_assert_eval_true (s, "n == 1 and r == 'first'");
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 1);
  tu_spin (200);
  tu_assert_eval_true (s, "n == 1");
  wait_threads_gone ();

  /* a value that cannot cross, returned or posted, is an error of the worker */
  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "[workererr@test] worker-2: *cannot convert*");
  tu_do (s, START ("workers.echo", "") "w:post{ op = 'bad' }");
  wait_lua (s, "e ~= nil");
  g_test_assert_expected_messages ();
  tu_assert_eval_str (s, "e.code", "error");
  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "[workererr@test] worker-3: *nil*");
  tu_do (s, START ("workers.echo", "") "w:post{ op = 'post_nil' }");
  wait_lua (s, "e ~= nil");
  g_test_assert_expected_messages ();
  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "[workererr@test] worker-4: *exactly one value*");
  tu_do (s, START ("workers.echo", "") "w:post{ op = 'post_two' }");
  wait_lua (s, "e ~= nil");
  g_test_assert_expected_messages ();
  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "[workererr@test] worker-5: *too large*");
  tu_do (s, START ("workers.echo", "") "w:post{ op = 'big', n = 16 * 1024 * 1024 + 1 }");
  wait_lua (s, "e ~= nil");
  g_test_assert_expected_messages ();
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 5);

  /* shell.log.fatal: logged by the worker (relayed), then the error */
  g_test_expect_message ("shell-lua/workererr@test", G_LOG_LEVEL_CRITICAL, "fatal in the worker");
  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "[workererr@test] worker-6: *fatal in the worker*");
  tu_do (s, START ("workers.echo", "") "w:post{ op = 'fatal' }");
  wait_lua (s, "e ~= nil");
  g_test_assert_expected_messages ();
  tu_assert_eval_str (s, "e.code", "error");

  /* without on_error: still logged and counted */
  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "[workererr@test] worker-7: *boom*");
  tu_do (s, "w = shell.worker.start{ module = 'workers.echo' }; w:post{ op = 'fail' }");
  wait_lua (s, "w.alive == false");
  g_test_assert_expected_messages ();
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 7);

  env_free (env);
}

static void
test_load_errors (void)
{
  Env *env = env_new ();
  ShellLuaState *s = worker_state (env, "workerload@test");
  ShellLuaExtension *ext = extension_of (env, "workerload@test");

  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "*worker-1: *not found*");
  tu_do (s, START ("workers.missing", "") "w:post('x')");
  wait_lua (s, "e ~= nil");
  g_test_assert_expected_messages ();
  tu_assert_eval_true (s, "e.code == 'load' and n == 0 and w.alive == false and alive_in_error == false");

  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "*worker-2: *syntax.lua*");
  tu_do (s, START ("workers.syntax", ""));
  wait_lua (s, "e ~= nil");
  g_test_assert_expected_messages ();
  tu_assert_eval_str (s, "e.code", "load");

  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "*worker-3: *at load time*");
  tu_do (s, START ("workers.raises", ""));
  wait_lua (s, "e ~= nil");
  g_test_assert_expected_messages ();
  tu_assert_eval_str (s, "e.code", "load");

  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "*worker-4: *must return a table with an on_message function*");
  tu_do (s, START ("workers.notable", ""));
  wait_lua (s, "e ~= nil");
  g_test_assert_expected_messages ();
  tu_assert_eval_str (s, "e.code", "load");

  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "*worker-5: *must return a table with an on_message function*");
  tu_do (s, START ("workers.nohandler", ""));
  wait_lua (s, "e ~= nil");
  g_test_assert_expected_messages ();
  tu_assert_eval_str (s, "e.code", "load");

  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 5);

  /* a module that never returns is stopped like any worker */
  tu_do (s, START ("workers.loadloop", ""));
  tu_spin (100);
  tu_assert_eval_true (s, "w.alive == true and e == nil");
  tu_do (s, "w:terminate()");
  wait_lua (s, "e ~= nil");
  tu_assert_eval_str (s, "e.code", "terminated");
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 5);

  env_free (env);
}

static void
test_memory_limit (void)
{
  Env *env = env_new ();
  ShellLuaState *s = worker_state (env, "workermem@test");
  ShellLuaExtension *ext = extension_of (env, "workermem@test");
  guint states_before = shell_lua_get_state_count ();
  gsize memory_before;

  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "*worker-1: *not enough memory*");
  tu_do (s, START ("workers.hog", "max_memory = 1024 * 1024, ") "w:post('go')");
  wait_lua (s, "e ~= nil");
  g_test_assert_expected_messages ();
  tu_assert_eval_str (s, "e.code", "memory");
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 1);

  /* the thread closes its state: the counts return */
  wait_threads_gone ();
  tu_spin (50);
  g_assert_cmpuint (shell_lua_get_state_count (), ==, states_before);
  memory_before = shell_lua_get_total_memory ();

  /* enough memory: the same work succeeds up to a point the limit allows */
  tu_do (s, START ("workers.echo", "max_memory = 8 * 1024 * 1024, ") "w:post{ op = 'big', n = 2 * 1024 * 1024 }");
  wait_lua (s, "n == 1");
  tu_assert_eval_true (s, "#r == 2 * 1024 * 1024");
  tu_do (s, "w:destroy()");
  wait_threads_gone ();
  tu_spin (50);
  /* the worker's heap is gone with its state; the result lives in the
   * extension's state until it is collected */
  tu_do (s, RESET);
  shell_lua_state_gc (s);
  g_assert_cmpuint (shell_lua_get_total_memory (), <=, memory_before + 512 * 1024);

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* Stopping                                                                  */
/* ------------------------------------------------------------------------ */

static guint ticks = 0;

static gboolean
tick_cb (gpointer data)
{
  ticks++;
  return G_SOURCE_CONTINUE;
}

static void
test_runaway_loop (void)
{
  Env *env = env_new ();
  ShellLuaState *s = worker_state (env, "workerloop@test");
  ShellLuaExtension *ext = extension_of (env, "workerloop@test");
  guint timer = g_timeout_add (10, tick_cb, NULL);
  gint64 start;

  tu_do (s, START ("workers.loop", "") "w:post('go')");

  /* the main loop runs while the worker loops */
  ticks = 0;
  tu_spin (200);
  g_assert_cmpuint (ticks, >, 0);
  tu_assert_eval_true (s, "w.alive == true and e == nil");
  g_assert_cmpuint (shell_lua_worker_get_live_count (), ==, 1);

  /* terminate returns at once; the handle is dead; on_error comes later */
  start = g_get_monotonic_time ();
  tu_do (s, "w:terminate(); alive_after = w.alive; e_inside = e");
  g_assert_cmpint (g_get_monotonic_time () - start, <, 50 * 1000);
  tu_assert_eval_true (s, "alive_after == false and e_inside == nil");
  wait_lua (s, "e ~= nil");
  tu_assert_eval_true (s, "e.code == 'terminated' and alive_in_error == false");
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 0);   /* not counted */

  /* the thread is gone on its own */
  wait_threads_gone ();

  /* a loop that catches its errors is stopped too */
  tu_do (s, START ("workers.catcher", "") "w:post('go')");
  tu_spin (100);
  tu_do (s, "w:terminate()");
  wait_lua (s, "e ~= nil");
  wait_threads_gone ();

  /* destroy (): no callback at all */
  tu_do (s, START ("workers.loop", "") "w:post('go')");
  tu_spin (100);
  tu_do (s, "w:destroy(); alive_after = w.alive");
  tu_assert_eval_true (s, "alive_after == false");
  wait_threads_gone ();
  tu_spin (200);
  tu_assert_eval_true (s, "e == nil and n == 0");

  /* terminate () twice, destroy () after terminate (): one callback at most */
  tu_do (s, START ("workers.loop", "") "w:post('go'); w:terminate(); w:terminate(); w:destroy()");
  wait_lua (s, "e ~= nil");
  tu_spin (200);
  tu_assert_eval_true (s, "e.code == 'terminated'");
  wait_threads_gone ();

  g_source_remove (timer);
  env_free (env);
}

static void
test_blocked_in_post (void)
{
  Env *env = env_new ();
  ShellLuaState *s = worker_state (env, "workerblock@test");

  /* the worker floods the outbox and waits for room; terminate () from the
   * first message wakes and ends it */
  tu_do (s, RESET "w = shell.worker.start{ module = 'workers.echo',\n"
            "  on_message = function(v) n = n + 1; if n == 1 then w:terminate() end end,\n"
            "  on_error = function(x) e = x end }\n"
            "w:post{ op = 'flood', n = 100000 }");
  wait_lua (s, "e ~= nil");
  tu_assert_eval_true (s, "e.code == 'terminated' and n == 1");
  wait_threads_gone ();
  tu_spin (100);
  tu_assert_eval_true (s, "n == 1");

  /* the flood delivered in order when nobody stops it: one per dispatch */
  tu_do (s, START ("workers.echo", "") "w:post{ op = 'flood', n = 500 }");
  wait_lua (s, "n == 501");
  tu_assert_eval_true (s, "msgs[1] == 1 and msgs[250] == 250 and msgs[500] == 500 and msgs[501] == 'flooded'");
  tu_do (s, "w:destroy()");

  env_free (env);
}

static void
test_teardown_at_disable (void)
{
  Env *env = env_new ();
  ShellLuaState *s = worker_state (env, "workerdown@test");
  guint states_before = shell_lua_get_state_count ();
  g_autoptr (GError) error = NULL;
  gint64 start;

  /* one looping, one blocked in post, one idle, one that posts to a handler
   * that would notice */
  tu_do (s, RESET
            "w1 = shell.worker.start{ module = 'workers.loop', on_error = function() ran = true end }; w1:post('go')\n"
            "w2 = shell.worker.start{ module = 'workers.echo', on_message = function() end }; w2:post{ op = 'flood', n = 100000 }\n"
            "w3 = shell.worker.start{ module = 'workers.echo', on_message = function() ran = true end }\n"
            "w4 = shell.worker.start{ module = 'workers.echo', on_message = function() ran = true end }\n"
            "w4:post{ op = 'slow', s = 0.3 }; w4:post('after')");
  tu_spin (150);
  g_assert_cmpuint (shell_lua_worker_get_live_count (), ==, 4);

  start = g_get_monotonic_time ();
  g_assert_true (shell_lua_extension_manager_disable (env->manager, "workerdown@test", &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_get_monotonic_time () - start, <, 500 * 1000);

  /* the threads end on their own and their states are closed; nothing runs */
  wait_threads_gone ();
  tu_spin (300);
  g_assert_cmpuint (shell_lua_get_state_count (), ==, states_before - 1);

  /* a second enable starts afresh */
  g_assert_true (shell_lua_extension_manager_enable (env->manager, "workerdown@test", &error));
  s = shell_lua_extension_get_lua_state (extension_of (env, "workerdown@test"));
  tu_do (s, START ("workers.echo", "") "w:post('again')");
  wait_lua (s, "n == 1");
  tu_assert_eval_str (s, "r", "again");

  g_assert_true (shell_lua_extension_manager_disable (env->manager, "workerdown@test", &error));
  wait_threads_gone ();

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* Limits                                                                    */
/* ------------------------------------------------------------------------ */

static void
test_limits (void)
{
  Env *env = env_new ();
  ShellLuaState *s = worker_state (env, "workerlimit@test");
  ShellLuaState *s2 = worker_state (env, "workerlimit2@test");
  guint limit = shell_lua_worker_get_limit ();

  g_assert_cmpuint (limit, >=, 8);

  /* four per extension; the slot comes back when the thread has exited */
  tu_do (s, "ws = {} for i = 1, 4 do ws[i] = shell.worker.start{ module = 'workers.echo' } end");
  tu_assert_fails (s, "shell.worker.start{ module = 'workers.echo' }", SHELL_LUA_ERROR_RUNTIME, "too many workers (limit 4)");
  tu_do (s, "ws[1]:destroy()");
  {
    gint64 deadline = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;

    while (shell_lua_worker_get_live_count () > 3 && g_get_monotonic_time () < deadline)
      tu_spin (10);
  }
  g_assert_cmpuint (shell_lua_worker_get_live_count (), ==, 3);
  tu_do (s, "ws[5] = shell.worker.start{ module = 'workers.echo' }");
  tu_assert_eval_true (s, "ws[5].alive");

  /* the shell-wide limit (lowered to the four that live): environmental,
   * nil and a message, for everyone */
  shell_lua_worker_set_limit (4);
  tu_assert_fails (s, "shell.worker.start{ module = 'workers.echo' }", SHELL_LUA_ERROR_RUNTIME, "too many workers (limit 4)");
  tu_do (s2, "w, err = shell.worker.start{ module = 'workers.echo' }");
  tu_assert_eval_true (s2, "w == nil and err:find('no room for another worker', 1, true) ~= nil");
  tu_do (s2, "w, err = shell.worker.start{ module = 'workers.echo' }");
  tu_assert_eval_true (s2, "w == nil");
  shell_lua_worker_set_limit (limit);
  tu_do (s2, "w, err = shell.worker.start{ module = 'workers.echo' }");
  tu_assert_eval_true (s2, "w ~= nil and w.alive");
  tu_assert_eval_true (s2, "shell.worker.limit == shell.worker.limit and shell.worker.limit >= 8");

  tu_do (s, "for i = 2, 5 do ws[i]:destroy() end");
  tu_do (s2, "w:destroy()");
  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* Time accounting                                                           */
/* ------------------------------------------------------------------------ */

static void
test_cpu_accounted (void)
{
  Env *env = env_new ();
  ShellLuaState *s = worker_state (env, "workercpu@test");
  ShellLuaExtension *ext = extension_of (env, "workercpu@test");
  ShellLuaWorkerStats workers;
  ShellLuaStats stats;

  g_assert_false (shell_lua_extension_get_worker_stats (ext, &workers));
  g_assert_cmpuint (workers.messages, ==, 0);

  tu_do (s, START ("workers.echo", "") "w:post{ op = 'slow', s = 0.2 }; w:post('quick')");
  wait_lua (s, "n == 2");

  shell_lua_worker_get_stats (s, &workers);
  g_assert_cmpuint (workers.live, ==, 1);
  g_assert_cmpuint (workers.messages, ==, 2);
  g_assert_cmpint (workers.total_us, >=, 150 * 1000);
  g_assert_cmpint (workers.longest_us, >=, 150 * 1000);
  g_assert_cmpint (workers.longest_us, <=, workers.total_us);
  g_assert_true (shell_lua_extension_get_worker_stats (ext, &workers));

  /* not added to the callbacks of the main thread */
  shell_lua_state_get_stats (s, &stats);
  g_assert_cmpint (stats.total_us, <, 150 * 1000);
  g_assert_cmpint (stats.longest_us, <, 150 * 1000);

  tu_do (s, "w:destroy()");
  wait_threads_gone ();
  shell_lua_worker_get_stats (s, &workers);
  g_assert_cmpuint (workers.live, ==, 0);
  g_assert_cmpuint (workers.messages, ==, 2);

  env_free (env);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/shell-lua/worker/capability", test_capability_gate);
  g_test_add_func ("/shell-lua/worker/echo", test_echo);
  g_test_add_func ("/shell-lua/worker/log-relayed", test_log_relayed);
  g_test_add_func ("/shell-lua/worker/post-misuse", test_post_misuse);
  g_test_add_func ("/shell-lua/worker/start-options", test_start_options);
  g_test_add_func ("/shell-lua/worker/errors/handler", test_handler_error);
  g_test_add_func ("/shell-lua/worker/errors/load", test_load_errors);
  g_test_add_func ("/shell-lua/worker/limits/memory", test_memory_limit);
  g_test_add_func ("/shell-lua/worker/stop/runaway-loop", test_runaway_loop);
  g_test_add_func ("/shell-lua/worker/stop/blocked-in-post", test_blocked_in_post);
  g_test_add_func ("/shell-lua/worker/stop/at-disable", test_teardown_at_disable);
  g_test_add_func ("/shell-lua/worker/limits/count", test_limits);
  g_test_add_func ("/shell-lua/worker/time/cpu-accounted", test_cpu_accounted);

  return g_test_run ();
}
