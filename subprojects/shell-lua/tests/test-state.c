/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaState: sandbox, error classification, watchdog, memory limit,
 * require(), teardown/destroy lifecycle. See docs/SANDBOX.md.
 */

#include "test-utils.h"

/* ------------------------------------------------------------------------ */
/* Sandbox                                                                   */
/* ------------------------------------------------------------------------ */

static void
test_sandbox_globals (void)
{
  ShellLuaState *s = tu_state_new ("sbx", SHELL_LUA_STATE_SANDBOXED);

  tu_assert_eval_true (s, "os == nil and io == nil and debug == nil and package == nil");
  tu_assert_eval_true (s, "load == nil and loadstring == nil and loadfile == nil and dofile == nil");
  tu_assert_eval_true (s, "string.dump == nil");
  tu_assert_eval_true (s, "getmetatable('') == nil");
  tu_assert_eval_true (s, "getmetatable({}) == nil");
  tu_assert_eval_true (s, "getmetatable(setmetatable({}, {__metatable = 'locked'})) == 'locked'");

  /* what must exist */
  tu_assert_eval_true (s, "type(assert) == 'function' and type(error) == 'function' and "
                          "type(ipairs) == 'function' and type(pairs) == 'function' and "
                          "type(next) == 'function' and type(pcall) == 'function' and "
                          "type(select) == 'function' and type(tonumber) == 'function' and "
                          "type(tostring) == 'function' and type(type) == 'function' and "
                          "type(rawequal) == 'function' and type(rawget) == 'function' and "
                          "type(rawset) == 'function' and type(rawlen) == 'function' and "
                          "type(xpcall) == 'function' and type(print) == 'function' and "
                          "type(require) == 'function' and type(_VERSION) == 'string'");
  tu_assert_eval_true (s, "type(string) == 'table' and type(table) == 'table' and "
                          "type(math) == 'table' and type(utf8) == 'table' and "
                          "type(coroutine) == 'table'");
  tu_assert_eval_true (s, "unpack == table.unpack");
  tu_assert_eval_true (s, "_G._G == _G and _G.shell == shell");
  tu_assert_eval_true (s, "rawequal(require('shell'), shell)");

  /* the string metatable's __index is the sandbox copy */
  tu_assert_eval_str (s, "('abc'):upper()", "ABC");
  tu_assert_eval_true (s, "getmetatable('') == nil and ('x'):rep(3) == 'xxx'");
  tu_assert_eval_true (s, "string.format('%d-%s', 4, 'x') == '4-x'");

  /* the libraries are per-state copies: breaking them cannot reach the
   * real ones, and the shell table survives */
  tu_do (s, "string.format = nil");
  tu_assert_eval_true (s, "type(shell.log.info) == 'function'");

  g_object_unref (s);
}

static void
test_sandbox_setmetatable_gc (void)
{
  ShellLuaState *s = tu_state_new ("sbx", SHELL_LUA_STATE_SANDBOXED);

  tu_assert_fails (s, "setmetatable({}, {__gc = function() end})",
                   SHELL_LUA_ERROR_RUNTIME, "__gc");
  /* same through pcall: the error is catchable and the table is untouched */
  tu_assert_eval_true (s, "(function() local t = {} "
                          "local ok, err = pcall(setmetatable, t, {__gc = function() end}) "
                          "return not ok and getmetatable(t) == nil end)()");
  /* ordinary metatables work, protected ones stay protected */
  tu_assert_eval_true (s, "getmetatable(setmetatable({}, {__index = {a = 1}})).__index.a == 1");
  tu_assert_fails (s, "local t = setmetatable({}, {__metatable = 1}) setmetatable(t, {})",
                   SHELL_LUA_ERROR_RUNTIME, "protected metatable");

  g_object_unref (s);
}

static void
test_sandbox_collectgarbage (void)
{
  ShellLuaState *s = tu_state_new ("sbx", SHELL_LUA_STATE_SANDBOXED);

  tu_assert_eval_true (s, "type(collectgarbage('count')) == 'number' and collectgarbage('count') > 0");
  tu_assert_fails (s, "collectgarbage('collect')", SHELL_LUA_ERROR_RUNTIME, "not permitted");
  tu_assert_fails (s, "collectgarbage()", SHELL_LUA_ERROR_RUNTIME, "not permitted");
  tu_assert_fails (s, "collectgarbage('stop')", SHELL_LUA_ERROR_RUNTIME, "not permitted");
  tu_assert_fails (s, "collectgarbage('incremental')", SHELL_LUA_ERROR_RUNTIME, "not permitted");

  g_object_unref (s);
}

static void
test_sandbox_api_version (void)
{
  ShellLuaState *s = tu_state_new ("sbx", SHELL_LUA_STATE_SANDBOXED);

  tu_assert_eval_true (s, "shell.api_version.major == 1 and shell.api_version.minor == 1");
  tu_assert_eval_str (s, "(function() local n = 0 for k, v in pairs(shell.api_version) "
                         "do n = n + 1 end return n end)()", "2");
  tu_assert_fails (s, "shell.api_version.major = 2", SHELL_LUA_ERROR_RUNTIME, "read-only");
  tu_assert_fails (s, "shell.api_version.patch = 2", SHELL_LUA_ERROR_RUNTIME, "read-only");
  tu_assert_eval_true (s, "getmetatable(shell.api_version) == false");
  tu_assert_eval_true (s, "shell.api_version.major == 1");

  g_object_unref (s);
}

static void
test_sandbox_xpcall (void)
{
  ShellLuaState *s = tu_state_new ("sbx", SHELL_LUA_STATE_SANDBOXED);

  /* the handler runs after unwinding and receives the message */
  tu_assert_eval_str (s,
                      "(function() local ok, m = xpcall(function() error('x', 0) end, "
                      "function(e) return 'H:' .. e end) return tostring(ok) .. m end)()",
                      "falseH:x");
  /* success passes the results through */
  tu_assert_eval_str (s,
                      "(function() local ok, a, b = xpcall(function(x, y) return x + y, 'b' end, "
                      "print, 1, 2) return tostring(ok) .. a .. b end)()",
                      "true3b");

  g_object_unref (s);
}

static void
test_sandbox_print (void)
{
  ShellLuaState *s = tu_state_new ("sbx", SHELL_LUA_STATE_SANDBOXED);

  g_test_expect_message ("ShellLua", G_LOG_LEVEL_MESSAGE, "[sbx] hello\tworld\t1\ttrue");
  tu_do (s, "print('hello', 'world', 1, true)");
  g_test_assert_expected_messages ();

  g_object_unref (s);
}

static void
test_binary_chunks_rejected (void)
{
  ShellLuaState *s = tu_state_new ("sbx", SHELL_LUA_STATE_SANDBOXED);
  g_autoptr (GError) error = NULL;

  /* the escape byte that starts a precompiled chunk; mode "t" refuses it */
  g_assert_false (shell_lua_state_load_string (s, "\033Lua", "=bin", &error));
  g_assert_error (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_SYNTAX);
  g_assert_nonnull (strstr (error->message, "binary"));

  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* Error classification                                                      */
/* ------------------------------------------------------------------------ */

typedef struct {
  guint  count;
  char  *last_message;
  GQuark domain;
  int    code;
} ErrorProbe;

static void
on_error (ShellLuaState *state, GError *error, ErrorProbe *probe)
{
  probe->count++;
  g_free (probe->last_message);
  probe->last_message = g_strdup (error->message);
  probe->domain = error->domain;
  probe->code = error->code;
}

static void
test_error_syntax (void)
{
  ShellLuaState *s = tu_state_new ("sbx", SHELL_LUA_STATE_SANDBOXED);
  ErrorProbe probe = { 0, };
  g_autoptr (GError) error = NULL;
  g_autoptr (GError) error2 = NULL;

  g_signal_connect (s, "error", G_CALLBACK (on_error), &probe);

  g_assert_false (shell_lua_state_do_string (s, "x = = 1", "=mychunk", &error));
  g_assert_error (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_SYNTAX);
  g_assert_nonnull (strstr (error->message, "mychunk:1:"));

  g_assert_false (shell_lua_state_load_string (s, "if then", "=other", &error2));
  g_assert_error (error2, SHELL_LUA_ERROR, SHELL_LUA_ERROR_SYNTAX);

  /* compile errors are not counted and emit nothing; nothing is left on the stack */
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 0);
  g_assert_cmpuint (probe.count, ==, 0);
  g_assert_cmpint (lua_gettop (shell_lua_state_get_L (s)), ==, 0);

  g_free (probe.last_message);
  g_object_unref (s);
}

static void
test_error_runtime (void)
{
  ShellLuaState *s = tu_state_new ("sbx", SHELL_LUA_STATE_SANDBOXED);
  ErrorProbe probe = { 0, };
  g_autoptr (GError) error = NULL;

  g_signal_connect (s, "error", G_CALLBACK (on_error), &probe);

  error = tu_do_fails (s, "local function inner() error('boom') end\n"
                          "local function outer() inner() end\n"
                          "outer()",
                       SHELL_LUA_ERROR_RUNTIME, "boom");
  /* the message carries a traceback through the Lua frames */
  g_assert_nonnull (strstr (error->message, "stack traceback:"));
  g_assert_nonnull (strstr (error->message, "inner"));
  g_assert_nonnull (strstr (error->message, "outer"));

  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 1);
  g_assert_cmpuint (probe.count, ==, 1);
  g_assert_cmpstr (probe.last_message, ==, error->message);
  g_assert_cmpint (probe.code, ==, SHELL_LUA_ERROR_RUNTIME);
  g_assert_true (probe.domain == SHELL_LUA_ERROR);

  /* the stack is balanced and the state keeps working */
  g_assert_cmpint (lua_gettop (shell_lua_state_get_L (s)), ==, 0);
  tu_do (s, "x = 1");

  /* non-string error objects */
  tu_assert_fails (s, "error({})", SHELL_LUA_ERROR_RUNTIME, "(error object is a table value)");
  tu_assert_fails (s, "error()", SHELL_LUA_ERROR_RUNTIME, "(error object is a nil value)");
  tu_assert_fails (s, "error(setmetatable({}, {__tostring = function() return 'custom!' end}))",
                   SHELL_LUA_ERROR_RUNTIME, "custom!");
  tu_assert_fails (s, "error(42)", SHELL_LUA_ERROR_RUNTIME, "42");

  /* runtime faults (not just error()) */
  tu_assert_fails (s, "local t = nil; return t.x", SHELL_LUA_ERROR_RUNTIME, "attempt to index");

  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 6);
  g_assert_cmpuint (probe.count, ==, 6);
  shell_lua_state_reset_error_count (s);
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 0);

  g_free (probe.last_message);
  g_object_unref (s);
}

static void
test_error_privileged_is_quiet (void)
{
  ShellLuaState *s = tu_state_new ("priv", SHELL_LUA_STATE_PRIVILEGED);
  g_autoptr (GError) error = NULL;

  /* privileged states log at debug level only: no g_warning to expect */
  g_assert_false (shell_lua_state_do_string (s, "error('quiet')", "=c", &error));
  g_assert_error (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_RUNTIME);
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 1);

  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* Watchdog                                                                  */
/* ------------------------------------------------------------------------ */

static ShellLuaState *
fast_watchdog_state (void)
{
  ShellLuaLimits limits = { .hook_interval = 1000, .callback_budget_us = 50 * 1000 };

  return shell_lua_state_new ("dog", SHELL_LUA_STATE_SANDBOXED, &limits);
}

static void
assert_watchdog (ShellLuaState *s, const char *code)
{
  gint64 start = g_get_monotonic_time ();
  g_autoptr (GError) error = NULL;
  gint64 elapsed_ms;

  error = tu_do_fails (s, code, SHELL_LUA_ERROR_WATCHDOG,
                       "watchdog: callback exceeded its time budget (50 ms)");
  elapsed_ms = (g_get_monotonic_time () - start) / 1000;
  g_assert_cmpint (elapsed_ms, >=, 40);
  g_assert_cmpint (elapsed_ms, <, 5000);
}

static void
test_watchdog_loop (void)
{
  ShellLuaState *s = fast_watchdog_state ();

  assert_watchdog (s, "while true do end");
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 1);

  /* the state is still usable, with a fresh budget */
  tu_do (s, "y = 1 + 1");
  tu_assert_eval_str (s, "y", "2");

  g_object_unref (s);
}

static void
test_watchdog_pcall_swallowing (void)
{
  ShellLuaState *s = fast_watchdog_state ();

  /* a script that catches the watchdog error and carries on is stopped again */
  assert_watchdog (s, "while true do pcall(function() while true do end end) end");
  assert_watchdog (s, "repeat local ok = pcall(function() while true do end end) until false");
  /* xpcall's handler runs after unwinding, still under the watchdog */
  assert_watchdog (s, "xpcall(function() error('x') end, function() while true do end end) return 1");
  assert_watchdog (s, "while true do xpcall(function() while true do end end, function() end) end");
  /* coroutines */
  assert_watchdog (s, "local co = coroutine.wrap(function() while true do end end) "
                      "while true do pcall(co) end");
  assert_watchdog (s, "local co = coroutine.create(function() while true do end end) "
                      "while true do coroutine.resume(co) end");

  /* an error object whose __tostring never returns is cut short as well */
  tu_assert_fails (s, "error(setmetatable({}, {__tostring = function() while true do end end}))",
                   SHELL_LUA_ERROR_RUNTIME, "(error object could not be converted to a string)");

  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 7);
  tu_do (s, "return 1");

  g_object_unref (s);
}

static void
test_watchdog_close_handlers (void)
{
  ShellLuaState *s = fast_watchdog_state ();

  /* to-be-closed variables run while the error unwinds: they must not be a
   * way to keep running past the budget */
  assert_watchdog (s, "local x <close> = setmetatable({}, {__close = function() while true do end end}) "
                      "while true do end");
  assert_watchdog (s, "local x <close> = setmetatable({}, {__close = function() "
                      "while true do pcall(function() while true do end end) end end}) "
                      "while true do end");
  /* the same when the loop is inside a coroutine */
  assert_watchdog (s, "local co = coroutine.wrap(function() "
                      "local x <close> = setmetatable({}, {__close = function() while true do end end}) "
                      "while true do end end) co()");
  /* metamethods are Lua functions like any other */
  assert_watchdog (s, "local t = setmetatable({}, {__index = function(t, k) while true do end end}) return t.x");
  assert_watchdog (s, "table.sort({3, 2, 1, 5, 4}, function(a, b) while true do end end)");
  assert_watchdog (s, "return string.gsub('abc', '%w', function() while true do end end)");
  tu_do (s, "return 1");

  /* a coroutine killed by the watchdog stays dead with its hooks off: later
   * callbacks must not run its __close metamethods either */
  assert_watchdog (s, "co = coroutine.create(function() "
                      "local x <close> = setmetatable({}, {__close = function() while true do end end}) "
                      "while true do end end) coroutine.resume(co) while true do end");
  assert_watchdog (s, "w = coroutine.wrap(function() "
                      "local x <close> = setmetatable({}, {__close = function() while true do end end}) "
                      "while true do end end) pcall(w) while true do end");
  tu_do (s, "local ok, msg = coroutine.close(co) assert(ok == false and msg:find('watchdog', 1, true), tostring(msg))");
  tu_do (s, "local ok, err = pcall(w) assert(not ok and err:find('dead coroutine', 1, true), tostring(err))");
  tu_do (s, "return 1");

  g_object_unref (s);
}

/* Runs @code, which must fail with a runtime, memory or watchdog error (how a
 * runaway recursion ends depends on which limit it reaches first). */
static void
assert_resource_error (ShellLuaState *s, const char *code, const char *message)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *pattern = g_strdup_printf ("[%s] *", shell_lua_state_get_id (s));

  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, pattern);
  g_assert_false (shell_lua_state_do_string (s, code, "=c", &error));
  g_test_assert_expected_messages ();
  /* (a slow build, e.g. under a sanitizer, may run into the time budget first) */
  g_assert_true (g_error_matches (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_RUNTIME) ||
                 g_error_matches (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_MEMORY) ||
                 g_error_matches (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_WATCHDOG));
  if (message != NULL && strstr (error->message, message) == NULL)
    g_error ("error of '%s' lacks '%s': %s", code, message, error->message);
}

static void
test_runaway_recursion_is_an_error (void)
{
  ShellLuaState *s = tu_state_new ("rec", SHELL_LUA_STATE_SANDBOXED);

  /* Lua stack overflow (or the memory limit, whichever comes first), C stack
   * overflow through metamethods and coroutines: errors, not crashes */
  assert_resource_error (s, "local function f() return 1 + f() end return f()", NULL);
  assert_resource_error (s, "local mt = {} mt.__index = function(t, k) return t[k] end "
                            "return setmetatable({}, mt).x", "overflow");
  assert_resource_error (s, "local function f() return coroutine.wrap(f)() end return f()", "overflow");
  /* pcall catches the overflow at the deepest level and everything unwinds normally */
  tu_do (s, "local function f() return pcall(f) end f()");
  tu_do (s, "return 1");
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 3);

  g_object_unref (s);
}

static void
test_sandbox_coroutines (void)
{
  ShellLuaState *s = tu_state_new ("co", SHELL_LUA_STATE_SANDBOXED);

  /* wrap() and close() are replaced in the sandbox (see shell-lua.c); they
   * must otherwise behave like the stock ones */
  tu_do (s,
         "local gen = coroutine.wrap(function(a, b)\n"
         "  local c = coroutine.yield(a + b)\n"
         "  local d = coroutine.yield(c * 2)\n"
         "  return 'done', d\n"
         "end)\n"
         "assert(gen(1, 2) == 3)\n"
         "assert(gen(10) == 20)\n"
         "local r1, r2 = gen('x')\n"
         "assert(r1 == 'done' and r2 == 'x')\n"
         "local ok, err = pcall(gen)\n"
         "assert(not ok and err:find('cannot resume dead coroutine', 1, true), err)\n"
         /* errors keep their value; strings get the position of the call */
         "local bad = coroutine.wrap(function() error('inside') end)\n"
         "local ok, err = pcall(bad)\n"
         "assert(not ok and err:find('inside', 1, true))\n"
         "local obj = {}\n"
         "local ok, e = pcall(coroutine.wrap(function() error(obj) end))\n"
         "assert(not ok and rawequal(e, obj))\n"
         "assert(not pcall(coroutine.wrap, 5))\n"
         /* to-be-closed variables of a failed coroutine are closed */
         "local log = {}\n"
         "local w = coroutine.wrap(function()\n"
         "  local x <close> = setmetatable({}, {__close = function() log[#log + 1] = 'closed' end})\n"
         "  error('boom')\n"
         "end)\n"
         "pcall(w)\n"
         "assert(log[1] == 'closed')\n"
         /* close() on a suspended coroutine runs them too, and reports errors of dead ones */
         "local co = coroutine.create(function()\n"
         "  local x <close> = setmetatable({}, {__close = function() log[#log + 1] = 'closed2' end})\n"
         "  coroutine.yield()\n"
         "end)\n"
         "coroutine.resume(co)\n"
         "assert(coroutine.close(co) == true and log[2] == 'closed2')\n"
         "assert(coroutine.status(co) == 'dead')\n"
         "local co2 = coroutine.create(function() error('x') end)\n"
         "coroutine.resume(co2)\n"
         "local ok2, err2 = coroutine.close(co2)\n"
         "assert(ok2 == false and err2:find('x', 1, true))\n"
         "assert(coroutine.close(coroutine.create(print)) == true)\n"
         "assert(select('#', coroutine.close(coroutine.create(print))) == 1)\n"
         "assert(not pcall(coroutine.close, 5))\n"
         /* generators */
         "local function range(n) return coroutine.wrap(function() for i = 1, n do coroutine.yield(i) end end) end\n"
         "local sum = 0 for i in range(10) do sum = sum + i end assert(sum == 55)\n"
         "assert(coroutine.isyieldable() == false and coroutine.running() ~= nil)\n");

  g_object_unref (s);
}

static void
test_watchdog_privileged_unlimited (void)
{
  ShellLuaState *s = tu_state_new ("priv", SHELL_LUA_STATE_PRIVILEGED);

  /* longer than the 500 ms default budget of a sandboxed state */
  tu_do (s, "local t = os.clock() while os.clock() - t < 0.6 do end");
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 0);

  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* Memory limit                                                              */
/* ------------------------------------------------------------------------ */

static void
test_memory_limit (void)
{
  ShellLuaLimits limits = { .memory_limit = 1024 * 1024 };
  ShellLuaState *s = shell_lua_state_new ("mem", SHELL_LUA_STATE_SANDBOXED, &limits);
  gsize baseline = shell_lua_state_get_memory_usage (s);

  g_assert_cmpuint (baseline, >, 0);
  g_assert_cmpuint (baseline, <, 1024 * 1024);

  tu_assert_fails (s, "local s = string.rep('x', 4 * 1024 * 1024)",
                   SHELL_LUA_ERROR_MEMORY, "not enough memory");
  tu_assert_fails (s, "local t = {} for i = 1, 1e9 do t[i] = {i} end",
                   SHELL_LUA_ERROR_MEMORY, "not enough memory");
  tu_assert_fails (s, "local t = {} for i = 1, 1e9 do t[i] = ('x'):rep(100) .. i end",
                   SHELL_LUA_ERROR_MEMORY, "not enough memory");

  /* garbage was collected after the failure and the state is still usable */
  g_assert_cmpuint (shell_lua_state_get_memory_usage (s), <=, 1024 * 1024);
  tu_do (s, "z = {1, 2, 3}");
  tu_assert_eval_str (s, "#z", "3");
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 3);

  g_object_unref (s);
}

static void
test_memory_accounting (void)
{
  gsize total_before = shell_lua_get_total_memory ();
  ShellLuaState *s = tu_state_new ("acct", SHELL_LUA_STATE_PRIVILEGED);
  gsize usage;

  g_assert_cmpuint (shell_lua_get_total_memory (), >=, total_before + shell_lua_state_get_memory_usage (s));

  tu_do (s, "garbage = {} for i = 1, 20000 do garbage[i] = {i} end");
  usage = shell_lua_state_get_memory_usage (s);
  tu_do (s, "garbage = nil");
  shell_lua_state_gc (s);
  g_assert_cmpuint (shell_lua_state_get_memory_usage (s), <, usage);

  g_object_unref (s);
  /* closing a state gives everything back */
  g_assert_cmpuint (shell_lua_get_total_memory (), ==, total_before);
}

/* ------------------------------------------------------------------------ */
/* require()                                                                 */
/* ------------------------------------------------------------------------ */

static char *
make_require_root (char **root_out)
{
  char *tmp = tu_tmpdir_new ();
  g_autofree char *root = g_build_filename (tmp, "root", NULL);

  tu_write_file (root, "mod.lua",
                 "local name, path = ...\n"
                 "SHARED = 42\n"
                 "COUNTER = (COUNTER or 0) + 1\n"
                 "return { name = name, path = path, has_os = (os ~= nil), has_io = (io ~= nil),\n"
                 "         has_load = (load ~= nil), counter = COUNTER }\n");
  tu_write_file (root, "sees.lua", "return SHARED\n");
  tu_write_file (root, "pkg/init.lua", "return { kind = 'pkg' }\n");
  tu_write_file (root, "a/b.lua", "return 'a.b'\n");
  tu_write_file (root, "a/c/init.lua", "return require('a.b') .. '+c'\n");
  tu_write_file (root, "a/both.lua", "return 'file'\n");
  tu_write_file (root, "a/both/init.lua", "return 'init'\n");
  tu_write_file (root, "cyc1.lua", "return require('cyc2')\n");
  tu_write_file (root, "cyc2.lua", "return require('cyc1')\n");
  tu_write_file (root, "bad.lua", "this is not lua\n");
  tu_write_file (root, "empty.lua", "");
  tu_write_file (root, "boom.lua", "error('module exploded')\n");
  /* outside the root */
  tu_write_file (tmp, "secret.lua", "return 'SECRET'\n");

  *root_out = g_steal_pointer (&root);
  return tmp;
}

static void
test_require (void)
{
  g_autofree char *root = NULL;
  g_autofree char *tmp = make_require_root (&root);
  g_autoptr (GFile) root_file = g_file_new_for_path (root);
  ShellLuaState *s = tu_state_new ("req", SHELL_LUA_STATE_SANDBOXED);

  shell_lua_state_add_search_root (s, root_file);

  /* a plain module: called with (name, path), cached */
  tu_do (s, "m = require('mod')");
  tu_assert_eval_str (s, "m.name", "mod");
  tu_assert_eval_true (s, "m.path:sub(-7) == 'mod.lua' and m.path:find('root', 1, true) ~= nil");
  tu_assert_eval_true (s, "rawequal(require('mod'), m) and m.counter == 1");
  /* a first load also returns the loader data (the path) */
  tu_assert_eval_true (s, "(function() local mod, path = require('pkg') "
                          "return mod.kind == 'pkg' and path:sub(-8) == 'init.lua' end)()");
  /* the sandbox holds inside modules */
  tu_assert_eval_true (s, "m.has_os == false and m.has_io == false and m.has_load == false");
  /* modules of one state share the environment */
  tu_assert_eval_str (s, "require('sees')", "42");
  tu_assert_eval_str (s, "SHARED", "42");

  /* pkg/init.lua, dotted names, init.lua inside a dotted name */
  tu_assert_eval_str (s, "require('pkg').kind", "pkg");
  tu_assert_eval_str (s, "require('a.b')", "a.b");
  tu_assert_eval_str (s, "require('a.c')", "a.b+c");
  /* <name>.lua wins over <name>/init.lua */
  tu_assert_eval_str (s, "require('a.both')", "file");

  /* a module returning nothing is cached as true */
  tu_assert_eval_str (s, "require('empty')", "true");
  tu_assert_eval_true (s, "require('shell') == shell");

  g_object_unref (s);
  tu_rm_rf (tmp);
}

static void
test_require_errors (void)
{
  g_autofree char *root = NULL;
  g_autofree char *tmp = make_require_root (&root);
  g_autoptr (GFile) root_file = g_file_new_for_path (root);
  ShellLuaState *s = tu_state_new ("req", SHELL_LUA_STATE_SANDBOXED);
  g_autofree char *long_name = g_strnfill (300, 'a');
  g_autofree char *long_code = g_strdup_printf ("require('%s')", long_name);
  const char *invalid[] = {
    "../secret", "..", ".", ".x", "x.", "a..b", "a/b", "a\\\\b", "", "x y", "-x", "a-b", "x.\\n",
  };
  guint i;

  shell_lua_state_add_search_root (s, root_file);

  /* require("../escape") and friends: SANDBOX, never touching the file system */
  for (i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      g_autofree char *code = g_strdup_printf ("require('%s')", invalid[i]);

      tu_assert_fails (s, code, SHELL_LUA_ERROR_SANDBOX, "sandbox: invalid module name");
    }
  tu_assert_fails (s, long_code, SHELL_LUA_ERROR_SANDBOX, "invalid module name");
  tu_assert_fails (s, "require(nil)", SHELL_LUA_ERROR_RUNTIME, "bad argument");

  /* not found: the tried files are listed */
  {
    g_autoptr (GError) error = tu_do_fails (s, "require('nope.deeper')",
                                            SHELL_LUA_ERROR_RUNTIME, "module 'nope.deeper' not found:");
    g_assert_nonnull (strstr (error->message, "nope/deeper.lua"));
    g_assert_nonnull (strstr (error->message, "nope/deeper/init.lua"));
  }

  /* cycles */
  tu_assert_fails (s, "require('cyc1')", SHELL_LUA_ERROR_RUNTIME,
                   "loop or previous error loading module");

  /* a module that fails to compile or run stays failed */
  tu_assert_fails (s, "require('bad')", SHELL_LUA_ERROR_RUNTIME, "error loading module 'bad'");
  tu_assert_fails (s, "require('boom')", SHELL_LUA_ERROR_RUNTIME, "module exploded");
  tu_assert_fails (s, "require('boom')", SHELL_LUA_ERROR_RUNTIME,
                   "loop or previous error loading module 'boom'");

  /* a pcall in Lua can catch the error; the state is fine */
  tu_assert_eval_true (s, "(function() local ok, e = pcall(require, '../secret') "
                          "return not ok and e:find('sandbox:', 1, true) ~= nil end)()");
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, G_N_ELEMENTS (invalid) + 7);

  g_object_unref (s);
  tu_rm_rf (tmp);
}

static void
test_require_roots_order (void)
{
  g_autofree char *tmp = tu_tmpdir_new ();
  g_autofree char *r1 = g_build_filename (tmp, "r1", NULL);
  g_autofree char *r2 = g_build_filename (tmp, "r2", NULL);
  g_autoptr (GFile) f1 = g_file_new_for_path (r1);
  g_autoptr (GFile) f2 = g_file_new_for_path (r2);
  ShellLuaState *s = tu_state_new ("req", SHELL_LUA_STATE_SANDBOXED);

  tu_write_file (r1, "both.lua", "return 'first'\n");
  tu_write_file (r2, "both.lua", "return 'second'\n");
  tu_write_file (r2, "only2.lua", "return 'only2'\n");
  shell_lua_state_add_search_root (s, f1);
  shell_lua_state_add_search_root (s, f2);

  tu_assert_eval_str (s, "require('both')", "first");
  tu_assert_eval_str (s, "require('only2')", "only2");

  g_object_unref (s);
  tu_rm_rf (tmp);
}

static void
test_require_privileged (void)
{
  g_autofree char *root = NULL;
  g_autofree char *tmp = make_require_root (&root);
  g_autoptr (GFile) root_file = g_file_new_for_path (root);
  ShellLuaState *s = tu_state_new ("priv", SHELL_LUA_STATE_PRIVILEGED);

  shell_lua_state_add_search_root (s, root_file);

  /* roots first, then the stock searchers */
  tu_assert_eval_str (s, "require('pkg').kind", "pkg");
  tu_assert_eval_true (s, "require('string') == string");
  tu_assert_eval_true (s, "require('shell') == shell and package.loaded.shell == shell");
  /* privileged modules see the real libraries */
  tu_assert_eval_str (s, "require('mod').has_os", "true");

  g_object_unref (s);
  tu_rm_rf (tmp);
}

/* ------------------------------------------------------------------------ */
/* Teardown and destroy                                                      */
/* ------------------------------------------------------------------------ */

static void
append_to_gstring (gpointer data)
{
  GString *log = ((gpointer *) data)[0];
  const char *what = ((gpointer *) data)[1];

  g_string_append (log, what);
}

static void
test_teardown_lifo (void)
{
  ShellLuaState *s = tu_state_new ("td", SHELL_LUA_STATE_SANDBOXED);
  GString *log = g_string_new (NULL);
  gpointer a[2] = { log, (gpointer) "a" };
  gpointer b[2] = { log, (gpointer) "b" };
  gpointer c[2] = { log, (gpointer) "c" };
  gpointer d[2] = { log, (gpointer) "d" };
  guint ida, idb, idc, idd;

  ida = shell_lua_state_add_teardown (s, append_to_gstring, a);
  idb = shell_lua_state_add_teardown (s, append_to_gstring, b);
  idc = shell_lua_state_add_teardown (s, append_to_gstring, c);
  idd = shell_lua_state_add_teardown (s, append_to_gstring, d);
  g_assert_true (ida != 0 && idb != 0 && idc != 0 && idd != 0);
  g_assert_true (ida != idb && idb != idc && idc != idd);

  /* removal does not call the function; unknown ids and 0 are ignored */
  shell_lua_state_remove_teardown (s, idc);
  shell_lua_state_remove_teardown (s, idc);
  shell_lua_state_remove_teardown (s, 0);
  shell_lua_state_remove_teardown (s, 9999);
  g_assert_cmpstr (log->str, ==, "");

  shell_lua_state_destroy (s);
  g_assert_cmpstr (log->str, ==, "dba");

  /* a second destroy runs nothing again */
  shell_lua_state_destroy (s);
  g_assert_cmpstr (log->str, ==, "dba");

  g_object_unref (s);
  g_assert_cmpstr (log->str, ==, "dba");
  g_string_free (log, TRUE);
}

static void
test_teardown_runs_on_dispose (void)
{
  ShellLuaState *s = tu_state_new ("td", SHELL_LUA_STATE_SANDBOXED);
  GString *log = g_string_new (NULL);
  gpointer a[2] = { log, (gpointer) "a" };

  shell_lua_state_add_teardown (s, append_to_gstring, a);
  g_object_unref (s);
  g_assert_cmpstr (log->str, ==, "a");
  g_string_free (log, TRUE);
}

typedef struct {
  guint   destroyed;
  gulong  id;
} DestroyedProbe;

static void
on_destroyed (ShellLuaState *state, DestroyedProbe *probe)
{
  probe->destroyed++;
  /* by the time the signal runs the lua_State is closed */
  g_assert_null (shell_lua_state_get_L (state));
  g_assert_false (shell_lua_state_is_alive (state));
}

static void
test_destroy_lifecycle (void)
{
  guint count_before = shell_lua_get_state_count ();
  gsize memory_before = shell_lua_get_total_memory ();
  ShellLuaState *s = tu_state_new ("life", SHELL_LUA_STATE_SANDBOXED);
  DestroyedProbe probe = { 0, };
  g_autoptr (GError) error = NULL;

  g_signal_connect (s, "destroyed", G_CALLBACK (on_destroyed), &probe);
  g_assert_cmpuint (shell_lua_get_state_count (), ==, count_before + 1);
  g_assert_cmpuint (shell_lua_get_total_memory (), >, memory_before);
  g_assert_cmpstr (shell_lua_state_get_id (s), ==, "life");
  g_assert_cmpint (shell_lua_state_get_kind (s), ==, SHELL_LUA_STATE_SANDBOXED);
  g_assert_nonnull (shell_lua_state_get_L (s));

  shell_lua_state_destroy (s);
  g_assert_cmpuint (probe.destroyed, ==, 1);
  g_assert_false (shell_lua_state_is_alive (s));
  g_assert_cmpuint (shell_lua_get_state_count (), ==, count_before);
  g_assert_cmpuint (shell_lua_get_total_memory (), ==, memory_before);

  /* idempotent: no second signal, no double decrement */
  shell_lua_state_destroy (s);
  g_assert_cmpuint (probe.destroyed, ==, 1);
  g_assert_cmpuint (shell_lua_get_state_count (), ==, count_before);

  /* a dead state answers DEAD everywhere */
  g_assert_false (shell_lua_state_load_string (s, "return 1", "=c", &error));
  g_assert_error (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_DEAD);
  g_clear_error (&error);
  g_assert_false (shell_lua_state_do_string (s, "return 1", "=c", &error));
  g_assert_error (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_DEAD);

  /* dispose of an already destroyed state does not signal again */
  g_object_unref (s);
  g_assert_cmpuint (probe.destroyed, ==, 1);
  g_assert_cmpuint (shell_lua_get_state_count (), ==, count_before);
}

static void
test_dispose_destroys (void)
{
  guint count_before = shell_lua_get_state_count ();
  ShellLuaState *s = tu_state_new ("life", SHELL_LUA_STATE_PRIVILEGED);
  DestroyedProbe probe = { 0, };

  g_signal_connect (s, "destroyed", G_CALLBACK (on_destroyed), &probe);
  g_assert_cmpuint (shell_lua_get_state_count (), ==, count_before + 1);
  g_object_unref (s);
  g_assert_cmpuint (probe.destroyed, ==, 1);
  g_assert_cmpuint (shell_lua_get_state_count (), ==, count_before);
}

typedef struct {
  ShellLuaState *state;
  guint          destroyed_signals;
  gboolean       marker_ran;
  guint          signals_at_marker;
  gboolean       alive_after_destroy;
  gboolean       L_valid_at_marker;
} DeferCtx;

static int
cf_destroy_me (lua_State *L)
{
  DeferCtx *ctx = lua_touserdata (L, lua_upvalueindex (1));

  shell_lua_state_destroy (ctx->state);
  ctx->alive_after_destroy = shell_lua_state_is_alive (ctx->state);
  return 0;
}

static int
cf_marker (lua_State *L)
{
  DeferCtx *ctx = lua_touserdata (L, lua_upvalueindex (1));

  ctx->marker_ran = TRUE;
  ctx->signals_at_marker = ctx->destroyed_signals;
  ctx->L_valid_at_marker = shell_lua_state_get_L (ctx->state) != NULL;
  return 0;
}

static void
on_defer_destroyed (ShellLuaState *state, DeferCtx *ctx)
{
  ctx->destroyed_signals++;
}

static void
test_destroy_deferred_in_callback (void)
{
  guint count_before = shell_lua_get_state_count ();
  ShellLuaState *s = tu_state_new ("defer", SHELL_LUA_STATE_SANDBOXED);
  DeferCtx ctx = { .state = s };
  g_autoptr (GError) error = NULL;

  g_signal_connect (s, "destroyed", G_CALLBACK (on_defer_destroyed), &ctx);
  tu_set_cfunction_with_data (s, "destroy_me", cf_destroy_me, &ctx);
  tu_set_cfunction_with_data (s, "marker", cf_marker, &ctx);

  /* the chunk keeps running on a still-valid lua_State, then the pcall
   * reports DEAD and only then is the state torn down */
  error = tu_do_fails (s, "destroy_me() marker() return 1", SHELL_LUA_ERROR_DEAD, "destroyed");
  g_assert_true (ctx.marker_ran);
  g_assert_true (ctx.L_valid_at_marker);
  g_assert_cmpuint (ctx.signals_at_marker, ==, 0);
  g_assert_false (ctx.alive_after_destroy);
  g_assert_cmpuint (ctx.destroyed_signals, ==, 1);
  g_assert_null (shell_lua_state_get_L (s));
  g_assert_cmpuint (shell_lua_get_state_count (), ==, count_before);

  g_object_unref (s);
}

static void
test_destroy_deferred_with_error (void)
{
  ShellLuaState *s = tu_state_new ("defer", SHELL_LUA_STATE_SANDBOXED);
  DeferCtx ctx = { .state = s };
  g_autoptr (GError) error = NULL;

  g_signal_connect (s, "destroyed", G_CALLBACK (on_defer_destroyed), &ctx);
  tu_set_cfunction_with_data (s, "destroy_me", cf_destroy_me, &ctx);

  /* the Lua error wins over DEAD and is reported before the teardown */
  error = tu_do_fails (s, "destroy_me() error('after destroy')", SHELL_LUA_ERROR_RUNTIME,
                       "after destroy");
  g_assert_cmpuint (ctx.destroyed_signals, ==, 1);
  g_assert_false (shell_lua_state_is_alive (s));

  g_object_unref (s);
}

typedef struct {
  ShellLuaState *state;
  guint          calls;
  GError        *first_error;
} RecurseCtx;

static int
cf_recurse (lua_State *L)
{
  RecurseCtx *ctx = lua_touserdata (L, lua_upvalueindex (1));
  GError *error = NULL;
  gboolean loaded;

  ctx->calls++;
  loaded = shell_lua_state_load_string (ctx->state, "recurse()", "=r", NULL);
  g_assert_true (loaded);
  if (!shell_lua_state_pcall (ctx->state, 0, 0, &error))
    {
      if (ctx->first_error == NULL)
        ctx->first_error = error;
      else
        g_error_free (error);
    }
  return 0;
}

static void
test_pcall_depth_limit (void)
{
  ShellLuaState *s = tu_state_new ("deep", SHELL_LUA_STATE_PRIVILEGED);
  RecurseCtx ctx = { .state = s };

  tu_set_cfunction_with_data (s, "recurse", cf_recurse, &ctx);
  tu_do (s, "recurse()");

  /* the outermost pcall is level 1; the 51st nested one is refused */
  g_assert_cmpuint (ctx.calls, ==, 50);
  g_assert_nonnull (ctx.first_error);
  g_assert_error (ctx.first_error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_RUNTIME);
  g_assert_nonnull (strstr (ctx.first_error->message, "nesting"));
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 1);
  g_clear_error (&ctx.first_error);

  /* back to normal afterwards */
  tu_do (s, "x = 1");
  g_assert_cmpint (lua_gettop (shell_lua_state_get_L (s)), ==, 0);

  g_object_unref (s);
}

static ShellLuaState *seen_current = NULL;

static int
cf_note_current (lua_State *L)
{
  seen_current = shell_lua_state_get_current ();
  return 0;
}

static int
cf_run_other (lua_State *L)
{
  ShellLuaState *other = lua_touserdata (L, lua_upvalueindex (1));
  g_autoptr (GError) error = NULL;
  gboolean ok;

  ok = shell_lua_state_do_string (other, "note()", "=o", &error);
  g_assert_no_error (error);
  g_assert_true (ok);
  /* back in the outer state's call */
  cf_note_current (L);
  return 0;
}

static void
test_current_state (void)
{
  ShellLuaState *outer = tu_state_new ("outer", SHELL_LUA_STATE_PRIVILEGED);
  ShellLuaState *inner = tu_state_new ("inner", SHELL_LUA_STATE_PRIVILEGED);

  g_assert_null (shell_lua_state_get_current ());

  tu_set_cfunction (outer, "note", cf_note_current);
  tu_do (outer, "note()");
  g_assert_true (seen_current == outer);

  tu_set_cfunction (inner, "note", cf_note_current);
  tu_set_cfunction_with_data (outer, "run_other", cf_run_other, inner);
  seen_current = NULL;
  tu_do (outer, "run_other()");
  g_assert_true (seen_current == outer);   /* last note: the outer state again */
  g_assert_null (shell_lua_state_get_current ());

  g_object_unref (inner);
  g_object_unref (outer);
}

static void
test_refs_and_owner (void)
{
  ShellLuaState *s = tu_state_new ("refs", SHELL_LUA_STATE_PRIVILEGED);
  lua_State *L = shell_lua_state_get_L (s);
  int ref, ref2;

  lua_pushstring (L, "kept");
  ref = shell_lua_state_ref (s);
  g_assert_true (ref != LUA_NOREF);
  g_assert_cmpint (lua_gettop (L), ==, 0);

  lua_pushstring (L, "other");
  ref2 = shell_lua_state_ref (s);
  g_assert_cmpint (ref, !=, ref2);

  shell_lua_state_push_ref (s, ref);
  g_assert_cmpstr (lua_tostring (L, -1), ==, "kept");
  lua_pop (L, 1);

  shell_lua_state_unref (s, ref);
  shell_lua_state_unref (s, LUA_NOREF);   /* harmless */
  shell_lua_state_push_ref (s, ref2);
  g_assert_cmpstr (lua_tostring (L, -1), ==, "other");
  lua_pop (L, 1);

  g_assert_null (shell_lua_state_get_owner (s));
  shell_lua_state_set_owner (s, s);
  g_assert_true (shell_lua_state_get_owner (s) == s);
  shell_lua_state_set_owner (s, NULL);

  /* a late unref after destruction is ignored */
  shell_lua_state_destroy (s);
  shell_lua_state_unref (s, ref2);

  g_object_unref (s);
}

static void
test_privileged_stdlib (void)
{
  ShellLuaState *s = tu_state_new ("priv", SHELL_LUA_STATE_PRIVILEGED);

  g_assert_cmpint (shell_lua_state_get_kind (s), ==, SHELL_LUA_STATE_PRIVILEGED);
  tu_assert_eval_true (s, "os ~= nil and io ~= nil and debug ~= nil and package ~= nil");
  tu_assert_eval_true (s, "load ~= nil and type(string.dump) == 'function'");
  tu_assert_eval_true (s, "type(os.time()) == 'number'");
  tu_assert_eval_true (s, "type(shell) == 'table' and shell.api_version.major == 1");
  /* privileged code may use the full collectgarbage */
  tu_do (s, "collectgarbage('collect')");
  /* and see Lua warnings */
  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "[priv] Lua warning: hello world");
  tu_do (s, "warn('hello ', 'world')");
  g_test_assert_expected_messages ();
  tu_do (s, "warn('@on')");

  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* Time accounting                                                           */
/* ------------------------------------------------------------------------ */

#define CALLBACKS_LUA \
  "local M = {}\n" \
  "function M.quick () end\n" \
  "function M.busy (ms)\n" \
  "  local t = shell.time.monotonic ()\n" \
  "  while shell.time.monotonic () - t < ms / 1000 do end\n" \
  "end\n" \
  "return M\n"

/* Pushes the module table of callbacks.lua, loaded from a root of @s */
static void
load_callbacks (ShellLuaState *s,
                const char    *root)
{
  g_autofree char *path = g_build_filename (root, "callbacks.lua", NULL);
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GFile) root_file = g_file_new_for_path (root);
  g_autoptr (GError) error = NULL;

  shell_lua_state_add_search_root (s, root_file);
  g_assert_true (shell_lua_state_load_file (s, file, &error));
  g_assert_true (shell_lua_state_pcall (s, 0, 1, &error));
  g_assert_no_error (error);
}

/* As the trampolines of the API: arg 1 is the function to run, the rest
 * its arguments */
static int
callee_trampoline (lua_State *L)
{
  shell_lua_state_note_callee (L, 1);
  lua_call (L, lua_gettop (L) - 1, 0);
  return 0;
}

/* Calls M[name] (ms) from C, through a trampoline or directly */
static void
call_callback (ShellLuaState *s,
               const char    *name,
               int            ms,
               gboolean       trampoline)
{
  lua_State *L = shell_lua_state_get_L (s);
  g_autoptr (GError) error = NULL;

  if (trampoline)
    lua_pushcfunction (L, callee_trampoline);
  lua_getfield (L, 1, name);
  lua_pushinteger (L, ms);
  g_assert_true (shell_lua_state_pcall (s, trampoline ? 2 : 1, 0, &error));
  g_assert_no_error (error);
}

typedef struct {
  guint  count;
  char  *last;
} Messages;

static void
count_message (const char     *domain,
               GLogLevelFlags  level,
               const char     *message,
               gpointer        user_data)
{
  Messages *messages = user_data;

  messages->count++;
  g_free (messages->last);
  messages->last = g_strdup (message);
}

static void
test_stats (void)
{
  g_autofree char *tmp = tu_tmpdir_new ();
  g_autofree char *root = g_build_filename (tmp, "root", NULL);
  ShellLuaState *s = tu_state_new ("stats", SHELL_LUA_STATE_SANDBOXED);
  Messages messages = { 0, NULL };
  ShellLuaStats stats;
  guint handler;

  shell_lua_state_get_stats (s, &stats);
  g_assert_cmpuint (stats.calls, ==, 0);
  g_assert_null (stats.longest_where);

  tu_write_file (root, "callbacks.lua", CALLBACKS_LUA);
  load_callbacks (s, root);
  shell_lua_state_get_stats (s, &stats);
  g_assert_cmpuint (stats.calls, ==, 1);

  handler = g_log_set_handler ("ShellLua", G_LOG_LEVEL_MESSAGE, count_message, &messages);

  /* each call from outside Lua counts once */
  call_callback (s, "quick", 0, TRUE);
  call_callback (s, "quick", 0, FALSE);
  shell_lua_state_get_stats (s, &stats);
  g_assert_cmpuint (stats.calls, ==, 3);

  /* a call longer than a frame: the longest, by the function it ran (a
   * trampoline names it), reported */
  call_callback (s, "busy", 30, TRUE);
  shell_lua_state_get_stats (s, &stats);
  g_assert_cmpuint (stats.calls, ==, 4);
  g_assert_cmpint (stats.longest_us, >=, 30000);
  g_assert_cmpint (stats.total_us, >=, stats.longest_us);
  g_assert_cmpstr (stats.longest_where, ==, "callbacks.lua:3");
  g_assert_cmpuint (stats.slow_calls, >=, 1);
  g_assert_cmpuint (messages.count, ==, 1);
  g_assert_true (g_pattern_match_simple ("[stats] a call took * ms (callbacks.lua:3)", messages.last));

  /* called directly, the function is named too; a second slow call within
   * 10 seconds is counted, not reported */
  call_callback (s, "busy", 40, FALSE);
  shell_lua_state_get_stats (s, &stats);
  g_assert_cmpint (stats.longest_us, >=, 40000);
  g_assert_cmpstr (stats.longest_where, ==, "callbacks.lua:3");
  g_assert_cmpuint (stats.slow_calls, >=, 2);
  g_assert_cmpuint (messages.count, ==, 1);

  g_log_remove_handler ("ShellLua", handler);
  g_free (messages.last);
  g_object_unref (s);
  tu_rm_rf (tmp);
}

static ShellLuaState *nested_state;

/* A function of one state that calls into another, as a signal emitted by
 * an API call can run another extension's callback */
static int
call_nested (lua_State *L)
{
  lua_State *nested_L = shell_lua_state_get_L (nested_state);
  g_autoptr (GError) error = NULL;

  lua_pushcfunction (nested_L, callee_trampoline);
  lua_getfield (nested_L, 1, "busy");
  lua_pushinteger (nested_L, 50);
  g_assert_true (shell_lua_state_pcall (nested_state, 2, 0, &error));
  return 0;
}

/* The time of the calls into another state counts for that state only */
static void
test_stats_nested (void)
{
  g_autofree char *tmp = tu_tmpdir_new ();
  g_autofree char *root = g_build_filename (tmp, "root", NULL);
  ShellLuaState *outer = tu_state_new ("outer", SHELL_LUA_STATE_PRIVILEGED);
  ShellLuaStats outer_stats, inner_stats;

  tu_write_file (root, "callbacks.lua", CALLBACKS_LUA);
  nested_state = tu_state_new ("inner", SHELL_LUA_STATE_PRIVILEGED);
  load_callbacks (nested_state, root);

  lua_register (shell_lua_state_get_L (outer), "call_nested", call_nested);
  tu_do (outer, "call_nested ()");

  shell_lua_state_get_stats (outer, &outer_stats);
  shell_lua_state_get_stats (nested_state, &inner_stats);
  g_assert_cmpuint (outer_stats.calls, ==, 1);
  g_assert_cmpuint (inner_stats.calls, ==, 2);
  g_assert_cmpint (inner_stats.longest_us, >=, 50000);
  g_assert_cmpstr (inner_stats.longest_where, ==, "callbacks.lua:3");
  g_assert_cmpint (outer_stats.total_us, <, inner_stats.longest_us);

  g_clear_object (&nested_state);
  g_object_unref (outer);
  tu_rm_rf (tmp);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/shell-lua/state/sandbox/globals", test_sandbox_globals);
  g_test_add_func ("/shell-lua/state/sandbox/setmetatable-gc", test_sandbox_setmetatable_gc);
  g_test_add_func ("/shell-lua/state/sandbox/collectgarbage", test_sandbox_collectgarbage);
  g_test_add_func ("/shell-lua/state/sandbox/api-version", test_sandbox_api_version);
  g_test_add_func ("/shell-lua/state/sandbox/xpcall", test_sandbox_xpcall);
  g_test_add_func ("/shell-lua/state/sandbox/print", test_sandbox_print);
  g_test_add_func ("/shell-lua/state/sandbox/binary-chunks", test_binary_chunks_rejected);
  g_test_add_func ("/shell-lua/state/error/syntax", test_error_syntax);
  g_test_add_func ("/shell-lua/state/error/runtime", test_error_runtime);
  g_test_add_func ("/shell-lua/state/error/privileged-quiet", test_error_privileged_is_quiet);
  g_test_add_func ("/shell-lua/state/watchdog/loop", test_watchdog_loop);
  g_test_add_func ("/shell-lua/state/watchdog/pcall-swallowing", test_watchdog_pcall_swallowing);
  g_test_add_func ("/shell-lua/state/watchdog/close-handlers", test_watchdog_close_handlers);
  g_test_add_func ("/shell-lua/state/watchdog/runaway-recursion", test_runaway_recursion_is_an_error);
  g_test_add_func ("/shell-lua/state/sandbox/coroutines", test_sandbox_coroutines);
  g_test_add_func ("/shell-lua/state/watchdog/privileged-unlimited", test_watchdog_privileged_unlimited);
  g_test_add_func ("/shell-lua/state/memory/limit", test_memory_limit);
  g_test_add_func ("/shell-lua/state/memory/accounting", test_memory_accounting);
  g_test_add_func ("/shell-lua/state/require/basic", test_require);
  g_test_add_func ("/shell-lua/state/require/errors", test_require_errors);
  g_test_add_func ("/shell-lua/state/require/roots-order", test_require_roots_order);
  g_test_add_func ("/shell-lua/state/require/privileged", test_require_privileged);
  g_test_add_func ("/shell-lua/state/teardown/lifo", test_teardown_lifo);
  g_test_add_func ("/shell-lua/state/teardown/dispose", test_teardown_runs_on_dispose);
  g_test_add_func ("/shell-lua/state/destroy/lifecycle", test_destroy_lifecycle);
  g_test_add_func ("/shell-lua/state/destroy/dispose", test_dispose_destroys);
  g_test_add_func ("/shell-lua/state/destroy/deferred", test_destroy_deferred_in_callback);
  g_test_add_func ("/shell-lua/state/destroy/deferred-with-error", test_destroy_deferred_with_error);
  g_test_add_func ("/shell-lua/state/pcall/depth-limit", test_pcall_depth_limit);
  g_test_add_func ("/shell-lua/state/pcall/current-state", test_current_state);
  g_test_add_func ("/shell-lua/state/refs-and-owner", test_refs_and_owner);
  g_test_add_func ("/shell-lua/state/privileged/stdlib", test_privileged_stdlib);
  g_test_add_func ("/shell-lua/state/stats", test_stats);
  g_test_add_func ("/shell-lua/state/stats/nested", test_stats_nested);

  return g_test_run ();
}
