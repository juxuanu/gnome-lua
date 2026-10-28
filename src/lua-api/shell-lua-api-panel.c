/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.panel and the shell.Indicator handle type (docs/api/panel.md): the
 * buttons of extensions in the top bar.
 *
 * An indicator is a plain #ShellPanelMenuButton (src/ui/panel-menu.c) that
 * the context of the extension owns. What the Lua side knows beyond the
 * object (the role, the box and the click gesture of an indicator without a
 * menu) is kept in a #IndicatorData attached to it. The placement is the
 * panel's: shell_panel_add_extension_indicator () (src/ui/panel.c) keeps
 * every box as [extensions at the start] [the shell's indicators]
 * [extensions at the end] and orders the extensions as README.md says.
 *
 * Other modules of src/lua-api:
 *  - `ind.menu` is a "shell.Menu" handle (shell-lua-api-menu.h): the menu is
 *    the #ShellPopupMenu the button creates for itself, which the indicator
 *    destroys.
 *  - `ind.child` is a "shell.Widget" handle (shell-lua-api-ui.h): a widget
 *    made by shell.ui, or the icon or label the shorthand fields create here
 *    and own like any widget.
 *  - `icon_file` is resolved by shell-lua-api-resolve.h, with the rule of
 *    ui.md.
 */

#include "config.h"

#include <string.h>

#include <clutter/clutter.h>

#include "shell-lua-api-menu.h"
#include "shell-lua-api-private.h"
#include "shell-lua-api-ui.h"
#include "st.h"
#include "ui/main.h"
#include "ui/panel.h"
#include "ui/popup-menu.h"

#define INDICATOR_TYPE "shell.Indicator"
#define INDICATOR_DATA_KEY "shell-lua-indicator"

typedef struct
{
  char *role;
  ShellPanelBox box;
  ShellPanelEdge edge;
  ClutterClickGesture *click; /* strong; only for an indicator with on_click */
} IndicatorData;

static void
indicator_data_free (gpointer data)
{
  IndicatorData *d = data;

  g_free (d->role);
  g_clear_object (&d->click);
  g_free (d);
}

static IndicatorData *
indicator_get_data (GObject *object)
{
  return g_object_get_data (object, INDICATOR_DATA_KEY);
}

/* ------------------------------------------------------------------------ */
/* Enumerations                                                              */
/* ------------------------------------------------------------------------ */

static const char *
box_to_string (ShellPanelBox box)
{
  switch (box)
    {
    case SHELL_PANEL_BOX_LEFT:
      return "left";
    case SHELL_PANEL_BOX_CENTER:
      return "center";
    case SHELL_PANEL_BOX_RIGHT:
    default:
      return "right";
    }
}

static gboolean
valid_role (const char *name)
{
  gsize len = strlen (name);
  gsize i;

  if (len < 1 || len > 128)
    return FALSE;

  for (i = 0; i < len; i++)
    {
      char c = name[i];

      if (!g_ascii_isalnum (c) && c != '_' && c != '.' && c != '@' && c != '-')
        return FALSE;
    }

  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* The children of an indicator                                              */
/* ------------------------------------------------------------------------ */

/* The shorthand fields are `shell.ui.icon{ style_class = "system-status-icon",
 * ... }` and `shell.ui.label{ text = ..., y_align = "center" }`: they are made
 * by the constructors of shell.ui (the table is the first upvalue of
 * add_indicator), so the child is a widget like any other. Leaves the handle
 * on the stack and returns the actor. */
static ClutterActor *
make_shorthand_child (lua_State  *L,
                      const char *icon_name,
                      const char *icon_file,
                      const char *text)
{
  const char *constructor = text != NULL ? "label" : "icon";

  lua_pushvalue (L, lua_upvalueindex (1));
  if (lua_getfield (L, -1, "ui") != LUA_TTABLE)
    luaL_error (L, "the fields 'icon_name', 'icon_file' and 'text' need shell.ui");
  lua_remove (L, -2);
  if (lua_getfield (L, -1, constructor) != LUA_TFUNCTION)
    luaL_error (L, "the fields 'icon_name', 'icon_file' and 'text' need shell.ui.%s", constructor);
  lua_remove (L, -2);

  lua_newtable (L);
  if (text != NULL)
    {
      lua_pushstring (L, text);
      lua_setfield (L, -2, "text");
      lua_pushliteral (L, "center");
      lua_setfield (L, -2, "y_align");
    }
  else
    {
      lua_pushliteral (L, "system-status-icon");
      lua_setfield (L, -2, "style_class");
      if (icon_name != NULL)
        {
          lua_pushstring (L, icon_name);
          lua_setfield (L, -2, "icon_name");
        }
      else
        {
          lua_pushstring (L, icon_file);
          lua_setfield (L, -2, "icon_file");
        }
    }
  lua_call (L, 1, 1);

  return shell_lua_api_ui_check_free_widget (L, -1);
}

/* Destroys a child the context owns through the runtime (so that its handles
 * die and the destruction waits for a callback that is running), or any other
 * actor directly. */
static void
destroy_child (lua_State    *L,
               ClutterActor *child)
{
  if (shell_lua_object_is_owned (L, G_OBJECT (child)))
    shell_lua_destroy_object (L, G_OBJECT (child));
  else
    clutter_actor_destroy (child);
}

/* ------------------------------------------------------------------------ */
/* shell.Indicator                                                           */
/* ------------------------------------------------------------------------ */

static ShellPanelMenuButton *
check_indicator (lua_State *L)
{
  return SHELL_PANEL_MENU_BUTTON (shell_lua_handle_check (L, 1, INDICATOR_TYPE));
}

static void
get_name (lua_State *L,
          GObject   *object)
{
  IndicatorData *data = indicator_get_data (object);

  lua_pushstring (L, data != NULL ? data->role : "");
}

static void
get_position (lua_State *L,
              GObject   *object)
{
  IndicatorData *data = indicator_get_data (object);

  lua_pushstring (L, box_to_string (data != NULL ? data->box : SHELL_PANEL_BOX_RIGHT));
}

static void
get_child (lua_State *L,
           GObject   *object)
{
  ClutterActor *child = clutter_actor_get_first_child (CLUTTER_ACTOR (object));

  if (child == NULL)
    {
      lua_pushnil (L);
      return;
    }

  shell_lua_api_ui_register_types (L);
  shell_lua_handle_push (L, NULL, G_OBJECT (child));
}

static void
set_child (lua_State *L,
           GObject   *object,
           int        idx)
{
  ClutterActor *self = CLUTTER_ACTOR (object);
  ClutterActor *old = clutter_actor_get_first_child (self);
  ClutterActor *child = NULL;

  if (!lua_isnil (L, idx))
    {
      child = shell_lua_api_ui_check_widget (L, idx);

      if (child == old)
        luaL_error (L, "the widget already has a parent");
      shell_lua_api_ui_check_free_widget (L, idx);
    }

  if (old != NULL)
    destroy_child (L, old);
  if (child != NULL)
    clutter_actor_add_child (self, child);
}

static void
get_menu (lua_State *L,
          GObject   *object)
{
  GObject *menu = shell_panel_menu_button_get_menu (SHELL_PANEL_MENU_BUTTON (object));

  if (menu != NULL && SHELL_IS_POPUP_MENU (menu))
    shell_lua_api_menu_push (L, NULL, SHELL_POPUP_MENU_BASE (menu));
  else
    lua_pushnil (L);
}

static void
get_sensitive (lua_State *L,
               GObject   *object)
{
  lua_pushboolean (L, clutter_actor_get_reactive (CLUTTER_ACTOR (object)));
}

static void
set_sensitive (lua_State *L,
               GObject   *object,
               int        idx)
{
  luaL_checktype (L, idx, LUA_TBOOLEAN);
  shell_panel_menu_button_set_sensitive (SHELL_PANEL_MENU_BUTTON (object), lua_toboolean (L, idx));
}

static void
get_active (lua_State *L,
            GObject   *object)
{
  lua_pushboolean (L, shell_panel_menu_button_get_menu_is_open (SHELL_PANEL_MENU_BUTTON (object)));
}

static void
set_accessible_name (lua_State *L,
                     GObject   *object,
                     int        idx)
{
  const char *name;

  luaL_checktype (L, idx, LUA_TSTRING);
  name = lua_tostring (L, idx);
  if (name[0] == '\0')
    luaL_error (L, "accessible_name must not be empty");
  clutter_actor_set_accessible_name (CLUTTER_ACTOR (object), name);
}

static void
get_accessible_name (lua_State *L,
                     GObject   *object)
{
  const char *name = clutter_actor_get_accessible_name (CLUTTER_ACTOR (object));

  lua_pushstring (L, name != NULL ? name : "");
}

static void
set_actor_visible (ClutterActor *actor,
                   gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

static void
set_visible (lua_State *L,
             GObject   *object,
             int        idx)
{
  luaL_checktype (L, idx, LUA_TBOOLEAN);
  set_actor_visible (CLUTTER_ACTOR (object), lua_toboolean (L, idx));
}

static void
get_visible (lua_State *L,
             GObject   *object)
{
  lua_pushboolean (L, clutter_actor_is_visible (CLUTTER_ACTOR (object)));
}

/* ---- Events ------------------------------------------------------------- */

static void
push_button_arg (lua_State *L,
                 gpointer   data)
{
  lua_pushinteger (L, 1);
}

static void
click_recognize_cb (ClutterGesture   *gesture,
                    ShellLuaCallback *cb)
{
  unsigned int button = clutter_press_gesture_get_button (CLUTTER_PRESS_GESTURE (gesture));

  /* The primary button, or a touch (which reports no button) */
  if (button > 1)
    return;

  shell_lua_callback_emit (cb, 1, push_button_arg, NULL);
}

static int
l_indicator_on_click (lua_State *L)
{
  ShellPanelMenuButton *indicator = check_indicator (L);
  IndicatorData *data = indicator_get_data (G_OBJECT (indicator));
  ShellLuaCallback *cb;
  gulong id;
  int ref;

  if (data == NULL || data->click == NULL)
    return luaL_error (L, "on_click is only for indicators without a menu");

  luaL_checktype (L, 2, LUA_TFUNCTION);
  lua_pushvalue (L, 2);
  ref = luaL_ref (L, LUA_REGISTRYINDEX);
  cb = shell_lua_callback_new (L, G_OBJECT (indicator), ref);
  id = shell_lua_callback_connect (cb, data->click, "recognize", G_CALLBACK (click_recognize_cb));

  return shell_lua_callback_push_connection (L, cb, data->click, id);
}

static const ShellLuaHandleMethod indicator_methods[] = {
  { "destroy",  shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { "on_click", l_indicator_on_click, 0 },
  { NULL }
};

static const ShellLuaHandleVirtual indicator_virtuals[] = {
  { "name",            get_name,            NULL },
  { "position",        get_position,        NULL },
  { "child",           get_child,           set_child },
  { "menu",            get_menu,            NULL },
  { "accessible_name", get_accessible_name, set_accessible_name },
  { "visible",         get_visible,         set_visible },
  { "sensitive",       get_sensitive,       set_sensitive },
  { "active",          get_active,          NULL },
  { NULL }
};

static ShellLuaHandleType indicator_type = {
  .name = INDICATOR_TYPE,
  .methods_ex = indicator_methods,
  .virtuals = indicator_virtuals,
};

/* ------------------------------------------------------------------------ */
/* shell.panel.add_indicator                                                 */
/* ------------------------------------------------------------------------ */

static const char * const indicator_fields[] = {
  "accessible_name", "name", "position", "edge", "index", "child", "icon_name",
  "icon_file", "text", "menu", "on_click", "visible", NULL
};

static const char * const position_names[] = { "left", "center", "right", NULL };
static const char * const edge_names[] = { "start", "end", NULL };

/* The index in @names of the string field @field of the table at 1, or
 * @def if it is absent. Raises for any other value. */
static int
opt_choice (lua_State         *L,
            const char        *field,
            const char *const *names,
            int                def)
{
  const char *value = shell_lua_opt_string (L, 1, field, NULL);
  int i;

  if (value == NULL)
    return def;

  for (i = 0; names[i] != NULL; i++)
    {
      if (strcmp (names[i], value) == 0)
        return i;
    }

  return luaL_error (L, "field '%s': unknown value '%s'", field, value);
}

static int
l_add_indicator (lua_State *L)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);
  ShellPanel *panel = shell_main_get_panel (shell_main_get_default ());
  ShellPanelMenuButton *indicator;
  IndicatorData *data;
  GError *error = NULL;
  const char *accessible_name, *name, *icon_name, *icon_file, *text;
  ClutterActor *child = NULL;
  gboolean with_menu, visible, has_click, has_index;
  gboolean created_ok;
  lua_Integer index;
  int position, edge, n_content = 0;
  int click_ref;

  luaL_checktype (L, 1, LUA_TTABLE);
  shell_lua_check_fields (L, 1, indicator_fields);

  if (panel == NULL)
    return luaL_error (L, "the top bar is not available");

  accessible_name = shell_lua_opt_string (L, 1, "accessible_name", NULL);
  if (accessible_name == NULL || accessible_name[0] == '\0')
    return luaL_error (L, "missing required field 'accessible_name' (a non-empty string)");

  name = shell_lua_opt_string (L, 1, "name", NULL);
  if (name == NULL)
    {
      if (extension == NULL)
        return luaL_error (L, "missing required field 'name' (a string)");
      name = shell_lua_extension_get_uuid (extension);
    }
  if (!valid_role (name))
    return luaL_error (L, "field 'name' must match [A-Za-z0-9_.@-]{1,128}");

  position = opt_choice (L, "position", position_names, SHELL_PANEL_BOX_RIGHT);
  edge = opt_choice (L, "edge", edge_names, SHELL_PANEL_EDGE_START);

  lua_getfield (L, 1, "index");
  has_index = !lua_isnil (L, -1);
  lua_pop (L, 1);
  index = shell_lua_opt_integer (L, 1, "index", 0);
  if (has_index && index < 1)
    return luaL_error (L, "field 'index' must be an integer >= 1");
  if (index > G_MAXINT)
    index = G_MAXINT;

  /* exactly one of child, icon_name, icon_file, text, or none */
  lua_getfield (L, 1, "child");
  if (!lua_isnil (L, -1))
    {
      child = shell_lua_api_ui_check_free_widget (L, -1);
      n_content++;
    }
  lua_pop (L, 1);
  icon_name = shell_lua_opt_string (L, 1, "icon_name", NULL);
  icon_file = shell_lua_opt_string (L, 1, "icon_file", NULL);
  text = shell_lua_opt_string (L, 1, "text", NULL);
  n_content += (icon_name != NULL) + (icon_file != NULL) + (text != NULL);
  if (n_content > 1)
    return luaL_error (L, "only one of 'child', 'icon_name', 'icon_file' and 'text' can be given");

  lua_getfield (L, 1, "on_click");
  has_click = !lua_isnil (L, -1);
  if (has_click && lua_type (L, -1) != LUA_TFUNCTION)
    return luaL_error (L, "field 'on_click' must be a function, got %s", luaL_typename (L, -1));
  lua_pop (L, 1);
  with_menu = shell_lua_opt_boolean (L, 1, "menu", !has_click);
  if (has_click && with_menu)
    return luaL_error (L, "'on_click' and 'menu = true' are mutually exclusive");
  visible = shell_lua_opt_boolean (L, 1, "visible", TRUE);

  shell_lua_check_owned_capacity (L);

  /* shell.ui checks the shorthand fields, before the indicator exists */
  if (child == NULL && n_content > 0)
    child = make_shorthand_child (L, icon_name, icon_file, text);

  /* Everything checked: create */
  indicator = g_object_new (SHELL_TYPE_PANEL_MENU_BUTTON,
                            "menu-alignment", 0.5,
                            "dont-create-menu", !with_menu,
                            NULL);
  shell_lua_own_object (L, G_OBJECT (indicator), shell_lua_api_ui_actor_ops ());   /* sinks the floating reference */
  shell_lua_api_ui_decorate (L, CLUTTER_ACTOR (indicator));   /* `lua-ext-<id>`, so a stylesheet can scope itself */

  data = g_new0 (IndicatorData, 1);
  data->role = g_strdup (name);
  data->box = position;
  data->edge = edge;
  g_object_set_data_full (G_OBJECT (indicator), INDICATOR_DATA_KEY, data, indicator_data_free);

  clutter_actor_set_accessible_name (CLUTTER_ACTOR (indicator), accessible_name);
  set_actor_visible (CLUTTER_ACTOR (indicator), visible);

  if (child != NULL)
    clutter_actor_add_child (CLUTTER_ACTOR (indicator), child);

  if (!with_menu)
    {
      /* A plain button: the click is the user's, not the menu's */
      data->click = CLUTTER_CLICK_GESTURE (g_object_ref_sink (clutter_click_gesture_new ()));
      clutter_actor_add_action (CLUTTER_ACTOR (indicator), CLUTTER_ACTION (data->click));
    }

  created_ok = shell_panel_add_extension_indicator (panel, name, indicator,
                                                    extension != NULL ? shell_lua_extension_get_uuid (extension) : "-",
                                                    position, edge, (int) index, &error);
  if (!created_ok)
    {
      lua_pushstring (L, error->message);
      g_error_free (error);
      shell_lua_destroy_object (L, G_OBJECT (indicator));
      return lua_error (L);
    }

  click_ref = shell_lua_opt_callback (L, 1, "on_click", FALSE);
  if (click_ref != LUA_NOREF)
    {
      ShellLuaCallback *cb = shell_lua_callback_new (L, G_OBJECT (indicator), click_ref);

      shell_lua_callback_connect (cb, data->click, "recognize", G_CALLBACK (click_recognize_cb));
    }

  shell_lua_handle_push (L, INDICATOR_TYPE, G_OBJECT (indicator));
  return 1;
}

static const luaL_Reg panel_funcs[] = {
  { "add_indicator", l_add_indicator },
  { NULL, NULL }
};

static void
shell_lua_api_panel_register (ShellLuaState *state,
                              lua_State     *L)
{
  indicator_type.gtype = SHELL_TYPE_PANEL_MENU_BUTTON;
  shell_lua_handle_register_type (L, &indicator_type);
  /* The handles of the children and of the menu */
  shell_lua_api_ui_register_types (L);
  shell_lua_api_menu_register_types (L);

  lua_newtable (L);
  lua_pushvalue (L, -2);   /* the shell table, to reach shell.ui when it is called */
  luaL_setfuncs (L, panel_funcs, 1);
  lua_setfield (L, -2, "panel");
}

static void __attribute__ ((constructor))
shell_lua_api_panel_init (void)
{
  shell_lua_api_register ("panel", shell_lua_api_panel_register);
}
