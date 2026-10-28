/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Helpers shared by the shell.* modules that live in libshell (src/lua-api/).
 * Not installed.
 */
#pragma once

#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>

#include "shell-lua-api-private.h"

G_BEGIN_DECLS

/* Resolves the `icon_file` field @field (value @rel) of a constructor or the
 * assignment to a property: a path relative to the extension directory, to a
 * .svg or .png file that exists and stays inside it (docs/api/ui.md, "icon").
 * Returns the absolute path as a Lua string left on the stack and the C
 * pointer to it (valid while the string is on the stack). Raises a Lua error
 * for an absolute path, a ".." component, a wrong suffix, a missing file or a
 * symbolic link out of the directory. */
static inline const char *
shell_lua_api_push_extension_file (lua_State  *L,
                                   const char *field,
                                   const char *rel)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);
  g_autofree char *dir = NULL;
  g_autofree char *candidate = NULL;
  g_autofree char *real = NULL;
  g_autofree char *real_dir = NULL;
  const char *problem = NULL;

  if (extension == NULL)
    problem = "needs an extension directory";
  else if (rel[0] == '\0' || g_path_is_absolute (rel))
    problem = "must be a path relative to the extension directory";
  else if (strstr (rel, "..") != NULL)
    {
      g_auto (GStrv) parts = g_strsplit (rel, G_DIR_SEPARATOR_S, -1);

      if (g_strv_contains ((const char * const *) parts, ".."))
        problem = "must not contain '..'";
    }

  if (problem == NULL)
    {
      g_autofree char *lower = g_ascii_strdown (rel, -1);

      if (!g_str_has_suffix (lower, ".svg") && !g_str_has_suffix (lower, ".png"))
        problem = "must be a .svg or .png file";
    }

  if (problem == NULL)
    {
      dir = g_file_get_path (shell_lua_extension_get_dir (extension));
      candidate = g_build_filename (dir, rel, NULL);
      real = realpath (candidate, NULL);
      real_dir = realpath (dir, NULL);
      if (real == NULL)
        problem = "no such file";
      else if (real_dir == NULL ||
               !g_str_has_prefix (real, real_dir) ||
               real[strlen (real_dir)] != G_DIR_SEPARATOR)
        problem = "must stay inside the extension directory";
      else if (!g_file_test (real, G_FILE_TEST_IS_REGULAR))
        problem = "is not a file";
    }

  if (problem != NULL)
    {
      lua_pushfstring (L, "field '%s' (%s): %s", field, rel, problem);
      g_clear_pointer (&candidate, g_free);
      g_clear_pointer (&dir, g_free);
      g_clear_pointer (&real, free);
      g_clear_pointer (&real_dir, free);
      luaL_where (L, 1);
      lua_insert (L, -2);
      lua_concat (L, 2);
      lua_error (L);
    }

  lua_pushstring (L, real);
  return lua_tostring (L, -1);
}

G_END_DECLS
