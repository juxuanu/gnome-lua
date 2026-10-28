/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Unit tests for ShellExtensionManager (src/ui/extension-system.c): which
 * Lua extensions run for the settings and the session mode, the answers of
 * org.gnome.Shell.Extensions, the errors, the removal and the reload of an
 * extension.
 *
 * The manager runs without a ShellGlobal: the extensions are written to a
 * temporary directory that a private ShellLuaExtensionManager reads, the
 * settings are those of org.gnome.shell in the memory backend, and nothing
 * touches the theme, the message tray or the network.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "shell-lua-extension-private.h"
#include "ui/extension-system.h"
#include "ui/extension-system-private.h"
#include "ui/session-mode.h"

typedef struct
{
  char *tmp;
  char *user;
  char *system;
  ShellLuaExtensionManager *lua;
  GSettings *settings;
  ShellSessionMode *session;
  ShellExtensionManager *manager;
  GString *log;
} Env;

static void
write_file (const char *dir, const char *name, const char *contents)
{
  g_autofree char *path = g_build_filename (dir, name, NULL);

  g_assert_true (g_file_set_contents (path, contents, -1, NULL));
}

static void
write_ext (const char *root, const char *uuid, const char *extra_metadata, const char *lua)
{
  g_autofree char *dir = g_build_filename (root, uuid, NULL);
  g_autofree char *metadata = g_strdup_printf ("{\"uuid\": \"%s\", \"name\": \"Test %s\", "
                                               "\"shell-api\": \"1\"%s%s}", uuid, uuid,
                                               extra_metadata ? ", " : "", extra_metadata ? extra_metadata : "");

  g_mkdir_with_parents (dir, 0700);
  write_file (dir, "metadata.json", metadata);
  write_file (dir, "extension.lua", lua ? lua : "return {}\n");
}

static void
rm_rf (const char *path)
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

      rm_rf (child);
    }
  g_rmdir (path);
}

static void
spin (void)
{
  int i;

  for (i = 0; i < 20; i++)
    while (g_main_context_iteration (NULL, FALSE))
      ;
}

static void
on_state_changed (ShellExtensionManager *manager, ShellLuaExtension *extension, GString *log)
{
  g_string_append_printf (log, "%s:%s;", shell_lua_extension_get_uuid (extension),
                          shell_lua_extension_state_to_string (shell_lua_extension_get_state (extension)));
}

static void
env_init (Env *e)
{
  g_autoptr (GFile) user = NULL;
  g_autoptr (GFile) system = NULL;
  GFile *dirs[2];

  e->tmp = g_dir_make_tmp ("ext-manager-XXXXXX", NULL);
  e->user = g_build_filename (e->tmp, "user", NULL);
  e->system = g_build_filename (e->tmp, "system", NULL);
  g_mkdir_with_parents (e->user, 0700);
  g_mkdir_with_parents (e->system, 0700);
  user = g_file_new_for_path (e->user);
  system = g_file_new_for_path (e->system);
  dirs[0] = user;
  dirs[1] = system;

  e->lua = g_object_new (SHELL_TYPE_LUA_EXTENSION_MANAGER, NULL);
  shell_lua_extension_manager_set_search_dirs (e->lua, dirs, 2);
  e->settings = g_settings_new ("org.gnome.shell");
  g_settings_reset (e->settings, "enabled-extensions");
  g_settings_reset (e->settings, "disabled-extensions");
  g_settings_reset (e->settings, "disable-user-extensions");
  g_settings_reset (e->settings, "allow-extension-installation");
  e->session = shell_session_mode_new ();
  e->manager = g_object_new (SHELL_TYPE_EXTENSION_MANAGER,
                             "session-mode", e->session,
                             "settings", e->settings,
                             "lua-manager", e->lua,
                             NULL);
  e->log = g_string_new (NULL);
  g_signal_connect (e->manager, "extension-state-changed", G_CALLBACK (on_state_changed), e->log);
}

static void
env_clear (Env *e)
{
  /* the callback of the update check holds the manager */
  spin ();
  g_object_unref (e->manager);
  g_object_unref (e->session);
  g_object_unref (e->settings);
  g_object_unref (e->lua);
  rm_rf (e->tmp);
  g_string_free (e->log, TRUE);
  g_free (e->tmp);
  g_free (e->user);
  g_free (e->system);
}

static void
set_enabled (Env *e, const char * const *uuids)
{
  g_settings_set_strv (e->settings, "enabled-extensions", uuids);
}

static ShellLuaExtensionState
state_of (Env *e, const char *uuid)
{
  ShellLuaExtension *ext = shell_extension_manager_lookup (e->manager, uuid);

  if (ext == NULL)
    g_error ("%s is not installed", uuid);
  return shell_lua_extension_get_state (ext);
}

#define ASSERT_STATE(e, uuid, expected) \
  G_STMT_START { \
    if (state_of ((e), (uuid)) != (expected)) \
      g_error ("%s is %s, expected %s", (uuid), \
               shell_lua_extension_state_to_string (state_of ((e), (uuid))), \
               shell_lua_extension_state_to_string (expected)); \
  } G_STMT_END

static void
test_basics (void)
{
  Env e;
  g_autoptr (GVariant) list = NULL;
  g_autoptr (GVariant) info = NULL;
  g_autoptr (GVariant) errors = NULL;
  g_autoptr (GVariant) a_info = NULL;
  g_auto (GStrv) uuids = NULL;
  gboolean flag;
  double number;

  env_init (&e);
  write_ext (e.user, "a@t", "\"version\": 3", NULL);
  write_ext (e.system, "b@t", NULL, NULL);
  set_enabled (&e, (const char * const[]) { "a@t", NULL });

  shell_extension_manager_start (e.manager);

  ASSERT_STATE (&e, "a@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  ASSERT_STATE (&e, "b@t", SHELL_LUA_EXTENSION_STATE_INITIALIZED);

  uuids = shell_extension_manager_get_uuids (e.manager);
  g_assert_cmpstrv (uuids, ((const char * const[]) { "a@t", "b@t", NULL }));

  list = shell_extension_manager_list_extensions (e.manager);
  g_assert_true (g_variant_is_of_type (list, G_VARIANT_TYPE ("a{sa{sv}}")));
  g_assert_cmpuint (g_variant_n_children (list), ==, 2);
  a_info = g_variant_lookup_value (list, "a@t", G_VARIANT_TYPE_VARDICT);
  g_assert_nonnull (a_info);
  g_assert_true (g_variant_lookup (a_info, "state", "d", &number));
  g_assert_cmpfloat (number, ==, 1.0);
  g_assert_true (g_variant_lookup (a_info, "type", "d", &number));
  g_assert_cmpfloat (number, ==, 2.0);
  g_assert_true (g_variant_lookup (a_info, "enabled", "b", &flag));
  g_assert_true (flag);
  g_assert_true (g_variant_lookup (a_info, "canChange", "b", &flag));
  g_assert_true (flag);
  g_assert_true (g_variant_lookup (a_info, "hasUpdate", "b", &flag));
  g_assert_false (flag);
  g_assert_true (g_variant_lookup (a_info, "hasPrefs", "b", &flag));
  g_assert_false (flag);
  g_assert_true (g_variant_lookup (a_info, "version", "d", &number));
  g_assert_cmpfloat (number, ==, 3.0);

  info = shell_extension_manager_get_extension_info (e.manager, "b@t");
  g_assert_true (g_variant_lookup (info, "enabled", "b", &flag));
  g_assert_false (flag);
  g_assert_true (g_variant_lookup (info, "type", "d", &number));
  g_assert_cmpfloat (number, ==, 1.0);
  g_clear_pointer (&info, g_variant_unref);

  info = shell_extension_manager_get_extension_info (e.manager, "nope@t");
  g_assert_true (g_variant_is_of_type (info, G_VARIANT_TYPE_VARDICT));
  g_assert_cmpuint (g_variant_n_children (info), ==, 0);

  errors = shell_extension_manager_get_extension_errors (e.manager, "a@t");
  g_assert_true (g_variant_is_of_type (errors, G_VARIANT_TYPE_STRING_ARRAY));
  g_assert_cmpuint (g_variant_n_children (errors), ==, 0);

  g_assert_false (shell_extension_manager_get_updates_supported (e.manager));
  env_clear (&e);
}

static void
test_enable_disable (void)
{
  Env e;
  g_auto (GStrv) enabled = NULL;
  g_auto (GStrv) disabled = NULL;

  env_init (&e);
  write_ext (e.user, "a@t", NULL, NULL);
  write_ext (e.user, "b@t", NULL, NULL);
  set_enabled (&e, (const char * const[]) { "a@t", NULL });
  shell_extension_manager_start (e.manager);
  spin ();

  g_assert_false (shell_extension_manager_enable_extension (e.manager, "nope@t"));
  g_assert_false (shell_extension_manager_disable_extension (e.manager, "nope@t"));

  g_assert_true (shell_extension_manager_enable_extension (e.manager, "b@t"));
  g_assert_true (shell_extension_manager_disable_extension (e.manager, "a@t"));
  enabled = g_settings_get_strv (e.settings, "enabled-extensions");
  disabled = g_settings_get_strv (e.settings, "disabled-extensions");
  g_assert_cmpstrv (enabled, ((const char * const[]) { "b@t", NULL }));
  g_assert_cmpstrv (disabled, ((const char * const[]) { "a@t", NULL }));
  spin ();
  ASSERT_STATE (&e, "a@t", SHELL_LUA_EXTENSION_STATE_INACTIVE);
  ASSERT_STATE (&e, "b@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);

  /* Enabling an extension takes it out of disabled-extensions */
  g_assert_true (shell_extension_manager_enable_extension (e.manager, "a@t"));
  g_clear_pointer (&disabled, g_strfreev);
  disabled = g_settings_get_strv (e.settings, "disabled-extensions");
  g_assert_cmpuint (g_strv_length (disabled), ==, 0);
  spin ();
  ASSERT_STATE (&e, "a@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  env_clear (&e);
}

static void
test_precedence_and_user_extensions (void)
{
  Env e;
  g_autoptr (GVariant) info = NULL;
  gboolean flag;

  env_init (&e);
  write_ext (e.user, "a@t", NULL, NULL);
  write_ext (e.user, "b@t", NULL, NULL);
  set_enabled (&e, (const char * const[]) { "a@t", "b@t", NULL });
  g_settings_set_strv (e.settings, "disabled-extensions", (const char * const[]) { "b@t", NULL });
  shell_extension_manager_start (e.manager);

  ASSERT_STATE (&e, "a@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  ASSERT_STATE (&e, "b@t", SHELL_LUA_EXTENSION_STATE_INITIALIZED);
  info = shell_extension_manager_get_extension_info (e.manager, "b@t");
  g_assert_true (g_variant_lookup (info, "enabled", "b", &flag));
  g_assert_false (flag);
  g_clear_pointer (&info, g_variant_unref);

  g_settings_set_boolean (e.settings, "disable-user-extensions", TRUE);
  spin ();
  ASSERT_STATE (&e, "a@t", SHELL_LUA_EXTENSION_STATE_INACTIVE);
  info = shell_extension_manager_get_extension_info (e.manager, "a@t");
  g_assert_true (g_variant_lookup (info, "canChange", "b", &flag));
  g_assert_false (flag);
  g_assert_true (g_variant_lookup (info, "enabled", "b", &flag));
  g_assert_false (flag);
  g_clear_pointer (&info, g_variant_unref);

  g_settings_set_boolean (e.settings, "disable-user-extensions", FALSE);
  spin ();
  ASSERT_STATE (&e, "a@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  info = shell_extension_manager_get_extension_info (e.manager, "a@t");
  g_assert_true (g_variant_lookup (info, "canChange", "b", &flag));
  g_assert_true (flag);
  env_clear (&e);
}

static void
switch_mode (Env *e, const char *mode)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (shell_session_mode_switch_mode (e->session, mode, &error));
  g_assert_no_error (error);
}

static void
test_session_modes (void)
{
  Env e;

  env_init (&e);
  write_ext (e.user, "a@t", NULL, NULL);
  write_ext (e.user, "g@t", "\"session-modes\": [\"gdm\"]", NULL);
  write_ext (e.system, "m@t", "\"session-modes\": [\"gdm\", \"user\"]", NULL);
  write_ext (e.system, "l@t", "\"session-modes\": [\"unlock-dialog\"]", NULL);
  set_enabled (&e, (const char * const[]) { "a@t", "g@t", "m@t", "l@t", NULL });
  shell_extension_manager_start (e.manager);

  /* The user session: the extensions that list it and the ones that do not
   * say, the ones with more session modes first */
  ASSERT_STATE (&e, "a@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  ASSERT_STATE (&e, "g@t", SHELL_LUA_EXTENSION_STATE_INITIALIZED);
  ASSERT_STATE (&e, "m@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  ASSERT_STATE (&e, "l@t", SHELL_LUA_EXTENSION_STATE_INITIALIZED);
  g_assert_true (strstr (e.log->str, "m@t:ACTIVATING;") < strstr (e.log->str, "a@t:ACTIVATING;"));

  /* The login screen */
  switch_mode (&e, "gdm");
  ASSERT_STATE (&e, "a@t", SHELL_LUA_EXTENSION_STATE_INACTIVE);
  ASSERT_STATE (&e, "g@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  ASSERT_STATE (&e, "m@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  ASSERT_STATE (&e, "l@t", SHELL_LUA_EXTENSION_STATE_INITIALIZED);

  /* The lock screen */
  switch_mode (&e, "unlock-dialog");
  ASSERT_STATE (&e, "a@t", SHELL_LUA_EXTENSION_STATE_INACTIVE);
  ASSERT_STATE (&e, "g@t", SHELL_LUA_EXTENSION_STATE_INACTIVE);
  ASSERT_STATE (&e, "m@t", SHELL_LUA_EXTENSION_STATE_INACTIVE);
  ASSERT_STATE (&e, "l@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);

  switch_mode (&e, "user");
  ASSERT_STATE (&e, "a@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  ASSERT_STATE (&e, "g@t", SHELL_LUA_EXTENSION_STATE_INACTIVE);
  ASSERT_STATE (&e, "m@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  ASSERT_STATE (&e, "l@t", SHELL_LUA_EXTENSION_STATE_INACTIVE);
  env_clear (&e);
}

static void
test_errors (void)
{
  Env e;
  g_autoptr (GVariant) errors = NULL;
  g_autoptr (GVariant) info = NULL;
  g_autofree char *message = NULL;
  const char *error_text = NULL;

  env_init (&e);
  write_ext (e.user, "e@t", NULL, "return { enable = function () error('boom') end }\n");
  set_enabled (&e, (const char * const[]) { "e@t", NULL });

  g_test_expect_message ("ShellLua", G_LOG_LEVEL_WARNING, "[e@t] *boom*");
  shell_extension_manager_start (e.manager);
  g_test_assert_expected_messages ();

  ASSERT_STATE (&e, "e@t", SHELL_LUA_EXTENSION_STATE_ERROR);
  errors = shell_extension_manager_get_extension_errors (e.manager, "e@t");
  g_assert_cmpuint (g_variant_n_children (errors), ==, 1);
  g_variant_get_child (errors, 0, "s", &message);
  g_assert_true (strstr (message, "boom") != NULL);

  info = shell_extension_manager_get_extension_info (e.manager, "e@t");
  g_assert_true (g_variant_lookup (info, "error", "&s", &error_text));
  g_assert_true (strstr (error_text, "boom") != NULL);

  /* Left alone while the settings do not ask for it again */
  spin ();
  ASSERT_STATE (&e, "e@t", SHELL_LUA_EXTENSION_STATE_ERROR);

  /* Manually reported errors */
  shell_extension_manager_log_extension_error (e.manager, "nope@t", "ignored");
  env_clear (&e);
}

static void
test_uninstall (void)
{
  Env e;
  g_autofree char *dir = NULL;
  g_autofree char *system_dir = NULL;

  env_init (&e);
  write_ext (e.user, "u@t", NULL, NULL);
  write_ext (e.system, "s@t", NULL, NULL);
  set_enabled (&e, (const char * const[]) { "u@t", "s@t", NULL });
  shell_extension_manager_start (e.manager);
  ASSERT_STATE (&e, "u@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  ASSERT_STATE (&e, "s@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);

  dir = g_build_filename (e.user, "u@t", NULL);
  system_dir = g_build_filename (e.system, "s@t", NULL);

  g_assert_false (shell_extension_manager_uninstall_extension (e.manager, "s@t"));
  g_assert_true (g_file_test (system_dir, G_FILE_TEST_IS_DIR));
  g_assert_false (shell_extension_manager_uninstall_extension (e.manager, "nope@t"));

  g_string_truncate (e.log, 0);
  g_assert_true (shell_extension_manager_uninstall_extension (e.manager, "u@t"));
  g_assert_false (g_file_test (dir, G_FILE_TEST_EXISTS));
  g_assert_null (shell_extension_manager_lookup (e.manager, "u@t"));
  g_assert_true (strstr (e.log->str, "u@t:UNINSTALLED;") != NULL);
  {
    g_autoptr (GVariant) list = shell_extension_manager_list_extensions (e.manager);

    g_assert_cmpuint (g_variant_n_children (list), ==, 1);
  }

  /* A link to a checkout is removed, not what it points to */
  {
    g_autofree char *checkout = g_build_filename (e.tmp, "checkout", NULL);
    g_autofree char *link = g_build_filename (e.user, "l@t", NULL);
    g_autofree char *kept = g_build_filename (checkout, "metadata.json", NULL);

    g_mkdir_with_parents (checkout, 0700);
    write_file (checkout, "metadata.json", "{\"uuid\": \"l@t\", \"name\": \"L\", \"shell-api\": \"1\"}");
    write_file (checkout, "extension.lua", "return {}\n");
    g_assert_cmpint (symlink (checkout, link), ==, 0);
    shell_lua_extension_manager_scan (e.lua);
    g_assert_nonnull (shell_extension_manager_lookup (e.manager, "l@t"));

    g_assert_true (shell_extension_manager_uninstall_extension (e.manager, "l@t"));
    g_assert_false (g_file_test (link, G_FILE_TEST_IS_SYMLINK));
    g_assert_true (g_file_test (kept, G_FILE_TEST_EXISTS));
    g_assert_null (shell_extension_manager_lookup (e.manager, "l@t"));
  }
  env_clear (&e);
}

static void
test_reload (void)
{
  Env e;
  g_autofree char *dir = NULL;
  g_autoptr (GVariant) info = NULL;
  g_autoptr (GError) error = NULL;
  double version;

  env_init (&e);
  write_ext (e.user, "r@t", "\"version\": 1", NULL);
  set_enabled (&e, (const char * const[]) { "r@t", NULL });
  shell_extension_manager_start (e.manager);
  ASSERT_STATE (&e, "r@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);

  write_ext (e.user, "r@t", "\"version\": 2", NULL);
  g_assert_true (shell_extension_manager_reload_extension (e.manager, "r@t", &error));
  g_assert_no_error (error);
  ASSERT_STATE (&e, "r@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  info = shell_extension_manager_get_extension_info (e.manager, "r@t");
  g_assert_true (g_variant_lookup (info, "version", "d", &version));
  g_assert_cmpfloat (version, ==, 2);

  g_assert_false (shell_extension_manager_reload_extension (e.manager, "nope@t", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  env_clear (&e);
}

static void
test_installation_not_allowed (void)
{
  Env e;
  g_autoptr (GVariant) list = NULL;
  g_auto (GStrv) uuids = NULL;

  env_init (&e);
  write_ext (e.user, "u@t", NULL, NULL);
  write_ext (e.system, "s@t", NULL, NULL);
  set_enabled (&e, (const char * const[]) { "u@t", "s@t", NULL });
  g_settings_set_boolean (e.settings, "allow-extension-installation", FALSE);
  shell_extension_manager_start (e.manager);

  g_assert_null (shell_extension_manager_lookup (e.manager, "u@t"));
  ASSERT_STATE (&e, "s@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  g_assert_cmpint (shell_lua_extension_get_state (shell_lua_extension_manager_lookup (e.lua, "u@t")), !=,
                   SHELL_LUA_EXTENSION_STATE_ACTIVE);
  uuids = shell_extension_manager_get_uuids (e.manager);
  g_assert_cmpstrv (uuids, ((const char * const[]) { "s@t", NULL }));
  list = shell_extension_manager_list_extensions (e.manager);
  g_assert_cmpuint (g_variant_n_children (list), ==, 1);
  g_assert_false (shell_extension_manager_enable_extension (e.manager, "u@t"));

  g_settings_set_boolean (e.settings, "allow-extension-installation", TRUE);
  spin ();
  ASSERT_STATE (&e, "u@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  env_clear (&e);
}

static void
test_picks_up_new_extensions (void)
{
  Env e;

  env_init (&e);
  write_ext (e.user, "a@t", NULL, NULL);
  set_enabled (&e, (const char * const[]) { "a@t", NULL });
  shell_extension_manager_start (e.manager);
  g_assert_null (shell_extension_manager_lookup (e.manager, "n@t"));

  write_ext (e.user, "n@t", NULL, NULL);
  set_enabled (&e, (const char * const[]) { "a@t", "n@t", NULL });
  spin ();
  ASSERT_STATE (&e, "n@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);
  g_assert_true (strstr (e.log->str, "n@t:ACTIVE;") != NULL);
  env_clear (&e);
}

static void
test_install_without_downloader (void)
{
  Env e;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) list = NULL;

  env_init (&e);
  write_ext (e.user, "a@t", NULL, NULL);
  shell_extension_manager_start (e.manager);

  /* add_installed through the private entry point: an extension that was
   * extracted to the extensions directory is loaded and enabled */
  write_ext (e.user, "i@t", NULL, NULL);
  g_assert_true (shell_extension_manager_add_installed (e.manager, "i@t", &error));
  g_assert_no_error (error);
  spin ();
  ASSERT_STATE (&e, "i@t", SHELL_LUA_EXTENSION_STATE_ACTIVE);

  /* ... and one that is no Lua extension is an error */
  g_assert_false (shell_extension_manager_add_installed (e.manager, "x@t", &error));
  g_assert_error (error, SHELL_EXTENSION_ERROR, SHELL_EXTENSION_ERROR_ENABLE_FAILED);
  env_clear (&e);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  g_unsetenv ("SHELL_LUA_EXTENSION_PATH");

  g_test_add_func ("/extension-manager/basics", test_basics);
  g_test_add_func ("/extension-manager/enable-disable", test_enable_disable);
  g_test_add_func ("/extension-manager/precedence", test_precedence_and_user_extensions);
  g_test_add_func ("/extension-manager/session-modes", test_session_modes);
  g_test_add_func ("/extension-manager/errors", test_errors);
  g_test_add_func ("/extension-manager/uninstall", test_uninstall);
  g_test_add_func ("/extension-manager/reload", test_reload);
  g_test_add_func ("/extension-manager/installation-not-allowed", test_installation_not_allowed);
  g_test_add_func ("/extension-manager/new-extensions", test_picks_up_new_extensions);
  g_test_add_func ("/extension-manager/add-installed", test_install_without_downloader);

  return g_test_run ();
}
