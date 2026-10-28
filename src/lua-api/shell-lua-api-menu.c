/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-lua-api-menu.c: `shell.Menu` and its items (docs/api/menu.md) over
 * ShellPopupMenuBase and the items of ui/popup-menu.[ch].
 *
 * Items are created by the add_* methods of a menu; each is a ClutterActor
 * owned by the extension context (so `item:destroy ()` frees its slot and the
 * menu destroys its items). Menus are owned by whoever created them (the
 * indicator, the quick settings item, the submenu item): the context only
 * keeps a reference so that callbacks can be bound to them and their handles
 * die with them (shell_lua_api_menu_push ()).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <clutter/clutter.h>
#include <limits.h>
#include <string.h>

#include "lua-api/shell-lua-api-menu.h"
#include "lua-api/shell-lua-api-ui.h"
#include "ui/box-pointer.h"

#define ICON_FILE_QUARK "shell-lua-menu-icon-file"
#define STATUS_QUARK "shell-lua-menu-status"

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static void
menu_no_destroy (GObject *object)
{
  /* Menus are destroyed by their owner, never by the context. */
}

static const ShellLuaOwnOps menu_ops = { menu_no_destroy, "destroy" };

void
shell_lua_api_menu_push (lua_State          *L,
                         const char         *type_name,
                         ShellPopupMenuBase *menu)
{
  GObject *object = G_OBJECT (menu);

  if (type_name == NULL)
    type_name = "shell.Menu";

  if (!shell_lua_object_is_owned (L, object))
    {
      shell_lua_check_owned_capacity (L);
      shell_lua_own_object (L, object, &menu_ops);
    }
  shell_lua_handle_push (L, type_name, object);
}

static G_GNUC_NORETURN void
raise_and_free (lua_State *L,
                char      *message)
{
  lua_pushstring (L, message);
  g_free (message);
  lua_error (L);
  g_assert_not_reached ();
}

static const char *
check_string_arg (lua_State  *L,
                  int         idx,
                  const char *what)
{
  if (lua_type (L, idx) != LUA_TSTRING)
    luaL_error (L, "%s must be a string, got %s", what, luaL_typename (L, idx));
  return lua_tostring (L, idx);
}

static ShellPopupMenuBase *
check_menu (lua_State *L,
            int        idx)
{
  return SHELL_POPUP_MENU_BASE (shell_lua_handle_check (L, idx, "shell.Menu"));
}

static ShellPopupBaseMenuItem *
check_item (lua_State *L,
            int        idx)
{
  return SHELL_POPUP_BASE_MENU_ITEM (shell_lua_handle_check (L, idx, "shell.MenuItem"));
}

typedef enum
{
  KIND_ITEM,
  KIND_IMAGE,
  KIND_SWITCH,
  KIND_SUBMENU,
  KIND_SEPARATOR,
  KIND_WIDGET,
} ItemKind;

static ItemKind
item_kind (GObject *object)
{
  if (SHELL_IS_POPUP_IMAGE_MENU_ITEM (object))
    return KIND_IMAGE;
  if (SHELL_IS_POPUP_SWITCH_MENU_ITEM (object))
    return KIND_SWITCH;
  if (SHELL_IS_POPUP_SUB_MENU_MENU_ITEM (object))
    return KIND_SUBMENU;
  if (SHELL_IS_POPUP_SEPARATOR_MENU_ITEM (object))
    return KIND_SEPARATOR;
  if (SHELL_IS_POPUP_MENU_ITEM (object))
    return KIND_ITEM;
  return KIND_WIDGET;
}

static const char *
item_kind_name (ItemKind kind)
{
  static const char * const names[] = { "item", "image", "switch", "submenu", "separator", "widget" };

  return names[kind];
}

static const char *
item_type_name (ItemKind kind)
{
  static const char * const names[] = {
    "shell.MenuActionItem", "shell.MenuActionItem", "shell.MenuSwitchItem",
    "shell.MenuSubmenuItem", "shell.MenuSeparator", "shell.MenuWidgetItem"
  };

  return names[kind];
}

/* ------------------------------------------------------------------------ */
/* shell.Menu                                                                */
/* ------------------------------------------------------------------------ */

static void
menu_get_n_items (lua_State *L,
                  GObject   *object)
{
  lua_pushinteger (L, shell_popup_menu_base_get_n_menu_items (SHELL_POPUP_MENU_BASE (object)));
}

static const char * const animate_fields[] = { "animate", NULL };

static ShellPopupAnimation
check_animate (lua_State *L,
               int        idx)
{
  if (lua_isnoneornil (L, idx))
    return SHELL_POPUP_ANIMATION_FULL;
  if (!lua_istable (L, idx))
    luaL_error (L, "options table expected, got %s", luaL_typename (L, idx));
  shell_lua_check_fields (L, idx, animate_fields);
  return shell_lua_opt_boolean (L, idx, "animate", TRUE) ? SHELL_POPUP_ANIMATION_FULL
                                                         : SHELL_POPUP_ANIMATION_NONE;
}

static int
l_menu_open (lua_State *L)
{
  ShellPopupMenuBase *menu = check_menu (L, 1);
  ShellPopupAnimation animate = check_animate (L, 2);

  lua_pushboolean (L, shell_popup_menu_base_open (menu, animate));
  return 1;
}

static int
l_menu_close (lua_State *L)
{
  ShellPopupMenuBase *menu = check_menu (L, 1);
  ShellPopupAnimation animate = check_animate (L, 2);

  lua_pushboolean (L, shell_popup_menu_base_close (menu, animate));
  return 1;
}

static int
l_menu_toggle (lua_State *L)
{
  shell_popup_menu_base_toggle (check_menu (L, 1));
  return 0;
}

static void
clear_silenced (lua_State *L,
                gpointer   data)
{
  ShellPopupMenuBase *menu = data;
  GList *items = shell_popup_menu_base_get_menu_items (menu);
  GList *l;

  for (l = items; l != NULL; l = l->next)
    g_object_ref (l->data);

  /* Items the extension owns go through the registry (slots, handles);
   * anything else is removed by the menu itself. */
  for (l = items; l != NULL; l = l->next)
    {
      if (!shell_lua_destroy_object (L, l->data) && CLUTTER_IS_ACTOR (l->data))
        clutter_actor_destroy (CLUTTER_ACTOR (l->data));
      g_object_unref (l->data);
    }
  g_list_free (items);
}

static int
l_menu_clear (lua_State *L)
{
  ShellPopupMenuBase *menu = check_menu (L, 1);

  /* removeAll () does not call back for items the extension destroys. */
  shell_lua_call_silenced (L, clear_silenced, menu);
  return 0;
}

static int
l_menu_items (lua_State *L)
{
  ShellPopupMenuBase *menu = check_menu (L, 1);
  GList *items = shell_popup_menu_base_get_menu_items (menu);
  GList *l;
  int i = 1;

  lua_newtable (L);
  for (l = items; l != NULL; l = l->next)
    {
      if (!shell_lua_object_is_owned (L, l->data))
        continue;
      shell_lua_handle_push (L, item_type_name (item_kind (l->data)), l->data);
      lua_rawseti (L, -2, i++);
    }
  g_list_free (items);
  return 1;
}

static void
push_open_arg (lua_State *L,
               gpointer   data)
{
  lua_pushboolean (L, GPOINTER_TO_INT (data));
}

static void
on_open_state_changed_cb (ShellPopupMenuBase *menu,
                          gboolean            open,
                          ShellLuaCallback   *cb)
{
  shell_lua_callback_emit (cb, 1, push_open_arg, GINT_TO_POINTER (open));
}

static int
l_menu_on_open_state_changed (lua_State *L)
{
  ShellPopupMenuBase *menu = check_menu (L, 1);
  ShellLuaCallback *cb;
  gulong id;

  luaL_checktype (L, 2, LUA_TFUNCTION);
  lua_pushvalue (L, 2);
  cb = shell_lua_callback_new (L, G_OBJECT (menu), luaL_ref (L, LUA_REGISTRYINDEX));
  id = shell_lua_callback_connect (cb, menu, "open-state-changed",
                                   G_CALLBACK (on_open_state_changed_cb));
  return shell_lua_callback_push_connection (L, cb, menu, id);
}

/* ------------------------------------------------------------------------ */
/* Item construction                                                         */
/* ------------------------------------------------------------------------ */

typedef struct
{
  const char         *method;
  ItemKind            kind;
  const char * const *fields;
} AddSpec;

static const char * const item_fields[] = {
  "label", "on_activate", "sensitive", "visible", "ornament", "index", NULL
};
static const char * const image_fields[] = {
  "label", "icon_name", "icon_file", "on_activate", "sensitive", "visible", "ornament", "index", NULL
};
static const char * const switch_fields[] = {
  "label", "state", "status", "on_toggled", "on_activate", "sensitive", "visible", "index", NULL
};
static const char * const submenu_fields[] = {
  "label", "icon_name", "icon_file", "sensitive", "visible", "index", NULL
};
static const char * const separator_fields[] = { "label", "visible", "index", NULL };
static const char * const widget_fields[] = { "child", "visible", "index", NULL };

static const AddSpec spec_item = { "add_item", KIND_ITEM, item_fields };
static const AddSpec spec_image = { "add_image_item", KIND_IMAGE, image_fields };
static const AddSpec spec_switch = { "add_switch", KIND_SWITCH, switch_fields };
static const AddSpec spec_submenu = { "add_submenu", KIND_SUBMENU, submenu_fields };
static const AddSpec spec_separator = { "add_separator", KIND_SEPARATOR, separator_fields };
static const AddSpec spec_widget = { "add_widget", KIND_WIDGET, widget_fields };

static void
on_item_activate (ShellPopupBaseMenuItem *item,
                  const ClutterEvent     *event,
                  ShellLuaCallback       *cb)
{
  shell_lua_callback_emit (cb, 0, NULL, NULL);
}

static void
on_item_toggled (ShellPopupSwitchMenuItem *item,
                 gboolean                  state,
                 ShellLuaCallback         *cb)
{
  shell_lua_callback_emit (cb, 1, push_open_arg, GINT_TO_POINTER (state));
}

static int
item_event_method (lua_State  *L,
                   const char *signal,
                   GCallback   handler)
{
  GObject *item = G_OBJECT (check_item (L, 1));
  ShellLuaCallback *cb;
  gulong id;

  luaL_checktype (L, 2, LUA_TFUNCTION);
  lua_pushvalue (L, 2);
  cb = shell_lua_callback_new (L, item, luaL_ref (L, LUA_REGISTRYINDEX));
  id = shell_lua_callback_connect (cb, item, signal, handler);
  return shell_lua_callback_push_connection (L, cb, item, id);
}

static int
l_item_on_activate (lua_State *L)
{
  return item_event_method (L, "activate", G_CALLBACK (on_item_activate));
}

static int
l_item_on_toggled (lua_State *L)
{
  return item_event_method (L, "toggled", G_CALLBACK (on_item_toggled));
}

/* Creates the (floating) item of @spec; may read construct-only values from
 * the options table at @opts. Nothing here raises. */
static ClutterActor *
new_item (const AddSpec *spec,
          gboolean       has_icon)
{
  switch (spec->kind)
    {
    case KIND_ITEM:
      return g_object_new (SHELL_TYPE_POPUP_MENU_ITEM, NULL);
    case KIND_IMAGE:
      return g_object_new (SHELL_TYPE_POPUP_IMAGE_MENU_ITEM, NULL);
    case KIND_SWITCH:
      return g_object_new (SHELL_TYPE_POPUP_SWITCH_MENU_ITEM, NULL);
    case KIND_SUBMENU:
      return g_object_new (SHELL_TYPE_POPUP_SUB_MENU_MENU_ITEM, "want-icon", has_icon, NULL);
    case KIND_SEPARATOR:
      return g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL);
    case KIND_WIDGET:
    default:
      return g_object_new (SHELL_TYPE_POPUP_BASE_MENU_ITEM,
                           "reactive", FALSE,
                           "activatable", FALSE,
                           "active-on-hover", FALSE,
                           NULL);
    }
}

/* Arguments: handle, options. Assigns the fields through the handle (so they
 * are validated like later assignments) and connects the on_<event> fields. */
static int
apply_item_fields (lua_State *L)
{
  static const struct { const char *field; const char *signal; GCallback handler; } events[] = {
    { "on_activate", "activate", G_CALLBACK (on_item_activate) },
    { "on_toggled", "toggled", G_CALLBACK (on_item_toggled) },
  };
  guint i;

  luaL_checkstack (L, 8, "menu item fields");

  lua_pushnil (L);
  while (lua_next (L, 2) != 0)
    {
      const char *key = lua_tostring (L, -2);

      if (strcmp (key, "index") != 0 && strcmp (key, "child") != 0 &&
          strncmp (key, "on_", 3) != 0)
        {
          lua_pushvalue (L, 1);
          lua_pushvalue (L, -3);
          lua_pushvalue (L, -3);
          lua_settable (L, -3);
          lua_pop (L, 1);   /* the handle */
        }
      lua_pop (L, 1);
    }

  for (i = 0; i < G_N_ELEMENTS (events); i++)
    {
      int ref = shell_lua_opt_callback (L, 2, events[i].field, FALSE);

      if (ref != LUA_NOREF)
        {
          GObject *item = shell_lua_handle_check (L, 1, "shell.MenuItem");
          ShellLuaCallback *cb = shell_lua_callback_new (L, item, ref);

          shell_lua_callback_connect (cb, item, events[i].signal, events[i].handler);
        }
    }
  return 0;
}

static int
add_item_common (lua_State     *L,
                 const AddSpec *spec)
{
  ShellPopupMenuBase *menu = check_menu (L, 1);
  lua_Integer index = 0;
  gboolean has_icon = FALSE;
  ClutterActor *child = NULL;
  ClutterActor *actor;
  GObject *object;
  int status;

  if (!lua_istable (L, 2))
    return luaL_error (L, "%s expects an options table, got %s", spec->method, luaL_typename (L, 2));
  lua_settop (L, 2);

  shell_lua_check_fields (L, 2, spec->fields);
  shell_lua_check_owned_capacity (L);

  if (spec->kind == KIND_SUBMENU && SHELL_IS_POPUP_SUB_MENU (menu))
    return luaL_error (L, "a submenu's menu cannot contain a submenu");

  if (spec->kind != KIND_WIDGET && spec->kind != KIND_SEPARATOR &&
      shell_lua_opt_string (L, 2, "label", NULL) == NULL)
    return luaL_error (L, "missing required field 'label' (a string)");

  if (spec->kind == KIND_IMAGE || spec->kind == KIND_SUBMENU)
    {
      gboolean name = lua_getfield (L, 2, "icon_name") != LUA_TNIL;
      gboolean file;

      lua_pop (L, 1);
      file = lua_getfield (L, 2, "icon_file") != LUA_TNIL;
      lua_pop (L, 1);
      if (name && file)
        return luaL_error (L, "fields 'icon_name' and 'icon_file' are mutually exclusive");
      if (spec->kind == KIND_IMAGE && !name && !file)
        return luaL_error (L, "missing required field 'icon_name' or 'icon_file'");
      has_icon = name || file;
    }

  if (spec->kind == KIND_WIDGET)
    {
      if (lua_getfield (L, 2, "child") == LUA_TNIL)
        return luaL_error (L, "missing required field 'child' (a widget)");
      child = shell_lua_api_ui_check_free_widget (L, -1);
      lua_pop (L, 1);
    }

  index = shell_lua_opt_integer (L, 2, "index", 0);
  if (lua_getfield (L, 2, "index") != LUA_TNIL && index < 1)
    return luaL_error (L, "field 'index' must be at least 1");
  lua_pop (L, 1);

  actor = new_item (spec, has_icon);
  object = G_OBJECT (actor);
  shell_lua_own_object (L, object, shell_lua_api_ui_actor_ops ());
  shell_lua_api_ui_decorate (L, actor);

  if (index > 0 && index <= INT_MAX)
    shell_popup_menu_base_insert_menu_item (menu, object, (int) index - 1);
  else
    shell_popup_menu_base_add_menu_item (menu, object);

  if (child != NULL)
    clutter_actor_add_child (actor, child);

  shell_lua_handle_push (L, item_type_name (spec->kind), object);   /* 3: the handle */
  lua_pushcfunction (L, apply_item_fields);
  lua_pushvalue (L, 3);
  lua_pushvalue (L, 2);
  status = lua_pcall (L, 2, 0, 0);
  if (status != LUA_OK)
    {
      shell_lua_destroy_object (L, object);
      return lua_error (L);
    }
  return 1;
}

#define ADD_METHOD(fn, spec) \
  static int fn (lua_State *L) { return add_item_common (L, &spec); }

ADD_METHOD (l_menu_add_item, spec_item)
ADD_METHOD (l_menu_add_image_item, spec_image)
ADD_METHOD (l_menu_add_switch, spec_switch)
ADD_METHOD (l_menu_add_submenu, spec_submenu)
ADD_METHOD (l_menu_add_separator, spec_separator)
ADD_METHOD (l_menu_add_widget, spec_widget)

static const luaL_Reg menu_methods[] = {
  { "add_item", l_menu_add_item },
  { "add_image_item", l_menu_add_image_item },
  { "add_switch", l_menu_add_switch },
  { "add_submenu", l_menu_add_submenu },
  { "add_separator", l_menu_add_separator },
  { "add_widget", l_menu_add_widget },
  { "clear", l_menu_clear },
  { "open", l_menu_open },
  { "close", l_menu_close },
  { "toggle", l_menu_toggle },
  { "items", l_menu_items },
  { "on_open_state_changed", l_menu_on_open_state_changed },
  { NULL, NULL }
};

static const ShellLuaHandleProperty menu_properties[] = {
  { "is_open", "is-open", TRUE, FALSE },
  { "sensitive", "sensitive", TRUE, TRUE },
  { NULL }
};

static const ShellLuaHandleVirtual menu_virtuals[] = {
  { "n_items", menu_get_n_items, NULL },
  { NULL }
};

/* ------------------------------------------------------------------------ */
/* Items                                                                     */
/* ------------------------------------------------------------------------ */

static void
item_get_kind (lua_State *L,
               GObject   *object)
{
  lua_pushstring (L, item_kind_name (item_kind (object)));
}

static void
item_get_index (lua_State *L,
                GObject   *object)
{
  ShellPopupMenuBase *parent = shell_popup_base_menu_item_get_parent_menu (SHELL_POPUP_BASE_MENU_ITEM (object));
  GList *items, *l;
  int i = 1;

  if (parent == NULL)
    {
      lua_pushnil (L);
      return;
    }

  items = shell_popup_menu_base_get_menu_items (parent);
  for (l = items; l != NULL; l = l->next, i++)
    if (l->data == object)
      break;
  if (l != NULL)
    lua_pushinteger (L, i);
  else
    lua_pushnil (L);
  g_list_free (items);
}

static const ShellLuaHandleMethod item_methods_ex[] = {
  { "destroy", shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty item_properties[] = {
  { "visible", "visible", TRUE, TRUE },
  { NULL }
};

static const ShellLuaHandleVirtual item_virtuals[] = {
  { "kind", item_get_kind, NULL },
  { "index", item_get_index, NULL },
  { NULL }
};

/* label, sensitive, ornament, icons: the items with a row of text */

static const char *
item_text (GObject *object)
{
  switch (item_kind (object))
    {
    case KIND_ITEM:
      return shell_popup_menu_item_get_text (SHELL_POPUP_MENU_ITEM (object));
    case KIND_IMAGE:
      return shell_popup_image_menu_item_get_text (SHELL_POPUP_IMAGE_MENU_ITEM (object));
    case KIND_SWITCH:
      return shell_popup_switch_menu_item_get_text (SHELL_POPUP_SWITCH_MENU_ITEM (object));
    case KIND_SUBMENU:
      return shell_popup_sub_menu_menu_item_get_text (SHELL_POPUP_SUB_MENU_MENU_ITEM (object));
    case KIND_SEPARATOR:
      return shell_popup_separator_menu_item_get_text (SHELL_POPUP_SEPARATOR_MENU_ITEM (object));
    case KIND_WIDGET:
    default:
      return NULL;
    }
}

static void
item_set_text (GObject    *object,
               const char *text)
{
  switch (item_kind (object))
    {
    case KIND_ITEM:
      shell_popup_menu_item_set_text (SHELL_POPUP_MENU_ITEM (object), text);
      break;
    case KIND_IMAGE:
      shell_popup_image_menu_item_set_text (SHELL_POPUP_IMAGE_MENU_ITEM (object), text);
      break;
    case KIND_SWITCH:
      shell_popup_switch_menu_item_set_text (SHELL_POPUP_SWITCH_MENU_ITEM (object), text);
      break;
    case KIND_SUBMENU:
      shell_popup_sub_menu_menu_item_set_text (SHELL_POPUP_SUB_MENU_MENU_ITEM (object), text);
      break;
    case KIND_SEPARATOR:
      shell_popup_separator_menu_item_set_text (SHELL_POPUP_SEPARATOR_MENU_ITEM (object), text);
      break;
    case KIND_WIDGET:
    default:
      break;
    }
}

static void
item_get_label (lua_State *L,
                GObject   *object)
{
  const char *text = item_text (object);

  lua_pushstring (L, text != NULL ? text : "");
}

static void
item_set_label (lua_State *L,
                GObject   *object,
                int        idx)
{
  item_set_text (object, check_string_arg (L, idx, "label"));
}

static void
item_get_sensitive (lua_State *L,
                    GObject   *object)
{
  lua_pushboolean (L, shell_popup_base_menu_item_get_sensitive (SHELL_POPUP_BASE_MENU_ITEM (object)));
}

static void
item_set_sensitive (lua_State *L,
                    GObject   *object,
                    int        idx)
{
  if (lua_type (L, idx) != LUA_TBOOLEAN)
    luaL_error (L, "sensitive must be a boolean, got %s", luaL_typename (L, idx));
  shell_popup_base_menu_item_set_sensitive (SHELL_POPUP_BASE_MENU_ITEM (object), lua_toboolean (L, idx));
}

/* The StIcon of an image or submenu item, NULL for the others and for a
 * submenu item created without one. */
static StIcon *
item_icon (GObject *object)
{
  switch (item_kind (object))
    {
    case KIND_IMAGE:
      return shell_popup_image_menu_item_get_icon (SHELL_POPUP_IMAGE_MENU_ITEM (object));
    case KIND_SUBMENU:
      return shell_popup_sub_menu_menu_item_get_icon (SHELL_POPUP_SUB_MENU_MENU_ITEM (object));
    default:
      return NULL;
    }
}

static void
item_get_icon_name (lua_State *L,
                    GObject   *object)
{
  StIcon *icon = item_icon (object);
  const char *name = icon != NULL ? st_icon_get_icon_name (icon) : NULL;

  if (name != NULL)
    lua_pushstring (L, name);
  else
    lua_pushnil (L);
}

static void
item_get_icon_file (lua_State *L,
                    GObject   *object)
{
  const char *file = item_icon (object) != NULL ? g_object_get_data (object, ICON_FILE_QUARK) : NULL;

  if (file != NULL)
    lua_pushstring (L, file);
  else
    lua_pushnil (L);
}

static StIcon *
check_icon_item (lua_State  *L,
                 GObject    *object,
                 const char *field)
{
  StIcon *icon = item_icon (object);

  if (icon == NULL)
    {
      if (item_kind (object) == KIND_SUBMENU)
        luaL_error (L, "this submenu item was created without an icon; '%s' cannot be assigned", field);
      luaL_error (L, "'%s' only exists on image and submenu items", field);
    }
  return icon;
}

static void
item_set_icon_name (lua_State *L,
                    GObject   *object,
                    int        idx)
{
  StIcon *icon = check_icon_item (L, object, "icon_name");
  const char *name = check_string_arg (L, idx, "icon_name");

  g_object_set_data (object, ICON_FILE_QUARK, NULL);
  st_icon_set_icon_name (icon, name);
}

static GIcon *
icon_for_file (lua_State  *L,
               const char *relative)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);
  g_autofree char *base_path = NULL;
  g_autofree char *full = NULL;
  g_autofree char *lower = NULL;
  g_auto (GStrv) parts = NULL;
  char *base_real, *real;
  char *error = NULL;
  GIcon *gicon = NULL;
  GFile *file;
  int i;

  if (extension == NULL)
    raise_and_free (L, g_strdup ("icon_file needs an extension directory"));
  if (relative[0] == '\0' || g_path_is_absolute (relative))
    raise_and_free (L, g_strdup ("icon_file must be a path relative to the extension directory"));

  parts = g_strsplit (relative, "/", -1);
  for (i = 0; error == NULL && parts[i] != NULL; i++)
    if (strcmp (parts[i], "..") == 0)
      error = g_strdup ("icon_file must not contain '..'");

  lower = g_ascii_strdown (relative, -1);
  if (error == NULL && !g_str_has_suffix (lower, ".svg") && !g_str_has_suffix (lower, ".png"))
    error = g_strdup ("icon_file must be a .svg or .png file");

  if (error == NULL)
    {
      base_path = g_file_get_path (shell_lua_extension_get_dir (extension));
      full = g_build_filename (base_path, relative, NULL);
      base_real = realpath (base_path, NULL);
      real = realpath (full, NULL);
      if (real == NULL)
        error = g_strdup_printf ("icon_file '%s' does not exist", relative);
      else if (base_real == NULL || !g_str_has_prefix (real, base_real) ||
               real[strlen (base_real)] != G_DIR_SEPARATOR)
        error = g_strdup_printf ("icon_file '%s' is outside the extension directory", relative);
      else
        {
          file = g_file_new_for_path (real);
          gicon = g_file_icon_new (file);
          g_object_unref (file);
        }
      free (base_real);
      free (real);
    }

  if (gicon == NULL)
    raise_and_free (L, error);
  return gicon;
}

static void
item_set_icon_file (lua_State *L,
                    GObject   *object,
                    int        idx)
{
  StIcon *icon = check_icon_item (L, object, "icon_file");
  const char *relative = check_string_arg (L, idx, "icon_file");
  GIcon *gicon = icon_for_file (L, relative);

  st_icon_set_gicon (icon, gicon);
  g_object_unref (gicon);
  g_object_set_data_full (object, ICON_FILE_QUARK, g_strdup (relative), g_free);
}

static const luaL_Reg action_methods[] = {
  { "on_activate", l_item_on_activate },
  { NULL, NULL }
};

static const ShellLuaHandleProperty labeled_properties[] = {
  { "ornament", "ornament", TRUE, TRUE },
  { NULL }
};

static const ShellLuaHandleVirtual labeled_virtuals[] = {
  { "label", item_get_label, item_set_label },
  { "sensitive", item_get_sensitive, item_set_sensitive },
  { "icon_name", item_get_icon_name, item_set_icon_name },
  { "icon_file", item_get_icon_file, item_set_icon_file },
  { NULL }
};

/* switch */

static void
switch_get_status (lua_State *L,
                   GObject   *object)
{
  const char *status = g_object_get_data (object, STATUS_QUARK);

  if (status != NULL)
    lua_pushstring (L, status);
  else
    lua_pushnil (L);
}

static void
switch_set_status (lua_State *L,
                   GObject   *object,
                   int        idx)
{
  const char *status = lua_isnil (L, idx) ? NULL : check_string_arg (L, idx, "status");

  g_object_set_data_full (object, STATUS_QUARK, g_strdup (status), g_free);
  shell_popup_switch_menu_item_set_status (SHELL_POPUP_SWITCH_MENU_ITEM (object), status);
}

static const luaL_Reg switch_methods[] = {
  { "on_activate", l_item_on_activate },
  { "on_toggled", l_item_on_toggled },
  { NULL, NULL }
};

static const ShellLuaHandleProperty switch_properties[] = {
  { "ornament", "ornament", TRUE, TRUE },
  { "state", "state", TRUE, TRUE },
  { NULL }
};

static const ShellLuaHandleVirtual switch_virtuals[] = {
  { "label", item_get_label, item_set_label },
  { "sensitive", item_get_sensitive, item_set_sensitive },
  { "status", switch_get_status, switch_set_status },
  { NULL }
};

/* submenu */

static void
submenu_get_menu (lua_State *L,
                  GObject   *object)
{
  ShellPopupSubMenu *menu = shell_popup_sub_menu_menu_item_get_menu (SHELL_POPUP_SUB_MENU_MENU_ITEM (object));

  shell_lua_api_menu_push (L, "shell.Menu", SHELL_POPUP_MENU_BASE (menu));
}

static const ShellLuaHandleVirtual submenu_virtuals[] = {
  { "label", item_get_label, item_set_label },
  { "sensitive", item_get_sensitive, item_set_sensitive },
  { "icon_name", item_get_icon_name, item_set_icon_name },
  { "icon_file", item_get_icon_file, item_set_icon_file },
  { "menu", submenu_get_menu, NULL },
  { NULL }
};

/* separator, widget */

static const ShellLuaHandleVirtual separator_virtuals[] = {
  { "label", item_get_label, item_set_label },
  { NULL }
};

static void
widget_item_get_child (lua_State *L,
                       GObject   *object)
{
  ClutterActor *child = clutter_actor_get_first_child (CLUTTER_ACTOR (object));

  shell_lua_api_ui_push_widget (L, child);
}

static const ShellLuaHandleVirtual widget_item_virtuals[] = {
  { "child", widget_item_get_child, NULL },
  { NULL }
};

/* ------------------------------------------------------------------------ */
/* Types and module                                                          */
/* ------------------------------------------------------------------------ */

static ShellLuaHandleType menu_type = {
  .name = "shell.Menu",
  .methods = menu_methods,
  .properties = menu_properties,
  .virtuals = menu_virtuals,
};

static ShellLuaHandleType item_type = {
  .name = "shell.MenuItem",
  .methods_ex = item_methods_ex,
  .properties = item_properties,
  .virtuals = item_virtuals,
};

static ShellLuaHandleType action_type = {
  .name = "shell.MenuActionItem", .parent = "shell.MenuItem",
  .methods = action_methods, .properties = labeled_properties, .virtuals = labeled_virtuals,
};

static ShellLuaHandleType switch_type = {
  .name = "shell.MenuSwitchItem", .parent = "shell.MenuItem",
  .methods = switch_methods, .properties = switch_properties, .virtuals = switch_virtuals,
};

static ShellLuaHandleType submenu_type = {
  .name = "shell.MenuSubmenuItem", .parent = "shell.MenuItem",
  .properties = labeled_properties, .virtuals = submenu_virtuals,
};

static ShellLuaHandleType separator_type = {
  .name = "shell.MenuSeparator", .parent = "shell.MenuItem",
  .virtuals = separator_virtuals,
};

static ShellLuaHandleType widget_item_type = {
  .name = "shell.MenuWidgetItem", .parent = "shell.MenuItem",
  .virtuals = widget_item_virtuals,
};

void
shell_lua_api_menu_register_types (lua_State *L)
{
  /* The widget types first: items refer to widgets */
  shell_lua_api_ui_register_types (L);

  menu_type.gtype = SHELL_TYPE_POPUP_MENU_BASE;
  item_type.gtype = SHELL_TYPE_POPUP_BASE_MENU_ITEM;
  action_type.gtype = SHELL_TYPE_POPUP_BASE_MENU_ITEM;
  switch_type.gtype = SHELL_TYPE_POPUP_SWITCH_MENU_ITEM;
  submenu_type.gtype = SHELL_TYPE_POPUP_SUB_MENU_MENU_ITEM;
  separator_type.gtype = SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM;
  widget_item_type.gtype = SHELL_TYPE_POPUP_BASE_MENU_ITEM;

  shell_lua_handle_register_type (L, &menu_type);
  shell_lua_handle_register_type (L, &item_type);
  shell_lua_handle_register_type (L, &action_type);
  shell_lua_handle_register_type (L, &switch_type);
  shell_lua_handle_register_type (L, &submenu_type);
  shell_lua_handle_register_type (L, &separator_type);
  shell_lua_handle_register_type (L, &widget_item_type);
}

static void
shell_lua_api_menu_populate (ShellLuaState *state,
                             lua_State     *L)
{
  shell_lua_api_menu_register_types (L);
}

static void __attribute__ ((constructor))
shell_lua_api_menu_init (void)
{
  shell_lua_api_register ("menu", shell_lua_api_menu_populate);
}
