/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaInspector: the privileged state behind Looking Glass.
 */

#include "test-utils.h"

#include "shell-lua-extension.h"
#include "shell-lua-inspector.h"

static ShellLuaInspector *
new_inspector (void)
{
  return g_object_new (SHELL_TYPE_LUA_INSPECTOR, NULL);
}

static char *
eval_ok (ShellLuaInspector *inspector, const char *code)
{
  g_autoptr (GError) error = NULL;
  char *result = shell_lua_inspector_eval (inspector, code, &error);

  if (result == NULL)
    g_error ("eval of '%s' failed: %s", code, error->message);
  return result;
}

#define ASSERT_EVAL(inspector, code, expected) G_STMT_START { \
    g_autofree char *_r = eval_ok ((inspector), (code)); \
    if (g_strcmp0 (_r, (expected)) != 0) \
      g_error ("%s:%d: eval of '%s' gave '%s', expected '%s'", __FILE__, __LINE__, \
               (code), _r, (expected)); \
  } G_STMT_END

static GError *
eval_fails (ShellLuaInspector *inspector, const char *code, ShellLuaError expected, const char *substr)
{
  GError *error = NULL;
  char *result = shell_lua_inspector_eval (inspector, code, &error);

  if (result != NULL)
    g_error ("eval of '%s' unexpectedly gave '%s'", code, result);
  if (!g_error_matches (error, SHELL_LUA_ERROR, (int) expected))
    g_error ("eval of '%s' failed with the wrong error: %s", code, error ? error->message : "(none)");
  if (substr != NULL && strstr (error->message, substr) == NULL)
    g_error ("error of '%s' lacks '%s': %s", code, substr, error->message);
  return error;
}

static void
test_eval_expressions (void)
{
  ShellLuaInspector *inspector = new_inspector ();

  ASSERT_EVAL (inspector, "1+1", "2");
  ASSERT_EVAL (inspector, "'a' .. 'b'", "ab");
  ASSERT_EVAL (inspector, "#'abc'", "3");
  ASSERT_EVAL (inspector, "1.5", "1.5");
  ASSERT_EVAL (inspector, "2^10", "1024.0");
  ASSERT_EVAL (inspector, "true", "true");
  ASSERT_EVAL (inspector, "nil", "nil");
  ASSERT_EVAL (inspector, "'two\\nlines'", "two\nlines");
  /* the full standard library and the shell API are there */
  ASSERT_EVAL (inspector, "type(os.time())", "number");
  ASSERT_EVAL (inspector, "shell.api_version.major", "1");
  ASSERT_EVAL (inspector, "require('shell') == shell", "true");
  ASSERT_EVAL (inspector, "type(shell.hooks.list)", "function");

  g_object_unref (inspector);
}

static void
test_eval_statements (void)
{
  ShellLuaInspector *inspector = new_inspector ();

  /* "return <code>" fails to compile, so it runs as written; nothing to show */
  ASSERT_EVAL (inspector, "x = 5", "");
  ASSERT_EVAL (inspector, "x", "5");
  ASSERT_EVAL (inspector, "x = x + 1", "");
  ASSERT_EVAL (inspector, "x", "6");
  ASSERT_EVAL (inspector, "for i = 1, 3 do z = (z or 0) + i end", "");
  ASSERT_EVAL (inspector, "z", "6");
  ASSERT_EVAL (inspector, "function twice(n) return n * 2 end", "");
  ASSERT_EVAL (inspector, "twice(21)", "42");
  /* locals live in their chunk only */
  ASSERT_EVAL (inspector, "local y = 1", "");
  ASSERT_EVAL (inspector, "y", "nil");
  /* an explicit return works as a statement */
  ASSERT_EVAL (inspector, "return 7", "7");
  ASSERT_EVAL (inspector, "return 1, 2", "1\n2");

  g_object_unref (inspector);
}

static void
test_eval_multiple_results (void)
{
  ShellLuaInspector *inspector = new_inspector ();

  ASSERT_EVAL (inspector, "1, 'two', true, nil, 4.5", "1\ntwo\ntrue\nnil\n4.5");
  ASSERT_EVAL (inspector, "(function() return 'a', 'b', 'c' end)()", "a\nb\nc");
  ASSERT_EVAL (inspector, "table.unpack({10, 20, 30})", "10\n20\n30");
  ASSERT_EVAL (inspector, "{1}, {2}", "{1}\n{2}");
  ASSERT_EVAL (inspector, "(function() end)()", "");

  /* results are capped */
  {
    g_autofree char *r = eval_ok (inspector, "table.unpack({}, 1, 250)");
    g_auto (GStrv) lines = g_strsplit (r, "\n", -1);

    g_assert_cmpuint (g_strv_length (lines), ==, 200);
    g_assert_cmpstr (lines[0], ==, "nil");
  }

  g_object_unref (inspector);
}

static void
test_eval_tables (void)
{
  ShellLuaInspector *inspector = new_inspector ();
  g_autofree char *big = NULL;

  ASSERT_EVAL (inspector, "{}", "{}");
  ASSERT_EVAL (inspector, "{1, 2, 3}", "{1, 2, 3}");
  ASSERT_EVAL (inspector, "{a = 1}", "{a = 1}");
  ASSERT_EVAL (inspector, "{1, 'x', true}", "{1, \"x\", true}");
  ASSERT_EVAL (inspector, "{1, 2, k = 'v'}", "{1, 2, k = \"v\"}");
  ASSERT_EVAL (inspector, "{[10] = 1}", "{[10] = 1}");
  ASSERT_EVAL (inspector, "{[true] = 'yes'}", "{[true] = \"yes\"}");
  /* only shallow: nested tables are elided */
  ASSERT_EVAL (inspector, "{{1}}", "{{...}}");
  ASSERT_EVAL (inspector, "{a = {b = {c = 1}}}", "{a = {...}}");
  ASSERT_EVAL (inspector, "{1, {2}, 3}", "{1, {...}, 3}");
  /* __tostring is honoured at the top level and inside */
  ASSERT_EVAL (inspector, "setmetatable({}, {__tostring = function() return 'custom!' end})", "custom!");
  ASSERT_EVAL (inspector, "{setmetatable({}, {__tostring = function() return 'T' end})}", "{T}");

  /* long tables are cut after 32 entries */
  big = eval_ok (inspector, "(function() local t = {} for i = 1, 100 do t[i] = i end return t end)()");
  g_assert_true (g_str_has_prefix (big, "{1, 2, 3, "));
  g_assert_true (g_str_has_suffix (big, ", 31, 32, ...}"));
  g_assert_null (strstr (big, "33"));

  /* a table of exactly 32 is shown whole */
  {
    g_autofree char *exact = eval_ok (inspector, "(function() local t = {} for i = 1, 32 do t[i] = i end return t end)()");

    g_assert_true (g_str_has_suffix (exact, ", 31, 32}"));
  }

  g_object_unref (inspector);
}

static void
test_eval_handles (void)
{
  ShellLuaInspector *inspector = new_inspector ();
  g_autofree char *r = eval_ok (inspector, "shell.timer.after{ms = 100000, callback = function() end}");
  g_autofree char *list = eval_ok (inspector, "{shell.timer.after{ms = 100000, callback = function() end}}");

  g_assert_true (g_pattern_match_simple ("<shell.Timer 0x*>", r));
  g_assert_true (g_pattern_match_simple ("{<shell.Timer 0x*>}", list));

  g_object_unref (inspector);
}

static void
test_eval_errors (void)
{
  ShellLuaInspector *inspector = new_inspector ();
  g_autoptr (GError) runtime = NULL;
  g_autoptr (GError) other = NULL;

  runtime = eval_fails (inspector, "error('boom')", SHELL_LUA_ERROR_RUNTIME, "boom");
  g_assert_nonnull (strstr (runtime->message, "stack traceback:"));
  g_assert_nonnull (strstr (runtime->message, "(inspector)"));

  other = eval_fails (inspector, "(function() local t = nil; return t.field end)()", SHELL_LUA_ERROR_RUNTIME,
                      "attempt to index a nil value");
  g_clear_error (&other);
  other = eval_fails (inspector, "error({})", SHELL_LUA_ERROR_RUNTIME, "(error object is a table value)");
  g_clear_error (&other);
  other = eval_fails (inspector, "nosuchfunction()", SHELL_LUA_ERROR_RUNTIME, "attempt to call a nil value (global 'nosuchfunction')");
  g_clear_error (&other);
  /* an error while rendering the result */
  other = eval_fails (inspector, "setmetatable({}, {__tostring = function() error('render failed') end})",
                      SHELL_LUA_ERROR_RUNTIME, "render failed");
  g_clear_error (&other);

  /* syntax errors are reported as such, from the statement form */
  other = eval_fails (inspector, "1 +", SHELL_LUA_ERROR_SYNTAX, "(inspector):1:");
  g_clear_error (&other);
  other = eval_fails (inspector, "local = 1", SHELL_LUA_ERROR_SYNTAX, NULL);
  g_clear_error (&other);
  other = eval_fails (inspector, "if x then", SHELL_LUA_ERROR_SYNTAX, NULL);
  g_clear_error (&other);

  /* the NULL-error form does not crash and the state is unharmed */
  g_assert_null (shell_lua_inspector_eval (inspector, "error('quiet')", NULL));
  ASSERT_EVAL (inspector, "1 + 1", "2");
  ASSERT_EVAL (inspector, "x = 3", "");
  ASSERT_EVAL (inspector, "x", "3");

  g_object_unref (inspector);
}

static void
test_reset (void)
{
  ShellLuaInspector *inspector = new_inspector ();

  /* before the first eval: creates a state */
  shell_lua_inspector_reset (inspector);
  ASSERT_EVAL (inspector, "x = 1", "");
  ASSERT_EVAL (inspector, "function f() return 'old' end", "");
  ASSERT_EVAL (inspector, "x", "1");
  ASSERT_EVAL (inspector, "f()", "old");

  shell_lua_inspector_reset (inspector);
  ASSERT_EVAL (inspector, "x", "nil");
  ASSERT_EVAL (inspector, "f", "nil");
  ASSERT_EVAL (inspector, "shell.api_version.minor", "0");

  g_object_unref (inspector);
}

typedef struct {
  guint    count;
  gboolean found_inspector;
  char    *state_name;
  guint64  bytes;
  guint    errors;
  guint    number;
  GString *uuids;
} StatesInfo;

static void
states_info_clear (StatesInfo *info)
{
  g_free (info->state_name);
  g_string_free (info->uuids, TRUE);
}

static void
read_states (GVariant *states, StatesInfo *info)
{
  GVariantIter iter;
  const char *uuid, *name;
  guint64 bytes;
  guint32 errors, number;

  memset (info, 0, sizeof *info);
  info->uuids = g_string_new (NULL);
  g_assert_true (g_variant_is_of_type (states, G_VARIANT_TYPE ("a(sstuu)")));
  g_variant_iter_init (&iter, states);
  while (g_variant_iter_next (&iter, "(&s&stuu)", &uuid, &name, &bytes, &errors, &number))
    {
      info->count++;
      g_string_append_printf (info->uuids, "%s;", uuid);
      if (strcmp (uuid, "inspector") == 0)
        {
          info->found_inspector = TRUE;
          g_free (info->state_name);
          info->state_name = g_strdup (name);
          info->bytes = bytes;
          info->errors = errors;
          info->number = number;
        }
    }
}

static void
test_get_states (void)
{
  ShellLuaInspector *inspector = new_inspector ();
  g_autoptr (GVariant) states = shell_lua_inspector_get_states (inspector);
  StatesInfo info;

  read_states (states, &info);
  g_assert_true (info.found_inspector);
  g_assert_cmpuint (info.count, ==, 1);
  g_assert_cmpstr (info.state_name, ==, "ACTIVE");
  g_assert_cmpuint (info.bytes, >, 0);
  g_assert_cmpuint (info.errors, ==, 0);
  g_assert_cmpuint (info.number, ==, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  states_info_clear (&info);

  /* the numbers follow the inspector's state */
  {
    g_autoptr (GError) error = eval_fails (inspector, "error('counted')", SHELL_LUA_ERROR_RUNTIME, "counted");
    g_autoptr (GVariant) after = shell_lua_inspector_get_states (inspector);

    read_states (after, &info);
    g_assert_cmpuint (info.errors, ==, 1);
    states_info_clear (&info);
  }
  ASSERT_EVAL (inspector, "big = {} for i = 1, 5000 do big[i] = {i} end", "");
  {
    g_autoptr (GVariant) after = shell_lua_inspector_get_states (inspector);

    read_states (after, &info);
    g_assert_cmpuint (info.bytes, >, 100 * 1000);
    states_info_clear (&info);
  }

  /* reset gives a fresh state */
  shell_lua_inspector_reset (inspector);
  {
    g_autoptr (GVariant) after = shell_lua_inspector_get_states (inspector);

    read_states (after, &info);
    g_assert_cmpuint (info.errors, ==, 0);
    g_assert_cmpuint (info.bytes, <, 100 * 1000);
    states_info_clear (&info);
  }

  g_object_unref (inspector);
}

static void
test_get_states_lists_extensions (void)
{
  ShellLuaInspector *inspector = new_inspector ();
  ShellLuaExtensionManager *manager = shell_lua_extension_manager_get_default ();
  g_autofree char *tmp = tu_tmpdir_new ();
  g_autofree char *root = g_build_filename (tmp, "ext", NULL);
  g_autoptr (GFile) root_file = NULL;
  GFile *dirs[1];
  g_autoptr (GVariant) states = NULL;
  g_autoptr (GError) error = NULL;
  StatesInfo info;
  GVariantIter iter;
  const char *uuid, *name;
  guint64 bytes;
  guint32 errors, number;
  gboolean saw_active = FALSE;

  tu_write_file (root, "active@test/metadata.json",
                 "{\"uuid\": \"active@test\", \"name\": \"A\", \"shell-api\": \"1\"}");
  tu_write_file (root, "active@test/extension.lua", "return {}");
  tu_write_file (root, "idle@test/metadata.json",
                 "{\"uuid\": \"idle@test\", \"name\": \"I\", \"shell-api\": \"1\"}");
  tu_write_file (root, "idle@test/extension.lua", "return {}");
  root_file = g_file_new_for_path (root);
  dirs[0] = root_file;
  shell_lua_extension_manager_set_search_dirs (manager, dirs, 1);
  shell_lua_extension_manager_scan (manager);
  g_assert_true (shell_lua_extension_manager_enable (manager, "active@test", &error));
  g_assert_no_error (error);

  /* only extensions with a live state are listed, then the inspector itself */
  states = shell_lua_inspector_get_states (inspector);
  read_states (states, &info);
  g_assert_cmpstr (info.uuids->str, ==, "active@test;inspector;");
  g_assert_true (info.found_inspector);
  states_info_clear (&info);

  g_variant_iter_init (&iter, states);
  while (g_variant_iter_next (&iter, "(&s&stuu)", &uuid, &name, &bytes, &errors, &number))
    {
      if (strcmp (uuid, "active@test") == 0)
        {
          saw_active = TRUE;
          g_assert_cmpstr (name, ==, "ACTIVE");
          g_assert_cmpuint (bytes, >, 0);
          g_assert_cmpuint (errors, ==, 0);
          g_assert_cmpuint (number, ==, SHELL_LUA_EXTENSION_STATE_ACTIVE);
        }
    }
  g_assert_true (saw_active);

  /* the default manager is a singleton owned by nobody: finalize it (this disables the extension) */
  g_object_unref (manager);
  g_object_unref (inspector);
  tu_rm_rf (tmp);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_unsetenv ("SHELL_LUA_EXTENSION_PATH");

  g_test_add_func ("/shell-lua/inspector/eval/expressions", test_eval_expressions);
  g_test_add_func ("/shell-lua/inspector/eval/statements", test_eval_statements);
  g_test_add_func ("/shell-lua/inspector/eval/multiple-results", test_eval_multiple_results);
  g_test_add_func ("/shell-lua/inspector/eval/tables", test_eval_tables);
  g_test_add_func ("/shell-lua/inspector/eval/handles", test_eval_handles);
  g_test_add_func ("/shell-lua/inspector/eval/errors", test_eval_errors);
  g_test_add_func ("/shell-lua/inspector/reset", test_reset);
  g_test_add_func ("/shell-lua/inspector/get-states", test_get_states);
  g_test_add_func ("/shell-lua/inspector/get-states-extensions", test_get_states_lists_extensions);

  return g_test_run ();
}
