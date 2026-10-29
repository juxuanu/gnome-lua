/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.log, shell.timer, shell.settings, shell.gettext (docs/api/core.md).
 *
 * Needs GSETTINGS_BACKEND=memory and GSETTINGS_SCHEMA_DIR pointing at the
 * compiled tests/schemas (tests/meson.build sets both).
 */

#include "test-utils.h"

#define TEST_SCHEMA "org.gnome-lua.test"

static int bumps = 0;

static int
cf_bump (lua_State *L)
{
  bumps++;
  return 0;
}

static ShellLuaState *
new_state (const char *id, ShellLuaStateKind kind)
{
  ShellLuaState *s = tu_state_new (id, kind);

  bumps = 0;
  tu_set_cfunction (s, "bump", cf_bump);
  return s;
}

typedef struct {
  const int *counter;
  int        target;
} AtLeast;

static gboolean
pred_at_least (gpointer data)
{
  AtLeast *a = data;

  return *a->counter >= a->target;
}

/* Wait (bounded) until @counter reaches @target. */
static void
wait_for_count (const int *counter, int target)
{
  AtLeast a = { counter, target };

  if (!tu_wait_for (pred_at_least, &a, 3000))
    g_error ("timed out waiting for the counter to reach %d (is %d)", target, *counter);
}

/* ------------------------------------------------------------------------ */
/* shell.log                                                                 */
/* ------------------------------------------------------------------------ */

static void
test_log (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);

  g_test_expect_message ("shell-lua/core", G_LOG_LEVEL_WARNING, "w 1");
  tu_do (s, "shell.log.warning('w %d', 1)");
  /* "error" maps to a critical: G_LOG_LEVEL_ERROR would abort */
  g_test_expect_message ("shell-lua/core", G_LOG_LEVEL_CRITICAL, "e x");
  tu_do (s, "shell.log.error('e %s', 'x')");
  g_test_expect_message ("shell-lua/core", G_LOG_LEVEL_INFO, "i");
  tu_do (s, "shell.log.info('i')");
  g_test_expect_message ("shell-lua/core", G_LOG_LEVEL_DEBUG, "d 2.5");
  tu_do (s, "shell.log.debug('d %.1f', 2.5)");
  /* shell.log(fmt, ...) is the same as info */
  g_test_expect_message ("shell-lua/core", G_LOG_LEVEL_INFO, "alias y");
  tu_do (s, "shell.log('alias %s', 'y')");
  /* string.format semantics, so a literal percent is written %% */
  g_test_expect_message ("shell-lua/core", G_LOG_LEVEL_INFO, "100%");
  tu_do (s, "shell.log.info('100%%')");
  g_test_assert_expected_messages ();

  /* argument errors */
  tu_assert_fails (s, "shell.log.info(nil)", SHELL_LUA_ERROR_RUNTIME, "string expected, got nil");
  tu_assert_fails (s, "shell.log.info('%d', 'x')", SHELL_LUA_ERROR_RUNTIME, "bad argument");
  tu_assert_fails (s, "shell.log()", SHELL_LUA_ERROR_RUNTIME, "string expected, got no value");

  g_object_unref (s);
}

static void
test_log_domain_follows_state_id (void)
{
  ShellLuaState *s = new_state ("my.ext@example", SHELL_LUA_STATE_SANDBOXED);

  g_test_expect_message ("shell-lua/my.ext@example", G_LOG_LEVEL_WARNING, "from the sandbox");
  tu_do (s, "shell.log.warning('from the sandbox')");
  g_test_assert_expected_messages ();

  g_object_unref (s);
}

/* What an extension logs on purpose never takes the shell down: this program
 * runs with G_DEBUG=fatal-warnings and g_test_init () makes warnings and
 * criticals fatal, and these messages are not announced with
 * g_test_expect_message (), so the test aborts if the logging is fatal. */
static void
test_log_is_not_fatal (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);
  GLogLevelFlags before, after;

  before = g_log_set_always_fatal (G_LOG_FATAL_MASK);
  g_log_set_always_fatal (before);
  g_assert_true ((before & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL)) ==
                 (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL));

  tu_do (s, "shell.log.warning('on purpose: warning')");
  tu_do (s, "shell.log.error('on purpose: error')");
  tu_do (s, "shell.log.warning('%s', 'on purpose again')");

  /* the mask of the shell is back, also after an error in the format */
  tu_assert_fails (s, "shell.log.error('%d', 'x')", SHELL_LUA_ERROR_RUNTIME, "bad argument");
  after = g_log_set_always_fatal (before);
  g_assert_cmphex (after, ==, before);

  g_object_unref (s);
}

static void
test_log_fatal (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);
  g_autoptr (GError) error = NULL;

  g_assert_true (tu_eval_bool (s, "type(shell.log.fatal) == 'function'"));

  /* logged as a critical, then raised with the formatted message */
  g_test_expect_message ("shell-lua/core", G_LOG_LEVEL_CRITICAL, "boom 3");
  tu_assert_fails (s, "shell.log.fatal('boom %d', 3)", SHELL_LUA_ERROR_RUNTIME, "boom 3");
  g_test_assert_expected_messages ();

  /* the message is the error value as is: no position, no prefix */
  g_test_expect_message ("shell-lua/core", G_LOG_LEVEL_CRITICAL, "plain");
  tu_assert_eval_str (s, "(select(2, pcall(shell.log.fatal, 'plain')))", "plain");
  g_test_assert_expected_messages ();

  /* it stops the code that follows */
  g_test_expect_message ("shell-lua/core", G_LOG_LEVEL_CRITICAL, "stop");
  tu_do (s, "reached = false; pcall(function() shell.log.fatal('stop'); reached = true end)");
  g_test_assert_expected_messages ();
  tu_assert_eval_str (s, "reached", "false");

  /* the arguments are checked like those of the other functions */
  tu_assert_fails (s, "shell.log.fatal()", SHELL_LUA_ERROR_RUNTIME, "string expected, got no value");
  tu_assert_fails (s, "shell.log.fatal('%d', 'x')", SHELL_LUA_ERROR_RUNTIME, "bad argument");

  g_object_unref (s);
}

static void
test_log_fatal_counts_as_an_error (void)
{
  ShellLuaState *s = new_state ("fatal@test", SHELL_LUA_STATE_SANDBOXED);

  /* in a callback of a sandboxed state: logged, then the runtime logs the
   * error with its traceback */
  g_test_expect_message ("shell-lua/fatal@test", G_LOG_LEVEL_CRITICAL, "cannot go on");
  tu_expect_state_warning (s, "cannot go on");
  tu_do (s, "shell.timer.after{ ms = 1, callback = function() shell.log.fatal('cannot go on') end }");
  tu_spin (100);
  g_test_assert_expected_messages ();

  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* shell.timer                                                               */
/* ------------------------------------------------------------------------ */

static void
test_timer_after_fires_once (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);

  tu_do (s, "t = shell.timer.after{ms = 10, callback = function() bump() end}");
  tu_assert_eval_str (s, "t.active", "true");
  tu_assert_eval_true (s, "tostring(t):find('^<shell%.Timer ') ~= nil");
  g_assert_cmpint (bumps, ==, 0);

  wait_for_count (&bumps, 1);
  tu_spin (80);
  g_assert_cmpint (bumps, ==, 1);
  tu_assert_eval_str (s, "t.active", "false");
  tu_do (s, "t:cancel()");      /* cancelling a spent timer is harmless */
  tu_assert_eval_str (s, "t.active", "false");

  /* positional form; zero delay */
  bumps = 0;
  tu_do (s, "shell.timer.after(0, function() bump() end)");
  wait_for_count (&bumps, 1);
  tu_spin (30);
  g_assert_cmpint (bumps, ==, 1);

  g_object_unref (s);
}

static void
test_timer_every_stops_on_false (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);

  tu_do (s,
         "n = 0\n"
         "t = shell.timer.every{ms = 5, callback = function() n = n + 1; bump(); return n < 3 end}\n");
  tu_assert_eval_str (s, "t.active", "true");
  wait_for_count (&bumps, 3);
  tu_spin (80);
  g_assert_cmpint (bumps, ==, 3);        /* returning false stopped it */
  tu_assert_eval_str (s, "n", "3");
  tu_assert_eval_str (s, "t.active", "false");

  g_object_unref (s);
}

static void
test_timer_every_repeats_and_cancel (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);
  int seen;

  /* any return value other than false keeps it going */
  tu_do (s,
         "t = shell.timer.every{ms = 5, callback = function() bump() end}\n"
         "u = shell.timer.every(5, function() bump(); return true end)\n");
  wait_for_count (&bumps, 8);
  tu_assert_eval_str (s, "t.active", "true");
  tu_do (s, "t:cancel(); u:cancel()");
  tu_assert_eval_str (s, "t.active", "false");
  tu_assert_eval_str (s, "u.active", "false");
  seen = bumps;
  tu_spin (60);
  g_assert_cmpint (bumps, ==, seen);

  /* a timer may cancel itself from its callback */
  bumps = 0;
  tu_do (s, "me = shell.timer.every{ms = 5, callback = function() bump(); me:cancel() end}");
  wait_for_count (&bumps, 1);
  tu_spin (60);
  g_assert_cmpint (bumps, ==, 1);
  tu_assert_eval_str (s, "me.active", "false");

  g_object_unref (s);
}

static void
test_timer_cancel_before_fire (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);

  tu_do (s, "t = shell.timer.after{ms = 30, callback = function() bump() end}");
  tu_assert_eval_str (s, "t.active", "true");
  tu_do (s, "t:cancel()");
  tu_assert_eval_str (s, "t.active", "false");
  tu_do (s, "t:cancel()");
  tu_spin (100);
  g_assert_cmpint (bumps, ==, 0);

  g_object_unref (s);
}

static void
test_timer_idle (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);

  tu_do (s, "t = shell.timer.idle{callback = function() bump() end}");
  tu_assert_eval_str (s, "t.active", "true");
  wait_for_count (&bumps, 1);
  tu_spin (40);
  g_assert_cmpint (bumps, ==, 1);       /* idle callbacks are one-shot */
  tu_assert_eval_str (s, "t.active", "false");

  tu_do (s, "shell.timer.idle(function() bump() end)");
  wait_for_count (&bumps, 2);
  tu_do (s, "c = shell.timer.idle{callback = function() bump() end}; c:cancel()");
  tu_spin (40);
  g_assert_cmpint (bumps, ==, 2);

  g_object_unref (s);
}

static void
on_state_error (ShellLuaState *state, GError *error, GString *messages)
{
  g_string_append (messages, error->message);
  g_string_append_c (messages, '\n');
}

static void
test_timer_callback_errors (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);
  GString *messages = g_string_new (NULL);

  g_signal_connect (s, "error", G_CALLBACK (on_state_error), messages);

  /* a raising callback bumps the error count and never repeats */
  tu_do (s,
         "once = shell.timer.after{ms = 5, callback = function() bump(); error('boom once') end}\n"
         "again = shell.timer.every{ms = 5, callback = function() bump(); error('boom every') end}\n");
  wait_for_count (&bumps, 2);
  tu_spin (100);
  g_assert_cmpint (bumps, ==, 2);
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 2);
  tu_assert_eval_str (s, "again.active", "false");
  tu_assert_eval_str (s, "once.active", "false");

  /* the "error" signal carries the message with its traceback */
  g_assert_nonnull (strstr (messages->str, "boom once"));
  g_assert_nonnull (strstr (messages->str, "boom every"));
  g_assert_nonnull (strstr (messages->str, "stack traceback:"));

  /* the state is unharmed */
  tu_do (s, "after_error = shell.timer.after{ms = 5, callback = function() bump() end}");
  wait_for_count (&bumps, 3);
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 2);

  g_string_free (messages, TRUE);
  g_object_unref (s);
}

static void
test_timer_sandboxed_error_is_logged (void)
{
  ShellLuaState *s = new_state ("sbx", SHELL_LUA_STATE_SANDBOXED);

  tu_do (s, "shell.timer.after{ms = 5, callback = function() bump(); error('sandbox boom') end}");
  tu_expect_state_warning (s, "sandbox boom");
  wait_for_count (&bumps, 1);
  tu_spin (20);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 1);

  g_object_unref (s);
}

static void
test_timer_cancelled_by_state_destroy (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);
  ShellLuaState *s2 = new_state ("core2", SHELL_LUA_STATE_PRIVILEGED);

  tu_do (s, "shell.timer.every{ms = 5, callback = function() bump() end}");
  tu_do (s, "shell.timer.after{ms = 20, callback = function() bump() end}");
  tu_do (s, "shell.timer.idle{callback = function() bump() end}");
  g_object_unref (s);          /* before any of them fired */
  tu_spin (80);
  g_assert_cmpint (bumps, ==, 0);

  /* after destroy() too, with the object still around */
  tu_do (s2, "shell.timer.every{ms = 5, callback = function() bump() end}");
  wait_for_count (&bumps, 2);
  shell_lua_state_destroy (s2);
  bumps = 0;
  tu_spin (60);
  g_assert_cmpint (bumps, ==, 0);

  g_object_unref (s2);
}

static void
test_timer_argument_errors (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);
  struct {
    const char *code;
    const char *message;
  } cases[] = {
    { "shell.timer.after{callback = print}", "missing required field 'ms'" },
    { "shell.timer.after{ms = 10}", "missing required field 'callback'" },
    { "shell.timer.after{ms = -1, callback = print}", "missing required field 'ms'" },
    { "shell.timer.after{ms = 'x', callback = print}", "field 'ms' must be an integer, got string" },
    { "shell.timer.after{ms = 1.5, callback = print}", "field 'ms' must be an integer" },
    { "shell.timer.after{ms = 10, callback = print, extra = 1}", "unknown field 'extra'" },
    { "shell.timer.after{ms = 10, callback = 5}", "field 'callback' must be a function, got number" },
    { "shell.timer.after{ms = 2^40, callback = print}", "field 'ms' is too large" },
    { "shell.timer.after{[1] = 1, ms = 10, callback = print}", "non-string key" },
    { "shell.timer.every{ms = 0, callback = print}", "field 'ms' must be at least 1" },
    { "shell.timer.every(0, print)", "must be at least 1" },
    { "shell.timer.idle{callback = print, ms = 5}", "unknown field 'ms'" },
    { "shell.timer.idle{}", "missing required field 'callback'" },
    { "shell.timer.after(10)", "function expected" },
    { "shell.timer.after('x', print)", "number expected, got string" },
    { "shell.timer.after(-5, print)", "milliseconds out of range" },
    { "shell.timer.after()", "number expected" },
    { "shell.timer.idle(5)", "function expected" },
    { "t = shell.timer.after{ms = 100000, callback = print}; t.active = true", "read-only" },
    { "t = shell.timer.after{ms = 100000, callback = print}; t.cancel = 1", "no property 'cancel' on shell.Timer" },
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    tu_assert_fails (s, cases[i].code, SHELL_LUA_ERROR_RUNTIME, cases[i].message);

  /* callable tables are accepted as callbacks */
  tu_do (s, "shell.timer.after{ms = 5, callback = setmetatable({}, {__call = function() bump() end})}");
  wait_for_count (&bumps, 1);

  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* shell.settings                                                            */
/* ------------------------------------------------------------------------ */

static void
reset_test_settings (void)
{
  g_autoptr (GSettings) settings = g_settings_new (TEST_SCHEMA);
  g_autoptr (GSettingsSchema) schema =
    g_settings_schema_source_lookup (g_settings_schema_source_get_default (), TEST_SCHEMA, TRUE);
  g_auto (GStrv) keys = g_settings_schema_list_keys (schema);
  int i;

  for (i = 0; keys[i] != NULL; i++)
    g_settings_reset (settings, keys[i]);
}

static void
test_settings_open_errors (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);
  ShellLuaState *sbx = new_state ("sbx", SHELL_LUA_STATE_SANDBOXED);

  tu_assert_fails (s, "shell.settings.open('org.gnome-lua.does-not-exist')", SHELL_LUA_ERROR_RUNTIME,
                   "settings schema 'org.gnome-lua.does-not-exist' is not installed");
  tu_assert_fails (s, "shell.settings.open()", SHELL_LUA_ERROR_RUNTIME, "string expected");
  /* only extensions have an own schema */
  tu_assert_fails (s, "shell.settings.extension()", SHELL_LUA_ERROR_RUNTIME,
                   "only available to extensions");
  /* a sandboxed state with no extension (so no capabilities) may not open anything */
  tu_assert_fails (sbx, "shell.settings.open('" TEST_SCHEMA "')", SHELL_LUA_ERROR_RUNTIME,
                   "requires the \"settings\" capability");

  g_object_unref (s);
  g_object_unref (sbx);
}

static void
test_settings_typed_access (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);
  g_autoptr (GSettings) check = g_settings_new (TEST_SCHEMA);
  g_auto (GStrv) strv = NULL;

  reset_test_settings ();
  tu_do (s, "st = shell.settings.open('" TEST_SCHEMA "')");
  tu_assert_eval_true (s, "tostring(st):find('^<shell%.Settings ') ~= nil");
  /* one handle per schema and state */
  tu_assert_eval_true (s, "rawequal(shell.settings.open('" TEST_SCHEMA "'), st)");

  /* defaults */
  tu_assert_eval_str (s, "st:get('flag')", "false");
  tu_assert_eval_str (s, "st:get_boolean('flag')", "false");
  tu_assert_eval_str (s, "st:get('count')", "5");
  tu_assert_eval_str (s, "st:get_int('count')", "5");
  tu_assert_eval_str (s, "st:get('ratio')", "0.5");
  tu_assert_eval_str (s, "st:get_double('ratio')", "0.5");
  tu_assert_eval_str (s, "st:get('name')", "default");
  tu_assert_eval_str (s, "st:get_string('name')", "default");
  tu_assert_eval_true (s, "(function() local l = st:get('list') return #l == 2 and l[1] == 'a' and l[2] == 'b' end)()");
  tu_assert_eval_true (s, "(function() local l = st:get_strv('list') return #l == 2 and l[2] == 'b' end)()");

  /* typed setters reach the same store another GSettings object sees */
  tu_do (s, "st:set_boolean('flag', true)");
  g_assert_true (g_settings_get_boolean (check, "flag"));
  tu_do (s, "st:set_int('count', 42)");
  g_assert_cmpint (g_settings_get_int (check, "count"), ==, 42);
  tu_do (s, "st:set_double('ratio', 1.25)");
  g_assert_cmpfloat (g_settings_get_double (check, "ratio"), ==, 1.25);
  tu_do (s, "st:set_string('name', 'änderung')");
  {
    g_autofree char *name = g_settings_get_string (check, "name");

    g_assert_cmpstr (name, ==, "änderung");
  }
  tu_do (s, "st:set_strv('list', {'x', 'y', 'z'})");
  strv = g_settings_get_strv (check, "list");
  g_assert_cmpuint (g_strv_length (strv), ==, 3);
  g_assert_cmpstr (strv[2], ==, "z");

  /* generic get/set use the variant mapping with the key's own type */
  tu_do (s, "st:set('count', 7); st:set('ratio', 2); st:set('flag', false); st:set('name', 'n'); st:set('list', {})");
  g_assert_cmpint (g_settings_get_int (check, "count"), ==, 7);
  g_assert_cmpfloat (g_settings_get_double (check, "ratio"), ==, 2.0);
  g_assert_false (g_settings_get_boolean (check, "flag"));
  {
    g_autoptr (GVariant) list = g_settings_get_value (check, "list");

    g_assert_cmpuint (g_variant_n_children (list), ==, 0);
  }
  tu_assert_eval_str (s, "st:get('count')", "7");
  tu_assert_eval_str (s, "st:get('ratio')", "2.0");

  /* reset and keys */
  tu_do (s, "st:reset('count')");
  g_assert_cmpint (g_settings_get_int (check, "count"), ==, 5);
  tu_assert_eval_true (s, "(function() local k = st:keys() return #k == 5 and k[1] == 'count' and "
                          "k[2] == 'flag' and k[3] == 'list' and k[4] == 'name' and k[5] == 'ratio' end)()");

  reset_test_settings ();
  g_object_unref (s);
}

static void
test_settings_errors (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);
  g_autoptr (GSettings) check = g_settings_new (TEST_SCHEMA);

  reset_test_settings ();
  tu_do (s, "st = shell.settings.open('" TEST_SCHEMA "')");

  /* unknown keys are Lua errors, not GSettings aborts */
  tu_assert_fails (s, "st:get('nope')", SHELL_LUA_ERROR_RUNTIME,
                   "no key 'nope' in settings schema '" TEST_SCHEMA "'");
  tu_assert_fails (s, "st:set('nope', 1)", SHELL_LUA_ERROR_RUNTIME, "no key 'nope' in settings schema");
  tu_assert_fails (s, "st:reset('nope')", SHELL_LUA_ERROR_RUNTIME, "no key 'nope' in settings schema");
  tu_assert_fails (s, "st:get_int('nope')", SHELL_LUA_ERROR_RUNTIME, "no key 'nope' in settings schema");

  /* typed getters check the key's type */
  tu_assert_fails (s, "st:get_boolean('count')", SHELL_LUA_ERROR_RUNTIME, "key 'count' has type 'i', not boolean");
  tu_assert_fails (s, "st:get_int('name')", SHELL_LUA_ERROR_RUNTIME, "key 'name' has type 's', not integer");
  tu_assert_fails (s, "st:get_double('flag')", SHELL_LUA_ERROR_RUNTIME, "key 'flag' has type 'b', not double");
  tu_assert_fails (s, "st:get_string('list')", SHELL_LUA_ERROR_RUNTIME, "key 'list' has type 'as', not string");
  tu_assert_fails (s, "st:get_strv('name')", SHELL_LUA_ERROR_RUNTIME, "key 'name' has type 's', not string array");
  tu_assert_fails (s, "st:set_int('name', 1)", SHELL_LUA_ERROR_RUNTIME, "key 'name' has type 's', not integer");

  /* typed setters check the Lua value first */
  tu_assert_fails (s, "st:set_boolean('flag', 1)", SHELL_LUA_ERROR_RUNTIME, "boolean expected, got number");
  tu_assert_fails (s, "st:set_int('count', 'x')", SHELL_LUA_ERROR_RUNTIME, "integer expected, got string");
  tu_assert_fails (s, "st:set_string('name', 5)", SHELL_LUA_ERROR_RUNTIME, "string expected, got number");
  tu_assert_fails (s, "st:set_strv('list', 'x')", SHELL_LUA_ERROR_RUNTIME, "string array expected, got string");

  /* variant conversion errors keep the documented wording */
  tu_assert_fails (s, "st:set('count', 1.5)", SHELL_LUA_ERROR_RUNTIME,
                   "expected integer for field value, got non-integral number 1.500000");
  tu_assert_fails (s, "st:set('count', 'x')", SHELL_LUA_ERROR_RUNTIME,
                   "expected integer for field value, got string (variant type 'i')");
  tu_assert_fails (s, "st:set('flag', 1)", SHELL_LUA_ERROR_RUNTIME,
                   "expected boolean for field value, got number (variant type 'b')");
  tu_assert_fails (s, "st:set('list', {1})", SHELL_LUA_ERROR_RUNTIME,
                   "expected string for field value[1], got number (variant type 's')");
  tu_assert_fails (s, "st:set('count', 2^40)", SHELL_LUA_ERROR_RUNTIME,
                   "out of range for field value (type 'i')");
  tu_assert_fails (s, "st:set('name')", SHELL_LUA_ERROR_RUNTIME, "value expected");
  /* a value the schema's range rejects */
  tu_assert_fails (s, "st:set_int('count', 1000)", SHELL_LUA_ERROR_RUNTIME,
                   "value rejected for key 'count' (out of range or not allowed)");
  tu_assert_fails (s, "st:set('count', -1)", SHELL_LUA_ERROR_RUNTIME,
                   "value rejected for key 'count'");
  g_assert_cmpint (g_settings_get_int (check, "count"), ==, 5);

  /* not a settings handle */
  tu_assert_fails (s, "st.get({}, 'flag')", SHELL_LUA_ERROR_RUNTIME, "shell.Settings expected");

  reset_test_settings ();
  g_object_unref (s);
}

static void
test_settings_on_changed (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);
  g_autoptr (GSettings) other = g_settings_new (TEST_SCHEMA);
  int seen;

  reset_test_settings ();
  tu_do (s,
         "st = shell.settings.open('" TEST_SCHEMA "')\n"
         "log = {}\n"
         "c_count = st:on_changed('count', function(key, value) log[#log + 1] = key .. '=' .. tostring(value); bump() end)\n"
         "c_all = st:on_changed(nil, function(key, value) log[#log + 1] = 'all:' .. key .. '=' .. tostring(value); bump() end)\n"
         "c_fn = st:on_changed(function(key) log[#log + 1] = 'fn:' .. key; bump() end)\n");
  tu_assert_eval_true (s, "c_count:is_connected() and c_all:is_connected() and c_fn:is_connected()");
  tu_assert_eval_true (s, "tostring(c_count):find('^<shell%.Connection ') ~= nil");

  /* a change made elsewhere reaches all three */
  g_settings_set_int (other, "count", 9);
  wait_for_count (&bumps, 3);
  tu_spin (20);
  g_assert_cmpint (bumps, ==, 3);
  tu_assert_eval_true (s, "(function() table.sort(log) return table.concat(log, ',') == "
                          "'all:count=9,count=9,fn:count' end)()");

  /* a change made from Lua too */
  bumps = 0;
  tu_do (s, "log = {}; st:set_int('count', 11)");
  wait_for_count (&bumps, 3);
  tu_assert_eval_str (s, "st:get('count')", "11");

  /* other keys only reach the wildcard listeners; the value is the new one */
  bumps = 0;
  tu_do (s, "log = {}");
  g_settings_set_string (other, "name", "renamed");
  wait_for_count (&bumps, 2);
  tu_spin (20);
  g_assert_cmpint (bumps, ==, 2);
  tu_assert_eval_true (s, "(function() table.sort(log) return table.concat(log, ',') == "
                          "'all:name=renamed,fn:name' end)()");

  /* disconnect one: the others keep going */
  tu_do (s, "c_all:disconnect(); log = {}");
  tu_assert_eval_true (s, "not c_all:is_connected() and c_count:is_connected()");
  bumps = 0;
  g_settings_set_int (other, "count", 12);
  wait_for_count (&bumps, 2);
  tu_spin (20);
  g_assert_cmpint (bumps, ==, 2);
  tu_assert_eval_true (s, "(function() table.sort(log) return table.concat(log, ',') == 'count=12,fn:count' end)()");

  /* disconnect everything (twice is fine) */
  tu_do (s, "c_count:disconnect(); c_count:disconnect(); c_fn:disconnect()");
  seen = bumps;
  g_settings_set_int (other, "count", 13);
  tu_spin (60);
  g_assert_cmpint (bumps, ==, seen);

  reset_test_settings ();
  g_object_unref (s);
}

static void
test_settings_on_changed_edge_cases (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);
  g_autoptr (GSettings) other = g_settings_new (TEST_SCHEMA);
  guint base_errors;
  int seen;

  reset_test_settings ();
  tu_do (s, "st = shell.settings.open('" TEST_SCHEMA "')");
  tu_assert_fails (s, "st:on_changed('nope', print)", SHELL_LUA_ERROR_RUNTIME, "no key 'nope' in settings schema");
  tu_assert_fails (s, "st:on_changed('count', 5)", SHELL_LUA_ERROR_RUNTIME, "function expected");
  tu_assert_fails (s, "st:on_changed('count')", SHELL_LUA_ERROR_RUNTIME, "function expected");

  /* a callback may disconnect itself; a raising callback is counted, not fatal */
  base_errors = shell_lua_state_get_error_count (s);
  tu_do (s,
         "once = st:on_changed('count', function() bump(); once:disconnect() end)\n"
         "bad = st:on_changed('count', function() bump(); error('listener failed') end)\n");
  g_settings_set_int (other, "count", 20);
  wait_for_count (&bumps, 2);
  tu_spin (20);
  g_assert_cmpint (bumps, ==, 2);
  tu_assert_eval_true (s, "not once:is_connected() and bad:is_connected()");
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, base_errors + 1);
  g_settings_set_int (other, "count", 21);
  wait_for_count (&bumps, 3);
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, base_errors + 2);

  /* the connection belongs to the state: destroying it silences everything */
  seen = bumps;
  g_object_unref (s);
  g_settings_set_int (other, "count", 22);
  tu_spin (60);
  g_assert_cmpint (bumps, ==, seen);

  reset_test_settings ();
}

/* ------------------------------------------------------------------------ */
/* shell.gettext                                                             */
/* ------------------------------------------------------------------------ */

static void
test_gettext (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);
  ShellLuaState *sbx = new_state ("sbx", SHELL_LUA_STATE_SANDBOXED);
  ShellLuaState *states[] = { s, sbx };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (states); i++)
    {
      ShellLuaState *state = states[i];

      tu_assert_eval_str (state, "shell.gettext('x')", "x");
      tu_assert_eval_str (state, "shell.gettext('Hello, world')", "Hello, world");
      tu_assert_eval_str (state, "shell.gettext('')", "");
      tu_assert_eval_str (state, "_('x')", "x");
      tu_assert_eval_str (state, "_('')", "");
      tu_assert_eval_str (state, "shell.ngettext('one', 'many', 1)", "one");
      tu_assert_eval_str (state, "shell.ngettext('one', 'many', 2)", "many");
      tu_assert_eval_str (state, "shell.ngettext('one', 'many', 0)", "many");
      tu_assert_eval_str (state, "shell.ngettext('one', 'many', -1)", "one");
      tu_assert_eval_str (state, "ngettext('one', 'many', 5)", "many");
      tu_assert_eval_str (state, "shell.pgettext('menu', 'Open')", "Open");
      tu_assert_eval_str (state, "shell.pgettext('menu', '')", "");
      tu_assert_eval_str (state, "pgettext('menu', 'Open')", "Open");
      /* the globals are the shell functions */
      tu_assert_eval_true (state, "rawequal(_, shell.gettext) and rawequal(ngettext, shell.ngettext) and "
                                  "rawequal(pgettext, shell.pgettext)");
    }

  tu_assert_fails (s, "_({})", SHELL_LUA_ERROR_RUNTIME, "string expected, got table");
  tu_assert_fails (s, "shell.ngettext('a', 'b')", SHELL_LUA_ERROR_RUNTIME, "number expected");
  tu_assert_fails (s, "shell.ngettext('a', 'b', 1.5)", SHELL_LUA_ERROR_RUNTIME, "number has no integer representation");
  tu_assert_fails (s, "shell.pgettext('ctx')", SHELL_LUA_ERROR_RUNTIME, "string expected");

  g_object_unref (s);
  g_object_unref (sbx);
}

static void
test_shell_table (void)
{
  ShellLuaState *s = new_state ("core", SHELL_LUA_STATE_PRIVILEGED);
  ShellLuaState *sbx = new_state ("sbx", SHELL_LUA_STATE_SANDBOXED);

  /* no extension: shell.extension is nil (it only exists in extension states) */
  tu_assert_eval_str (s, "shell.extension", "nil");
  tu_assert_eval_str (sbx, "shell.extension", "nil");
  tu_assert_eval_true (s, "require('shell') == shell");
  tu_assert_eval_true (sbx, "require('shell') == shell");
  tu_assert_eval_true (sbx, "type(shell.log) == 'table' and type(shell.timer) == 'table' and "
                            "type(shell.settings) == 'table' and type(shell.hooks) == 'table'");

  g_object_unref (s);
  g_object_unref (sbx);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/shell-lua/api-core/log/levels", test_log);
  g_test_add_func ("/shell-lua/api-core/log/domain", test_log_domain_follows_state_id);
  g_test_add_func ("/shell-lua/api-core/log/not-fatal", test_log_is_not_fatal);
  g_test_add_func ("/shell-lua/api-core/log/fatal", test_log_fatal);
  g_test_add_func ("/shell-lua/api-core/log/fatal-in-callback", test_log_fatal_counts_as_an_error);
  g_test_add_func ("/shell-lua/api-core/timer/after", test_timer_after_fires_once);
  g_test_add_func ("/shell-lua/api-core/timer/every-stops-on-false", test_timer_every_stops_on_false);
  g_test_add_func ("/shell-lua/api-core/timer/every-and-cancel", test_timer_every_repeats_and_cancel);
  g_test_add_func ("/shell-lua/api-core/timer/cancel-before-fire", test_timer_cancel_before_fire);
  g_test_add_func ("/shell-lua/api-core/timer/idle", test_timer_idle);
  g_test_add_func ("/shell-lua/api-core/timer/callback-errors", test_timer_callback_errors);
  g_test_add_func ("/shell-lua/api-core/timer/sandboxed-error-logged", test_timer_sandboxed_error_is_logged);
  g_test_add_func ("/shell-lua/api-core/timer/state-destroy-cancels", test_timer_cancelled_by_state_destroy);
  g_test_add_func ("/shell-lua/api-core/timer/argument-errors", test_timer_argument_errors);
  g_test_add_func ("/shell-lua/api-core/settings/open-errors", test_settings_open_errors);
  g_test_add_func ("/shell-lua/api-core/settings/typed-access", test_settings_typed_access);
  g_test_add_func ("/shell-lua/api-core/settings/errors", test_settings_errors);
  g_test_add_func ("/shell-lua/api-core/settings/on-changed", test_settings_on_changed);
  g_test_add_func ("/shell-lua/api-core/settings/on-changed-edge-cases", test_settings_on_changed_edge_cases);
  g_test_add_func ("/shell-lua/api-core/gettext", test_gettext);
  g_test_add_func ("/shell-lua/api-core/shell-table", test_shell_table);

  return g_test_run ();
}
