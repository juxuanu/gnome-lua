/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaExtension and ShellLuaExtensionManager: discovery, validation,
 * lifecycle, ownership of resources, error policy, serialization.
 *
 * Extensions are written to temporary directories. Their runtime is observed
 * through a small test-only API module (shell.test.tick / shell.test.ping),
 * through shell.log (g_test_expect_message) and by evaluating Lua in the
 * extension's own state.
 */

#include "test-utils.h"

#include "shell-hooks.h"
#include "shell-lua-extension.h"
#include "shell-lua-extension-private.h"

#define TEST_SCHEMA "org.gnome-lua.test"

/* ------------------------------------------------------------------------ */
/* shell.test.*: counters the extensions can bump                            */
/* ------------------------------------------------------------------------ */

static int ticks = 0;
static int pings = 0;

static int
cf_tick (lua_State *L)
{
  ticks++;
  return 0;
}

static int
cf_ping (lua_State *L)
{
  pings++;
  return 0;
}

static void
test_api_register (ShellLuaState *state, lua_State *L)
{
  lua_newtable (L);
  lua_pushcfunction (L, cf_tick);
  lua_setfield (L, -2, "tick");
  lua_pushcfunction (L, cf_ping);
  lua_setfield (L, -2, "ping");
  lua_setfield (L, -2, "test");
}

/* ------------------------------------------------------------------------ */
/* Fixtures                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
  char                     *tmp;
  char                     *user;      /* highest priority, type PER_USER */
  char                     *system;    /* type SYSTEM */
  ShellLuaExtensionManager *manager;
} Env;

static void
env_init (Env *e)
{
  g_autoptr (GFile) user = NULL;
  g_autoptr (GFile) system = NULL;
  GFile *dirs[2];

  e->tmp = tu_tmpdir_new ();
  e->user = g_build_filename (e->tmp, "user", NULL);
  e->system = g_build_filename (e->tmp, "system", NULL);
  g_assert_cmpint (g_mkdir_with_parents (e->user, 0700), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (e->system, 0700), ==, 0);

  user = g_file_new_for_path (e->user);
  system = g_file_new_for_path (e->system);
  dirs[0] = user;
  dirs[1] = system;
  e->manager = g_object_new (SHELL_TYPE_LUA_EXTENSION_MANAGER, NULL);
  shell_lua_extension_manager_set_search_dirs (e->manager, dirs, 2);
}

static void
env_clear (Env *e)
{
  g_object_unref (e->manager);
  tu_rm_rf (e->tmp);
  g_free (e->tmp);
  g_free (e->user);
  g_free (e->system);
}

/* Writes <root>/<dirname>/{metadata.json,extension.lua}. @metadata is the
 * full JSON text, or NULL for a minimal valid one (uuid = dirname). */
static void
write_ext (const char *root, const char *dirname, const char *metadata, const char *lua)
{
  g_autofree char *dir = g_build_filename (root, dirname, NULL);
  g_autofree char *default_metadata = NULL;

  if (metadata == NULL)
    {
      default_metadata = g_strdup_printf ("{\"uuid\": \"%s\", \"name\": \"Test %s\", \"shell-api\": \"1\"}",
                                          dirname, dirname);
      metadata = default_metadata;
    }
  tu_write_file (dir, "metadata.json", metadata);
  tu_write_file (dir, "extension.lua", lua);
}

/* metadata with extra members spliced in */
static char *
metadata_with (const char *uuid, const char *extra)
{
  return g_strdup_printf ("{\"uuid\": \"%s\", \"name\": \"Test %s\", \"shell-api\": \"1\"%s%s}",
                          uuid, uuid, extra != NULL && extra[0] ? ", " : "", extra ? extra : "");
}

static ShellLuaExtension *
lookup (Env *e, const char *uuid)
{
  return shell_lua_extension_manager_lookup (e->manager, uuid);
}

static ShellLuaExtension *
must_lookup (Env *e, const char *uuid)
{
  ShellLuaExtension *ext = lookup (e, uuid);

  if (ext == NULL)
    g_error ("extension %s was not found", uuid);
  return ext;
}

/* scan + must_lookup */
static ShellLuaExtension *
scan_one (Env *e, const char *uuid)
{
  shell_lua_extension_manager_scan (e->manager);
  return must_lookup (e, uuid);
}

static void
enable_ok (Env *e, const char *uuid)
{
  g_autoptr (GError) error = NULL;

  if (!shell_lua_extension_manager_enable (e->manager, uuid, &error))
    g_error ("enabling %s failed: %s", uuid, error->message);
}

static void
disable_ok (Env *e, const char *uuid)
{
  g_autoptr (GError) error = NULL;

  if (!shell_lua_extension_manager_disable (e->manager, uuid, &error))
    g_error ("disabling %s failed: %s", uuid, error->message);
}

typedef struct {
  ShellLuaExtension      *ext;
  ShellLuaExtensionState  state;
} StateWait;

static gboolean
pred_state (gpointer data)
{
  StateWait *w = data;

  return shell_lua_extension_get_state (w->ext) == w->state;
}

static void
wait_for_state (ShellLuaExtension *ext, ShellLuaExtensionState state)
{
  StateWait w = { ext, state };

  if (!tu_wait_for (pred_state, &w, 3000))
    g_error ("%s: timed out waiting for state %s (is %s)", shell_lua_extension_get_uuid (ext),
             shell_lua_extension_state_to_string (state),
             shell_lua_extension_state_to_string (shell_lua_extension_get_state (ext)));
}

static void
assert_state (ShellLuaExtension *ext, ShellLuaExtensionState state)
{
  if (shell_lua_extension_get_state (ext) != state)
    g_error ("%s: state is %s, expected %s", shell_lua_extension_get_uuid (ext),
             shell_lua_extension_state_to_string (shell_lua_extension_get_state (ext)),
             shell_lua_extension_state_to_string (state));
}

static gboolean
pred_ticks_at_least (gpointer data)
{
  return ticks >= GPOINTER_TO_INT (data);
}

/* Wait (bounded) until the extensions' timers have ticked @n times in all. */
static void
wait_for_ticks (int n)
{
  if (!tu_wait_for (pred_ticks_at_least, GINT_TO_POINTER (n), 3000))
    g_error ("timed out waiting for %d ticks (have %d)", n, ticks);
}

typedef struct {
  ShellLuaExtension *ext;
  guint              errors;
} ErrorWait;

static gboolean
pred_errors (gpointer data)
{
  ErrorWait *w = data;

  return shell_lua_extension_get_error_count (w->ext) >= w->errors;
}

static void
on_state_changed (ShellLuaExtensionManager *manager, ShellLuaExtension *ext, GString *log)
{
  g_string_append_printf (log, "%s:%s;", shell_lua_extension_get_uuid (ext),
                          shell_lua_extension_state_to_string (shell_lua_extension_get_state (ext)));
}

static void
count_loaded (ShellLuaExtensionManager *manager, int *count)
{
  (*count)++;
}

/* Expect the warning of a failed pcall in the state of extension @uuid. */
static void
expect_ext_warning (const char *uuid, const char *substr)
{
  g_autofree char *domain_pattern = g_strdup_printf ("[%s] *%s*", uuid, substr);

  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, domain_pattern);
}

static void
expect_ext_log (const char *uuid, GLogLevelFlags level, const char *message)
{
  g_autofree char *domain = g_strdup_printf ("shell-lua/%s", uuid);

  g_test_expect_message (domain, level, message);
}

static const char *const BASIC_LUA =
  "local helper = require('helper')\n"
  "ENABLED = 0\n"
  "local ext = {}\n"
  "function ext.enable()\n"
  "  ENABLED = ENABLED + 1\n"
  "  HELPER = helper.value\n"
  "end\n"
  "function ext.disable()\n"
  "  shell.log.info('disable called')\n"
  "end\n"
  "return ext\n";

static const char *const PLAIN_LUA = "return {}\n";

/* ------------------------------------------------------------------------ */
/* Discovery                                                                 */
/* ------------------------------------------------------------------------ */

static void
test_scan (void)
{
  Env e;
  GString *states = g_string_new (NULL);
  int loaded = 0;
  g_autoptr (GList) list = NULL;
  ShellLuaExtension *a, *b, *c;
  g_autofree char *expected_path = NULL;

  env_init (&e);
  g_signal_connect (e.manager, "extension-state-changed", G_CALLBACK (on_state_changed), states);
  g_signal_connect (e.manager, "extensions-loaded", G_CALLBACK (count_loaded), &loaded);

  write_ext (e.user, "c@test", NULL, PLAIN_LUA);
  write_ext (e.user, "a@test", NULL, PLAIN_LUA);
  write_ext (e.system, "b@test", NULL, PLAIN_LUA);
  write_ext (e.system, "a@test", NULL, PLAIN_LUA);     /* shadowed by the per-user one */
  /* not extensions: missing extension.lua, missing metadata, a plain file */
  tu_write_file (e.user, "nolua@test/metadata.json", "{}");
  tu_write_file (e.user, "nometadata@test/extension.lua", "return {}");
  tu_write_file (e.user, "just-a-file", "x");

  shell_lua_extension_manager_scan (e.manager);

  list = shell_lua_extension_manager_list (e.manager);
  g_assert_cmpuint (g_list_length (list), ==, 3);
  g_assert_cmpstr (shell_lua_extension_get_uuid (list->data), ==, "a@test");       /* sorted */
  g_assert_cmpstr (shell_lua_extension_get_uuid (list->next->data), ==, "b@test");
  g_assert_cmpstr (shell_lua_extension_get_uuid (list->next->next->data), ==, "c@test");
  a = must_lookup (&e, "a@test");
  b = must_lookup (&e, "b@test");
  c = must_lookup (&e, "c@test");
  g_assert_null (lookup (&e, "nolua@test"));
  g_assert_null (lookup (&e, "nometadata@test"));
  g_assert_null (lookup (&e, "just-a-file"));
  g_assert_null (lookup (&e, "missing@test"));

  /* the first search directory is the per-user one and shadows the others */
  g_assert_cmpint (shell_lua_extension_get_extension_type (a), ==, SHELL_LUA_EXTENSION_TYPE_PER_USER);
  g_assert_cmpint (shell_lua_extension_get_extension_type (b), ==, SHELL_LUA_EXTENSION_TYPE_SYSTEM);
  g_assert_cmpint (shell_lua_extension_get_extension_type (c), ==, SHELL_LUA_EXTENSION_TYPE_PER_USER);
  expected_path = g_build_filename (e.user, "a@test", NULL);
  {
    g_autofree char *path = g_file_get_path (shell_lua_extension_get_dir (a));

    g_assert_cmpstr (path, ==, expected_path);
  }
  g_assert_cmpstr (shell_lua_extension_get_uuid (a), ==, "a@test");
  g_assert_cmpstr (json_object_get_string_member (shell_lua_extension_get_metadata (a), "name"), ==, "Test a@test");
  assert_state (a, SHELL_LUA_EXTENSION_STATE_INITIALIZED);
  g_assert_null (shell_lua_extension_get_error (a));
  g_assert_null (shell_lua_extension_get_lua_state (a));
  g_assert_false (shell_lua_extension_get_enabled (a));

  /* one state-changed per new extension, sorted, then extensions-loaded */
  g_assert_cmpstr (states->str, ==, "a@test:INITIALIZED;b@test:INITIALIZED;c@test:INITIALIZED;");
  g_assert_cmpint (loaded, ==, 1);

  /* rescanning changes nothing, new directories are picked up */
  g_string_truncate (states, 0);
  shell_lua_extension_manager_scan (e.manager);
  g_assert_cmpstr (states->str, ==, "");
  g_assert_cmpint (loaded, ==, 2);
  g_assert_true (a == lookup (&e, "a@test"));
  write_ext (e.user, "d@test", NULL, PLAIN_LUA);
  shell_lua_extension_manager_scan (e.manager);
  g_assert_cmpstr (states->str, ==, "d@test:INITIALIZED;");
  g_assert_nonnull (lookup (&e, "d@test"));

  g_string_free (states, TRUE);
  env_clear (&e);
}

static void
test_scan_missing_directories (void)
{
  ShellLuaExtensionManager *manager = g_object_new (SHELL_TYPE_LUA_EXTENSION_MANAGER, NULL);
  g_autoptr (GFile) nothing = g_file_new_for_path ("/nonexistent/shell-lua/extensions");
  GFile *dirs[1] = { nothing };
  g_autoptr (GList) list = NULL;

  shell_lua_extension_manager_set_search_dirs (manager, dirs, 1);
  shell_lua_extension_manager_scan (manager);      /* no warning, no crash */
  list = shell_lua_extension_manager_list (manager);
  g_assert_null (list);
  g_object_unref (manager);
}

static void
test_scan_rejects_invalid_metadata (void)
{
  static const struct {
    const char *dirname;
    const char *metadata;
    const char *message;      /* glob */
  } cases[] = {
    { "dir@test", "{\"uuid\": \"other@test\", \"name\": \"N\", \"shell-api\": \"1\"}",
      "*UUID \"other@test\" from metadata.json does not match directory name \"dir@test\"" },
    { "noname@test", "{\"uuid\": \"noname@test\", \"shell-api\": \"1\"}",
      "*missing \"name\" property in metadata.json" },
    { "noapi@test", "{\"uuid\": \"noapi@test\", \"name\": \"N\"}",
      "*missing \"shell-api\" property in metadata.json" },
    { "nouuid@test", "{\"name\": \"N\", \"shell-api\": \"1\"}",
      "*missing \"uuid\" property in metadata.json" },
    { "numuuid@test", "{\"uuid\": 5, \"name\": \"N\", \"shell-api\": \"1\"}",
      "*property \"uuid\" is not of type string" },
    { "badjson@test", "{\"uuid\": ", "*Failed to parse metadata.json*" },
    { "array@test", "[1, 2]", "*metadata.json does not contain an object" },
    { "cap@test", "{\"uuid\": \"cap@test\", \"name\": \"N\", \"shell-api\": \"1\", \"capabilities\": [\"teleport\"]}",
      "*unknown capability \"teleport\" in metadata.json" },
    { "caparr@test", "{\"uuid\": \"caparr@test\", \"name\": \"N\", \"shell-api\": \"1\", \"capabilities\": \"dbus\"}",
      "*property \"capabilities\" is not of type string array" },
    { "capnum@test", "{\"uuid\": \"capnum@test\", \"name\": \"N\", \"shell-api\": \"1\", \"capabilities\": [1]}",
      "*property \"capabilities\" is not of type string array" },
    { "desc@test", "{\"uuid\": \"desc@test\", \"name\": \"N\", \"shell-api\": \"1\", \"description\": 1}",
      "*property \"description\" is not of type string" },
    { "schema@test", "{\"uuid\": \"schema@test\", \"name\": \"N\", \"shell-api\": \"1\", \"settings-schema\": []}",
      "*property \"settings-schema\" is not of type string" },
  };
  Env e;
  guint i;

  env_init (&e);

  /* one bad extension per scan: the order of the warnings is then fixed */
  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *dir = g_build_filename (e.user, cases[i].dirname, NULL);
      g_autofree char *pattern = g_strdup_printf ("Could not load Lua extension %s: %s",
                                                  cases[i].dirname, cases[i].message);

      write_ext (e.user, cases[i].dirname, cases[i].metadata, PLAIN_LUA);
      g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, pattern);
      shell_lua_extension_manager_scan (e.manager);
      g_test_assert_expected_messages ();
      g_assert_null (lookup (&e, cases[i].dirname));
      tu_rm_rf (dir);
    }

  /* valid ones are unaffected by the failures */
  write_ext (e.user, "good@test", NULL, PLAIN_LUA);
  shell_lua_extension_manager_scan (e.manager);
  g_assert_nonnull (lookup (&e, "good@test"));

  env_clear (&e);
}

static void
test_shell_api_versions (void)
{
  static const struct {
    const char *api;
    gboolean    supported;
  } cases[] = {
    { "1", TRUE }, { "1.0", TRUE }, { "1.1", TRUE },
    /* additive minors: the running minor (SHELL_LUA_API_MINOR) must be at least N */
    { "1.2", FALSE }, { "1.7", FALSE }, { "1.", FALSE }, { "1.x", FALSE }, { "1.0.1", FALSE },
    { "1.-1", FALSE }, { ".1", FALSE },
    { "2", FALSE }, { "0", FALSE }, { "10", FALSE }, { "11.0", FALSE }, { "x", FALSE },
    { "1x", FALSE }, { "-1", FALSE }, { "1,0", FALSE }, { "47", FALSE },
  };
  Env e;
  guint i;

  env_init (&e);
  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *uuid = g_strdup_printf ("api%u@test", i);
      g_autofree char *extra = g_strdup_printf ("\"x\": 1");
      g_autofree char *metadata = g_strdup_printf ("{\"uuid\": \"%s\", \"name\": \"N\", \"shell-api\": \"%s\", %s}",
                                                   uuid, cases[i].api, extra);

      write_ext (e.user, uuid, metadata, PLAIN_LUA);
    }
  shell_lua_extension_manager_scan (e.manager);

  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *uuid = g_strdup_printf ("api%u@test", i);
      ShellLuaExtension *ext = must_lookup (&e, uuid);
      g_autoptr (GError) error = NULL;

      if (cases[i].supported)
        {
          assert_state (ext, SHELL_LUA_EXTENSION_STATE_INITIALIZED);
          g_assert_null (shell_lua_extension_get_error (ext));
        }
      else
        {
          g_autofree char *message = g_strdup_printf ("shell-api %s not supported", cases[i].api);

          assert_state (ext, SHELL_LUA_EXTENSION_STATE_OUT_OF_DATE);
          g_assert_cmpstr (shell_lua_extension_get_error (ext), ==, message);
          /* it is listed but cannot be enabled */
          g_assert_false (shell_lua_extension_manager_enable (e.manager, uuid, &error));
          g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
          assert_state (ext, SHELL_LUA_EXTENSION_STATE_OUT_OF_DATE);
          g_assert_null (shell_lua_extension_get_lua_state (ext));
        }
    }

  env_clear (&e);
}

static void
test_capabilities (void)
{
  Env e;
  ShellLuaExtension *ext;
  const char * const *caps;

  env_init (&e);
  write_ext (e.user, "caps@test",
             "{\"uuid\": \"caps@test\", \"name\": \"N\", \"shell-api\": \"1\", "
             "\"capabilities\": [\"dbus\", \"settings\", \"search\", \"fs\", \"spawn\", \"network\", \"dbus\"]}",
             PLAIN_LUA);
  write_ext (e.user, "nocaps@test", NULL, PLAIN_LUA);
  shell_lua_extension_manager_scan (e.manager);

  ext = must_lookup (&e, "caps@test");
  caps = shell_lua_extension_get_capabilities (ext);
  g_assert_cmpuint (g_strv_length ((char **) caps), ==, 6);      /* duplicates collapse */
  g_assert_cmpstr (caps[0], ==, "dbus");
  g_assert_cmpstr (caps[2], ==, "search");
  g_assert_cmpstr (caps[5], ==, "network");
  g_assert_true (shell_lua_extension_has_capability (ext, "search"));
  g_assert_true (shell_lua_extension_has_capability (ext, "settings"));
  g_assert_true (shell_lua_extension_has_capability (ext, "spawn"));
  g_assert_false (shell_lua_extension_has_capability (ext, "teleport"));

  ext = must_lookup (&e, "nocaps@test");
  g_assert_nonnull (shell_lua_extension_get_capabilities (ext));
  g_assert_null (shell_lua_extension_get_capabilities (ext)[0]);
  g_assert_false (shell_lua_extension_has_capability (ext, "dbus"));

  env_clear (&e);
}

static void
test_search_path_env (void)
{
  g_autofree char *tmp = tu_tmpdir_new ();
  g_autofree char *dir_a = g_build_filename (tmp, "a", NULL);
  g_autofree char *dir_b = g_build_filename (tmp, "b", NULL);
  g_autofree char *path = g_strjoin (G_SEARCHPATH_SEPARATOR_S, dir_a, "", dir_b, NULL);
  ShellLuaExtensionManager *manager;
  ShellLuaExtension *shared, *only_b;

  write_ext (dir_a, "shared@test", NULL, PLAIN_LUA);
  write_ext (dir_b, "shared@test", NULL, PLAIN_LUA);
  write_ext (dir_b, "only-b@test", NULL, PLAIN_LUA);

  /* the variable is read when the manager is created */
  g_assert_true (g_setenv ("SHELL_LUA_EXTENSION_PATH", path, TRUE));
  manager = g_object_new (SHELL_TYPE_LUA_EXTENSION_MANAGER, NULL);
  g_unsetenv ("SHELL_LUA_EXTENSION_PATH");

  shell_lua_extension_manager_scan (manager);
  shared = shell_lua_extension_manager_lookup (manager, "shared@test");
  only_b = shell_lua_extension_manager_lookup (manager, "only-b@test");
  g_assert_nonnull (shared);
  g_assert_nonnull (only_b);
  {
    g_autofree char *shared_path = g_file_get_path (shell_lua_extension_get_dir (shared));
    g_autofree char *expected = g_build_filename (dir_a, "shared@test", NULL);

    g_assert_cmpstr (shared_path, ==, expected);       /* earlier entries win */
  }
  /* every entry behaves like a per-user directory */
  g_assert_cmpint (shell_lua_extension_get_extension_type (shared), ==, SHELL_LUA_EXTENSION_TYPE_PER_USER);
  g_assert_cmpint (shell_lua_extension_get_extension_type (only_b), ==, SHELL_LUA_EXTENSION_TYPE_PER_USER);

  g_object_unref (manager);
  tu_rm_rf (tmp);
}

static void
test_per_user_shadows_system_and_removal (void)
{
  Env e;
  GString *states = g_string_new (NULL);
  ShellLuaExtension *ext;
  g_autofree char *user_dir = NULL;

  env_init (&e);
  write_ext (e.user, "dup@test", NULL, BASIC_LUA);
  tu_write_file (e.user, "dup@test/helper.lua", "return {value = 'user'}");
  write_ext (e.system, "dup@test", NULL, BASIC_LUA);
  tu_write_file (e.system, "dup@test/helper.lua", "return {value = 'system'}");

  ext = scan_one (&e, "dup@test");
  g_assert_cmpint (shell_lua_extension_get_extension_type (ext), ==, SHELL_LUA_EXTENSION_TYPE_PER_USER);
  enable_ok (&e, "dup@test");
  tu_assert_eval_str (shell_lua_extension_get_lua_state (ext), "HELPER", "user");

  /* removing the per-user copy: the running extension is disabled and
   * uninstalled, and the system copy shows up in its place */
  g_signal_connect (e.manager, "extension-state-changed", G_CALLBACK (on_state_changed), states);
  user_dir = g_build_filename (e.user, "dup@test", NULL);
  tu_rm_rf (user_dir);
  expect_ext_log ("dup@test", G_LOG_LEVEL_INFO, "disable called");
  shell_lua_extension_manager_scan (e.manager);
  g_test_assert_expected_messages ();
  g_assert_cmpstr (states->str, ==, "dup@test:UNINSTALLED;dup@test:INITIALIZED;");
  ext = must_lookup (&e, "dup@test");
  g_assert_cmpint (shell_lua_extension_get_extension_type (ext), ==, SHELL_LUA_EXTENSION_TYPE_SYSTEM);
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_INITIALIZED);

  /* and removing that one too makes it disappear */
  g_string_truncate (states, 0);
  g_free (user_dir);
  user_dir = g_build_filename (e.system, "dup@test", NULL);
  tu_rm_rf (user_dir);
  shell_lua_extension_manager_scan (e.manager);
  g_assert_null (lookup (&e, "dup@test"));
  g_assert_cmpstr (states->str, ==, "dup@test:UNINSTALLED;");

  g_string_free (states, TRUE);
  env_clear (&e);
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

static void
test_enable_disable_lifecycle (void)
{
  Env e;
  GString *states = g_string_new (NULL);
  ShellLuaExtension *ext;
  ShellLuaState *state;
  lua_State *L;
  guint state_count = shell_lua_get_state_count ();
  g_autoptr (GError) error = NULL;

  env_init (&e);
  write_ext (e.user, "life@test", NULL, BASIC_LUA);
  tu_write_file (e.user, "life@test/helper.lua", "return {value = 'from helper'}");
  ext = scan_one (&e, "life@test");
  g_signal_connect (e.manager, "extension-state-changed", G_CALLBACK (on_state_changed), states);

  g_assert_null (shell_lua_extension_get_lua_state (ext));
  g_assert_cmpuint (shell_lua_get_state_count (), ==, state_count);

  enable_ok (&e, "life@test");
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  g_assert_cmpstr (states->str, ==, "life@test:ACTIVATING;life@test:ACTIVE;");
  g_assert_null (shell_lua_extension_get_error (ext));

  /* the extension runs in its own sandboxed state, which knows its owner */
  state = g_object_ref (shell_lua_extension_get_lua_state (ext));     /* outlive the disable below */
  g_assert_nonnull (state);
  g_assert_cmpuint (shell_lua_get_state_count (), ==, state_count + 1);
  g_assert_cmpstr (shell_lua_state_get_id (state), ==, "life@test");
  g_assert_cmpint (shell_lua_state_get_kind (state), ==, SHELL_LUA_STATE_SANDBOXED);
  g_assert_true (shell_lua_state_get_owner (state) == ext);
  /* extension.lua ran once, enable() once, and require() works inside the directory */
  L = shell_lua_state_get_L (state);
  lua_getglobal (L, "ENABLED");
  g_assert_cmpint (lua_tointeger (L, -1), ==, 1);
  lua_pop (L, 1);
  tu_assert_eval_str (state, "HELPER", "from helper");
  tu_assert_eval_true (state, "os == nil and io == nil");

  /* enabling an active extension is a no-op */
  g_string_truncate (states, 0);
  enable_ok (&e, "life@test");
  g_assert_cmpstr (states->str, ==, "");
  g_assert_true (shell_lua_extension_get_lua_state (ext) == state);

  /* disable(): the Lua disable() runs, then the state is gone */
  expect_ext_log ("life@test", G_LOG_LEVEL_INFO, "disable called");
  disable_ok (&e, "life@test");
  g_test_assert_expected_messages ();
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_INACTIVE);
  g_assert_null (shell_lua_extension_get_lua_state (ext));
  g_assert_cmpstr (states->str, ==, "life@test:DEACTIVATING;life@test:INACTIVE;");
  g_assert_false (shell_lua_state_is_alive (state));
  g_assert_cmpuint (shell_lua_get_state_count (), ==, state_count);
  g_clear_object (&state);

  /* disabling again changes nothing */
  g_string_truncate (states, 0);
  disable_ok (&e, "life@test");
  g_assert_cmpstr (states->str, ==, "");

  /* enable again: a fresh state, extension.lua runs again from scratch */
  enable_ok (&e, "life@test");
  state = shell_lua_extension_get_lua_state (ext);
  g_assert_nonnull (state);
  tu_assert_eval_str (state, "ENABLED", "1");
  expect_ext_log ("life@test", G_LOG_LEVEL_INFO, "disable called");
  disable_ok (&e, "life@test");
  g_test_assert_expected_messages ();

  /* unknown uuids */
  g_assert_false (shell_lua_extension_manager_enable (e.manager, "nope@test", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);
  g_assert_false (shell_lua_extension_manager_disable (e.manager, "nope@test", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);

  g_string_free (states, TRUE);
  env_clear (&e);
}

static void
test_extension_table (void)
{
  Env e;
  ShellLuaExtension *ext;
  ShellLuaState *state;
  g_autofree char *dir = NULL;
  g_autofree char *metadata = NULL;
  g_autofree char *expr = NULL;

  env_init (&e);
  metadata = metadata_with ("table@test",
                            "\"version\": 3, \"description\": \"d\", \"capabilities\": [\"dbus\", \"settings\"], "
                            "\"extra\": {\"list\": [1, \"two\", true], \"obj\": {\"k\": \"v\"}, \"ratio\": 0.5, \"none\": null}");
  write_ext (e.user, "table@test", metadata, PLAIN_LUA);
  ext = scan_one (&e, "table@test");
  enable_ok (&e, "table@test");
  state = shell_lua_extension_get_lua_state (ext);
  dir = g_build_filename (e.user, "table@test", NULL);

  tu_assert_eval_str (state, "shell.extension.uuid", "table@test");
  expr = g_strdup_printf ("shell.extension.dir == '%s'", dir);
  tu_assert_eval_true (state, expr);
  tu_assert_eval_str (state, "shell.extension.metadata.name", "Test table@test");
  tu_assert_eval_str (state, "shell.extension.metadata.uuid", "table@test");
  tu_assert_eval_str (state, "shell.extension.metadata['shell-api']", "1");
  tu_assert_eval_str (state, "shell.extension.metadata.version", "3");
  tu_assert_eval_str (state, "math.type(shell.extension.metadata.version)", "integer");
  tu_assert_eval_true (state, "shell.extension.metadata.extra.list[1] == 1 and "
                              "shell.extension.metadata.extra.list[2] == 'two' and "
                              "shell.extension.metadata.extra.list[3] == true and "
                              "#shell.extension.metadata.extra.list == 3");
  tu_assert_eval_str (state, "shell.extension.metadata.extra.obj.k", "v");
  tu_assert_eval_str (state, "shell.extension.metadata.extra.ratio", "0.5");
  tu_assert_eval_str (state, "shell.extension.metadata.extra.none", "nil");
  tu_assert_eval_true (state, "#shell.extension.capabilities == 2 and shell.extension.capabilities[1] == 'dbus' "
                              "and shell.extension.capabilities[2] == 'settings'");
  tu_assert_eval_true (state, "(function() local n = 0 for i, c in ipairs(shell.extension.capabilities) do n = n + 1 end "
                              "return n == 2 end)()");
  tu_assert_eval_true (state, "(function() local n = 0 for k, v in pairs(shell.extension.metadata.extra.obj) do n = n + 1 end "
                              "return n == 1 end)()");
  tu_assert_eval_true (state, "rawequal(shell.extension, shell.extension)");

  /* read-only copies */
  tu_assert_fails (state, "shell.extension.metadata.name = 'x'", SHELL_LUA_ERROR_RUNTIME, "read-only");
  tu_assert_fails (state, "shell.extension.metadata.extra.list[1] = 5", SHELL_LUA_ERROR_RUNTIME, "read-only");
  tu_assert_fails (state, "shell.extension.capabilities[3] = 'fs'", SHELL_LUA_ERROR_RUNTIME, "read-only");
  tu_assert_fails (state, "shell.extension.uuid = 'x'", SHELL_LUA_ERROR_RUNTIME, "read-only");
  tu_assert_eval_str (state, "shell.extension.metadata.name", "Test table@test");

  disable_ok (&e, "table@test");
  env_clear (&e);
}

static void
test_disable_disposes_owned_resources (void)
{
  Env e;
  ShellLuaExtension *ext;
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  int before;

  shell_hook_registry_declare (registry, "ext.ping", "", FALSE, NULL);
  env_init (&e);
  write_ext (e.user, "owner@test", NULL,
             "local ext = {}\n"
             "function ext.enable()\n"
             "  shell.timer.every{ms = 5, callback = function() shell.test.tick() end}\n"
             "  shell.timer.after{ms = 100000, callback = function() shell.test.tick() end}\n"
             "  shell.timer.idle{callback = function() shell.test.tick() end}\n"
             "  shell.hooks.on('ext.ping', function() shell.test.ping() end)\n"
             "  conn = shell.hooks.on('ext.ping', function() shell.test.ping() end)\n"
             "end\n"
             "return ext\n");
  ext = scan_one (&e, "owner@test");

  ticks = pings = 0;
  enable_ok (&e, "owner@test");
  g_assert_cmpuint (shell_hook_registry_count_listeners (registry, "ext.ping", SHELL_HOOK_PHASE_AFTER), ==, 2);

  /* the timers run while enabled */
  wait_for_ticks (3);
  shell_hook_registry_emit_after (registry, "ext.ping", NULL);
  g_assert_cmpint (pings, ==, 2);

  /* disabling releases all of it */
  disable_ok (&e, "owner@test");
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_INACTIVE);
  before = ticks;
  tu_spin (100);
  g_assert_cmpint (ticks, ==, before);        /* no callback after disable */
  pings = 0;
  shell_hook_registry_emit_after (registry, "ext.ping", NULL);
  g_assert_cmpint (pings, ==, 0);
  g_assert_cmpuint (shell_hook_registry_count_listeners (registry, "ext.ping", SHELL_HOOK_PHASE_AFTER), ==, 0);

  /* and a second round behaves the same */
  enable_ok (&e, "owner@test");
  wait_for_ticks (before + 2);
  disable_ok (&e, "owner@test");
  before = ticks;
  tu_spin (60);
  g_assert_cmpint (ticks, ==, before);

  env_clear (&e);
}

static void
test_manager_dispose_disables (void)
{
  Env e;
  ShellLuaExtension *ext;
  int before;

  env_init (&e);
  write_ext (e.user, "gone@test", NULL,
             "local ext = {}\n"
             "function ext.enable() shell.timer.every{ms = 5, callback = function() shell.test.tick() end} end\n"
             "function ext.disable() shell.log.info('disabled by dispose') end\n"
             "return ext\n");
  ext = scan_one (&e, "gone@test");
  g_object_ref (ext);
  ticks = 0;
  enable_ok (&e, "gone@test");
  wait_for_ticks (2);

  expect_ext_log ("gone@test", G_LOG_LEVEL_INFO, "disabled by dispose");
  g_object_unref (e.manager);
  e.manager = g_object_new (SHELL_TYPE_LUA_EXTENSION_MANAGER, NULL);     /* for env_clear */
  g_test_assert_expected_messages ();
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_INACTIVE);
  g_assert_null (shell_lua_extension_get_lua_state (ext));
  before = ticks;
  tu_spin (50);
  g_assert_cmpint (ticks, ==, before);
  g_object_unref (ext);

  env_clear (&e);
}

/* ------------------------------------------------------------------------ */
/* Failures                                                                  */
/* ------------------------------------------------------------------------ */

static void
test_enable_failures (void)
{
  static const struct {
    const char          *uuid;
    const char          *lua;
    ShellLuaError        code;
    const char          *message;
    gboolean             logs;       /* a failed pcall: the sandbox warning */
  } cases[] = {
    { "raise-enable@test",
      "return { enable = function() error('enable exploded') end }",
      SHELL_LUA_ERROR_RUNTIME, "enable exploded", TRUE },
    { "raise-load@test",
      "error('load exploded')",
      SHELL_LUA_ERROR_RUNTIME, "load exploded", TRUE },
    { "syntax@test",
      "this is not lua",
      SHELL_LUA_ERROR_SYNTAX, "extension.lua:1:", FALSE },
    { "no-table@test",
      "return 5",
      SHELL_LUA_ERROR_RUNTIME, "extension.lua must return a table", FALSE },
    { "nothing@test",
      "local x = 1",
      SHELL_LUA_ERROR_RUNTIME, "extension.lua must return a table", FALSE },
    { "sandbox@test",
      "return { enable = function() return os.execute('true') end }",
      SHELL_LUA_ERROR_RUNTIME, "attempt to index a nil value (global 'os')", TRUE },
    { "require@test",
      "require('../escape')",
      SHELL_LUA_ERROR_SANDBOX, "invalid module name", TRUE },
  };
  Env e;
  guint i;

  env_init (&e);
  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    write_ext (e.user, cases[i].uuid, NULL, cases[i].lua);
  shell_lua_extension_manager_scan (e.manager);

  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      ShellLuaExtension *ext = must_lookup (&e, cases[i].uuid);
      g_autoptr (GError) error = NULL;

      if (cases[i].logs)
        expect_ext_warning (cases[i].uuid, cases[i].message);
      g_assert_false (shell_lua_extension_manager_enable (e.manager, cases[i].uuid, &error));
      g_test_assert_expected_messages ();
      g_assert_error (error, SHELL_LUA_ERROR, (int) cases[i].code);
      g_assert_nonnull (strstr (error->message, cases[i].message));

      /* ERROR state, the message is kept, nothing is left running */
      assert_state (ext, SHELL_LUA_EXTENSION_STATE_ERROR);
      g_assert_nonnull (shell_lua_extension_get_error (ext));
      g_assert_nonnull (strstr (shell_lua_extension_get_error (ext), cases[i].message));
      g_assert_null (shell_lua_extension_get_lua_state (ext));
    }

  /* disabling an extension in ERROR is fine and leaves it there */
  disable_ok (&e, "raise-enable@test");
  assert_state (must_lookup (&e, "raise-enable@test"), SHELL_LUA_EXTENSION_STATE_ERROR);

  env_clear (&e);
}

static void
test_enable_without_enable_function (void)
{
  Env e;
  ShellLuaExtension *ext;

  env_init (&e);
  write_ext (e.user, "plain@test", NULL, "RAN = true return {}");
  write_ext (e.user, "weird@test", NULL, "return { enable = 'not a function', disable = 5 }");
  write_ext (e.user, "meta@test", NULL,
             "return setmetatable({}, {__index = function() error('must not be consulted') end})");
  shell_lua_extension_manager_scan (e.manager);

  /* enable()/disable() are optional, and only plain functions count */
  enable_ok (&e, "plain@test");
  assert_state (must_lookup (&e, "plain@test"), SHELL_LUA_EXTENSION_STATE_ACTIVE);
  tu_assert_eval_str (shell_lua_extension_get_lua_state (must_lookup (&e, "plain@test")), "RAN", "true");
  enable_ok (&e, "weird@test");
  assert_state (must_lookup (&e, "weird@test"), SHELL_LUA_EXTENSION_STATE_ACTIVE);
  enable_ok (&e, "meta@test");
  ext = must_lookup (&e, "meta@test");
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  disable_ok (&e, "plain@test");
  disable_ok (&e, "weird@test");
  disable_ok (&e, "meta@test");
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_INACTIVE);

  env_clear (&e);
}

static void
test_error_in_disable (void)
{
  Env e;
  ShellLuaExtension *ext;

  env_init (&e);
  write_ext (e.user, "baddisable@test", NULL,
             "return { enable = function() shell.timer.every{ms = 5, callback = function() shell.test.tick() end} end,\n"
             "         disable = function() error('disable exploded') end }\n");
  ext = scan_one (&e, "baddisable@test");
  enable_ok (&e, "baddisable@test");

  /* the failure is logged twice (the state, then the extension), and the
   * teardown carries on regardless */
  expect_ext_warning ("baddisable@test", "disable exploded");
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
                         "Error in disable() of Lua extension baddisable@test: *disable exploded*");
  disable_ok (&e, "baddisable@test");
  g_test_assert_expected_messages ();
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_INACTIVE);
  g_assert_null (shell_lua_extension_get_lua_state (ext));
  {
    int before = ticks;

    tu_spin (50);
    g_assert_cmpint (ticks, ==, before);
  }

  env_clear (&e);
}

static void
test_error_policy_limit (void)
{
  Env e;
  ShellLuaExtension *ext;
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  GString *states = g_string_new (NULL);
  g_autoptr (GError) error = NULL;
  int i;

  shell_hook_registry_declare (registry, "ext.boom", "", FALSE, NULL);
  env_init (&e);
  shell_lua_extension_manager_set_error_limit (e.manager, 3);
  write_ext (e.user, "boom@test", NULL,
             "return { enable = function()\n"
             "  shell.hooks.on('ext.boom', function() error('boom') end)\n"
             "end }\n");
  ext = scan_one (&e, "boom@test");
  g_signal_connect (e.manager, "extension-state-changed", G_CALLBACK (on_state_changed), states);
  enable_ok (&e, "boom@test");
  g_string_truncate (states, 0);

  /* up to the limit the extension stays active */
  for (i = 0; i < 3; i++)
    {
      expect_ext_warning ("boom@test", "boom");
      shell_hook_registry_emit_after (registry, "ext.boom", NULL);
    }
  g_test_assert_expected_messages ();
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 3);
  tu_spin (50);
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_ACTIVE);

  /* one more tips it over: disabled from an idle, in state ERROR */
  expect_ext_warning ("boom@test", "boom");
  shell_hook_registry_emit_after (registry, "ext.boom", NULL);
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 4);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
                         "Lua extension boom@test raised more than 3 errors, disabling it");
  wait_for_state (ext, SHELL_LUA_EXTENSION_STATE_ERROR);
  g_test_assert_expected_messages ();
  g_assert_nonnull (shell_lua_extension_get_error (ext));
  g_assert_nonnull (strstr (shell_lua_extension_get_error (ext), "boom"));
  g_assert_null (shell_lua_extension_get_lua_state (ext));
  g_assert_cmpstr (states->str, ==, "boom@test:ERROR;");
  /* everything it owned is gone */
  g_assert_cmpuint (shell_hook_registry_count_listeners (registry, "ext.boom", SHELL_HOOK_PHASE_AFTER), ==, 0);
  shell_hook_registry_emit_after (registry, "ext.boom", NULL);

  /* enabling by hand gives it a fresh start */
  enable_ok (&e, "boom@test");
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  g_assert_null (shell_lua_extension_get_error (ext));
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 0);
  expect_ext_warning ("boom@test", "boom");
  shell_hook_registry_emit_after (registry, "ext.boom", NULL);
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 1);
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  disable_ok (&e, "boom@test");

  g_string_free (states, TRUE);
  env_clear (&e);
}

static void
test_failing_timer_callback_does_not_repeat (void)
{
  Env e;
  ShellLuaExtension *ext;

  env_init (&e);
  shell_lua_extension_manager_set_error_limit (e.manager, 3);
  write_ext (e.user, "timer-boom@test", NULL,
             "return { enable = function()\n"
             "  shell.timer.every{ms = 5, callback = function() shell.test.tick(); error('tick failed') end}\n"
             "end }\n");
  ext = scan_one (&e, "timer-boom@test");
  ticks = 0;
  enable_ok (&e, "timer-boom@test");

  expect_ext_warning ("timer-boom@test", "tick failed");
  {
    ErrorWait w = { ext, 1 };

    g_assert_true (tu_wait_for (pred_errors, &w, 3000));
  }
  tu_spin (100);       /* a repeating timer would have ticked again by now */
  g_test_assert_expected_messages ();
  /* it ran once, failed once and stopped: no error flood, still enabled */
  g_assert_cmpint (ticks, ==, 1);
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 1);
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_ACTIVE);

  disable_ok (&e, "timer-boom@test");
  env_clear (&e);
}

static void
test_limits_apply (void)
{
  ShellLuaLimits limits = { .hook_interval = 1000, .callback_budget_us = 50 * 1000 };
  Env e;
  ShellLuaExtension *ext;
  g_autoptr (GError) error = NULL;

  env_init (&e);
  shell_lua_extension_manager_set_limits (e.manager, &limits);
  write_ext (e.user, "loop@test", NULL, "return { enable = function() while true do end end }");
  write_ext (e.user, "quick@test", NULL, "return { enable = function() END = 1 end }");
  shell_lua_extension_manager_scan (e.manager);

  ext = must_lookup (&e, "loop@test");
  expect_ext_warning ("loop@test", "watchdog");
  g_assert_false (shell_lua_extension_manager_enable (e.manager, "loop@test", &error));
  g_test_assert_expected_messages ();
  g_assert_error (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_WATCHDOG);
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_ERROR);
  g_assert_nonnull (strstr (shell_lua_extension_get_error (ext), "watchdog: callback exceeded its time budget (50 ms)"));

  /* limits set on a manager apply to extensions found later too */
  write_ext (e.user, "later@test", NULL, "return { enable = function() while true do end end }");
  shell_lua_extension_manager_scan (e.manager);
  g_clear_error (&error);
  expect_ext_warning ("later@test", "watchdog");
  g_assert_false (shell_lua_extension_manager_enable (e.manager, "later@test", &error));
  g_test_assert_expected_messages ();
  g_assert_error (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_WATCHDOG);

  /* well-behaved extensions are not affected */
  enable_ok (&e, "quick@test");
  assert_state (must_lookup (&e, "quick@test"), SHELL_LUA_EXTENSION_STATE_ACTIVE);

  env_clear (&e);
}

/* ------------------------------------------------------------------------ */
/* Serialization                                                             */
/* ------------------------------------------------------------------------ */

static double
lookup_double (GVariant *dict, const char *key)
{
  double value = -1;

  if (!g_variant_lookup (dict, key, "d", &value))
    g_error ("key '%s' is missing or not a double in %s", key, g_variant_print (dict, TRUE));
  return value;
}

static char *
lookup_string (GVariant *dict, const char *key)
{
  char *value = NULL;

  if (!g_variant_lookup (dict, key, "s", &value))
    g_error ("key '%s' is missing or not a string in %s", key, g_variant_print (dict, TRUE));
  return value;
}

static gboolean
lookup_bool (GVariant *dict, const char *key)
{
  gboolean value = FALSE;

  if (!g_variant_lookup (dict, key, "b", &value))
    g_error ("key '%s' is missing or not a boolean in %s", key, g_variant_print (dict, TRUE));
  return value;
}

static void
test_serialize (void)
{
  Env e;
  ShellLuaExtension *ext, *plain;
  g_autoptr (GVariant) v = NULL;
  g_autoptr (GVariant) modes = NULL;
  g_autofree char *path = NULL;
  g_autofree char *s = NULL;
  g_autofree char *metadata = NULL;
  GVariantIter iter;
  GVariant *child;
  GString *mode_list = g_string_new (NULL);

  env_init (&e);
  metadata = metadata_with ("ser@test",
                            "\"description\": \"A description\", \"version\": 3, \"url\": \"https://example.org\", "
                            "\"session-modes\": [\"user\", \"unlock-dialog\"], "
                            "\"nested\": {\"list\": [1, 2], \"flag\": true}, \"skipped\": null");
  write_ext (e.user, "ser@test", metadata, PLAIN_LUA);
  tu_write_file (e.user, "ser@test/prefs.lua", "return {}");
  write_ext (e.system, "plain@test", NULL, PLAIN_LUA);
  shell_lua_extension_manager_scan (e.manager);
  ext = must_lookup (&e, "ser@test");
  plain = must_lookup (&e, "plain@test");

  v = shell_lua_extension_serialize (ext);
  g_assert_true (g_variant_is_of_type (v, G_VARIANT_TYPE_VARDICT));
  g_assert_false (g_variant_is_floating (v));
  s = lookup_string (v, "uuid");
  g_assert_cmpstr (s, ==, "ser@test");
  g_free (s);
  s = lookup_string (v, "name");
  g_assert_cmpstr (s, ==, "Test ser@test");
  g_free (s);
  s = lookup_string (v, "description");
  g_assert_cmpstr (s, ==, "A description");
  g_free (s);
  s = lookup_string (v, "url");
  g_assert_cmpstr (s, ==, "https://example.org");
  g_free (s);
  s = lookup_string (v, "shell-api");
  g_assert_cmpstr (s, ==, "1");
  g_free (s);
  s = lookup_string (v, "path");
  path = g_build_filename (e.user, "ser@test", NULL);
  g_assert_cmpstr (s, ==, path);
  g_free (s);
  s = lookup_string (v, "error");
  g_assert_cmpstr (s, ==, "");
  g_clear_pointer (&s, g_free);
  /* numbers are doubles, as in the JS implementation */
  g_assert_cmpfloat (lookup_double (v, "type"), ==, 2.0);
  g_assert_cmpfloat (lookup_double (v, "state"), ==, 6.0);
  g_assert_cmpfloat (lookup_double (v, "version"), ==, 3.0);
  g_assert_true (lookup_bool (v, "hasPrefs"));
  g_assert_false (lookup_bool (v, "hasUpdate"));
  g_assert_false (lookup_bool (v, "canChange"));
  g_assert_false (lookup_bool (v, "enabled"));
  /* nested metadata is packed as variants; null members are dropped */
  g_assert_true (g_variant_lookup (v, "nested", "@a{sv}", &child));
  g_assert_true (lookup_bool (child, "flag"));
  g_variant_unref (child);
  g_assert_null (g_variant_lookup_value (v, "skipped", NULL));

  modes = g_variant_lookup_value (v, "sessionModes", G_VARIANT_TYPE ("av"));
  g_assert_nonnull (modes);
  g_variant_iter_init (&iter, modes);
  while ((child = g_variant_iter_next_value (&iter)) != NULL)
    {
      GVariant *inner = g_variant_get_variant (child);

      g_string_append_printf (mode_list, "%s;", g_variant_get_string (inner, NULL));
      g_variant_unref (inner);
      g_variant_unref (child);
    }
  g_assert_cmpstr (mode_list->str, ==, "user;unlock-dialog;");
  g_string_free (mode_list, TRUE);

  /* the flags that come from org.gnome.shell settings */
  shell_lua_extension_set_enabled (ext, TRUE);
  shell_lua_extension_set_can_change (ext, TRUE);
  g_assert_true (shell_lua_extension_get_enabled (ext));
  g_clear_pointer (&v, g_variant_unref);
  v = shell_lua_extension_serialize (ext);
  g_assert_true (lookup_bool (v, "enabled"));
  g_assert_true (lookup_bool (v, "canChange"));

  /* state follows the lifecycle; the system extension has no prefs and one session mode */
  enable_ok (&e, "ser@test");
  g_clear_pointer (&v, g_variant_unref);
  v = shell_lua_extension_serialize (ext);
  g_assert_cmpfloat (lookup_double (v, "state"), ==, 1.0);
  disable_ok (&e, "ser@test");
  g_clear_pointer (&v, g_variant_unref);
  v = shell_lua_extension_serialize (ext);
  g_assert_cmpfloat (lookup_double (v, "state"), ==, 2.0);

  g_clear_pointer (&v, g_variant_unref);
  v = shell_lua_extension_serialize (plain);
  g_assert_cmpfloat (lookup_double (v, "type"), ==, 1.0);
  g_assert_false (lookup_bool (v, "hasPrefs"));
  g_clear_pointer (&modes, g_variant_unref);
  modes = g_variant_lookup_value (v, "sessionModes", G_VARIANT_TYPE ("av"));
  g_assert_cmpuint (g_variant_n_children (modes), ==, 1);
  g_assert_null (g_variant_lookup_value (v, "description", NULL));

  /* a failed extension reports its error */
  write_ext (e.user, "failed@test", NULL, "return { enable = function() error('nope') end }");
  shell_lua_extension_manager_scan (e.manager);
  expect_ext_warning ("failed@test", "nope");
  g_assert_false (shell_lua_extension_manager_enable (e.manager, "failed@test", NULL));
  g_test_assert_expected_messages ();
  g_clear_pointer (&v, g_variant_unref);
  v = shell_lua_extension_serialize (must_lookup (&e, "failed@test"));
  g_assert_cmpfloat (lookup_double (v, "state"), ==, 3.0);
  s = lookup_string (v, "error");
  g_assert_nonnull (strstr (s, "nope"));

  env_clear (&e);
}

static void
test_state_changed_signal_on_flags (void)
{
  Env e;
  ShellLuaExtension *ext;
  GString *states = g_string_new (NULL);

  env_init (&e);
  write_ext (e.user, "flags@test", NULL, PLAIN_LUA);
  ext = scan_one (&e, "flags@test");
  g_signal_connect (e.manager, "extension-state-changed", G_CALLBACK (on_state_changed), states);

  /* enabled / canChange are reported through the same signal, only on change */
  shell_lua_extension_set_enabled (ext, TRUE);
  shell_lua_extension_set_enabled (ext, TRUE);
  shell_lua_extension_set_can_change (ext, TRUE);
  shell_lua_extension_set_can_change (ext, TRUE);
  shell_lua_extension_set_enabled (ext, FALSE);
  g_assert_cmpstr (states->str, ==,
                   "flags@test:INITIALIZED;flags@test:INITIALIZED;flags@test:INITIALIZED;");

  g_string_free (states, TRUE);
  env_clear (&e);
}

/* ------------------------------------------------------------------------ */
/* Settings and capabilities                                                 */
/* ------------------------------------------------------------------------ */

static void
reset_test_settings (void)
{
  g_autoptr (GSettings) settings = g_settings_new (TEST_SCHEMA);

  g_settings_reset (settings, "count");
}

static void
test_settings_capability_and_schema (void)
{
  Env e;
  ShellLuaExtension *with_cap, *without_cap, *own_schema;
  ShellLuaState *state;
  g_autoptr (GSettings) other = g_settings_new (TEST_SCHEMA);
  g_autofree char *metadata_own = NULL;
  int before;

  reset_test_settings ();
  env_init (&e);
  write_ext (e.user, "nocap@test", NULL,
             "return { enable = function()\n"
             "  OPEN_OK, OPEN_ERR = pcall(shell.settings.open, '" TEST_SCHEMA "')\n"
             "  EXT_OK, EXT_ERR = pcall(shell.settings.extension)\n"
             "end }\n");
  write_ext (e.user, "withcap@test",
             "{\"uuid\": \"withcap@test\", \"name\": \"N\", \"shell-api\": \"1\", \"capabilities\": [\"settings\"]}",
             "return { enable = function()\n"
             "  OPEN_OK, OPEN_ERR = pcall(shell.settings.open, '" TEST_SCHEMA "')\n"
             "  COUNT = OPEN_OK and OPEN_ERR:get_int('count')\n"
             "  MISSING_OK, MISSING_ERR = pcall(shell.settings.open, 'org.gnome-lua.missing')\n"
             "end }\n");
  metadata_own = metadata_with ("ownschema@test", "\"settings-schema\": \"" TEST_SCHEMA "\"");
  write_ext (e.user, "ownschema@test", metadata_own,
             "return { enable = function()\n"
             "  local s = shell.settings.extension()\n"
             "  SAME = rawequal(s, shell.settings.open('" TEST_SCHEMA "'))\n"
             "  COUNT = s:get_int('count')\n"
             "  s:on_changed('count', function(key, value) shell.test.tick() end)\n"
             "end }\n");
  shell_lua_extension_manager_scan (e.manager);
  without_cap = must_lookup (&e, "nocap@test");
  with_cap = must_lookup (&e, "withcap@test");
  own_schema = must_lookup (&e, "ownschema@test");

  /* without the capability: only the extension's own schema would be allowed */
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
                         "Lua extension nocap@test has no \"settings-schema\" in its metadata");
  g_assert_null (shell_lua_extension_get_settings (without_cap));
  g_test_assert_expected_messages ();
  enable_ok (&e, "nocap@test");
  state = shell_lua_extension_get_lua_state (without_cap);
  tu_assert_eval_str (state, "OPEN_OK", "false");
  tu_assert_eval_true (state, "OPEN_ERR:find('requires the \"settings\" capability', 1, true) ~= nil");
  tu_assert_eval_str (state, "EXT_OK", "false");
  tu_assert_eval_true (state, "EXT_ERR:find('no \"settings-schema\"', 1, true) ~= nil");
  disable_ok (&e, "nocap@test");

  /* with it: any installed schema */
  enable_ok (&e, "withcap@test");
  state = shell_lua_extension_get_lua_state (with_cap);
  tu_assert_eval_str (state, "OPEN_OK", "true");
  tu_assert_eval_str (state, "COUNT", "5");
  tu_assert_eval_str (state, "MISSING_OK", "false");
  tu_assert_eval_true (state, "MISSING_ERR:find('is not installed', 1, true) ~= nil");
  disable_ok (&e, "withcap@test");

  /* settings-schema in the metadata: shell.settings.extension() works without capability */
  enable_ok (&e, "ownschema@test");
  state = shell_lua_extension_get_lua_state (own_schema);
  tu_assert_eval_str (state, "SAME", "true");
  tu_assert_eval_str (state, "COUNT", "5");
  g_assert_nonnull (shell_lua_extension_get_settings (own_schema));
  g_assert_true (G_IS_SETTINGS (shell_lua_extension_get_settings (own_schema)));
  g_assert_true (shell_lua_extension_get_settings (own_schema) == shell_lua_extension_get_settings (own_schema));

  /* the on_changed connection is owned by the extension */
  ticks = 0;
  g_settings_set_int (other, "count", 8);
  wait_for_ticks (1);
  tu_spin (30);
  g_assert_cmpint (ticks, ==, 1);
  disable_ok (&e, "ownschema@test");
  before = ticks;
  g_settings_set_int (other, "count", 9);
  tu_spin (60);
  g_assert_cmpint (ticks, ==, before);

  reset_test_settings ();
  env_clear (&e);
}

/* ------------------------------------------------------------------------ */
/* Sync with org.gnome.shell                                                 */
/* ------------------------------------------------------------------------ */

static void
set_shell_strv (GSettings *settings, const char *key, const char * const *values)
{
  g_assert_true (g_settings_set_strv (settings, key, values));
}

typedef struct {
  ShellLuaExtension *a;
  ShellLuaExtension *b;
  ShellLuaExtensionState a_state;
  ShellLuaExtensionState b_state;
} Pair;

static gboolean
pred_pair (gpointer data)
{
  Pair *p = data;

  return shell_lua_extension_get_state (p->a) == p->a_state &&
         shell_lua_extension_get_state (p->b) == p->b_state;
}

static void
wait_for_pair (Pair *p)
{
  if (!tu_wait_for (pred_pair, p, 3000))
    g_error ("timed out: a is %s, b is %s",
             shell_lua_extension_state_to_string (shell_lua_extension_get_state (p->a)),
             shell_lua_extension_state_to_string (shell_lua_extension_get_state (p->b)));
}

static void
test_sync_with_shell_settings (void)
{
  Env e;
  g_autoptr (GSettings) shell_settings = g_settings_new ("org.gnome.shell");
  const char * const only_a[] = { "a@test", "ghost@test", NULL };
  const char * const only_b[] = { "b@test", NULL };
  const char * const both[] = { "a@test", "b@test", NULL };
  const char * const none[] = { NULL };
  Pair p;

  env_init (&e);
  write_ext (e.user, "a@test", NULL, PLAIN_LUA);
  write_ext (e.user, "b@test", NULL, PLAIN_LUA);
  shell_lua_extension_manager_scan (e.manager);
  p.a = must_lookup (&e, "a@test");
  p.b = must_lookup (&e, "b@test");

  g_settings_reset (shell_settings, "enabled-extensions");
  g_settings_reset (shell_settings, "disabled-extensions");
  g_settings_reset (shell_settings, "disable-user-extensions");

  /* nothing enabled by default; unknown uuids in the list are ignored */
  shell_lua_extension_manager_sync (e.manager);
  assert_state (p.a, SHELL_LUA_EXTENSION_STATE_INITIALIZED);
  assert_state (p.b, SHELL_LUA_EXTENSION_STATE_INITIALIZED);
  g_assert_false (shell_lua_extension_get_enabled (p.a));

  set_shell_strv (shell_settings, "enabled-extensions", only_a);
  shell_lua_extension_manager_sync (e.manager);
  assert_state (p.a, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  assert_state (p.b, SHELL_LUA_EXTENSION_STATE_INITIALIZED);
  g_assert_true (shell_lua_extension_get_enabled (p.a));
  g_assert_false (shell_lua_extension_get_enabled (p.b));
  {
    g_autoptr (GVariant) v = shell_lua_extension_serialize (p.a);

    g_assert_true (lookup_bool (v, "canChange"));
    g_assert_true (lookup_bool (v, "enabled"));
  }

  /* later changes are picked up from an idle, without another explicit sync */
  set_shell_strv (shell_settings, "enabled-extensions", only_b);
  p.a_state = SHELL_LUA_EXTENSION_STATE_INACTIVE;
  p.b_state = SHELL_LUA_EXTENSION_STATE_ACTIVE;
  wait_for_pair (&p);
  g_assert_false (shell_lua_extension_get_enabled (p.a));
  g_assert_true (shell_lua_extension_get_enabled (p.b));

  set_shell_strv (shell_settings, "enabled-extensions", both);
  p.a_state = SHELL_LUA_EXTENSION_STATE_ACTIVE;
  p.b_state = SHELL_LUA_EXTENSION_STATE_ACTIVE;
  wait_for_pair (&p);

  /* disabled-extensions wins over enabled-extensions */
  set_shell_strv (shell_settings, "disabled-extensions", only_b);
  p.b_state = SHELL_LUA_EXTENSION_STATE_INACTIVE;
  wait_for_pair (&p);
  set_shell_strv (shell_settings, "disabled-extensions", none);
  p.b_state = SHELL_LUA_EXTENSION_STATE_ACTIVE;
  wait_for_pair (&p);

  /* disable-user-extensions turns everything off and locks the switch */
  g_settings_set_boolean (shell_settings, "disable-user-extensions", TRUE);
  p.a_state = p.b_state = SHELL_LUA_EXTENSION_STATE_INACTIVE;
  wait_for_pair (&p);
  {
    g_autoptr (GVariant) v = shell_lua_extension_serialize (p.a);

    g_assert_false (lookup_bool (v, "canChange"));
    g_assert_false (lookup_bool (v, "enabled"));
  }
  g_settings_set_boolean (shell_settings, "disable-user-extensions", FALSE);
  p.a_state = p.b_state = SHELL_LUA_EXTENSION_STATE_ACTIVE;
  wait_for_pair (&p);

  g_settings_reset (shell_settings, "enabled-extensions");
  g_settings_reset (shell_settings, "disabled-extensions");
  g_settings_reset (shell_settings, "disable-user-extensions");
  env_clear (&e);
}

/* ------------------------------------------------------------------------ */
/* Reload                                                                    */
/* ------------------------------------------------------------------------ */

static void
test_reload (void)
{
  Env e;
  ShellLuaExtension *ext, *reloaded;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) v = NULL;
  g_autofree char *dir = NULL;
  g_autofree char *metadata = NULL;
  g_autofree char *version = NULL;
  GString *log = g_string_new (NULL);
  gulong handler;

  env_init (&e);

  metadata = metadata_with ("r@test", "\"version\": \"1\"");
  write_ext (e.user, "r@test", metadata, "return { enable = function () shell.test.ping() end }\n");
  ext = scan_one (&e, "r@test");
  v = shell_lua_extension_serialize (ext);
  g_assert_false (lookup_bool (v, "hasPrefs"));
  g_clear_pointer (&v, g_variant_unref);

  /* A running extension is disabled, reported as uninstalled, and read again */
  enable_ok (&e, "r@test");
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_ACTIVE);

  dir = g_build_filename (e.user, "r@test", NULL);
  g_free (metadata);
  metadata = metadata_with ("r@test", "\"version\": \"2\"");
  tu_write_file (dir, "metadata.json", metadata);
  tu_write_file (dir, "prefs.lua", "return {}\n");

  g_object_ref (ext);
  handler = g_signal_connect (e.manager, "extension-state-changed", G_CALLBACK (on_state_changed), log);
  g_assert_true (shell_lua_extension_manager_reload (e.manager, "r@test", &error));
  g_assert_no_error (error);
  g_signal_handler_disconnect (e.manager, handler);

  assert_state (ext, SHELL_LUA_EXTENSION_STATE_UNINSTALLED);
  g_object_unref (ext);

  reloaded = must_lookup (&e, "r@test");
  g_assert_true (reloaded != ext);
  assert_state (reloaded, SHELL_LUA_EXTENSION_STATE_INITIALIZED);
  v = shell_lua_extension_serialize (reloaded);
  g_assert_true (lookup_bool (v, "hasPrefs"));
  version = lookup_string (v, "version");
  g_assert_cmpstr (version, ==, "2");
  g_assert_true (g_str_has_suffix (log->str, "r@test:INITIALIZED;"));
  g_assert_true (strstr (log->str, "r@test:UNINSTALLED;") != NULL);

  /* It can be enabled again */
  enable_ok (&e, "r@test");
  assert_state (reloaded, SHELL_LUA_EXTENSION_STATE_ACTIVE);

  /* Unknown extensions are an error; a directory that cannot be read any
   * more leaves the extension forgotten */
  g_assert_false (shell_lua_extension_manager_reload (e.manager, "nope@test", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  tu_rm_rf (dir);
  g_assert_false (shell_lua_extension_manager_reload (e.manager, "r@test", &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_assert_null (lookup (&e, "r@test"));

  g_string_free (log, TRUE);
  env_clear (&e);
}

/* ------------------------------------------------------------------------ */
/* The example extension                                                     */
/* ------------------------------------------------------------------------ */

static void
test_hello_example (void)
{
  g_autofree char *examples = g_test_build_filename (G_TEST_DIST, "..", "examples", NULL);
  g_autoptr (GFile) dir = g_file_new_for_path (examples);
  ShellLuaExtensionManager *manager = g_object_new (SHELL_TYPE_LUA_EXTENSION_MANAGER, NULL);
  GFile *dirs[1] = { dir };
  ShellLuaExtension *ext;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) v = NULL;

  shell_lua_extension_manager_set_search_dirs (manager, dirs, 1);
  shell_lua_extension_manager_scan (manager);
  ext = shell_lua_extension_manager_lookup (manager, "hello@gnome-lua.example");
  if (ext == NULL)
    g_error ("hello@gnome-lua.example not found in %s", examples);
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_INITIALIZED);
  g_assert_null (shell_lua_extension_get_error (ext));
  g_assert_false (shell_lua_extension_has_capability (ext, "dbus"));

  expect_ext_log ("hello@gnome-lua.example", G_LOG_LEVEL_INFO, "hello from Lua");
  g_assert_true (shell_lua_extension_manager_enable (manager, "hello@gnome-lua.example", &error));
  g_assert_no_error (error);
  g_test_assert_expected_messages ();
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 0);

  v = shell_lua_extension_serialize (ext);
  g_assert_cmpfloat (lookup_double (v, "state"), ==, 1.0);

  tu_spin (30);
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 0);
  g_assert_true (shell_lua_extension_manager_disable (manager, "hello@gnome-lua.example", &error));
  g_assert_no_error (error);
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_INACTIVE);
  g_assert_null (shell_lua_extension_get_lua_state (ext));

  g_object_unref (manager);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  /* these tests build their own search paths */
  g_unsetenv ("SHELL_LUA_EXTENSION_PATH");
  shell_lua_api_register ("zz-test", test_api_register);

  g_test_add_func ("/shell-lua/extension/scan/basic", test_scan);
  g_test_add_func ("/shell-lua/extension/scan/missing-directories", test_scan_missing_directories);
  g_test_add_func ("/shell-lua/extension/scan/invalid-metadata", test_scan_rejects_invalid_metadata);
  g_test_add_func ("/shell-lua/extension/scan/shell-api", test_shell_api_versions);
  g_test_add_func ("/shell-lua/extension/scan/capabilities", test_capabilities);
  g_test_add_func ("/shell-lua/extension/scan/search-path-env", test_search_path_env);
  g_test_add_func ("/shell-lua/extension/scan/shadowing-and-removal", test_per_user_shadows_system_and_removal);
  g_test_add_func ("/shell-lua/extension/lifecycle/enable-disable", test_enable_disable_lifecycle);
  g_test_add_func ("/shell-lua/extension/lifecycle/extension-table", test_extension_table);
  g_test_add_func ("/shell-lua/extension/lifecycle/disposes-owned-resources", test_disable_disposes_owned_resources);
  g_test_add_func ("/shell-lua/extension/lifecycle/manager-dispose", test_manager_dispose_disables);
  g_test_add_func ("/shell-lua/extension/lifecycle/optional-functions", test_enable_without_enable_function);
  g_test_add_func ("/shell-lua/extension/failure/enable", test_enable_failures);
  g_test_add_func ("/shell-lua/extension/failure/error-in-disable", test_error_in_disable);
  g_test_add_func ("/shell-lua/extension/failure/error-limit", test_error_policy_limit);
  g_test_add_func ("/shell-lua/extension/failure/timer-does-not-repeat", test_failing_timer_callback_does_not_repeat);
  g_test_add_func ("/shell-lua/extension/failure/limits", test_limits_apply);
  g_test_add_func ("/shell-lua/extension/serialize", test_serialize);
  g_test_add_func ("/shell-lua/extension/state-changed-on-flags", test_state_changed_signal_on_flags);
  g_test_add_func ("/shell-lua/extension/settings-capability", test_settings_capability_and_schema);
  g_test_add_func ("/shell-lua/extension/sync", test_sync_with_shell_settings);
  g_test_add_func ("/shell-lua/extension/reload", test_reload);
  g_test_add_func ("/shell-lua/extension/example-hello", test_hello_example);

  return g_test_run ();
}
