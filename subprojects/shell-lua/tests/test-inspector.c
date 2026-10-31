/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaInspector: the privileged state behind Looking Glass.
 */

#include "test-utils.h"

#include "shell-lua-extension.h"
#include "shell-lua-handle.h"
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
  ASSERT_EVAL (inspector, "shell.api_version.minor", "2");

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

/* ------------------------------------------------------------------------ */
/* The Evaluator API of Looking Glass                                        */
/* ------------------------------------------------------------------------ */

typedef struct {
  guint64 id;
  char *type;
  char *summary;
} Entry;

static GArray *
entries_new (void)
{
  return g_array_new (FALSE, TRUE, sizeof (Entry));
}

static void
entries_free (GArray *entries)
{
  guint i;

  for (i = 0; i < entries->len; i++)
    {
      g_free (g_array_index (entries, Entry, i).type);
      g_free (g_array_index (entries, Entry, i).summary);
    }
  g_array_free (entries, TRUE);
}

/* Evaluates, which must work; returns the entries and the printed output */
static GArray *
evaluate_ok (ShellLuaInspector *inspector, const char *code, char **output)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) result = NULL;
  g_autofree char *local_output = NULL;
  GArray *entries = entries_new ();
  GVariantIter iter;
  Entry entry;
  const char *type, *summary;

  result = shell_lua_inspector_evaluate (inspector, code, &local_output, &error);
  if (result == NULL)
    g_error ("evaluate of '%s' failed: %s", code, error->message);
  g_assert_true (g_variant_is_of_type (result, G_VARIANT_TYPE ("a(tss)")));

  g_variant_iter_init (&iter, result);
  while (g_variant_iter_next (&iter, "(t&s&s)", &entry.id, &type, &summary))
    {
      entry.type = g_strdup (type);
      entry.summary = g_strdup (summary);
      g_array_append_val (entries, entry);
    }

  if (output != NULL)
    *output = g_steal_pointer (&local_output);
  return entries;
}

static void
test_evaluate_results (void)
{
  ShellLuaInspector *inspector = new_inspector ();
  g_autofree char *output = NULL;
  GArray *entries;

  entries = evaluate_ok (inspector, "1 + 1", &output);
  g_assert_cmpuint (entries->len, ==, 1);
  g_assert_cmpuint (g_array_index (entries, Entry, 0).id, ==, 1);
  g_assert_cmpstr (g_array_index (entries, Entry, 0).type, ==, "number");
  g_assert_cmpstr (g_array_index (entries, Entry, 0).summary, ==, "2");
  g_assert_cmpstr (output, ==, "");
  entries_free (entries);

  /* each value is a result; r () and `it` get them back */
  entries = evaluate_ok (inspector, "'two', {3, x = 4}", NULL);
  g_assert_cmpuint (entries->len, ==, 2);
  g_assert_cmpuint (g_array_index (entries, Entry, 0).id, ==, 2);
  g_assert_cmpstr (g_array_index (entries, Entry, 0).summary, ==, "two");
  g_assert_cmpuint (g_array_index (entries, Entry, 1).id, ==, 3);
  g_assert_cmpstr (g_array_index (entries, Entry, 1).type, ==, "table");
  g_assert_cmpstr (g_array_index (entries, Entry, 1).summary, ==, "{3, x = 4}");
  entries_free (entries);

  ASSERT_EVAL (inspector, "r(1)", "2");
  ASSERT_EVAL (inspector, "r(2)", "two");
  /* `it` is the last result of evaluate (); eval () does not record */
  ASSERT_EVAL (inspector, "it", "{3, x = 4}");
  ASSERT_EVAL (inspector, "r(3).x", "4");
  {
    g_autoptr (GError) error = eval_fails (inspector, "r(99)", SHELL_LUA_ERROR_RUNTIME,
                                           "Unknown result at index 99");

    g_assert_nonnull (error);
  }

  g_object_unref (inspector);
}

static void
test_evaluate_print_and_errors (void)
{
  ShellLuaInspector *inspector = new_inspector ();
  g_autoptr (GError) error = NULL;
  g_autofree char *output = NULL;
  g_autoptr (GVariant) result = NULL;
  GArray *entries;

  /* a statement has no value; print is captured, not written to stdout */
  entries = evaluate_ok (inspector, "print('a', 1, nil, true) print('b')", &output);
  g_assert_cmpuint (entries->len, ==, 0);
  g_assert_cmpstr (output, ==, "a\t1\tnil\ttrue\nb\n");
  entries_free (entries);

  g_clear_pointer (&output, g_free);
  entries = evaluate_ok (inspector, "print('x'); return 5", &output);
  g_assert_cmpuint (entries->len, ==, 1);
  g_assert_cmpstr (g_array_index (entries, Entry, 0).summary, ==, "5");
  g_assert_cmpstr (output, ==, "x\n");
  entries_free (entries);

  /* the output of a failing chunk is not lost */
  g_clear_pointer (&output, g_free);
  result = shell_lua_inspector_evaluate (inspector, "print('before') error('after')", &output, &error);
  g_assert_null (result);
  g_assert_error (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_RUNTIME);
  g_assert_nonnull (strstr (error->message, "after"));
  g_assert_cmpstr (output, ==, "before\n");

  /* an unknown result is an error naming the index */
  g_clear_error (&error);
  g_clear_pointer (&output, g_free);
  result = shell_lua_inspector_evaluate (inspector, "r(7)", NULL, &error);
  g_assert_null (result);
  g_assert_nonnull (strstr (error->message, "Unknown result at index 7"));

  /* invalid UTF-8 does not reach the variants */
  g_clear_error (&error);
  entries = evaluate_ok (inspector, "'a\\xffb'", NULL);
  g_assert_cmpuint (entries->len, ==, 1);
  g_assert_true (g_utf8_validate (g_array_index (entries, Entry, 0).summary, -1, NULL));
  entries_free (entries);

  /* summaries are one line */
  entries = evaluate_ok (inspector, "'one\\ntwo'", NULL);
  g_assert_cmpstr (g_array_index (entries, Entry, 0).summary, ==, "one\\ntwo");
  entries_free (entries);

  g_object_unref (inspector);
}

static void
test_inspect_table (void)
{
  ShellLuaInspector *inspector = new_inspector ();
  g_autoptr (GVariant) info = NULL;
  g_autoptr (GVariant) members = NULL;
  g_autoptr (GError) error = NULL;
  const char *type, *summary, *key, *value;
  guint64 pin, inner_pin = 0;
  GVariantIter iter;
  GString *keys = g_string_new (NULL);
  GArray *entries;

  entries = evaluate_ok (inspector, "{10, 20, name = 'n', inner = {1, 2}, f = print, [3] = true}", NULL);
  info = shell_lua_inspector_inspect (inspector, g_array_index (entries, Entry, 0).id, &error);
  g_assert_no_error (error);
  entries_free (entries);

  g_variant_get (info, "(&s&s@a(sst))", &type, &summary, &members);
  g_assert_cmpstr (type, ==, "table");
  g_variant_iter_init (&iter, members);
  while (g_variant_iter_next (&iter, "(&s&st)", &key, &value, &pin))
    {
      g_string_append_printf (keys, "%s;", key);
      if (strcmp (key, "inner") == 0)
        {
          g_assert_cmpstr (value, ==, "{1, 2}");
          g_assert_cmpuint (pin, >, 0);
          inner_pin = pin;
        }
      else if (strcmp (key, "f") == 0)
        {
          g_assert_cmpuint (pin, >, 0);
        }
      else
        {
          g_assert_cmpuint (pin, ==, 0);
        }
    }
  /* integer keys first, then the names in order */
  g_assert_cmpstr (keys->str, ==, "[1];[2];[3];f;inner;name;");
  g_string_free (keys, TRUE);

  /* the member can be inspected in turn, and inserted as a result */
  g_clear_pointer (&info, g_variant_unref);
  g_clear_pointer (&members, g_variant_unref);
  info = shell_lua_inspector_inspect (inspector, inner_pin, &error);
  g_assert_no_error (error);
  g_variant_get (info, "(&s&s@a(sst))", &type, &summary, &members);
  g_assert_cmpstr (summary, ==, "{1, 2}");
  g_assert_cmpuint (g_variant_n_children (members), ==, 2);

  {
    g_autoptr (GVariant) inserted = shell_lua_inspector_insert (inspector, inner_pin, &error);
    guint64 id;
    const char *itype, *isummary;

    g_assert_no_error (error);
    g_variant_get (inserted, "(t&s&s)", &id, &itype, &isummary);
    g_assert_cmpuint (id, <, 100);
    g_assert_cmpstr (isummary, ==, "{1, 2}");
    ASSERT_EVAL (inspector, "it[2]", "2");
  }

  /* released ids are gone */
  shell_lua_inspector_release (inspector, inner_pin);
  {
    g_autoptr (GVariant) gone = shell_lua_inspector_inspect (inspector, inner_pin, &error);

    g_assert_null (gone);
    g_assert_nonnull (error);
    g_clear_error (&error);
  }

  g_object_unref (inspector);
}

static void
test_objects (void)
{
  ShellLuaInspector *inspector = new_inspector ();
  g_autoptr (GSimpleAction) action = g_simple_action_new ("do-it", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) entry = NULL;
  g_autoptr (GVariant) info = NULL;
  g_autoptr (GVariant) members = NULL;
  g_autoptr (GObject) object = NULL;
  g_autofree char *expected = NULL;
  GError *expected_error = NULL;
  const char *type, *summary, *key, *value;
  GVariantIter iter;
  guint64 id, pin;
  gboolean saw_enabled = FALSE, saw_method = FALSE;
  GArray *entries;

  shell_lua_inspector_set_global_object (inspector, "act", G_OBJECT (action));
  entries = evaluate_ok (inspector, "act", NULL);
  g_assert_cmpstr (g_array_index (entries, Entry, 0).type, ==, "GSimpleAction");
  expected = g_strdup_printf ("<GSimpleAction \"do-it\" %p>", (gpointer) action);
  g_assert_cmpstr (g_array_index (entries, Entry, 0).summary, ==, expected);
  id = g_array_index (entries, Entry, 0).id;
  entries_free (entries);

  object = shell_lua_inspector_get_object (inspector, id);
  g_assert_true (object == G_OBJECT (action));
  g_clear_object (&object);

  /* properties: dashes become underscores; read and write */
  ASSERT_EVAL (inspector, "act.name", "do-it");
  ASSERT_EVAL (inspector, "act.enabled", "true");
  ASSERT_EVAL (inspector, "act.enabled = false", "");
  g_assert_false (g_action_get_enabled (G_ACTION (action)));
  ASSERT_EVAL (inspector, "act.enabled", "false");
  expected_error = eval_fails (inspector, "act.name = 'x'", SHELL_LUA_ERROR_RUNTIME, "read-only");
  g_clear_error (&expected_error);
  ASSERT_EVAL (inspector, "act:gtype()", "GSimpleAction");
  ASSERT_EVAL (inspector, "#act:properties() > 3", "true");
  ASSERT_EVAL (inspector, "act.no_such_thing", "nil");
  /* unreadable by Lua: no conversion */
  expected_error = eval_fails (inspector, "act.parameter_type", SHELL_LUA_ERROR_RUNTIME, "cannot convert");
  g_clear_error (&expected_error);

  /* the members of an object */
  info = shell_lua_inspector_inspect (inspector, id, &error);
  g_assert_no_error (error);
  g_variant_get (info, "(&s&s@a(sst))", &type, &summary, &members);
  g_assert_cmpstr (type, ==, "GSimpleAction");
  g_variant_iter_init (&iter, members);
  while (g_variant_iter_next (&iter, "(&s&st)", &key, &value, &pin))
    {
      if (strcmp (key, "enabled") == 0)
        {
          saw_enabled = TRUE;
          g_assert_cmpstr (value, ==, "false");
        }
      else if (strcmp (key, "gtype ()") == 0)
        {
          saw_method = TRUE;
        }
    }
  g_assert_true (saw_enabled);
  g_assert_true (saw_method);

  /* an object that is made a result by the host */
  entry = shell_lua_inspector_add_object_result (inspector, G_OBJECT (action), &error);
  g_assert_no_error (error);
  g_variant_get (entry, "(t&s&s)", &id, &type, &summary);
  g_assert_cmpstr (type, ==, "GSimpleAction");
  {
    g_autofree char *code = g_strdup_printf ("r(%" G_GUINT64_FORMAT ") == act and it == act", id);

    ASSERT_EVAL (inspector, code, "true");
  }

  /* pinning gives an id without making a result */
  {
    g_autoptr (GVariant) pinned_info = NULL;
    guint64 pin = shell_lua_inspector_pin_object (inspector, G_OBJECT (action), &error);

    g_assert_no_error (error);
    g_assert_cmpuint (pin, >=, ((guint64) 1) << 32);
    pinned_info = shell_lua_inspector_inspect (inspector, pin, &error);
    g_assert_no_error (error);
    g_variant_get_child (pinned_info, 0, "&s", &type);
    g_assert_cmpstr (type, ==, "GSimpleAction");
    shell_lua_inspector_release (inspector, pin);
  }

  /* objects are weak: a dead one is a dead handle, not a crash */
  g_clear_object (&action);
  ASSERT_EVAL (inspector, "tostring(it):match('dead') ~= nil", "true");
  object = shell_lua_inspector_get_object (inspector, id);
  g_assert_null (object);

  g_object_unref (inspector);
}

static void
test_object_properties (void)
{
  ShellLuaInspector *inspector = new_inspector ();
  g_autoptr (GSocketAddress) address = g_inet_socket_address_new_from_string ("127.0.0.1", 8080);

  shell_lua_inspector_set_global_object (inspector, "sa", G_OBJECT (address));

  /* the properties which are objects give handles of their own type */
  ASSERT_EVAL (inspector, "sa:gtype()", "GInetSocketAddress");
  ASSERT_EVAL (inspector, "sa.port", "8080");
  ASSERT_EVAL (inspector, "sa.address:gtype()", "GInetAddress");
  ASSERT_EVAL (inspector, "sa.address.is_loopback", "true");
  ASSERT_EVAL (inspector, "sa.address == sa.address", "true");
  /* the enum is a nick */
  ASSERT_EVAL (inspector, "sa.family", "ipv4");
  /* the properties of the parent class are there too */
  ASSERT_EVAL (inspector, "#sa:properties() >= 4", "true");
  /* tab completion knows them */
  {
    g_auto (GStrv) words = shell_lua_inspector_complete (inspector, "sa.address.is_l", NULL);

    g_assert_cmpuint (g_strv_length (words), ==, 2);
    g_assert_cmpstr (words[0], ==, "is_link_local");
    g_assert_cmpstr (words[1], ==, "is_loopback");
  }

  g_object_unref (inspector);
}

static int
l_answer (lua_State *L)
{
  lua_pushinteger (L, 42);
  return 1;
}

static int
l_marker (lua_State *L)
{
  GObject *object = shell_lua_handle_check (L, 1, NULL);

  lua_pushstring (L, G_OBJECT_TYPE_NAME (object));
  return 1;
}

static void
test_host_integration (void)
{
  static const luaL_Reg methods[] = {
    { "marker", l_marker },
    { NULL, NULL }
  };
  ShellLuaInspector *inspector = new_inspector ();
  g_autoptr (GSimpleAction) action = g_simple_action_new ("x", NULL);

  shell_lua_inspector_add_object_methods (G_TYPE_SIMPLE_ACTION, methods);
  shell_lua_inspector_add_function (inspector, "answer", l_answer);
  shell_lua_inspector_set_global_object (inspector, "act", G_OBJECT (action));

  ASSERT_EVAL (inspector, "answer()", "42");
  ASSERT_EVAL (inspector, "act:marker()", "GSimpleAction");

  /* all of it comes back after a reset, and after the object went away */
  shell_lua_inspector_reset (inspector);
  ASSERT_EVAL (inspector, "answer()", "42");
  ASSERT_EVAL (inspector, "act.name", "x");
  g_clear_object (&action);
  shell_lua_inspector_reset (inspector);
  ASSERT_EVAL (inspector, "act", "nil");

  /* results start over */
  {
    GArray *entries = evaluate_ok (inspector, "1", NULL);

    g_assert_cmpuint (g_array_index (entries, Entry, 0).id, ==, 1);
    entries_free (entries);
  }

  /* a global can be defined while the state is alive, and removed */
  {
    g_autoptr (GSimpleAction) other = g_simple_action_new ("other", NULL);

    shell_lua_inspector_set_global_object (inspector, "other", G_OBJECT (other));
    ASSERT_EVAL (inspector, "other.name", "other");
    shell_lua_inspector_set_global_object (inspector, "other", NULL);
    shell_lua_inspector_reset (inspector);
    ASSERT_EVAL (inspector, "other", "nil");
  }

  g_object_unref (inspector);
}

static char *
complete_join (ShellLuaInspector *inspector, const char *text, char **head)
{
  g_auto (GStrv) words = shell_lua_inspector_complete (inspector, text, head);

  return g_strjoinv (",", words);
}

static void
test_complete (void)
{
  ShellLuaInspector *inspector = new_inspector ();
  g_autoptr (GSimpleAction) action = g_simple_action_new ("x", NULL);
  char *words = NULL;
  char *head = NULL;

  ASSERT_EVAL (inspector, "tbl = {alpha = 1, alpine = function() end, beta = 2, ['not valid'] = 3, 5}", "");
  shell_lua_inspector_set_global_object (inspector, "act", G_OBJECT (action));

  /* globals and keywords */
  words = complete_join (inspector, "tb", &head);
  g_assert_cmpstr (words, ==, "tbl");
  g_assert_cmpstr (head, ==, "tb");
  g_free (words);
  g_clear_pointer (&head, g_free);

  words = complete_join (inspector, "x = fun", &head);
  g_assert_cmpstr (words, ==, "function");
  g_assert_cmpstr (head, ==, "fun");
  g_free (words);
  g_clear_pointer (&head, g_free);

  words = complete_join (inspector, "str", NULL);
  g_assert_cmpstr (words, ==, "string");
  g_free (words);

  /* fields */
  words = complete_join (inspector, "tbl.al", &head);
  g_assert_cmpstr (words, ==, "alpha,alpine");
  g_assert_cmpstr (head, ==, "al");
  g_free (words);
  g_clear_pointer (&head, g_free);

  words = complete_join (inspector, "tbl.", &head);
  g_assert_cmpstr (words, ==, "alpha,alpine,beta");
  g_assert_cmpstr (head, ==, "");
  g_free (words);
  g_clear_pointer (&head, g_free);

  /* only the methods after a colon */
  words = complete_join (inspector, "tbl:", NULL);
  g_assert_cmpstr (words, ==, "alpine");
  g_free (words);

  /* nested paths and the standard library */
  words = complete_join (inspector, "string.fo", NULL);
  g_assert_cmpstr (words, ==, "format");
  g_free (words);
  words = complete_join (inspector, "shell.api_v", NULL);
  g_assert_cmpstr (words, ==, "api_version");
  g_free (words);

  /* the methods of a string, through its metatable */
  ASSERT_EVAL (inspector, "s = 'text'", "");
  words = complete_join (inspector, "s:upp", NULL);
  g_assert_cmpstr (words, ==, "upper");
  g_free (words);

  /* objects: properties and methods */
  words = complete_join (inspector, "act.ena", NULL);
  g_assert_cmpstr (words, ==, "enabled");
  g_free (words);
  words = complete_join (inspector, "act:gt", NULL);
  g_assert_cmpstr (words, ==, "gtype");
  g_free (words);
  words = complete_join (inspector, "act:ena", NULL);
  g_assert_cmpstr (words, ==, "");
  g_free (words);

  /* what is not a plain path, or fails, gives nothing and changes nothing */
  words = complete_join (inspector, "f(x).al", NULL);
  g_assert_cmpstr (words, ==, "");
  g_free (words);
  words = complete_join (inspector, "nothing.al", NULL);
  g_assert_cmpstr (words, ==, "");
  g_free (words);
  words = complete_join (inspector, "3.", NULL);
  g_assert_cmpstr (words, ==, "");
  g_free (words);
  words = complete_join (inspector, "a..", NULL);
  g_assert_cmpstr (words, ==, "");
  g_free (words);
  ASSERT_EVAL (inspector, "1 + 1", "2");

  g_object_unref (inspector);
}

static void
test_gc (void)
{
  ShellLuaInspector *inspector = new_inspector ();
  g_autoptr (GVariant) before = NULL;
  g_autoptr (GVariant) after = NULL;
  StatesInfo info_before, info_after;

  ASSERT_EVAL (inspector, "junk = {} for i = 1, 20000 do junk[i] = {i} end junk = nil", "");
  before = shell_lua_inspector_get_states (inspector);
  shell_lua_inspector_gc (inspector);
  after = shell_lua_inspector_get_states (inspector);

  read_states (before, &info_before);
  read_states (after, &info_after);
  g_assert_cmpuint (info_after.bytes, <, info_before.bytes);
  states_info_clear (&info_before);
  states_info_clear (&info_after);

  g_object_unref (inspector);
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
  g_test_add_func ("/shell-lua/inspector/evaluate/results", test_evaluate_results);
  g_test_add_func ("/shell-lua/inspector/evaluate/print-and-errors", test_evaluate_print_and_errors);
  g_test_add_func ("/shell-lua/inspector/inspect/table", test_inspect_table);
  g_test_add_func ("/shell-lua/inspector/objects", test_objects);
  g_test_add_func ("/shell-lua/inspector/object-properties", test_object_properties);
  g_test_add_func ("/shell-lua/inspector/host-integration", test_host_integration);
  g_test_add_func ("/shell-lua/inspector/complete", test_complete);
  g_test_add_func ("/shell-lua/inspector/gc", test_gc);

  return g_test_run ();
}
