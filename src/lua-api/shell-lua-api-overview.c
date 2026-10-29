/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.overview (docs/api/overview.md): the state of the overview and
 * show(), hide() and toggle().
 *
 * The functions go to ShellOverview with the reason "extension" and the uuid
 * of the extension as the owner of the hooks (shell_overview_*_for_extension).
 * Called from inside an overview.* hook, an action is deferred to an idle
 * (README.md rule 8): the overview is in the middle of a transition then, and
 * asking for the same transition from its own hook would recurse.
 */

#include "config.h"

#include <string.h>

#include "shell-lua-api-private.h"
#include "ui/main.h"
#include "ui/overview.h"

typedef enum
{
  ACTION_SHOW,
  ACTION_HIDE,
  ACTION_TOGGLE,
} Action;

typedef struct
{
  Action action;
  ShellControlsState state;
  char *owner;
  gboolean has_extension;
  GWeakRef extension;
} DeferredAction;

static ShellOverview *
get_overview (void)
{
  ShellMain *main = shell_main_get_default ();

  return main != NULL ? shell_main_get_overview (main) : NULL;
}

static void
perform (Action             action,
         ShellControlsState state,
         const char        *owner)
{
  ShellOverview *overview = get_overview ();

  if (overview == NULL)
    return;

  switch (action)
    {
    case ACTION_SHOW:
      shell_overview_show_for_extension (overview, state, owner);
      break;

    case ACTION_HIDE:
      shell_overview_hide_for_extension (overview, owner);
      break;

    case ACTION_TOGGLE:
      shell_overview_toggle_for_extension (overview, owner);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
deferred_free (gpointer data)
{
  DeferredAction *deferred = data;

  g_weak_ref_clear (&deferred->extension);
  g_free (deferred->owner);
  g_free (deferred);
}

static gboolean
deferred_run (gpointer data)
{
  DeferredAction *deferred = data;
  g_autoptr (ShellLuaExtension) extension = g_weak_ref_get (&deferred->extension);

  /* The extension was disabled meanwhile: nothing it asked for happens */
  if (deferred->has_extension &&
      (extension == NULL ||
       shell_lua_extension_get_state (extension) != SHELL_LUA_EXTENSION_STATE_ACTIVE))
    return G_SOURCE_REMOVE;

  perform (deferred->action, deferred->state, deferred->owner);
  return G_SOURCE_REMOVE;
}

static void
run_action (lua_State          *L,
            Action              action,
            ShellControlsState  state)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);
  const char *owner = extension != NULL ? shell_lua_extension_get_uuid (extension) : "";

  if (shell_overview_hooks_are_emitting ())
    {
      DeferredAction *deferred = g_new0 (DeferredAction, 1);

      deferred->action = action;
      deferred->state = state;
      deferred->owner = g_strdup (owner);
      deferred->has_extension = extension != NULL;
      g_weak_ref_init (&deferred->extension, extension);

      g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, deferred_run, deferred, deferred_free);
      return;
    }

  perform (action, state, owner);
}

static const char * const show_fields[] = { "target", NULL };

static int
l_show (lua_State *L)
{
  ShellControlsState state = SHELL_CONTROLS_STATE_WINDOW_PICKER;

  if (!lua_isnoneornil (L, 1))
    {
      const char *target;

      luaL_checktype (L, 1, LUA_TTABLE);
      shell_lua_check_fields (L, 1, show_fields);

      target = shell_lua_opt_string (L, 1, "target", "window-picker");
      if (strcmp (target, "app-grid") == 0)
        state = SHELL_CONTROLS_STATE_APP_GRID;
      else if (strcmp (target, "window-picker") != 0)
        return luaL_error (L, "field 'target': unknown value '%s' (expected 'window-picker' or 'app-grid')",
                           target);
    }

  run_action (L, ACTION_SHOW, state);
  return 0;
}

static int
l_hide (lua_State *L)
{
  run_action (L, ACTION_HIDE, SHELL_CONTROLS_STATE_WINDOW_PICKER);
  return 0;
}

static int
l_toggle (lua_State *L)
{
  run_action (L, ACTION_TOGGLE, SHELL_CONTROLS_STATE_WINDOW_PICKER);
  return 0;
}

/* ------------------------------------------------------------------------ */
/* The table                                                                 */
/* ------------------------------------------------------------------------ */

/* __index of shell.overview: the three properties are computed on every read,
 * the functions live in the table of the upvalue */
static int
overview_index (lua_State *L)
{
  ShellOverview *overview = get_overview ();
  const char *key = lua_type (L, 2) == LUA_TSTRING ? lua_tostring (L, 2) : NULL;

  if (key == NULL)
    {
      lua_pushnil (L);
      return 1;
    }

  if (strcmp (key, "visible") == 0)
    {
      lua_pushboolean (L, overview != NULL && shell_overview_get_visible (overview));
    }
  else if (strcmp (key, "animating") == 0)
    {
      lua_pushboolean (L, overview != NULL && shell_overview_get_animation_in_progress (overview));
    }
  else if (strcmp (key, "target") == 0)
    {
      ShellControlsState state = overview != NULL ? shell_overview_get_target_state (overview)
                                                   : SHELL_CONTROLS_STATE_HIDDEN;

      switch (state)
        {
        case SHELL_CONTROLS_STATE_WINDOW_PICKER:
          lua_pushliteral (L, "window-picker");
          break;

        case SHELL_CONTROLS_STATE_APP_GRID:
          lua_pushliteral (L, "app-grid");
          break;

        default:
          lua_pushliteral (L, "hidden");
          break;
        }
    }
  else
    {
      lua_pushvalue (L, 2);
      lua_rawget (L, lua_upvalueindex (1));
    }

  return 1;
}

static int
overview_newindex (lua_State *L)
{
  const char *key = lua_type (L, 2) == LUA_TSTRING ? lua_tostring (L, 2) : "?";

  return luaL_error (L, "attempt to modify shell.overview: '%s' is read-only", key);
}

static int
overview_pairs_next (lua_State *L)
{
  static const char * const names[] = { "visible", "target", "animating", "show", "hide", "toggle", NULL };
  int i = 0;

  if (!lua_isnil (L, 2))
    {
      const char *key = luaL_checkstring (L, 2);

      for (i = 0; names[i] != NULL && strcmp (names[i], key) != 0; i++)
        ;
      i++;
    }

  if (names[i] == NULL)
    {
      lua_pushnil (L);
      return 1;
    }

  lua_pushstring (L, names[i]);
  lua_pushvalue (L, -1);
  lua_gettable (L, 1);
  return 2;
}

static int
overview_pairs (lua_State *L)
{
  lua_pushcfunction (L, overview_pairs_next);
  lua_pushvalue (L, 1);
  lua_pushnil (L);
  return 3;
}

static const luaL_Reg overview_funcs[] = {
  { "show",   l_show },
  { "hide",   l_hide },
  { "toggle", l_toggle },
  { NULL, NULL }
};

static void
shell_lua_api_overview_register (ShellLuaState *state,
                                 lua_State     *L)
{
  lua_newtable (L);                       /* the proxy */

  lua_newtable (L);                       /* the metatable */
  lua_newtable (L);                       /* the functions */
  luaL_setfuncs (L, overview_funcs, 0);
  lua_pushcclosure (L, overview_index, 1);
  lua_setfield (L, -2, "__index");
  lua_pushcfunction (L, overview_newindex);
  lua_setfield (L, -2, "__newindex");
  lua_pushcfunction (L, overview_pairs);
  lua_setfield (L, -2, "__pairs");
  lua_pushboolean (L, 0);
  lua_setfield (L, -2, "__metatable");
  lua_setmetatable (L, -2);

  lua_setfield (L, -2, "overview");
}

static void __attribute__ ((constructor))
shell_lua_api_overview_init (void)
{
  shell_lua_api_register ("overview", shell_lua_api_overview_register);
}
