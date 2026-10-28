/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.dash and the shell.DashButton handle type (docs/api/dash.md): the
 * launcher-style buttons of extensions in the dash of the overview.
 *
 * A button is a #ShellDashButton (src/ui/dash.c) that the context of the
 * extension owns; the dash keeps the buttons in two groups beside the box of
 * the applications and orders them as README.md says
 * (shell_dash_add_extension_button ()). Destroying a button from Lua fades it
 * out like the icon of an application that goes away; the context destroys
 * it at once at disable.
 *
 * `icon_file` is resolved by shell-lua-api-resolve.h, with the rule of ui.md.
 */

#include "config.h"

#include <string.h>

#include <clutter/clutter.h>

#include "shell-lua-api-private.h"
#include "shell-lua-api-resolve.h"
#include "shell-lua-api-ui.h"
#include "st.h"
#include "ui/dash.h"
#include "ui/main.h"
#include "ui/overview.h"

#define BUTTON_TYPE "shell.DashButton"
/* The icon_file as the extension gave it, for reading it back */
#define ICON_FILE_KEY "shell-lua-dash-icon-file"

/* Set while `button:destroy()` runs, so that the button fades out. At
 * disable, and for a destruction that was deferred, it goes at once. */
static gboolean destroy_animated = FALSE;

static ShellDash *
find_dash (ClutterActor *actor)
{
  for (; actor != NULL; actor = clutter_actor_get_parent (actor))
    {
      if (SHELL_IS_DASH (actor))
        return SHELL_DASH (actor);
    }

  return NULL;
}

static void
button_destroy (GObject *object)
{
  ShellDashButton *button = SHELL_DASH_BUTTON (object);
  ShellDash *dash = find_dash (CLUTTER_ACTOR (button));

  if (dash != NULL && destroy_animated)
    shell_dash_remove_extension_button (dash, button);
  else
    clutter_actor_destroy (CLUTTER_ACTOR (button));
}

static const ShellLuaOwnOps button_ops = {
  .destroy = button_destroy,
  .destroyed_signal = "destroy",
};

static int
l_button_destroy (lua_State *L)
{
  int n;

  destroy_animated = TRUE;
  n = shell_lua_handle_destroy_method (L);
  destroy_animated = FALSE;

  return n;
}

/* ------------------------------------------------------------------------ */
/* shell.DashButton                                                          */
/* ------------------------------------------------------------------------ */

static ShellDashButton *
check_button (lua_State *L)
{
  return SHELL_DASH_BUTTON (shell_lua_handle_check (L, 1, BUTTON_TYPE));
}

static void
get_visible (lua_State *L,
             GObject   *object)
{
  lua_pushboolean (L, clutter_actor_is_visible (CLUTTER_ACTOR (object)));
}

static void
set_visible (lua_State *L,
             GObject   *object,
             int        idx)
{
  luaL_checktype (L, idx, LUA_TBOOLEAN);
  if (lua_toboolean (L, idx))
    clutter_actor_show (CLUTTER_ACTOR (object));
  else
    clutter_actor_hide (CLUTTER_ACTOR (object));
}

static void
get_icon_name (lua_State *L,
               GObject   *object)
{
  const char *name = shell_dash_button_get_icon_name (SHELL_DASH_BUTTON (object));

  if (name != NULL)
    lua_pushstring (L, name);
  else
    lua_pushnil (L);
}

static void
set_icon_name (lua_State *L,
               GObject   *object,
               int        idx)
{
  const char *name;

  luaL_checktype (L, idx, LUA_TSTRING);
  name = lua_tostring (L, idx);
  if (name[0] == '\0')
    luaL_error (L, "icon_name must not be empty");

  g_object_set_data (object, ICON_FILE_KEY, NULL);
  shell_dash_button_set_icon_name (SHELL_DASH_BUTTON (object), name);
}

static void
get_icon_file (lua_State *L,
               GObject   *object)
{
  const char *rel = g_object_get_data (object, ICON_FILE_KEY);

  if (rel != NULL && shell_dash_button_get_icon_file (SHELL_DASH_BUTTON (object)) != NULL)
    lua_pushstring (L, rel);
  else
    lua_pushnil (L);
}

static void
set_icon_file (lua_State *L,
               GObject   *object,
               int        idx)
{
  const char *rel;
  const char *path;

  luaL_checktype (L, idx, LUA_TSTRING);
  rel = lua_tostring (L, idx);
  path = shell_lua_api_push_extension_file (L, "icon_file", rel);

  g_object_set_data_full (object, ICON_FILE_KEY, g_strdup (rel), g_free);
  shell_dash_button_set_icon_file (SHELL_DASH_BUTTON (object), path);
  lua_pop (L, 1);
}

static void
push_button_arg (lua_State *L,
                 gpointer   data)
{
  lua_pushinteger (L, GPOINTER_TO_INT (data));
}

static void
button_clicked_cb (ShellDashButton  *button,
                   int               clicked_button,
                   ShellLuaCallback *cb)
{
  shell_lua_callback_emit (cb, 1, push_button_arg, GINT_TO_POINTER (clicked_button));
}

static int
l_button_on_click (lua_State *L)
{
  ShellDashButton *button = check_button (L);
  ShellLuaCallback *cb;
  gulong id;
  int ref;

  luaL_checktype (L, 2, LUA_TFUNCTION);
  lua_pushvalue (L, 2);
  ref = luaL_ref (L, LUA_REGISTRYINDEX);
  cb = shell_lua_callback_new (L, G_OBJECT (button), ref);
  id = shell_lua_callback_connect (cb, button, "clicked", G_CALLBACK (button_clicked_cb));

  return shell_lua_callback_push_connection (L, cb, button, id);
}

static const ShellLuaHandleMethod button_methods[] = {
  { "destroy",  l_button_destroy, SHELL_LUA_METHOD_DEAD_OK },
  { "on_click", l_button_on_click, 0 },
  { NULL }
};

static const ShellLuaHandleProperty button_properties[] = {
  { "label",         "label-text",     TRUE, TRUE },
  { "checked",       "checked",        TRUE, TRUE },
  { "sensitive",     "sensitive",      TRUE, TRUE },
  { "hide_overview", "hide-overview",  TRUE, TRUE },
  { "position",      "edge",           TRUE, FALSE },
  { "index",         "index",          TRUE, FALSE },
  { NULL }
};

static const ShellLuaHandleVirtual button_virtuals[] = {
  { "visible",   get_visible,   set_visible },
  { "icon_name", get_icon_name, set_icon_name },
  { "icon_file", get_icon_file, set_icon_file },
  { NULL }
};

static ShellLuaHandleType button_type = {
  .name = BUTTON_TYPE,
  .properties = button_properties,
  .methods_ex = button_methods,
  .virtuals = button_virtuals,
};

/* ------------------------------------------------------------------------ */
/* Constructors                                                              */
/* ------------------------------------------------------------------------ */

static const char * const button_fields[] = {
  "label", "icon_name", "icon_file", "on_click", "position", "hide_overview",
  "checked", "visible", "sensitive", NULL
};

static const char * const insert_fields[] = {
  "label", "icon_name", "icon_file", "on_click", "position", "hide_overview",
  "checked", "visible", "sensitive", "index", NULL
};

static int
create_button (lua_State *L,
               gboolean   with_index)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);
  ShellOverview *overview = shell_main_get_overview (shell_main_get_default ());
  ShellDash *dash = overview != NULL ? shell_overview_get_dash (overview) : NULL;
  ShellDashButton *button;
  GError *error = NULL;
  const char *label, *icon_name, *icon_file, *position;
  const char *icon_file_path = NULL;
  gboolean hide_overview, checked, visible, sensitive;
  lua_Integer index = 0;
  ShellDashEdge edge = SHELL_DASH_EDGE_END;
  int click_ref;

  luaL_checktype (L, 1, LUA_TTABLE);
  shell_lua_check_fields (L, 1, with_index ? insert_fields : button_fields);

  if (dash == NULL)
    return luaL_error (L, "the dash is not available");

  label = shell_lua_opt_string (L, 1, "label", NULL);
  if (label == NULL || label[0] == '\0')
    return luaL_error (L, "missing required field 'label' (a non-empty string)");

  icon_name = shell_lua_opt_string (L, 1, "icon_name", NULL);
  icon_file = shell_lua_opt_string (L, 1, "icon_file", NULL);
  if (icon_name == NULL && icon_file == NULL)
    return luaL_error (L, "one of 'icon_name' and 'icon_file' is required");
  if (icon_name != NULL && icon_file != NULL)
    return luaL_error (L, "only one of 'icon_name' and 'icon_file' can be given");
  if (icon_name != NULL && icon_name[0] == '\0')
    return luaL_error (L, "field 'icon_name' must not be empty");

  position = shell_lua_opt_string (L, 1, "position", "end");
  if (strcmp (position, "start") == 0)
    edge = SHELL_DASH_EDGE_START;
  else if (strcmp (position, "end") != 0)
    return luaL_error (L, "field 'position': unknown value '%s'", position);

  if (with_index)
    {
      index = shell_lua_opt_integer (L, 1, "index", 0);
      if (index < 1)
        return luaL_error (L, "missing required field 'index' (an integer >= 1)");
      if (index > G_MAXINT)
        index = G_MAXINT;
    }

  lua_getfield (L, 1, "on_click");
  if (!lua_isnil (L, -1) && lua_type (L, -1) != LUA_TFUNCTION)
    return luaL_error (L, "field 'on_click' must be a function, got %s", luaL_typename (L, -1));
  lua_pop (L, 1);

  hide_overview = shell_lua_opt_boolean (L, 1, "hide_overview", TRUE);
  checked = shell_lua_opt_boolean (L, 1, "checked", FALSE);
  visible = shell_lua_opt_boolean (L, 1, "visible", TRUE);
  sensitive = shell_lua_opt_boolean (L, 1, "sensitive", TRUE);

  if (icon_file != NULL)
    icon_file_path = shell_lua_api_push_extension_file (L, "icon_file", icon_file);   /* kept on the stack */

  shell_lua_check_owned_capacity (L);

  button = shell_dash_button_new (label);
  shell_lua_own_object (L, G_OBJECT (button), &button_ops);   /* sinks the floating reference */
  shell_lua_api_ui_decorate (L, CLUTTER_ACTOR (button));   /* `lua-ext-<id>`, so a stylesheet can scope itself */

  if (icon_name != NULL)
    {
      shell_dash_button_set_icon_name (button, icon_name);
    }
  else
    {
      shell_dash_button_set_icon_file (button, icon_file_path);
      g_object_set_data_full (G_OBJECT (button), ICON_FILE_KEY, g_strdup (icon_file), g_free);
    }

  g_object_set (button,
                "hide-overview", hide_overview,
                "checked", checked,
                "sensitive", sensitive,
                NULL);
  if (!visible)
    clutter_actor_hide (CLUTTER_ACTOR (button));

  if (!shell_dash_add_extension_button (dash, button,
                                        extension != NULL ? shell_lua_extension_get_uuid (extension) : "-",
                                        edge, (int) index, &error))
    {
      lua_pushstring (L, error->message);
      g_error_free (error);
      shell_lua_destroy_object (L, G_OBJECT (button));
      return lua_error (L);
    }

  click_ref = shell_lua_opt_callback (L, 1, "on_click", FALSE);
  if (click_ref != LUA_NOREF)
    {
      ShellLuaCallback *cb = shell_lua_callback_new (L, G_OBJECT (button), click_ref);

      shell_lua_callback_connect (cb, button, "clicked", G_CALLBACK (button_clicked_cb));
    }

  shell_lua_handle_push (L, BUTTON_TYPE, G_OBJECT (button));
  return 1;
}

static int
l_add_button (lua_State *L)
{
  return create_button (L, FALSE);
}

static int
l_insert_at (lua_State *L)
{
  return create_button (L, TRUE);
}

static const luaL_Reg dash_funcs[] = {
  { "add_button", l_add_button },
  { "insert_at",  l_insert_at },
  { NULL, NULL }
};

static void
shell_lua_api_dash_register (ShellLuaState *state,
                             lua_State     *L)
{
  button_type.gtype = SHELL_TYPE_DASH_BUTTON;
  shell_lua_handle_register_type (L, &button_type);

  lua_newtable (L);
  luaL_setfuncs (L, dash_funcs, 0);
  lua_setfield (L, -2, "dash");
}

static void __attribute__ ((constructor))
shell_lua_api_dash_init (void)
{
  shell_lua_api_register ("dash", shell_lua_api_dash_register);
}
