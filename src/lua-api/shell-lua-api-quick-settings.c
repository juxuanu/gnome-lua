/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-lua-api-quick-settings.c: `shell.quick_settings`, the toggles,
 * sliders and status icons that an extension adds to the system menu
 * (docs/api/quick-settings.md).
 *
 * Handle types:
 *
 *   shell.QuickItem        ShellQuickSettingsItem (abstract base)
 *   shell.QuickToggle      ShellQuickToggle
 *   shell.QuickMenuToggle  ShellQuickMenuToggle (a shell.QuickToggle)
 *   shell.QuickSlider      ShellQuickSlider
 *   shell.QuickIndicator   a status icon (private StIcon subclass, so that the
 *                          type does not claim #StIcon for every handle)
 *   shell.QuickMenu        ShellQuickToggleMenu (a shell.Menu, menu.md)
 *
 * The items are owned by the extension context (shell_lua_own_object ()):
 * destroying an item, or the context at disable, removes it from the grid.
 * Where an item goes is decided by shell_panel_items_add_extension_item ()
 * (ui/panel-items.c), by the ordering rule of the README.
 *
 * Events: `on_toggle` and `on_click` are the "clicked" signal of the item (so
 * only the user triggers them; `checked` has already been flipped for a
 * toggle), `on_changed` is the value of the slider, `on_icon_click` the
 * "icon-clicked" signal. An assignment made by Lua code never calls back
 * (README rule 7): the callbacks are emitted through
 * shell_lua_callback_emit (), which the handle layer silences while a
 * property is assigned.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <gio/gio.h>

#include "shell-global.h"
#include "shell-lua-api-menu.h"
#include "shell-lua-api-private.h"
#include "shell-lua-api-resolve.h"
#include "ui/main.h"
#include "ui/panel-items.h"
#include "ui/quick-settings.h"

#define MODE_KEY "shell-lua-quick-item-mode"

typedef enum
{
  ITEM_MODE_DISPLAY = 0,
  ITEM_MODE_TOGGLE = 1,
  ITEM_MODE_CLICK = 2,
} ItemMode;

/* ------------------------------------------------------------------------ */
/* ShellLuaQuickIndicator: the status icon of shell.quick_settings.add_indicator */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LUA_QUICK_INDICATOR (shell_lua_quick_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaQuickIndicator, shell_lua_quick_indicator,
                      SHELL, LUA_QUICK_INDICATOR, StIcon)

struct _ShellLuaQuickIndicator
{
  StIcon parent;
};

G_DEFINE_FINAL_TYPE (ShellLuaQuickIndicator, shell_lua_quick_indicator, ST_TYPE_ICON)

static void
shell_lua_quick_indicator_class_init (ShellLuaQuickIndicatorClass *klass)
{
}

static void
shell_lua_quick_indicator_init (ShellLuaQuickIndicator *self)
{
  st_widget_set_style_class_name (ST_WIDGET (self), "system-status-icon");
}

static void shell_lua_api_quick_settings_register (ShellLuaState *state,
                                                   lua_State     *L);

static void __attribute__ ((constructor))
shell_lua_api_quick_settings_init (void)
{
  shell_lua_api_register ("quick-settings", shell_lua_api_quick_settings_register);
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static void
actor_destroy (GObject *object)
{
  clutter_actor_destroy (CLUTTER_ACTOR (object));
}

static const ShellLuaOwnOps actor_ops = {
  .destroy = actor_destroy,
  .destroyed_signal = "destroy",
};

static void
set_actor_visible (ClutterActor *actor,
                   gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

static ShellPanel *
get_panel (void)
{
  ShellMain *shell_main = shell_main_get_default ();

  return shell_main != NULL ? shell_main_get_panel (shell_main) : NULL;
}

static const char *
get_owner (lua_State *L)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);

  return extension != NULL ? shell_lua_extension_get_uuid (extension) : "(inspector)";
}

static gboolean
field_present (lua_State  *L,
               int         tbl,
               const char *field)
{
  gboolean present = lua_getfield (L, tbl, field) != LUA_TNIL;

  lua_pop (L, 1);
  return present;
}

/* Raises unless the field is absent or callable (the reference is taken
 * later, once nothing can fail any more) */
static void
check_callable_field (lua_State  *L,
                      int         tbl,
                      const char *field)
{
  if (lua_getfield (L, tbl, field) != LUA_TNIL && lua_type (L, -1) != LUA_TFUNCTION)
    {
      if (luaL_getmetafield (L, -1, "__call") == LUA_TNIL)
        luaL_error (L, "field '%s' must be a function, got %s", field, luaL_typename (L, -1));
      lua_pop (L, 1);
    }
  lua_pop (L, 1);
}

static lua_Integer
opt_colspan (lua_State  *L,
             int         tbl,
             lua_Integer def)
{
  lua_Integer colspan = shell_lua_opt_integer (L, tbl, "colspan", def);

  if (colspan != 1 && colspan != 2)
    luaL_error (L, "field 'colspan' must be 1 or 2");
  return colspan;
}

/* 0 when absent: after the last item of the extension */
static int
opt_index (lua_State *L,
           int        tbl)
{
  lua_Integer index = shell_lua_opt_integer (L, tbl, "index", 0);

  if (field_present (L, tbl, "index") && index < 1)
    luaL_error (L, "field 'index' must be an integer >= 1");
  return (int) MIN (index, G_MAXINT);
}

static void
check_one_icon (lua_State *L,
                int        tbl)
{
  if (field_present (L, tbl, "icon_name") && field_present (L, tbl, "icon_file"))
    luaL_error (L, "fields 'icon_name' and 'icon_file' are mutually exclusive");
}

/* The icon of the file @rel of the field @field (docs/api/ui.md, "icon": a
 * .svg or .png file inside the extension directory) */
static GIcon *
icon_from_file (lua_State  *L,
                const char *field,
                const char *rel)
{
  GFile *file;
  GIcon *icon;

  shell_lua_api_push_extension_file (L, field, rel);
  file = g_file_new_for_path (lua_tostring (L, -1));
  lua_pop (L, 1);

  icon = g_file_icon_new (file);
  g_object_unref (file);
  return icon;
}

static void
push_icon_file (lua_State *L,
                GIcon     *icon)
{
  if (G_IS_FILE_ICON (icon))
    {
      char *path = g_file_get_path (g_file_icon_get_file (G_FILE_ICON (icon)));

      if (path != NULL)
        {
          lua_pushstring (L, path);
          g_free (path);
          return;
        }
    }

  lua_pushnil (L);
}

/* Takes ownership of @item (floating): puts it in the grid, or raises and
 * leaves nothing behind */
static void
place_item (lua_State    *L,
            ClutterActor *item,
            int           colspan,
            int           index)
{
  ShellPanel *panel = get_panel ();
  GError *error = NULL;

  if (panel == NULL)
    {
      shell_lua_destroy_object (L, G_OBJECT (item));
      luaL_error (L, "the top bar does not exist");
      return;
    }

  if (!shell_panel_items_add_extension_item (panel, item, colspan, get_owner (L), index, &error))
    {
      lua_pushstring (L, error->message);
      g_clear_error (&error);
      shell_lua_destroy_object (L, G_OBJECT (item));
      lua_error (L);
    }
}

/* ------------------------------------------------------------------------ */
/* Options shared by the items                                               */
/* ------------------------------------------------------------------------ */

typedef struct
{
  const char *title;
  const char *subtitle;
  const char *icon_name;
  const char *icon_file;
  const char *accessible_name;
  const char *icon_accessible_name;
  const char *menu_button_accessible_name;
  gboolean checked;
  gboolean visible;
  gboolean sensitive;
  gboolean menu_enabled;
  gboolean has_menu_enabled;
  gboolean icon_reactive;
  lua_Integer colspan;
  int index;
  double value;
  ItemMode mode;
} ItemOptions;

static const char * const toggle_fields[] = {
  "title", "subtitle", "icon_name", "icon_file", "checked", "on_toggle", "on_click",
  "visible", "colspan", "index", "sensitive", NULL
};

static const char * const menu_toggle_fields[] = {
  "title", "subtitle", "icon_name", "icon_file", "checked", "on_toggle", "on_click",
  "visible", "colspan", "index", "sensitive",
  "menu_enabled", "menu_button_accessible_name", NULL
};

static const char * const slider_fields[] = {
  "icon_name", "icon_file", "value", "on_changed", "icon_reactive", "on_icon_click",
  "icon_accessible_name", "accessible_name", "menu_enabled", "menu_button_accessible_name",
  "visible", "colspan", "index", "sensitive", NULL
};

static const char * const indicator_fields[] = {
  "icon_name", "icon_file", "visible", "accessible_name", "index", NULL
};

static void
parse_common_options (lua_State   *L,
                      int          tbl,
                      lua_Integer  default_colspan,
                      ItemOptions *opts)
{
  opts->visible = shell_lua_opt_boolean (L, tbl, "visible", TRUE);
  opts->sensitive = shell_lua_opt_boolean (L, tbl, "sensitive", TRUE);
  opts->colspan = opt_colspan (L, tbl, default_colspan);
  opts->index = opt_index (L, tbl);
}

static void
parse_menu_options (lua_State   *L,
                    int          tbl,
                    ItemOptions *opts)
{
  opts->has_menu_enabled = field_present (L, tbl, "menu_enabled");
  opts->menu_enabled = shell_lua_opt_boolean (L, tbl, "menu_enabled", FALSE);
  opts->menu_button_accessible_name =
    shell_lua_opt_string (L, tbl, "menu_button_accessible_name", NULL);
}

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */

static void
push_checked (lua_State *L,
              gpointer   user_data)
{
  lua_pushboolean (L, st_button_get_checked (ST_BUTTON (user_data)));
}

static void
item_clicked_toggle_cb (StButton         *button,
                        int               clicked_button,
                        ShellLuaCallback *cb)
{
  shell_lua_callback_emit (cb, 1, push_checked, button);
}

static void
item_clicked_cb (StButton         *button,
                 int               clicked_button,
                 ShellLuaCallback *cb)
{
  shell_lua_callback_emit (cb, 0, NULL, NULL);
}

static void
push_slider_value (lua_State *L,
                   gpointer   user_data)
{
  lua_pushnumber (L, shell_bar_level_get_value (SHELL_BAR_LEVEL (user_data)));
}

static void
slider_value_notify_cb (GObject          *slider,
                        GParamSpec       *pspec,
                        ShellLuaCallback *cb)
{
  shell_lua_callback_emit (cb, 1, push_slider_value, slider);
}

static void
slider_icon_clicked_cb (ShellQuickSlider *slider,
                        ShellLuaCallback *cb)
{
  shell_lua_callback_emit (cb, 0, NULL, NULL);
}

static ItemMode
item_get_mode (GObject *item)
{
  return (ItemMode) GPOINTER_TO_INT (g_object_get_data (item, MODE_KEY));
}

/* Connects @ref (taken over) as on_toggle; makes the item a toggle */
static gulong
connect_toggle (lua_State        *L,
                GObject          *item,
                int               ref,
                ShellLuaCallback **cb_out)
{
  ShellLuaCallback *cb = shell_lua_callback_new (L, item, ref);

  g_object_set (item, "toggle-mode", TRUE, NULL);
  g_object_set_data (item, MODE_KEY, GINT_TO_POINTER (ITEM_MODE_TOGGLE));
  *cb_out = cb;
  return shell_lua_callback_connect (cb, item, "clicked", G_CALLBACK (item_clicked_toggle_cb));
}

static gulong
connect_click (lua_State        *L,
               GObject          *item,
               int               ref,
               ShellLuaCallback **cb_out)
{
  ShellLuaCallback *cb = shell_lua_callback_new (L, item, ref);

  g_object_set_data (item, MODE_KEY, GINT_TO_POINTER (ITEM_MODE_CLICK));
  *cb_out = cb;
  return shell_lua_callback_connect (cb, item, "clicked", G_CALLBACK (item_clicked_cb));
}

static int
ref_function_arg (lua_State *L,
                  int        idx)
{
  if (lua_type (L, idx) != LUA_TFUNCTION && luaL_getmetafield (L, idx, "__call") == LUA_TNIL)
    return luaL_typeerror (L, idx, "function");
  if (lua_type (L, idx) != LUA_TFUNCTION)
    lua_pop (L, 1);

  lua_pushvalue (L, idx);
  return luaL_ref (L, LUA_REGISTRYINDEX);
}

/* h:on_toggle(fn) */
static int
l_item_on_toggle (lua_State *L)
{
  GObject *item = shell_lua_handle_check (L, 1, "shell.QuickToggle");
  ShellLuaCallback *cb;
  gulong id;
  int ref;

  if (item_get_mode (item) == ITEM_MODE_CLICK)
    return luaL_error (L, "an item cannot have both on_toggle and on_click");

  ref = ref_function_arg (L, 2);
  id = connect_toggle (L, item, ref, &cb);
  return shell_lua_callback_push_connection (L, cb, item, id);
}

/* h:on_click(fn) */
static int
l_item_on_click (lua_State *L)
{
  GObject *item = shell_lua_handle_check (L, 1, "shell.QuickToggle");
  ShellLuaCallback *cb;
  gulong id;
  int ref;

  if (item_get_mode (item) == ITEM_MODE_TOGGLE)
    return luaL_error (L, "an item cannot have both on_toggle and on_click");

  ref = ref_function_arg (L, 2);
  id = connect_click (L, item, ref, &cb);
  return shell_lua_callback_push_connection (L, cb, item, id);
}

/* h:on_changed(fn) */
static int
l_slider_on_changed (lua_State *L)
{
  GObject *item = shell_lua_handle_check (L, 1, "shell.QuickSlider");
  GObject *slider = G_OBJECT (shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (item)));
  ShellLuaCallback *cb;
  gulong id;
  int ref;

  ref = ref_function_arg (L, 2);
  cb = shell_lua_callback_new (L, item, ref);
  id = shell_lua_callback_connect (cb, slider, "notify::value", G_CALLBACK (slider_value_notify_cb));
  return shell_lua_callback_push_connection (L, cb, slider, id);
}

/* h:on_icon_click(fn) */
static int
l_slider_on_icon_click (lua_State *L)
{
  GObject *item = shell_lua_handle_check (L, 1, "shell.QuickSlider");
  ShellLuaCallback *cb;
  gulong id;
  int ref;

  if (!shell_quick_slider_get_icon_reactive (SHELL_QUICK_SLIDER (item)))
    return luaL_error (L, "on_icon_click requires icon_reactive = true");

  ref = ref_function_arg (L, 2);
  cb = shell_lua_callback_new (L, item, ref);
  id = shell_lua_callback_connect (cb, item, "icon-clicked", G_CALLBACK (slider_icon_clicked_cb));
  return shell_lua_callback_push_connection (L, cb, item, id);
}

/* ------------------------------------------------------------------------ */
/* Virtual properties                                                        */
/* ------------------------------------------------------------------------ */

static void
get_colspan (lua_State *L,
             GObject   *object)
{
  ShellPanel *panel = get_panel ();
  ShellQuickSettingsMenu *menu = panel != NULL ? shell_panel_items_get_quick_settings_menu (panel) : NULL;
  int span = menu != NULL ? shell_quick_settings_menu_get_item_col_span (menu, CLUTTER_ACTOR (object)) : 0;

  lua_pushinteger (L, span > 0 ? span : 1);
}

static void
set_colspan (lua_State *L,
             GObject   *object,
             int        idx)
{
  ShellPanel *panel = get_panel ();
  ShellQuickSettingsMenu *menu = panel != NULL ? shell_panel_items_get_quick_settings_menu (panel) : NULL;
  lua_Integer span;
  int isnum;

  span = lua_type (L, idx) == LUA_TNUMBER ? lua_tointegerx (L, idx, &isnum) : (isnum = 0);
  if (!isnum || (span != 1 && span != 2))
    luaL_error (L, "colspan must be the integer 1 or 2");

  if (menu != NULL)
    shell_quick_settings_menu_set_item_col_span (menu, CLUTTER_ACTOR (object), (int) span);
}

static void
get_index (lua_State *L,
           GObject   *object)
{
  ShellPanel *panel = get_panel ();

  lua_pushinteger (L, panel != NULL ? shell_panel_items_get_extension_index (panel, CLUTTER_ACTOR (object)) : 0);
}

/* item.menu: pushed through the menu module, which has the context take the
 * menu (so that events can be bound to it); the item destroys it */
static void
get_menu (lua_State *L,
          GObject   *object)
{
  ShellQuickToggleMenu *menu = shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (object));

  if (menu != NULL)
    shell_lua_api_menu_push (L, "shell.QuickMenu", SHELL_POPUP_MENU_BASE (menu));
  else
    lua_pushnil (L);
}

static void
get_icon_file (lua_State *L,
               GObject   *object)
{
  GIcon *icon = NULL;

  g_object_get (object, "gicon", &icon, NULL);
  push_icon_file (L, icon);
  g_clear_object (&icon);
}

static void
set_icon_file (lua_State *L,
               GObject   *object,
               int        idx)
{
  if (lua_isnil (L, idx))
    {
      g_object_set (object, "gicon", NULL, NULL);
    }
  else
    {
      GIcon *icon;

      if (lua_type (L, idx) != LUA_TSTRING)
        luaL_error (L, "icon_file must be a string or nil");

      icon = icon_from_file (L, "icon_file", lua_tostring (L, idx));
      g_object_set (object, "gicon", icon, NULL);
      g_object_unref (icon);
    }
}

static void
get_subtitle (lua_State *L,
              GObject   *object)
{
  char *subtitle = NULL;

  g_object_get (object, "subtitle", &subtitle, NULL);
  if (subtitle != NULL)
    lua_pushstring (L, subtitle);
  else
    lua_pushnil (L);
  g_free (subtitle);
}

/* nil or an empty string hides the subtitle */
static void
set_subtitle (lua_State *L,
              GObject   *object,
              int        idx)
{
  const char *subtitle = NULL;

  if (!lua_isnil (L, idx))
    {
      if (lua_type (L, idx) != LUA_TSTRING)
        luaL_error (L, "subtitle must be a string or nil");
      subtitle = lua_tostring (L, idx);
      if (subtitle[0] == '\0')
        subtitle = NULL;
    }

  g_object_set (object, "subtitle", subtitle, NULL);
}

static void
get_slider_value (lua_State *L,
                  GObject   *object)
{
  lua_pushnumber (L, shell_bar_level_get_value (SHELL_BAR_LEVEL (shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (object)))));
}

static void
set_slider_value (lua_State *L,
                  GObject   *object,
                  int        idx)
{
  lua_Number value;

  if (lua_type (L, idx) != LUA_TNUMBER)
    luaL_error (L, "value must be a number, got %s", luaL_typename (L, idx));
  value = lua_tonumber (L, idx);
  if (!(value >= 0.0 && value <= 1.0))
    luaL_error (L, "value must be between 0.0 and 1.0");

  shell_bar_level_set_value (SHELL_BAR_LEVEL (shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (object))),
                             value);
}

static void
get_slider_accessible_name (lua_State *L,
                            GObject   *object)
{
  ClutterActor *slider = CLUTTER_ACTOR (shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (object)));
  const char *name = clutter_actor_get_accessible_name (slider);

  if (name != NULL)
    lua_pushstring (L, name);
  else
    lua_pushnil (L);
}

static void
set_slider_accessible_name (lua_State *L,
                            GObject   *object,
                            int        idx)
{
  ClutterActor *slider = CLUTTER_ACTOR (shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (object)));

  if (lua_type (L, idx) != LUA_TSTRING)
    luaL_error (L, "accessible_name must be a string");
  clutter_actor_set_accessible_name (slider, lua_tostring (L, idx));
}

static void
get_slider_sensitive (lua_State *L,
                      GObject   *object)
{
  lua_pushboolean (L, shell_quick_slider_get_sensitive (SHELL_QUICK_SLIDER (object)));
}

static void
set_slider_sensitive (lua_State *L,
                      GObject   *object,
                      int        idx)
{
  if (lua_type (L, idx) != LUA_TBOOLEAN)
    luaL_error (L, "sensitive must be a boolean");
  shell_quick_slider_set_sensitive (SHELL_QUICK_SLIDER (object), lua_toboolean (L, idx));
}

static void
get_indicator_accessible_name (lua_State *L,
                               GObject   *object)
{
  const char *name = clutter_actor_get_accessible_name (CLUTTER_ACTOR (object));

  if (name != NULL)
    lua_pushstring (L, name);
  else
    lua_pushnil (L);
}

static void
set_indicator_accessible_name (lua_State *L,
                               GObject   *object,
                               int        idx)
{
  if (lua_type (L, idx) != LUA_TSTRING)
    luaL_error (L, "accessible_name must be a string");
  clutter_actor_set_accessible_name (CLUTTER_ACTOR (object), lua_tostring (L, idx));
}

/* ------------------------------------------------------------------------ */
/* Handle types                                                              */
/* ------------------------------------------------------------------------ */

static const ShellLuaHandleMethod item_methods_ex[] = {
  { "destroy", shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty item_properties[] = {
  { "visible", "visible", TRUE, TRUE },
  { "sensitive", "reactive", TRUE, TRUE },
  { NULL }
};

static const ShellLuaHandleVirtual item_virtuals[] = {
  { "colspan", get_colspan, set_colspan },
  { "index", get_index, NULL },
  { NULL }
};

static ShellLuaHandleType item_type = {
  .name = "shell.QuickItem",
  .methods_ex = item_methods_ex,
  .properties = item_properties,
  .virtuals = item_virtuals,
};

static const luaL_Reg toggle_methods[] = {
  { "on_toggle", l_item_on_toggle },
  { "on_click", l_item_on_click },
  { NULL, NULL }
};

static const ShellLuaHandleProperty toggle_properties[] = {
  { "title", "title", TRUE, TRUE },
  { "icon_name", "icon-name", TRUE, TRUE },
  { "checked", "checked", TRUE, TRUE },
  { NULL }
};

static const ShellLuaHandleVirtual toggle_virtuals[] = {
  { "subtitle", get_subtitle, set_subtitle },
  { "icon_file", get_icon_file, set_icon_file },
  { NULL }
};

static ShellLuaHandleType toggle_type = {
  .name = "shell.QuickToggle",
  .parent = "shell.QuickItem",
  .methods = toggle_methods,
  .properties = toggle_properties,
  .virtuals = toggle_virtuals,
};

static const ShellLuaHandleProperty menu_toggle_properties[] = {
  { "menu_enabled", "menu-enabled", TRUE, TRUE },
  { "menu_button_accessible_name", "menu-button-accessible-name", TRUE, TRUE },
  { NULL }
};

static const ShellLuaHandleVirtual menu_toggle_virtuals[] = {
  { "menu", get_menu, NULL },
  { NULL }
};

static ShellLuaHandleType menu_toggle_type = {
  .name = "shell.QuickMenuToggle",
  .parent = "shell.QuickToggle",
  .properties = menu_toggle_properties,
  .virtuals = menu_toggle_virtuals,
};

static const luaL_Reg slider_methods[] = {
  { "on_changed", l_slider_on_changed },
  { "on_icon_click", l_slider_on_icon_click },
  { NULL, NULL }
};

static const ShellLuaHandleProperty slider_properties[] = {
  { "icon_name", "icon-name", TRUE, TRUE },
  { "icon_reactive", "icon-reactive", TRUE, TRUE },
  { "icon_accessible_name", "icon-label", TRUE, TRUE },
  { "menu_enabled", "menu-enabled", TRUE, TRUE },
  { "menu_button_accessible_name", "menu-button-accessible-name", TRUE, TRUE },
  { NULL }
};

static const ShellLuaHandleVirtual slider_virtuals[] = {
  { "value", get_slider_value, set_slider_value },
  { "menu", get_menu, NULL },
  { "icon_file", get_icon_file, set_icon_file },
  { "accessible_name", get_slider_accessible_name, set_slider_accessible_name },
  { "sensitive", get_slider_sensitive, set_slider_sensitive },
  { NULL }
};

static ShellLuaHandleType slider_type = {
  .name = "shell.QuickSlider",
  .parent = "shell.QuickItem",
  .methods = slider_methods,
  .properties = slider_properties,
  .virtuals = slider_virtuals,
};

static const ShellLuaHandleProperty indicator_properties[] = {
  { "icon_name", "icon-name", TRUE, TRUE },
  { "visible", "visible", TRUE, TRUE },
  { NULL }
};

static const ShellLuaHandleVirtual indicator_virtuals[] = {
  { "icon_file", get_icon_file, set_icon_file },
  { "accessible_name", get_indicator_accessible_name, set_indicator_accessible_name },
  { "index", get_index, NULL },
  { NULL }
};

static ShellLuaHandleType indicator_type = {
  .name = "shell.QuickIndicator",
  .methods_ex = item_methods_ex,
  .properties = indicator_properties,
  .virtuals = indicator_virtuals,
};

/* menu:set_header{ icon_name|icon_file, title, subtitle } */
static const char * const header_fields[] = { "icon_name", "icon_file", "title", "subtitle", NULL };

static int
l_menu_set_header (lua_State *L)
{
  ShellQuickToggleMenu *menu = SHELL_QUICK_TOGGLE_MENU (shell_lua_handle_check (L, 1, "shell.QuickMenu"));
  const char *icon_name, *icon_file, *title, *subtitle;

  shell_lua_check_fields (L, 2, header_fields);
  check_one_icon (L, 2);
  icon_name = shell_lua_opt_string (L, 2, "icon_name", NULL);
  icon_file = shell_lua_opt_string (L, 2, "icon_file", NULL);
  title = shell_lua_opt_string (L, 2, "title", NULL);
  subtitle = shell_lua_opt_string (L, 2, "subtitle", NULL);
  if (title == NULL)
    return luaL_error (L, "missing required field 'title' (a string)");

  if (icon_file != NULL)
    {
      GIcon *icon = icon_from_file (L, "icon_file", icon_file);

      shell_quick_toggle_menu_set_header_gicon (menu, icon, title, subtitle);
      g_object_unref (icon);
    }
  else
    {
      shell_quick_toggle_menu_set_header (menu, icon_name, title, subtitle);
    }

  return 0;
}

static const luaL_Reg quick_menu_methods[] = {
  { "set_header", l_menu_set_header },
  { NULL, NULL }
};

static ShellLuaHandleType quick_menu_type = {
  .name = "shell.QuickMenu",
  .parent = "shell.Menu",
  .methods = quick_menu_methods,
};

/* ------------------------------------------------------------------------ */
/* Constructors                                                              */
/* ------------------------------------------------------------------------ */

static void
apply_common (ClutterActor      *item,
              const ItemOptions *opts)
{
  set_actor_visible (item, opts->visible);
  if (!opts->sensitive)
    {
      if (SHELL_IS_QUICK_SLIDER (item))
        shell_quick_slider_set_sensitive (SHELL_QUICK_SLIDER (item), FALSE);
      else
        clutter_actor_set_reactive (item, FALSE);
    }
}

/* @icon: the icon of the file given by `icon_file`, resolved before anything
 * was created (it raises for a bad file) */
static void
apply_icon (GObject           *item,
            GIcon             *icon,
            const ItemOptions *opts)
{
  if (opts->icon_name != NULL)
    g_object_set (item, "icon-name", opts->icon_name, NULL);
  else if (icon != NULL)
    g_object_set (item, "gicon", icon, NULL);
}

static GIcon *
resolve_icon (lua_State         *L,
              const ItemOptions *opts)
{
  return opts->icon_file != NULL ? icon_from_file (L, "icon_file", opts->icon_file) : NULL;
}

static int
add_toggle_common (lua_State *L,
                   gboolean   with_menu)
{
  ItemOptions opts = { 0, };
  GObject *item;
  GIcon *icon;
  ClutterActor *actor;
  ShellLuaCallback *cb;
  int toggle_ref = LUA_NOREF;
  int click_ref = LUA_NOREF;

  shell_lua_check_fields (L, 1, with_menu ? menu_toggle_fields : toggle_fields);

  opts.title = shell_lua_opt_string (L, 1, "title", NULL);
  if (opts.title == NULL)
    return luaL_error (L, "missing required field 'title' (a string)");
  opts.subtitle = shell_lua_opt_string (L, 1, "subtitle", NULL);
  opts.icon_name = shell_lua_opt_string (L, 1, "icon_name", NULL);
  opts.icon_file = shell_lua_opt_string (L, 1, "icon_file", NULL);
  check_one_icon (L, 1);
  opts.checked = shell_lua_opt_boolean (L, 1, "checked", FALSE);
  parse_common_options (L, 1, 1, &opts);
  if (with_menu)
    parse_menu_options (L, 1, &opts);

  check_callable_field (L, 1, "on_toggle");
  check_callable_field (L, 1, "on_click");
  if (field_present (L, 1, "on_toggle") && field_present (L, 1, "on_click"))
    return luaL_error (L, "fields 'on_toggle' and 'on_click' are mutually exclusive");

  icon = resolve_icon (L, &opts);
  shell_lua_check_owned_capacity (L);

  item = g_object_new (with_menu ? SHELL_TYPE_QUICK_MENU_TOGGLE : SHELL_TYPE_QUICK_TOGGLE,
                       "title", opts.title,
                       NULL);
  actor = CLUTTER_ACTOR (item);
  shell_lua_own_object (L, item, &actor_ops);

  if (opts.subtitle != NULL && opts.subtitle[0] != '\0')
    g_object_set (item, "subtitle", opts.subtitle, NULL);
  apply_icon (item, icon, &opts);
  g_clear_object (&icon);
  if (with_menu)
    {
      if (opts.has_menu_enabled)
        g_object_set (item, "menu-enabled", opts.menu_enabled, NULL);
      if (opts.menu_button_accessible_name != NULL)
        g_object_set (item, "menu-button-accessible-name", opts.menu_button_accessible_name, NULL);
    }

  toggle_ref = shell_lua_opt_callback (L, 1, "on_toggle", FALSE);
  click_ref = shell_lua_opt_callback (L, 1, "on_click", FALSE);
  if (toggle_ref != LUA_NOREF)
    connect_toggle (L, item, toggle_ref, &cb);
  else if (click_ref != LUA_NOREF)
    connect_click (L, item, click_ref, &cb);

  /* Before the initial state: no callback is called for it (and none is
   * connected for a toggle without handlers) */
  g_object_set (item, "checked", opts.checked, NULL);

  apply_common (actor, &opts);
  place_item (L, actor, (int) opts.colspan, opts.index);

  shell_lua_handle_push (L, with_menu ? "shell.QuickMenuToggle" : "shell.QuickToggle", item);
  return 1;
}

static int
l_add_toggle (lua_State *L)
{
  return add_toggle_common (L, FALSE);
}

static int
l_add_menu_toggle (lua_State *L)
{
  return add_toggle_common (L, TRUE);
}

static int
l_add_slider (lua_State *L)
{
  ItemOptions opts = { 0, };
  GObject *item;
  GIcon *icon;
  ShellLuaCallback *cb;
  int changed_ref, icon_click_ref;
  gboolean icon_click_present;

  shell_lua_check_fields (L, 1, slider_fields);

  opts.accessible_name = shell_lua_opt_string (L, 1, "accessible_name", NULL);
  if (opts.accessible_name == NULL)
    return luaL_error (L, "missing required field 'accessible_name' (a string)");
  opts.icon_name = shell_lua_opt_string (L, 1, "icon_name", NULL);
  opts.icon_file = shell_lua_opt_string (L, 1, "icon_file", NULL);
  check_one_icon (L, 1);
  opts.icon_accessible_name = shell_lua_opt_string (L, 1, "icon_accessible_name", NULL);
  opts.icon_reactive = shell_lua_opt_boolean (L, 1, "icon_reactive", FALSE);
  opts.value = shell_lua_opt_number (L, 1, "value", 0.0);
  if (!(opts.value >= 0.0 && opts.value <= 1.0))
    return luaL_error (L, "field 'value' must be between 0.0 and 1.0");
  parse_common_options (L, 1, 2, &opts);
  parse_menu_options (L, 1, &opts);

  check_callable_field (L, 1, "on_changed");
  check_callable_field (L, 1, "on_icon_click");
  icon_click_present = field_present (L, 1, "on_icon_click");
  if (icon_click_present && !opts.icon_reactive)
    return luaL_error (L, "field 'on_icon_click' requires 'icon_reactive' = true");

  icon = resolve_icon (L, &opts);
  shell_lua_check_owned_capacity (L);

  item = g_object_new (SHELL_TYPE_QUICK_SLIDER,
                       "icon-reactive", opts.icon_reactive,
                       NULL);
  shell_lua_own_object (L, item, &actor_ops);

  apply_icon (item, icon, &opts);
  g_clear_object (&icon);
  if (opts.icon_accessible_name != NULL)
    g_object_set (item, "icon-label", opts.icon_accessible_name, NULL);
  if (opts.has_menu_enabled)
    g_object_set (item, "menu-enabled", opts.menu_enabled, NULL);
  if (opts.menu_button_accessible_name != NULL)
    g_object_set (item, "menu-button-accessible-name", opts.menu_button_accessible_name, NULL);
  clutter_actor_set_accessible_name (CLUTTER_ACTOR (shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (item))),
                                     opts.accessible_name);
  shell_bar_level_set_value (SHELL_BAR_LEVEL (shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (item))),
                             opts.value);

  changed_ref = shell_lua_opt_callback (L, 1, "on_changed", FALSE);
  icon_click_ref = shell_lua_opt_callback (L, 1, "on_icon_click", FALSE);
  if (changed_ref != LUA_NOREF)
    {
      cb = shell_lua_callback_new (L, item, changed_ref);
      shell_lua_callback_connect (cb, shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (item)),
                                  "notify::value", G_CALLBACK (slider_value_notify_cb));
    }
  if (icon_click_ref != LUA_NOREF)
    {
      cb = shell_lua_callback_new (L, item, icon_click_ref);
      shell_lua_callback_connect (cb, item, "icon-clicked", G_CALLBACK (slider_icon_clicked_cb));
    }

  apply_common (CLUTTER_ACTOR (item), &opts);
  place_item (L, CLUTTER_ACTOR (item), (int) opts.colspan, opts.index);

  shell_lua_handle_push (L, "shell.QuickSlider", item);
  return 1;
}

static int
l_add_indicator (lua_State *L)
{
  ItemOptions opts = { 0, };
  ShellPanel *panel = get_panel ();
  GError *error = NULL;
  GObject *icon;
  GIcon *gicon;

  shell_lua_check_fields (L, 1, indicator_fields);

  opts.icon_name = shell_lua_opt_string (L, 1, "icon_name", NULL);
  opts.icon_file = shell_lua_opt_string (L, 1, "icon_file", NULL);
  if (opts.icon_name == NULL && opts.icon_file == NULL)
    return luaL_error (L, "missing required field 'icon_name' or 'icon_file' (a string)");
  check_one_icon (L, 1);
  opts.accessible_name = shell_lua_opt_string (L, 1, "accessible_name", NULL);
  opts.visible = shell_lua_opt_boolean (L, 1, "visible", TRUE);
  opts.index = opt_index (L, 1);

  if (panel == NULL)
    return luaL_error (L, "the top bar does not exist");

  gicon = resolve_icon (L, &opts);
  shell_lua_check_owned_capacity (L);

  icon = g_object_new (SHELL_TYPE_LUA_QUICK_INDICATOR, NULL);
  shell_lua_own_object (L, icon, &actor_ops);

  apply_icon (icon, gicon, &opts);
  g_clear_object (&gicon);
  if (opts.accessible_name != NULL)
    clutter_actor_set_accessible_name (CLUTTER_ACTOR (icon), opts.accessible_name);
  set_actor_visible (CLUTTER_ACTOR (icon), opts.visible);

  if (!shell_panel_items_add_extension_indicator (panel, CLUTTER_ACTOR (icon),
                                                  get_owner (L), opts.index, &error))
    {
      lua_pushstring (L, error->message);
      g_clear_error (&error);
      shell_lua_destroy_object (L, icon);
      return lua_error (L);
    }

  shell_lua_handle_push (L, "shell.QuickIndicator", icon);
  return 1;
}

static const luaL_Reg quick_settings_funcs[] = {
  { "add_toggle", l_add_toggle },
  { "add_menu_toggle", l_add_menu_toggle },
  { "add_slider", l_add_slider },
  { "add_indicator", l_add_indicator },
  { NULL, NULL }
};

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

static void
shell_lua_api_quick_settings_register (ShellLuaState *state,
                                       lua_State     *L)
{
  int shell = lua_absindex (L, -1);

  luaL_checkstack (L, 6, "shell.quick_settings");

  /* The class declares the hook quick-settings.opened: an extension can
   * connect to it even where the top bar has no quick settings (yet) */
  g_type_ensure (SHELL_TYPE_QUICK_SETTINGS_MENU);

  item_type.gtype = SHELL_TYPE_QUICK_SETTINGS_ITEM;
  toggle_type.gtype = SHELL_TYPE_QUICK_TOGGLE;
  menu_toggle_type.gtype = SHELL_TYPE_QUICK_MENU_TOGGLE;
  slider_type.gtype = SHELL_TYPE_QUICK_SLIDER;
  indicator_type.gtype = SHELL_TYPE_LUA_QUICK_INDICATOR;
  quick_menu_type.gtype = SHELL_TYPE_QUICK_TOGGLE_MENU;

  shell_lua_api_menu_register_types (L);   /* the parent of shell.QuickMenu */

  shell_lua_handle_register_type (L, &item_type);
  shell_lua_handle_register_type (L, &toggle_type);
  shell_lua_handle_register_type (L, &menu_toggle_type);
  shell_lua_handle_register_type (L, &slider_type);
  shell_lua_handle_register_type (L, &indicator_type);
  shell_lua_handle_register_type (L, &quick_menu_type);

  lua_newtable (L);
  luaL_setfuncs (L, quick_settings_funcs, 0);
  lua_setfield (L, shell, "quick_settings");
}
