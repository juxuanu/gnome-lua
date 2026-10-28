/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * test-extension-prefs-schema.c: prefs.lua loading and validation
 * (docs/api/prefs.md), on extensions written to a temporary directory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <glib/gstdio.h>

#include "extension-prefs-schema.h"
#include "shell-lua-extension-private.h"

#define UUID "prefs-test@gnome-lua.example"
#define SCHEMA_ID "org.gnome-lua.example.prefs-test"

static const char schema_xml[] =
  "<schemalist>\n"
  "  <schema id='" SCHEMA_ID "' path='/org/gnome-lua/example/prefs-test/'>\n"
  "    <key name='show-seconds' type='b'><default>false</default></key>\n"
  "    <key name='other-flag' type='b'><default>true</default></key>\n"
  "    <key name='format' type='s'>\n"
  "      <choices><choice value='24h'/><choice value='12h'/></choices>\n"
  "      <default>'24h'</default>\n"
  "    </key>\n"
  "    <key name='update-interval' type='i'>\n"
  "      <range min='1' max='60'/><default>1</default>\n"
  "    </key>\n"
  "    <key name='big' type='x'><default>5</default></key>\n"
  "    <key name='small' type='y'><default>5</default></key>\n"
  "    <key name='ratio' type='d'><default>0.5</default></key>\n"
  "    <key name='label' type='s'><default>''</default></key>\n"
  "    <key name='accent' type='s'><default>'#3584e4'</default></key>\n"
  "    <key name='toggle-shortcut' type='as'><default>['&lt;Super&gt;c']</default></key>\n"
  "    <key name='mode' type='i'><default>1</default></key>\n"
  "    <key name='names' type='as'><default>[]</default></key>\n"
  "  </schema>\n"
  "</schemalist>\n";

typedef struct
{
  char *dir;                 /* the extension directory */
  char *root;                /* its parent, removed at the end */
  ShellLuaExtension *extension;
} Fixture;

static void
write_file (const char *dir,
            const char *name,
            const char *contents)
{
  g_autofree char *path = g_build_filename (dir, name, NULL);

  g_assert_true (g_file_set_contents (path, contents, -1, NULL));
}

static void
fixture_setup (Fixture    *fixture,
               const char *prefs_lua,
               gboolean    with_schema,
               gboolean    with_settings_schema_key)
{
  g_autofree char *metadata = NULL;
  g_autofree char *schemas = NULL;
  g_autoptr (GFile) dir = NULL;
  g_autoptr (GError) error = NULL;

  fixture->root = g_dir_make_tmp ("prefs-schema-XXXXXX", &error);
  g_assert_no_error (error);
  fixture->dir = g_build_filename (fixture->root, UUID, NULL);
  g_assert_cmpint (g_mkdir (fixture->dir, 0700), ==, 0);

  metadata = g_strdup_printf ("{\"uuid\": \"%s\", \"name\": \"Prefs test\", "
                              "\"shell-api\": \"1\"%s}",
                              UUID,
                              with_settings_schema_key
                              ? ", \"settings-schema\": \"" SCHEMA_ID "\"" : "");
  write_file (fixture->dir, "metadata.json", metadata);
  write_file (fixture->dir, "extension.lua", "return {}\n");
  if (prefs_lua != NULL)
    write_file (fixture->dir, "prefs.lua", prefs_lua);

  if (with_schema)
    {
      gint status = 0;
      const char *argv[] = { "glib-compile-schemas", "--strict", NULL, NULL };

      schemas = g_build_filename (fixture->dir, "schemas", NULL);
      g_assert_cmpint (g_mkdir (schemas, 0700), ==, 0);
      write_file (schemas, SCHEMA_ID ".gschema.xml", schema_xml);

      argv[2] = schemas;
      g_assert_true (g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH,
                                   NULL, NULL, NULL, NULL, &status, &error));
      g_assert_no_error (error);
      g_assert_true (g_spawn_check_wait_status (status, NULL));
    }

  dir = g_file_new_for_path (fixture->dir);
  fixture->extension = shell_lua_extension_new_from_dir (dir, SHELL_LUA_EXTENSION_TYPE_PER_USER, &error);
  g_assert_no_error (error);
  g_assert_nonnull (fixture->extension);
}

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
  const char *name;

  if (dir == NULL)
    {
      g_remove (path);
      return;
    }

  while ((name = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *child = g_build_filename (path, name, NULL);

      remove_tree (child);
    }
  g_rmdir (path);
}

static void
fixture_teardown (Fixture *fixture)
{
  g_clear_object (&fixture->extension);
  remove_tree (fixture->root);
  g_free (fixture->dir);
  g_free (fixture->root);
}

static gboolean
problems_mention (char       **problems,
                  const char  *needle)
{
  guint i;

  for (i = 0; problems != NULL && problems[i] != NULL; i++)
    {
      if (strstr (problems[i], needle) != NULL)
        return TRUE;
    }

  return FALSE;
}

/* Loads and expects failure; @needles are substrings of the problems */
static void
expect_problems (const char *prefs_lua,
                 gboolean    lua_warning,
                 ...)
{
  Fixture fixture = { 0, };
  g_auto (GStrv) problems = NULL;
  ExtPrefsSchema *schema;
  const char *needle;
  va_list args;

  fixture_setup (&fixture, prefs_lua, TRUE, TRUE);

  if (lua_warning)
    g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "*");

  schema = ext_prefs_schema_load (fixture.extension, &problems);
  g_test_assert_expected_messages ();

  if (schema != NULL || problems == NULL || problems[0] == NULL)
    g_error ("expected problems for:\n%s", prefs_lua);

  va_start (args, lua_warning);
  while ((needle = va_arg (args, const char *)) != NULL)
    {
      if (!problems_mention (problems, needle))
        {
          g_autofree char *all = g_strjoinv ("\n  ", problems);

          g_error ("no problem mentions '%s' in:\n  %s\nfor:\n%s", needle, all, prefs_lua);
        }
    }
  va_end (args);

  fixture_teardown (&fixture);
}

static const char example_prefs[] =
  "return {\n"
  "  pages = {{\n"
  "    title = _('General'), icon_name = 'preferences-system-symbolic',\n"
  "    groups = {\n"
  "      { title = _('Display'), rows = {\n"
  "        { type = 'switch', key = 'show-seconds', title = _('Show seconds') },\n"
  "        { type = 'combo', key = 'format', title = _('Time format'),\n"
  "          choices = { { value = '24h', label = _('24-hour') }, { value = '12h', label = _('12-hour') } } },\n"
  "        { type = 'entry', key = 'label', title = _('Prefix'), max_length = 16 },\n"
  "        { type = 'color', key = 'accent', title = _('Accent colour') },\n"
  "      } },\n"
  "      { title = _('Behaviour'), description = _('How often the clock refreshes'), rows = {\n"
  "        { type = 'spin', key = 'update-interval', title = _('Update interval'),\n"
  "          subtitle = _('Seconds'), min = 1, max = 60, depends_on = 'show-seconds' },\n"
  "        { type = 'keybinding', key = 'toggle-shortcut', title = _('Toggle shortcut') },\n"
  "      } },\n"
  "    },\n"
  "  }},\n"
  "}\n";

static void
test_valid_example (void)
{
  Fixture fixture = { 0, };
  g_auto (GStrv) problems = NULL;
  ExtPrefsSchema *schema;
  ExtPrefsPage *page;
  ExtPrefsGroup *group;
  ExtPrefsRow *row;

  fixture_setup (&fixture, example_prefs, TRUE, TRUE);
  schema = ext_prefs_schema_load (fixture.extension, &problems);
  if (schema == NULL)
    {
      g_autofree char *all = g_strjoinv ("\n  ", problems);

      g_error ("unexpected problems:\n  %s", all);
    }

  g_assert_null (schema->title);
  g_assert_true (G_IS_SETTINGS (schema->settings));
  g_assert_cmpuint (schema->pages->len, ==, 1);

  page = g_ptr_array_index (schema->pages, 0);
  g_assert_cmpstr (page->title, ==, "General");
  g_assert_cmpstr (page->icon_name, ==, "preferences-system-symbolic");
  g_assert_cmpuint (page->groups->len, ==, 2);

  group = g_ptr_array_index (page->groups, 0);
  g_assert_cmpstr (group->title, ==, "Display");
  g_assert_cmpuint (group->rows->len, ==, 4);
  row = g_ptr_array_index (group->rows, 1);
  g_assert_cmpint (row->type, ==, EXT_PREFS_ROW_COMBO);
  g_assert_cmpuint (row->choices->len, ==, 2);
  row = g_ptr_array_index (group->rows, 2);
  g_assert_cmpint (row->type, ==, EXT_PREFS_ROW_ENTRY);
  g_assert_cmpuint (row->max_length, ==, 16);

  group = g_ptr_array_index (page->groups, 1);
  g_assert_cmpstr (group->description, ==, "How often the clock refreshes");
  row = g_ptr_array_index (group->rows, 0);
  g_assert_cmpint (row->type, ==, EXT_PREFS_ROW_SPIN);
  g_assert_cmpfloat (row->min, ==, 1);
  g_assert_cmpfloat (row->max, ==, 60);
  g_assert_cmpfloat (row->step, ==, 1);
  g_assert_cmpuint (row->digits, ==, 0);
  g_assert_nonnull (row->depends_on);
  g_assert_cmpstr (row->depends_on->key, ==, "show-seconds");
  g_assert_true (g_variant_get_boolean (row->depends_on->value));
  row = g_ptr_array_index (group->rows, 1);
  g_assert_cmpint (row->type, ==, EXT_PREFS_ROW_KEYBINDING);
  g_assert_cmpuint (row->max_accels, ==, 1);
  g_assert_true (row->allow_empty);

  ext_prefs_schema_free (schema);
  fixture_teardown (&fixture);
}

static void
test_valid_groups_form (void)
{
  Fixture fixture = { 0, };
  g_auto (GStrv) problems = NULL;
  ExtPrefsSchema *schema;
  ExtPrefsPage *page;
  ExtPrefsGroup *group;
  ExtPrefsRow *row;

  fixture_setup (&fixture,
                 "return { title = 'Custom', groups = {\n"
                 "  { rows = {\n"
                 "    { type = 'spin', key = 'ratio', title = 'Ratio', min = 0, max = 1 },\n"
                 "    { type = 'spin', key = 'small', title = 'Small', min = 0, max = 200, step = 5 },\n"
                 "    { type = 'switch', key = 'other-flag', title = 'Flag', depends_on = '!show-seconds' },\n"
                 "    { type = 'combo', key = 'mode', title = 'Mode',\n"
                 "      choices = { { value = 1, label = 'One' }, { value = 2, label = 'Two' } },\n"
                 "      depends_on = { key = 'format', value = '12h' } },\n"
                 "    { type = 'keybinding', key = 'names', title = 'Names', max = 3, allow_empty = false },\n"
                 "  } },\n"
                 "} }\n",
                 TRUE, TRUE);
  schema = ext_prefs_schema_load (fixture.extension, &problems);
  if (schema == NULL)
    {
      g_autofree char *all = g_strjoinv ("\n  ", problems);

      g_error ("unexpected problems:\n  %s", all);
    }

  g_assert_cmpstr (schema->title, ==, "Custom");
  g_assert_cmpuint (schema->pages->len, ==, 1);
  page = g_ptr_array_index (schema->pages, 0);
  g_assert_null (page->title);
  group = g_ptr_array_index (page->groups, 0);
  g_assert_cmpuint (group->rows->len, ==, 5);

  row = g_ptr_array_index (group->rows, 0);
  g_assert_cmpfloat (row->step, ==, 0.1);
  g_assert_cmpuint (row->digits, ==, 2);
  row = g_ptr_array_index (group->rows, 1);
  g_assert_cmpfloat (row->step, ==, 5);
  row = g_ptr_array_index (group->rows, 2);
  g_assert_false (g_variant_get_boolean (row->depends_on->value));
  row = g_ptr_array_index (group->rows, 3);
  g_assert_true (g_variant_is_of_type (((ExtPrefsChoice *) g_ptr_array_index (row->choices, 0))->value,
                                       G_VARIANT_TYPE_INT32));
  g_assert_cmpstr (row->depends_on->key, ==, "format");
  g_assert_cmpstr (g_variant_get_string (row->depends_on->value, NULL), ==, "12h");
  row = g_ptr_array_index (group->rows, 4);
  g_assert_cmpuint (row->max_accels, ==, 3);
  g_assert_false (row->allow_empty);

  ext_prefs_schema_free (schema);
  fixture_teardown (&fixture);
}

static void
test_file_and_schema_errors (void)
{
  Fixture fixture = { 0, };
  g_auto (GStrv) problems = NULL;

  /* No prefs.lua */
  fixture_setup (&fixture, NULL, TRUE, TRUE);
  g_assert_null (ext_prefs_schema_load (fixture.extension, &problems));
  g_assert_true (problems_mention (problems, "prefs.lua not found"));
  g_clear_pointer (&problems, g_strfreev);
  fixture_teardown (&fixture);

  /* No settings-schema in metadata.json (2) */
  fixture_setup (&fixture, "return { groups = {} }", TRUE, FALSE);
  g_assert_null (ext_prefs_schema_load (fixture.extension, &problems));
  g_assert_true (problems_mention (problems, "settings-schema"));
  g_clear_pointer (&problems, g_strfreev);
  fixture_teardown (&fixture);

  /* No compiled schema (2) */
  fixture_setup (&fixture, "return { groups = {} }", FALSE, TRUE);
  g_assert_null (ext_prefs_schema_load (fixture.extension, &problems));
  g_assert_true (problems_mention (problems, "is not installed"));
  g_clear_pointer (&problems, g_strfreev);
  fixture_teardown (&fixture);
}

static void
test_lua_failures (void)
{
  expect_problems ("return function () end", FALSE, "must return a table", NULL);
  expect_problems ("return 42", FALSE, "must return a table", NULL);
  expect_problems ("this is not lua", FALSE, "prefs.lua", NULL);
  expect_problems ("error('boom')", TRUE, "boom", NULL);
  expect_problems ("local t = {} t.self = t return { groups = t }", FALSE, "contains itself", NULL);
  expect_problems ("return { groups = { { rows = { { type = 'switch', key = 'show-seconds',"
                   " title = 'x', subtitle = function () end } } } } }",
                   FALSE, "groups[1].rows[1].subtitle", "function value", NULL);
  expect_problems ("return { groups = { 1, 2, nil, 4 } }", FALSE, "holes", NULL);
}

static void
test_sandbox (void)
{
  /* Only api_version, extension, log and gettext remain of `shell` */
  expect_problems ("shell.timer.after(1, function () end) return {}", TRUE, "timer", NULL);
  expect_problems ("assert(shell.hooks == nil and shell.settings == nil)\n"
                   "assert(shell.extension.uuid == '" UUID "')\n"
                   "assert(shell.api_version.major == 1)\n"
                   "assert(_('x') == 'x' and shell.gettext('x') == 'x')\n"
                   "shell.log.debug('loaded')\n"
                   "return { groups = 1 }",
                   FALSE, "groups", NULL);
  expect_problems ("while true do end", TRUE, "watchdog", NULL);
}

static void
test_structure_problems (void)
{
  expect_problems ("return { }", FALSE, "exactly one of 'pages' and 'groups'", NULL);
  expect_problems ("return { pages = {{ groups = {} }}, groups = {} }", FALSE,
                   "exactly one of", NULL);
  expect_problems ("return { colour = 'red', groups = {} }", FALSE, "colour: unknown field", NULL);
  expect_problems ("return { pages = {\n"
                   " { groups = { { rows = { { type = 'switch', key = 'show-seconds', title = 'a' } } } } },\n"
                   " { title = 'B', groups = { { rows = { { type = 'switch', key = 'other-flag', title = 'b' } } } } },\n"
                   "} }", FALSE, "pages[1].title: required", NULL);
  expect_problems ("return { groups = { { rows = {} } } }", FALSE, "groups[1].rows: needs at least one row", NULL);
  expect_problems ("return { groups = { { rows = { { type = 'knob', key = 'x', title = 'x' } } } } }", FALSE,
                   "groups[1].rows[1].type: unknown row type 'knob'", NULL);
  expect_problems ("return { groups = { { rows = { { type = 'switch', key = 'show-seconds',"
                   " title = 'x', colour = 1 } } } } }", FALSE,
                   "groups[1].rows[1].colour: unknown field", NULL);
  expect_problems ("return { groups = { { rows = { { type = 'switch', key = 'show-seconds' } } } } }", FALSE,
                   "groups[1].rows[1].title: required", NULL);
  expect_problems ("return { groups = { { rows = { { type = 'switch', key = 'show-seconds',"
                   " title = '' } } } } }", FALSE, "title: must not be empty", NULL);
  expect_problems ("return { groups = { { rows = { { type = 'switch', key = 'show-seconds',"
                   " title = string.rep('x', 201) } } } } }", FALSE, "title: must not be longer than 200", NULL);
}

static void
test_key_problems (void)
{
  expect_problems ("return { groups = { { rows = {\n"
                   " { type = 'switch', key = 'show-seconds', title = 'a' },\n"
                   " { type = 'switch', key = 'show-seconds', title = 'b' },\n"
                   " { type = 'switch', key = 'nope', title = 'c' },\n"
                   " { type = 'switch', key = 'label', title = 'd' },\n"
                   " { type = 'spin', key = 'label', title = 'e', min = 0, max = 1 },\n"
                   "} } } }", FALSE,
                   "rows[2].key: the key 'show-seconds' is already bound by groups[1].rows[1]",
                   "rows[3].key: the schema has no key 'nope'",
                   "rows[4].key: a switch row cannot show the key 'label' of type 's'",
                   "rows[5].key: a spin row cannot show the key 'label' of type 's'", NULL);
}

static void
test_spin_problems (void)
{
  expect_problems ("return { groups = { { rows = {\n"
                   " { type = 'spin', key = 'big', title = 'a' },\n"
                   " { type = 'spin', key = 'update-interval', title = 'b', min = 0, max = 60 },\n"
                   " { type = 'spin', key = 'update-interval', title = 'c', min = 1, max = 61 },\n"
                   "} } } }", FALSE,
                   "rows[1].min: required", "rows[1].max: required",
                   "rows[2].min: lies outside the range 1..60 of the key",
                   "bound", NULL);
  expect_problems ("return { groups = { { rows = {\n"
                   " { type = 'spin', key = 'small', title = 'a', min = 0, max = 300 },\n"
                   " { type = 'spin', key = 'big', title = 'b', min = 5, max = 1 },\n"
                   " { type = 'spin', key = 'update-interval', title = 'c', min = 1.5, max = 3 },\n"
                   " { type = 'spin', key = 'ratio', title = 'd', min = 0, max = 1, step = 0 },\n"
                   " { type = 'spin', key = 'mode', title = 'e', min = 0, max = 5, step = 0.5 },\n"
                   " { type = 'spin', key = 'names', title = 'f', min = 0, max = 5 },\n"
                   "} } } }", FALSE,
                   "rows[1].max: does not fit the type 'y'",
                   "rows[2].min: must not be greater than max",
                   "rows[3].min: must be an integer",
                   "rows[4].step: must be greater than 0",
                   "rows[5].step: must be an integer",
                   "rows[6].key: a spin row cannot show", NULL);
}

static void
test_combo_problems (void)
{
  expect_problems ("return { groups = { { rows = {\n"
                   " { type = 'combo', key = 'format', title = 'a', choices = {} },\n"
                   " { type = 'combo', key = 'mode', title = 'b', choices = {\n"
                   "     { value = 1, label = 'x' }, { value = 1, label = 'y' } } },\n"
                   " { type = 'combo', key = 'label', title = 'c', choices = {\n"
                   "     { value = 1, label = 'x' } } },\n"
                   "} } } }", FALSE,
                   "rows[1].choices: needs at least one choice",
                   "rows[2].choices[2].value: is already used by choices[1]",
                   "rows[3].choices[1].value: must be a string", NULL);
  expect_problems ("return { groups = { { rows = {\n"
                   " { type = 'combo', key = 'format', title = 'a', choices = {\n"
                   "     { value = '24h', label = 'x' }, { value = '36h', label = 'y' } } },\n"
                   "} } } }", FALSE,
                   "rows[1].choices[2].value: is not one of the values of the key (24h, 12h)", NULL);
  expect_problems ("return { groups = { { rows = {\n"
                   " { type = 'combo', key = 'mode', title = 'a', choices = {\n"
                   "     { value = 1.5, label = 'x' }, { label = 'y' }, { value = 3 } } },\n"
                   "} } } }", FALSE,
                   "choices[1].value: must be an integer", "choices[2].value: required",
                   "choices[3].label: required", NULL);
}

static void
test_depends_and_misc_problems (void)
{
  expect_problems ("return { groups = { { rows = {\n"
                   " { type = 'switch', key = 'show-seconds', title = 'a', depends_on = 'nope' },\n"
                   " { type = 'switch', key = 'other-flag', title = 'b', depends_on = 'label' },\n"
                   " { type = 'entry', key = 'label', title = 'c', depends_on = { key = 'format', value = 3 } },\n"
                   " { type = 'color', key = 'accent', title = 'd', depends_on = { key = 'zzz', value = 3 } },\n"
                   " { type = 'keybinding', key = 'toggle-shortcut', title = 'e', max = 5 },\n"
                   " { type = 'entry', key = 'format', title = 'f', max_length = 0 },\n"
                   " { type = 'color', key = 'mode', title = 'g', alpha = 'yes' },\n"
                   "} } } }", FALSE,
                   "rows[1].depends_on: the schema has no key 'nope'",
                   "rows[2].depends_on: the key 'label' is not a boolean",
                   "rows[3].depends_on.value: must be a string",
                   "rows[4].depends_on.key: the schema has no key 'zzz'",
                   "rows[5].max: must be an integer between 1 and 4",
                   "rows[6].max_length: must be an integer between 1 and 65535",
                   NULL);
}

static void
test_limits (void)
{
  expect_problems ("local pages = {}\n"
                   "for i = 1, 9 do pages[i] = { title = 'p' .. i, groups = {} } end\n"
                   "return { pages = pages }", FALSE, "pages: has more than 8 pages", NULL);
  expect_problems ("local groups = {}\n"
                   "for i = 1, 17 do groups[i] = { rows = {} } end\n"
                   "return { groups = groups }", FALSE, "groups: has more than 16 groups", NULL);
  expect_problems ("local rows = {}\n"
                   "for i = 1, 65 do rows[i] = { type = 'switch', key = 'show-seconds', title = 'x' } end\n"
                   "return { groups = { { rows = rows } } }", FALSE, "groups[1].rows: has more than 64 rows", NULL);
}

/* The prefs.lua of the example extension of subprojects/shell-lua/examples */
static void
test_hello_example (void)
{
  const char *path = g_getenv ("HELLO_EXTENSION_DIR");
  g_autoptr (GFile) dir = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (ShellLuaExtension) extension = NULL;
  g_auto (GStrv) problems = NULL;
  ExtPrefsSchema *schema;
  ExtPrefsPage *page;
  guint rows = 0, i;

  if (path == NULL)
    {
      g_test_skip ("HELLO_EXTENSION_DIR is not set");
      return;
    }

  dir = g_file_new_for_path (path);
  extension = shell_lua_extension_new_from_dir (dir, SHELL_LUA_EXTENSION_TYPE_PER_USER, &error);
  g_assert_no_error (error);

  schema = ext_prefs_schema_load (extension, &problems);
  if (schema == NULL)
    {
      g_autofree char *all = g_strjoinv ("\n  ", problems);

      g_error ("the prefs.lua of the example is not valid:\n  %s", all);
    }

  g_assert_cmpuint (schema->pages->len, ==, 1);
  page = g_ptr_array_index (schema->pages, 0);
  for (i = 0; i < page->groups->len; i++)
    rows += ((ExtPrefsGroup *) g_ptr_array_index (page->groups, i))->rows->len;
  g_assert_cmpuint (rows, ==, 4);

  ext_prefs_schema_free (schema);
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  /* the tests make their own schema sources */
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);

  g_test_add_func ("/extension-prefs-schema/valid/example", test_valid_example);
  g_test_add_func ("/extension-prefs-schema/valid/groups-form", test_valid_groups_form);
  g_test_add_func ("/extension-prefs-schema/valid/hello-example", test_hello_example);
  g_test_add_func ("/extension-prefs-schema/invalid/file-and-schema", test_file_and_schema_errors);
  g_test_add_func ("/extension-prefs-schema/invalid/lua", test_lua_failures);
  g_test_add_func ("/extension-prefs-schema/invalid/sandbox", test_sandbox);
  g_test_add_func ("/extension-prefs-schema/invalid/structure", test_structure_problems);
  g_test_add_func ("/extension-prefs-schema/invalid/keys", test_key_problems);
  g_test_add_func ("/extension-prefs-schema/invalid/spin", test_spin_problems);
  g_test_add_func ("/extension-prefs-schema/invalid/combo", test_combo_problems);
  g_test_add_func ("/extension-prefs-schema/invalid/depends-on-and-fields", test_depends_and_misc_problems);
  g_test_add_func ("/extension-prefs-schema/invalid/limits", test_limits);

  return g_test_run ();
}
