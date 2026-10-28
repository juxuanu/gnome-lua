/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-lua-api-ui.c: `shell.ui` (docs/api/ui.md): the nine widget
 * constructors over St, the common `shell.Widget` properties, containers, the
 * per-widget fields and events.
 *
 * Every widget an extension creates is owned by its extension context
 * (shell_lua_own_object ()): dropping the last Lua reference never destroys
 * it, destroying a container destroys its children, and everything dies in
 * reverse creation order when the extension is disabled. Handles are weak
 * and turn dead with their actor.
 *
 * Differences from the document: reading `style_class` returns the classes
 * the extension set, without the `lua-ext-<id>` class the shell always adds
 * (`has_style_class ()` does see it).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <clutter/clutter.h>
#include <clutter/clutter-pango.h>
#include <limits.h>
#include <pango/pango.h>
#include <stdlib.h>
#include <string.h>

#include "lua-api/shell-lua-api-ui.h"
#include "ui/popup-menu.h"
#include "ui/slider.h"

#define UI_KIND_QUARK "shell-lua-ui-kind"
#define ICON_FILE_QUARK "shell-lua-ui-icon-file"
#define SPACER_WIDTH_QUARK "shell-lua-ui-spacer-width"
#define SPACER_HEIGHT_QUARK "shell-lua-ui-spacer-height"
#define MAX_STYLE_LENGTH 1024

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static void
actor_destroy (GObject *object)
{
  clutter_actor_destroy (CLUTTER_ACTOR (object));
}

static const ShellLuaOwnOps actor_ops = { actor_destroy, "destroy" };

const ShellLuaOwnOps *
shell_lua_api_ui_actor_ops (void)
{
  return &actor_ops;
}

char *
shell_lua_api_ui_extension_class (lua_State *L)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);
  const char *uuid;
  GString *class_name;

  if (extension == NULL)
    return g_strdup ("");

  uuid = shell_lua_extension_get_uuid (extension);
  class_name = g_string_new ("lua-ext-");
  for (; *uuid != '\0'; uuid++)
    {
      char c = g_ascii_tolower (*uuid);

      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
        g_string_append_c (class_name, c);
      else
        g_string_append_c (class_name, '-');
    }
  return g_string_free (class_name, FALSE);
}

void
shell_lua_api_ui_decorate (lua_State    *L,
                           ClutterActor *actor)
{
  g_autofree char *class_name = NULL;

  if (!ST_IS_WIDGET (actor))
    return;

  class_name = shell_lua_api_ui_extension_class (L);
  if (class_name[0] != '\0')
    st_widget_add_style_class_name (ST_WIDGET (actor), class_name);
}

static gboolean
actor_contains (GObject *ancestor,
                GObject *descendant)
{
  if (!CLUTTER_IS_ACTOR (ancestor) || !CLUTTER_IS_ACTOR (descendant))
    return FALSE;
  return clutter_actor_contains (CLUTTER_ACTOR (ancestor), CLUTTER_ACTOR (descendant));
}

ClutterActor *
shell_lua_api_ui_check_widget (lua_State *L,
                               int        idx)
{
  return CLUTTER_ACTOR (shell_lua_handle_check (L, idx, "shell.Widget"));
}

ClutterActor *
shell_lua_api_ui_check_free_widget (lua_State *L,
                                    int        idx)
{
  ClutterActor *actor = shell_lua_api_ui_check_widget (L, idx);

  if (clutter_actor_get_parent (actor) != NULL)
    luaL_error (L, "widget already has a parent; call remove() first");
  return actor;
}

static void
set_kind (GObject    *object,
          const char *type_name)
{
  g_object_set_data (object, UI_KIND_QUARK, (gpointer) type_name);
}

/* Pushes the handle of an owned widget, nil for anything else. */
void
shell_lua_api_ui_push_widget (lua_State    *L,
                              ClutterActor *actor)
{
  const char *kind;

  if (actor == NULL || !shell_lua_object_is_owned (L, G_OBJECT (actor)))
    {
      lua_pushnil (L);
      return;
    }

  kind = g_object_get_data (G_OBJECT (actor), UI_KIND_QUARK);
  shell_lua_handle_push (L, kind != NULL ? kind : "shell.Widget", G_OBJECT (actor));
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

/* ------------------------------------------------------------------------ */
/* shell.Widget                                                              */
/* ------------------------------------------------------------------------ */

/* style_class: the extension's classes, without lua-ext-<id> */

static char *
strip_extension_class (lua_State  *L,
                       const char *classes)
{
  g_autofree char *own = shell_lua_api_ui_extension_class (L);
  g_auto (GStrv) parts = NULL;
  GString *out = g_string_new (NULL);
  int i;

  if (classes != NULL)
    {
      parts = g_strsplit (classes, " ", -1);
      for (i = 0; parts[i] != NULL; i++)
        {
          if (parts[i][0] == '\0' || strcmp (parts[i], own) == 0)
            continue;
          if (out->len > 0)
            g_string_append_c (out, ' ');
          g_string_append (out, parts[i]);
        }
    }
  return g_string_free (out, FALSE);
}

static void
widget_get_style_class (lua_State *L,
                        GObject   *object)
{
  g_autofree char *classes = strip_extension_class (L, st_widget_get_style_class_name (ST_WIDGET (object)));

  lua_pushstring (L, classes);
}

static void
widget_set_style_class (lua_State *L,
                        GObject   *object,
                        int        idx)
{
  StWidget *widget = ST_WIDGET (object);
  const char *value = lua_isnil (L, idx) ? "" : check_string_arg (L, idx, "style_class");
  g_autofree char *own = shell_lua_api_ui_extension_class (L);
  g_autofree char *classes = strip_extension_class (L, value);

  st_widget_set_style_class_name (widget, classes);
  if (own[0] != '\0')
    st_widget_add_style_class_name (widget, own);
}

static void
widget_get_accessible_name (lua_State *L,
                            GObject   *object)
{
  const char *name = clutter_actor_get_accessible_name (CLUTTER_ACTOR (object));

  if (name != NULL)
    lua_pushstring (L, name);
  else
    lua_pushnil (L);
}

static void
widget_set_accessible_name (lua_State *L,
                            GObject   *object,
                            int        idx)
{
  const char *name = lua_isnil (L, idx) ? NULL : check_string_arg (L, idx, "accessible_name");

  clutter_actor_set_accessible_name (CLUTTER_ACTOR (object), name);
}

static void
widget_get_style (lua_State *L,
                  GObject   *object)
{
  const char *style = st_widget_get_style (ST_WIDGET (object));

  if (style != NULL && style[0] != '\0')
    lua_pushstring (L, style);
  else
    lua_pushstring (L, "");
}

static void
widget_set_style (lua_State *L,
                  GObject   *object,
                  int        idx)
{
  const char *style = lua_isnil (L, idx) ? "" : check_string_arg (L, idx, "style");

  if (strlen (style) > MAX_STYLE_LENGTH)
    luaL_error (L, "style is too long (at most %d bytes)", MAX_STYLE_LENGTH);
  if (strchr (style, '{') != NULL || strchr (style, '}') != NULL)
    luaL_error (L, "style takes declarations only (no selectors or blocks)");

  st_widget_set_style (ST_WIDGET (object), style[0] != '\0' ? style : NULL);
}

static int
l_widget_add_style_class (lua_State *L)
{
  ClutterActor *actor = shell_lua_api_ui_check_widget (L, 1);
  const char *name = luaL_checkstring (L, 2);

  if (name[0] == '\0' || strchr (name, ' ') != NULL)
    return luaL_argerror (L, 2, "a class name is a non-empty word");
  st_widget_add_style_class_name (ST_WIDGET (actor), name);
  return 0;
}

static int
l_widget_remove_style_class (lua_State *L)
{
  ClutterActor *actor = shell_lua_api_ui_check_widget (L, 1);
  const char *name = luaL_checkstring (L, 2);
  g_autofree char *own = shell_lua_api_ui_extension_class (L);

  /* The shell's own class stays. */
  if (strcmp (name, own) != 0)
    st_widget_remove_style_class_name (ST_WIDGET (actor), name);
  return 0;
}

static int
l_widget_has_style_class (lua_State *L)
{
  ClutterActor *actor = shell_lua_api_ui_check_widget (L, 1);
  const char *name = luaL_checkstring (L, 2);

  lua_pushboolean (L, st_widget_has_style_class_name (ST_WIDGET (actor), name));
  return 1;
}

typedef struct
{
  ShellLuaState *state;
  ClutterActor  *actor;
} RemoveLater;

static void
remove_later (gpointer data)
{
  RemoveLater *later = data;
  lua_State *L = shell_lua_state_is_alive (later->state) ? shell_lua_state_get_L (later->state) : NULL;
  ClutterActor *parent;

  if (L == NULL || !shell_lua_object_is_owned (L, G_OBJECT (later->actor)))
    return;
  parent = clutter_actor_get_parent (later->actor);
  if (parent != NULL)
    clutter_actor_remove_child (parent, later->actor);
}

static void
remove_later_free (gpointer data)
{
  RemoveLater *later = data;

  g_object_unref (later->actor);
  g_free (later);
}

static int
l_widget_remove (lua_State *L)
{
  ClutterActor *actor = shell_lua_api_ui_check_widget (L, 1);
  ClutterActor *parent = clutter_actor_get_parent (actor);
  RemoveLater *later;

  if (parent == NULL)
    return 0;

  /* README rule 8: reparenting the emitting widget waits for its callback. */
  later = g_new0 (RemoveLater, 1);
  later->state = shell_lua_state_from_L (L);
  later->actor = g_object_ref (actor);
  if (shell_lua_defer_if_emitting (L, G_OBJECT (actor), remove_later, later, remove_later_free))
    return 0;
  remove_later_free (later);

  clutter_actor_remove_child (parent, actor);
  return 0;
}

static int
l_widget_focus (lua_State *L)
{
  ClutterActor *actor = shell_lua_api_ui_check_widget (L, 1);
  ClutterActor *stage, *focus;

  if (!clutter_actor_is_mapped (actor) ||
      (ST_IS_WIDGET (actor) && !st_widget_get_can_focus (ST_WIDGET (actor))))
    {
      lua_pushboolean (L, 0);
      return 1;
    }

  stage = clutter_actor_get_stage (actor);
  clutter_actor_grab_key_focus (actor);
  focus = stage != NULL ? clutter_stage_get_key_focus (CLUTTER_STAGE (stage)) : NULL;
  lua_pushboolean (L, focus != NULL && clutter_actor_contains (actor, focus));
  return 1;
}

static const luaL_Reg widget_methods[] = {
  { "remove", l_widget_remove },
  { "add_style_class", l_widget_add_style_class },
  { "remove_style_class", l_widget_remove_style_class },
  { "has_style_class", l_widget_has_style_class },
  { "focus", l_widget_focus },
  { NULL, NULL }
};

static const ShellLuaHandleMethod widget_methods_ex[] = {
  { "destroy", shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty widget_properties[] = {
  { "visible", "visible", TRUE, TRUE },
  { "reactive", "reactive", TRUE, TRUE },
  { "x_expand", "x-expand", TRUE, TRUE },
  { "y_expand", "y-expand", TRUE, TRUE },
  { "x_align", "x-align", TRUE, TRUE },
  { "y_align", "y-align", TRUE, TRUE },
  { "opacity", "opacity", TRUE, TRUE },
  { NULL }
};

static const ShellLuaHandleVirtual widget_virtuals[] = {
  { "style_class", widget_get_style_class, widget_set_style_class },
  { "style", widget_get_style, widget_set_style },
  { "accessible_name", widget_get_accessible_name, widget_set_accessible_name },
  { NULL }
};

/* sensitive: reactive and focusable, which also gives :insensitive */

static void
sensitive_get (lua_State *L,
               GObject   *object)
{
  lua_pushboolean (L, clutter_actor_get_reactive (CLUTTER_ACTOR (object)));
}

static void
sensitive_set (lua_State *L,
               GObject   *object,
               int        idx)
{
  gboolean sensitive;

  if (lua_type (L, idx) != LUA_TBOOLEAN)
    luaL_error (L, "sensitive must be a boolean, got %s", luaL_typename (L, idx));
  sensitive = lua_toboolean (L, idx);

  clutter_actor_set_reactive (CLUTTER_ACTOR (object), sensitive);
  st_widget_set_can_focus (ST_WIDGET (object), sensitive);
  if (ST_IS_ENTRY (object))
    {
      ClutterText *text = ST_ENTRY (object) != NULL ? st_entry_get_clutter_text (ST_ENTRY (object)) : NULL;

      if (text != NULL)
        {
          clutter_actor_set_reactive (CLUTTER_ACTOR (text), sensitive);
          clutter_text_set_editable (text, sensitive);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Events: h:on_<event>(fn) and the on_<event> constructor fields            */
/* ------------------------------------------------------------------------ */

typedef enum
{
  EVENT_ON_WIDGET,
  EVENT_ON_TEXT,
} EventSource;

typedef struct
{
  const char  *event;
  const char  *signal;
  GCallback    handler;
  EventSource  source;
} EventDef;

static void
push_int_arg (lua_State *L,
              gpointer   data)
{
  lua_pushinteger (L, GPOINTER_TO_INT (data));
}

static void
push_bool_arg (lua_State *L,
               gpointer   data)
{
  lua_pushboolean (L, GPOINTER_TO_INT (data));
}

static void
push_string_arg (lua_State *L,
                 gpointer   data)
{
  lua_pushstring (L, data);
}

static void
push_number_arg (lua_State *L,
                 gpointer   data)
{
  lua_pushnumber (L, *(double *) data);
}

static void
on_button_clicked (StButton         *button,
                   int               which,
                   ShellLuaCallback *cb)
{
  shell_lua_callback_emit (cb, 1, push_int_arg, GINT_TO_POINTER (which));
}

static void
on_entry_changed (ClutterText     *text,
                  ShellLuaCallback *cb)
{
  shell_lua_callback_emit (cb, 1, push_string_arg, (gpointer) clutter_text_get_text (text));
}

static void
on_entry_activate (ClutterText      *text,
                   ShellLuaCallback *cb)
{
  shell_lua_callback_emit (cb, 1, push_string_arg, (gpointer) clutter_text_get_text (text));
}

static void
on_switch_changed (ShellSwitch      *self,
                   GParamSpec       *pspec,
                   ShellLuaCallback *cb)
{
  shell_lua_callback_emit (cb, 1, push_bool_arg, GINT_TO_POINTER (shell_switch_get_state (self)));
}

static void
on_slider_changed (ShellSlider      *self,
                   GParamSpec       *pspec,
                   ShellLuaCallback *cb)
{
  double value = shell_bar_level_get_value (SHELL_BAR_LEVEL (self));

  shell_lua_callback_emit (cb, 1, push_number_arg, &value);
}

static void
on_slider_drag (ShellSlider      *self,
                ShellLuaCallback *cb)
{
  shell_lua_callback_emit (cb, 0, NULL, NULL);
}

static const EventDef button_events[] = {
  { "clicked", "clicked", G_CALLBACK (on_button_clicked), EVENT_ON_WIDGET },
  { NULL }
};

static const EventDef entry_events[] = {
  { "changed", "text-changed", G_CALLBACK (on_entry_changed), EVENT_ON_TEXT },
  { "activate", "activate", G_CALLBACK (on_entry_activate), EVENT_ON_TEXT },
  { NULL }
};

static const EventDef switch_events[] = {
  { "changed", "notify::state", G_CALLBACK (on_switch_changed), EVENT_ON_WIDGET },
  { NULL }
};

static const EventDef slider_events[] = {
  { "changed", "notify::value", G_CALLBACK (on_slider_changed), EVENT_ON_WIDGET },
  { "drag_begin", "drag-begin", G_CALLBACK (on_slider_drag), EVENT_ON_WIDGET },
  { "drag_end", "drag-end", G_CALLBACK (on_slider_drag), EVENT_ON_WIDGET },
  { NULL }
};

static const EventDef *
find_event (const EventDef *events,
            const char     *event)
{
  for (; events->event != NULL; events++)
    if (strcmp (events->event, event) == 0)
      return events;
  return NULL;
}

/* Connects Lua function @ref to @def on @object; returns the callback and the
 * handler id and instance, for a connection handle. */
static ShellLuaCallback *
connect_event (lua_State       *L,
               GObject         *object,
               const EventDef  *def,
               int              ref,
               gpointer        *instance,
               gulong          *handler_id)
{
  ShellLuaCallback *cb = shell_lua_callback_new (L, object, ref);

  *instance = object;
  if (def->source == EVENT_ON_TEXT)
    *instance = st_entry_get_clutter_text (ST_ENTRY (object));
  *handler_id = shell_lua_callback_connect (cb, *instance, def->signal, def->handler);
  return cb;
}

static int
event_method (lua_State      *L,
              const char     *type_name,
              const EventDef *events,
              const char     *event)
{
  GObject *object = shell_lua_handle_check (L, 1, type_name);
  const EventDef *def = find_event (events, event);
  ShellLuaCallback *cb;
  gpointer instance;
  gulong id;

  g_assert (def != NULL);
  luaL_checktype (L, 2, LUA_TFUNCTION);
  lua_pushvalue (L, 2);
  cb = connect_event (L, object, def, luaL_ref (L, LUA_REGISTRYINDEX), &instance, &id);
  return shell_lua_callback_push_connection (L, cb, instance, id);
}

#define EVENT_METHOD(fn, type_name, events, event) \
  static int fn (lua_State *L) { return event_method (L, type_name, events, event); }

EVENT_METHOD (l_button_on_clicked, "shell.Button", button_events, "clicked")
EVENT_METHOD (l_entry_on_changed, "shell.Entry", entry_events, "changed")
EVENT_METHOD (l_entry_on_activate, "shell.Entry", entry_events, "activate")
EVENT_METHOD (l_switch_on_changed, "shell.Switch", switch_events, "changed")
EVENT_METHOD (l_slider_on_changed, "shell.Slider", slider_events, "changed")
EVENT_METHOD (l_slider_on_drag_begin, "shell.Slider", slider_events, "drag_begin")
EVENT_METHOD (l_slider_on_drag_end, "shell.Slider", slider_events, "drag_end")

/* ------------------------------------------------------------------------ */
/* Containers: box, scroll                                                   */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellLuaState *state;
  ClutterActor  *box;
  ClutterActor  *child;
  int            index;   /* 0 = append, else 1-based */
} AddLater;

static void
box_do_add (ClutterActor *box,
            ClutterActor *child,
            int           index)
{
  int n = (int) clutter_actor_get_n_children (box);

  if (index <= 0 || index > n)
    clutter_actor_add_child (box, child);
  else
    clutter_actor_insert_child_at_index (box, child, index - 1);
}

static void
add_later (gpointer data)
{
  AddLater *later = data;
  lua_State *L = shell_lua_state_is_alive (later->state) ? shell_lua_state_get_L (later->state) : NULL;

  if (L == NULL ||
      !shell_lua_object_is_owned (L, G_OBJECT (later->box)) ||
      !shell_lua_object_is_owned (L, G_OBJECT (later->child)) ||
      clutter_actor_get_parent (later->child) != NULL ||
      clutter_actor_contains (later->child, later->box))
    return;
  box_do_add (later->box, later->child, later->index);
}

static void
add_later_free (gpointer data)
{
  AddLater *later = data;

  g_object_unref (later->box);
  g_object_unref (later->child);
  g_free (later);
}

/* box:add (child [, index]) with the checks of ui.md; @index_idx is 0 when
 * there is no index argument. */
static void
box_add (lua_State    *L,
         ClutterActor *box,
         int           child_idx,
         int           index_idx)
{
  ClutterActor *child = shell_lua_api_ui_check_free_widget (L, child_idx);
  lua_Integer index = 0;
  AddLater *later;

  if (child == box || clutter_actor_contains (child, box))
    luaL_error (L, "cannot add a widget to itself or to one of its descendants");

  if (index_idx != 0 && !lua_isnoneornil (L, index_idx))
    {
      index = luaL_checkinteger (L, index_idx);
      if (index < 1)
        luaL_error (L, "index must be at least 1");
      if (index > INT_MAX)
        index = INT_MAX;
    }

  later = g_new0 (AddLater, 1);
  later->state = shell_lua_state_from_L (L);
  later->box = g_object_ref (box);
  later->child = g_object_ref (child);
  later->index = (int) index;
  if (shell_lua_defer_if_emitting (L, G_OBJECT (child), add_later, later, add_later_free))
    return;
  add_later_free (later);

  box_do_add (box, child, (int) index);
}

static int
l_box_add (lua_State *L)
{
  ClutterActor *box = CLUTTER_ACTOR (shell_lua_handle_check (L, 1, "shell.Box"));

  box_add (L, box, 2, 3);
  return 0;
}

static int
l_box_children (lua_State *L)
{
  ClutterActor *box = CLUTTER_ACTOR (shell_lua_handle_check (L, 1, "shell.Box"));
  g_autoptr (GList) children = clutter_actor_get_children (box);
  GList *l;
  int i = 1;

  lua_newtable (L);
  for (l = children; l != NULL; l = l->next)
    {
      if (!shell_lua_object_is_owned (L, l->data))
        continue;
      shell_lua_api_ui_push_widget (L, l->data);
      lua_rawseti (L, -2, i++);
    }
  return 1;
}

static int
l_box_clear (lua_State *L)
{
  ClutterActor *box = CLUTTER_ACTOR (shell_lua_handle_check (L, 1, "shell.Box"));
  g_autoptr (GList) children = clutter_actor_get_children (box);
  GList *l;

  for (l = children; l != NULL; l = l->next)
    {
      if (!shell_lua_destroy_object (L, l->data))
        clutter_actor_destroy (l->data);
    }
  return 0;
}

static const luaL_Reg box_methods[] = {
  { "add", l_box_add },
  { "children", l_box_children },
  { "clear", l_box_clear },
  { NULL, NULL }
};

static const ShellLuaHandleProperty box_properties[] = {
  { "orientation", "orientation", TRUE, TRUE },
  { NULL }
};

static void
scroll_get_child (lua_State *L,
                  GObject   *object)
{
  StScrollable *child = st_scroll_view_get_child (ST_SCROLL_VIEW (object));

  shell_lua_api_ui_push_widget (L, child != NULL ? CLUTTER_ACTOR (child) : NULL);
}

static void
scroll_set_child (lua_State *L,
                  GObject   *object,
                  int        idx)
{
  StScrollView *scroll = ST_SCROLL_VIEW (object);

  if (lua_isnil (L, idx))
    {
      st_scroll_view_set_child (scroll, NULL);
      return;
    }

  {
    ClutterActor *child = CLUTTER_ACTOR (shell_lua_handle_check (L, idx, "shell.Box"));

    if (child == (ClutterActor *) st_scroll_view_get_child (scroll))
      return;
    if (clutter_actor_get_parent (child) != NULL)
      luaL_error (L, "widget already has a parent; call remove() first");
    if (clutter_actor_contains (child, CLUTTER_ACTOR (scroll)))
      luaL_error (L, "cannot add a widget to itself or to one of its descendants");
    st_scroll_view_set_child (scroll, ST_SCROLLABLE (child));
  }
}

static const ShellLuaHandleProperty scroll_properties[] = {
  { "hscrollbar_policy", "hscrollbar-policy", TRUE, TRUE },
  { "vscrollbar_policy", "vscrollbar-policy", TRUE, TRUE },
  { NULL }
};

static const ShellLuaHandleVirtual scroll_virtuals[] = {
  { "child", scroll_get_child, scroll_set_child },
  { NULL }
};

/* ------------------------------------------------------------------------ */
/* label                                                                     */
/* ------------------------------------------------------------------------ */

static void
label_get_text (lua_State *L,
                GObject   *object)
{
  ClutterText *text = st_label_get_clutter_text (ST_LABEL (object));

  lua_pushstring (L, clutter_text_get_text (text));
}

static void
label_set_text (lua_State *L,
                GObject   *object,
                int        idx)
{
  ClutterText *text = st_label_get_clutter_text (ST_LABEL (object));
  const char *value = check_string_arg (L, idx, "text");

  /* A label set with markup before shows the new text literally. */
  clutter_text_set_use_markup (text, FALSE);
  st_label_set_text (ST_LABEL (object), value);
}

static const char * const ellipsize_nicks[] = { "none", "start", "middle", "end", NULL };
static const PangoEllipsizeMode ellipsize_modes[] = {
  PANGO_ELLIPSIZE_NONE, PANGO_ELLIPSIZE_START, PANGO_ELLIPSIZE_MIDDLE, PANGO_ELLIPSIZE_END
};

static void
label_get_ellipsize (lua_State *L,
                     GObject   *object)
{
  PangoEllipsizeMode mode = clutter_text_get_ellipsize (st_label_get_clutter_text (ST_LABEL (object)));
  guint i;

  for (i = 0; i < G_N_ELEMENTS (ellipsize_modes); i++)
    if (ellipsize_modes[i] == mode)
      {
        lua_pushstring (L, ellipsize_nicks[i]);
        return;
      }
  lua_pushstring (L, "none");
}

static void
label_set_ellipsize (lua_State *L,
                     GObject   *object,
                     int        idx)
{
  const char *value = check_string_arg (L, idx, "ellipsize");
  guint i;

  for (i = 0; ellipsize_nicks[i] != NULL; i++)
    if (strcmp (ellipsize_nicks[i], value) == 0)
      {
        clutter_text_set_ellipsize (st_label_get_clutter_text (ST_LABEL (object)), ellipsize_modes[i]);
        return;
      }
  luaL_error (L, "invalid ellipsize '%s' (valid: none, start, middle, end)", value);
}

static int
l_label_set_markup (lua_State *L)
{
  StLabel *label = ST_LABEL (shell_lua_handle_check (L, 1, "shell.Label"));
  const char *markup = luaL_checkstring (L, 2);
  GError *error = NULL;

  if (!pango_parse_markup (markup, -1, 0, NULL, NULL, NULL, &error))
    {
      lua_pushfstring (L, "invalid markup: %s", error->message);
      g_error_free (error);
      return lua_error (L);
    }

  clutter_text_set_markup (st_label_get_clutter_text (label), markup);
  return 0;
}

static const luaL_Reg label_methods[] = {
  { "set_markup", l_label_set_markup },
  { NULL, NULL }
};

static const ShellLuaHandleVirtual label_virtuals[] = {
  { "text", label_get_text, label_set_text },
  { "ellipsize", label_get_ellipsize, label_set_ellipsize },
  { NULL }
};


/* ------------------------------------------------------------------------ */
/* icons: icon_name, icon_file, icon_size                                    */
/* ------------------------------------------------------------------------ */

static G_GNUC_NORETURN void
raise_and_free (lua_State *L,
                char      *message)
{
  lua_pushstring (L, message);
  g_free (message);
  lua_error (L);
  g_assert_not_reached ();
}

/* Resolves @relative inside the extension directory. Returns NULL with a
 * newly allocated message in @error for an absolute path, "..", a wrong
 * suffix, a missing file or a link out of the directory. */
static GFile *
resolve_extension_file (lua_State   *L,
                        const char  *relative,
                        char       **error)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);
  g_autofree char *base_path = NULL;
  g_autofree char *full = NULL;
  g_autofree char *lower = NULL;
  g_auto (GStrv) parts = NULL;
  char *base_real, *real;
  GFile *file = NULL;
  int i;

  if (extension == NULL)
    {
      *error = g_strdup ("icon_file needs an extension directory");
      return NULL;
    }
  if (relative[0] == '\0' || g_path_is_absolute (relative))
    {
      *error = g_strdup ("icon_file must be a path relative to the extension directory");
      return NULL;
    }

  parts = g_strsplit (relative, "/", -1);
  for (i = 0; parts[i] != NULL; i++)
    if (strcmp (parts[i], "..") == 0)
      {
        *error = g_strdup ("icon_file must not contain '..'");
        return NULL;
      }

  lower = g_ascii_strdown (relative, -1);
  if (!g_str_has_suffix (lower, ".svg") && !g_str_has_suffix (lower, ".png"))
    {
      *error = g_strdup ("icon_file must be a .svg or .png file");
      return NULL;
    }

  base_path = g_file_get_path (shell_lua_extension_get_dir (extension));
  full = g_build_filename (base_path, relative, NULL);
  base_real = realpath (base_path, NULL);
  real = realpath (full, NULL);
  if (real == NULL)
    *error = g_strdup_printf ("icon_file '%s' does not exist", relative);
  else if (base_real == NULL || !g_str_has_prefix (real, base_real) ||
           real[strlen (base_real)] != G_DIR_SEPARATOR)
    *error = g_strdup_printf ("icon_file '%s' is outside the extension directory", relative);
  else
    file = g_file_new_for_path (real);

  free (base_real);
  free (real);
  return file;
}

/* A GIcon for the extension file @relative; raises (after cleaning up) when
 * the file is not acceptable. */
static GIcon *
icon_for_file (lua_State  *L,
               const char *relative)
{
  char *error = NULL;
  GFile *file = resolve_extension_file (L, relative, &error);
  GIcon *icon;

  if (file == NULL)
    raise_and_free (L, error);

  icon = g_file_icon_new (file);
  g_object_unref (file);
  return icon;
}

static void
icon_get_name (lua_State *L,
               GObject   *object)
{
  const char *name = st_icon_get_icon_name (ST_ICON (object));

  if (name != NULL)
    lua_pushstring (L, name);
  else
    lua_pushnil (L);
}

static void
icon_set_name (lua_State *L,
               GObject   *object,
               int        idx)
{
  const char *name = lua_isnil (L, idx) ? NULL : check_string_arg (L, idx, "icon_name");

  g_object_set_data (object, ICON_FILE_QUARK, NULL);
  st_icon_set_icon_name (ST_ICON (object), name);
}

static void
icon_get_file (lua_State *L,
               GObject   *object)
{
  const char *file = g_object_get_data (object, ICON_FILE_QUARK);

  if (file != NULL)
    lua_pushstring (L, file);
  else
    lua_pushnil (L);
}

static void
icon_set_file (lua_State *L,
               GObject   *object,
               int        idx)
{
  const char *relative;
  GIcon *gicon;

  if (lua_isnil (L, idx))
    {
      g_object_set_data (object, ICON_FILE_QUARK, NULL);
      st_icon_set_gicon (ST_ICON (object), NULL);
      return;
    }

  relative = check_string_arg (L, idx, "icon_file");
  gicon = icon_for_file (L, relative);
  st_icon_set_gicon (ST_ICON (object), gicon);
  g_object_unref (gicon);
  g_object_set_data_full (object, ICON_FILE_QUARK, g_strdup (relative), g_free);
}

static void
icon_get_size (lua_State *L,
               GObject   *object)
{
  int size = st_icon_get_icon_size (ST_ICON (object));

  if (size > 0)
    lua_pushinteger (L, size);
  else
    lua_pushnil (L);
}

static void
icon_set_size (lua_State *L,
               GObject   *object,
               int        idx)
{
  lua_Integer size = -1;

  if (!lua_isnil (L, idx))
    {
      size = luaL_checkinteger (L, idx);
      if (size < 1 || size > 1024)
        luaL_error (L, "icon_size must be between 1 and 1024");
    }
  st_icon_set_icon_size (ST_ICON (object), (int) size);
}

static const ShellLuaHandleVirtual icon_virtuals[] = {
  { "icon_name", icon_get_name, icon_set_name },
  { "icon_file", icon_get_file, icon_set_file },
  { "icon_size", icon_get_size, icon_set_size },
  { NULL }
};

/* ------------------------------------------------------------------------ */
/* button                                                                    */
/* ------------------------------------------------------------------------ */

static void
button_get_label (lua_State *L,
                  GObject   *object)
{
  const char *label = st_button_get_label (ST_BUTTON (object));

  if (label != NULL)
    lua_pushstring (L, label);
  else
    lua_pushnil (L);
}

static void
button_set_label (lua_State *L,
                  GObject   *object,
                  int        idx)
{
  const char *label = lua_isnil (L, idx) ? NULL : check_string_arg (L, idx, "label");

  g_object_set_data (object, ICON_FILE_QUARK, NULL);
  st_button_set_label (ST_BUTTON (object), label);
}

static void
button_get_icon_name (lua_State *L,
                      GObject   *object)
{
  const char *name = st_button_get_icon_name (ST_BUTTON (object));

  if (name != NULL)
    lua_pushstring (L, name);
  else
    lua_pushnil (L);
}

static void
button_set_icon_name (lua_State *L,
                      GObject   *object,
                      int        idx)
{
  const char *name = lua_isnil (L, idx) ? NULL : check_string_arg (L, idx, "icon_name");

  g_object_set_data (object, ICON_FILE_QUARK, NULL);
  st_button_set_icon_name (ST_BUTTON (object), name);
}

static void
button_get_icon_file (lua_State *L,
                      GObject   *object)
{
  const char *file = g_object_get_data (object, ICON_FILE_QUARK);

  if (file != NULL)
    lua_pushstring (L, file);
  else
    lua_pushnil (L);
}

static void
button_set_icon_file (lua_State *L,
                      GObject   *object,
                      int        idx)
{
  StButton *button = ST_BUTTON (object);
  const char *relative;
  GIcon *gicon;
  ClutterActor *icon;

  if (lua_isnil (L, idx))
    {
      g_object_set_data (object, ICON_FILE_QUARK, NULL);
      st_bin_set_child (ST_BIN (button), NULL);
      return;
    }

  relative = check_string_arg (L, idx, "icon_file");
  gicon = icon_for_file (L, relative);

  /* A fresh icon replaces whatever the button showed. */
  icon = g_object_new (ST_TYPE_ICON, "gicon", gicon, NULL);
  st_bin_set_child (ST_BIN (button), icon);
  g_object_unref (gicon);
  g_object_set_data_full (object, ICON_FILE_QUARK, g_strdup (relative), g_free);
}

static void
button_get_child (lua_State *L,
                  GObject   *object)
{
  shell_lua_api_ui_push_widget (L, st_bin_get_child (ST_BIN (object)));
}

static void
button_set_child (lua_State *L,
                  GObject   *object,
                  int        idx)
{
  StBin *bin = ST_BIN (object);

  if (lua_isnil (L, idx))
    {
      st_bin_set_child (bin, NULL);
      return;
    }

  {
    ClutterActor *child = shell_lua_api_ui_check_widget (L, idx);

    if (child == st_bin_get_child (bin))
      return;
    if (clutter_actor_get_parent (child) != NULL)
      luaL_error (L, "widget already has a parent; call remove() first");
    if (clutter_actor_contains (child, CLUTTER_ACTOR (bin)))
      luaL_error (L, "cannot add a widget to itself or to one of its descendants");

    g_object_set_data (object, ICON_FILE_QUARK, NULL);
    st_bin_set_child (bin, child);
  }
}

static const luaL_Reg button_methods[] = {
  { "on_clicked", l_button_on_clicked },
  { NULL, NULL }
};

static const ShellLuaHandleProperty button_properties[] = {
  { "checked", "checked", TRUE, TRUE },
  { "toggle_mode", "toggle-mode", TRUE, FALSE },
  { NULL }
};

static const ShellLuaHandleVirtual button_virtuals[] = {
  { "label", button_get_label, button_set_label },
  { "icon_name", button_get_icon_name, button_set_icon_name },
  { "icon_file", button_get_icon_file, button_set_icon_file },
  { "child", button_get_child, button_set_child },
  { "sensitive", sensitive_get, sensitive_set },
  { NULL }
};

/* ------------------------------------------------------------------------ */
/* entry, switch, slider, spacer                                             */
/* ------------------------------------------------------------------------ */

static const luaL_Reg entry_methods[] = {
  { "on_changed", l_entry_on_changed },
  { "on_activate", l_entry_on_activate },
  { NULL, NULL }
};

static const ShellLuaHandleProperty entry_properties[] = {
  { "text", "text", TRUE, TRUE },
  { "hint_text", "hint-text", TRUE, TRUE },
  { NULL }
};

static const ShellLuaHandleVirtual sensitive_virtuals[] = {
  { "sensitive", sensitive_get, sensitive_set },
  { NULL }
};

static const luaL_Reg switch_methods[] = {
  { "on_changed", l_switch_on_changed },
  { NULL, NULL }
};

static const ShellLuaHandleProperty switch_properties[] = {
  { "state", "state", TRUE, TRUE },
  { NULL }
};

static void
slider_get_value (lua_State *L,
                  GObject   *object)
{
  lua_pushnumber (L, shell_bar_level_get_value (SHELL_BAR_LEVEL (object)));
}

static void
slider_set_value (lua_State *L,
                  GObject   *object,
                  int        idx)
{
  lua_Number value = luaL_checknumber (L, idx);

  if (!(value >= 0.0 && value <= 1.0))
    luaL_error (L, "value must be between 0.0 and 1.0");
  shell_bar_level_set_value (SHELL_BAR_LEVEL (object), value);
}

static const luaL_Reg slider_methods[] = {
  { "on_changed", l_slider_on_changed },
  { "on_drag_begin", l_slider_on_drag_begin },
  { "on_drag_end", l_slider_on_drag_end },
  { NULL, NULL }
};

static const ShellLuaHandleVirtual slider_virtuals[] = {
  { "value", slider_get_value, slider_set_value },
  { "sensitive", sensitive_get, sensitive_set },
  { NULL }
};

static void
spacer_get_dimension (lua_State  *L,
                      GObject    *object,
                      const char *quark)
{
  gpointer p = g_object_get_data (object, quark);

  lua_pushinteger (L, p != NULL ? GPOINTER_TO_INT (p) - 2 : -1);
}

static void
spacer_set_dimension (lua_State  *L,
                      GObject    *object,
                      int         idx,
                      const char *quark,
                      gboolean    is_width)
{
  lua_Integer value = luaL_checkinteger (L, idx);

  if (value < -1 || value > 100000)
    luaL_error (L, "%s must be -1 (natural) or between 0 and 100000", is_width ? "width" : "height");

  g_object_set_data (object, quark, GINT_TO_POINTER ((int) value + 2));
  if (is_width)
    clutter_actor_set_width (CLUTTER_ACTOR (object), (float) value);
  else
    clutter_actor_set_height (CLUTTER_ACTOR (object), (float) value);
}

static void
spacer_get_width (lua_State *L,
                  GObject   *object)
{
  spacer_get_dimension (L, object, SPACER_WIDTH_QUARK);
}

static void
spacer_get_height (lua_State *L,
                   GObject   *object)
{
  spacer_get_dimension (L, object, SPACER_HEIGHT_QUARK);
}

static void
spacer_set_width (lua_State *L,
                  GObject   *object,
                  int        idx)
{
  spacer_set_dimension (L, object, idx, SPACER_WIDTH_QUARK, TRUE);
}

static void
spacer_set_height (lua_State *L,
                   GObject   *object,
                   int        idx)
{
  spacer_set_dimension (L, object, idx, SPACER_HEIGHT_QUARK, FALSE);
}

static const ShellLuaHandleVirtual spacer_virtuals[] = {
  { "width", spacer_get_width, spacer_set_width },
  { "height", spacer_get_height, spacer_set_height },
  { NULL }
};

/* ------------------------------------------------------------------------ */
/* Handle types                                                              */
/* ------------------------------------------------------------------------ */

static ShellLuaHandleType widget_type = {
  .name = "shell.Widget",
  .methods = widget_methods,
  .methods_ex = widget_methods_ex,
  .properties = widget_properties,
  .virtuals = widget_virtuals,
};

static ShellLuaHandleType box_type = {
  .name = "shell.Box", .parent = "shell.Widget",
  .methods = box_methods, .properties = box_properties,
};

static ShellLuaHandleType label_type = {
  .name = "shell.Label", .parent = "shell.Widget",
  .methods = label_methods, .virtuals = label_virtuals,
};

static ShellLuaHandleType icon_type = {
  .name = "shell.Icon", .parent = "shell.Widget",
  .virtuals = icon_virtuals,
};

static ShellLuaHandleType button_type = {
  .name = "shell.Button", .parent = "shell.Widget",
  .methods = button_methods, .properties = button_properties, .virtuals = button_virtuals,
};

static ShellLuaHandleType entry_type = {
  .name = "shell.Entry", .parent = "shell.Widget",
  .methods = entry_methods, .properties = entry_properties, .virtuals = sensitive_virtuals,
};

static ShellLuaHandleType switch_type = {
  .name = "shell.Switch", .parent = "shell.Widget",
  .methods = switch_methods, .properties = switch_properties, .virtuals = sensitive_virtuals,
};

static ShellLuaHandleType slider_type = {
  .name = "shell.Slider", .parent = "shell.Widget",
  .methods = slider_methods, .virtuals = slider_virtuals,
};

static ShellLuaHandleType spacer_type = {
  .name = "shell.Spacer", .parent = "shell.Widget",
  .virtuals = spacer_virtuals,
};

static ShellLuaHandleType scroll_type = {
  .name = "shell.Scroll", .parent = "shell.Widget",
  .properties = scroll_properties, .virtuals = scroll_virtuals,
};

void
shell_lua_api_ui_register_types (lua_State *L)
{
  widget_type.gtype = ST_TYPE_WIDGET;
  box_type.gtype = ST_TYPE_BOX_LAYOUT;
  label_type.gtype = ST_TYPE_LABEL;
  icon_type.gtype = ST_TYPE_ICON;
  button_type.gtype = ST_TYPE_BUTTON;
  entry_type.gtype = ST_TYPE_ENTRY;
  switch_type.gtype = SHELL_TYPE_SWITCH;
  slider_type.gtype = SHELL_TYPE_SLIDER;
  spacer_type.gtype = ST_TYPE_WIDGET;
  scroll_type.gtype = ST_TYPE_SCROLL_VIEW;

  /* The base first: it owns the GType -> type mapping of StWidget. */
  shell_lua_handle_register_type (L, &widget_type);
  shell_lua_handle_register_type (L, &box_type);
  shell_lua_handle_register_type (L, &label_type);
  shell_lua_handle_register_type (L, &icon_type);
  shell_lua_handle_register_type (L, &button_type);
  shell_lua_handle_register_type (L, &entry_type);
  shell_lua_handle_register_type (L, &switch_type);
  shell_lua_handle_register_type (L, &slider_type);
  shell_lua_handle_register_type (L, &spacer_type);
  shell_lua_handle_register_type (L, &scroll_type);
}

/* ------------------------------------------------------------------------ */
/* Constructors                                                              */
/* ------------------------------------------------------------------------ */

#define COMMON_FIELDS "style_class", "style", "visible", "reactive", "x_expand", \
  "y_expand", "x_align", "y_align", "opacity", "accessible_name"

static const char * const box_fields[] = { COMMON_FIELDS, "orientation", "children", NULL };
static const char * const label_fields[] = { COMMON_FIELDS, "text", "ellipsize", NULL };
static const char * const icon_fields[] = { COMMON_FIELDS, "icon_name", "icon_file", "icon_size", NULL };
static const char * const button_fields[] = {
  COMMON_FIELDS, "label", "icon_name", "icon_file", "child", "toggle_mode", "checked",
  "sensitive", "on_clicked", NULL
};
static const char * const entry_fields[] = {
  COMMON_FIELDS, "text", "hint_text", "sensitive", "on_changed", "on_activate", NULL
};
static const char * const switch_fields[] = { COMMON_FIELDS, "state", "sensitive", "on_changed", NULL };
static const char * const slider_fields[] = {
  COMMON_FIELDS, "value", "sensitive", "on_changed", "on_drag_begin", "on_drag_end", NULL
};
static const char * const spacer_fields[] = { COMMON_FIELDS, "width", "height", NULL };
static const char * const scroll_fields[] = {
  COMMON_FIELDS, "child", "hscrollbar_policy", "vscrollbar_policy", NULL
};

typedef struct
{
  const char         *constructor;
  const char         *type_name;
  const char * const *fields;
  const EventDef     *events;
} WidgetKind;

static const EventDef no_events[] = { { NULL } };

static const WidgetKind kind_box = { "box", "shell.Box", box_fields, no_events };
static const WidgetKind kind_label = { "label", "shell.Label", label_fields, no_events };
static const WidgetKind kind_icon = { "icon", "shell.Icon", icon_fields, no_events };
static const WidgetKind kind_button = { "button", "shell.Button", button_fields, button_events };
static const WidgetKind kind_entry = { "entry", "shell.Entry", entry_fields, entry_events };
static const WidgetKind kind_switch = { "switch", "shell.Switch", switch_fields, switch_events };
static const WidgetKind kind_slider = { "slider", "shell.Slider", slider_fields, slider_events };
static const WidgetKind kind_spacer = { "spacer", "shell.Spacer", spacer_fields, no_events };
static const WidgetKind kind_scroll = { "scroll", "shell.Scroll", scroll_fields, no_events };

static gboolean
field_present (lua_State  *L,
               int         tbl,
               const char *field)
{
  gboolean present = lua_getfield (L, tbl, field) != LUA_TNIL;

  lua_pop (L, 1);
  return present;
}

static ClutterActor *
new_actor (lua_State        *L,
           const WidgetKind *kind,
           int               opts)
{
  if (kind == &kind_box)
    return g_object_new (ST_TYPE_BOX_LAYOUT, NULL);
  if (kind == &kind_label)
    return g_object_new (ST_TYPE_LABEL, NULL);
  if (kind == &kind_icon)
    return g_object_new (ST_TYPE_ICON, NULL);
  if (kind == &kind_button)
    return g_object_new (ST_TYPE_BUTTON,
                         "toggle-mode", shell_lua_opt_boolean (L, opts, "toggle_mode", FALSE),
                         NULL);
  if (kind == &kind_entry)
    return g_object_new (ST_TYPE_ENTRY, NULL);
  if (kind == &kind_switch)
    return CLUTTER_ACTOR (shell_switch_new (FALSE));
  if (kind == &kind_slider)
    {
      ClutterActor *slider = CLUTTER_ACTOR (shell_slider_new (0));

      /* ShellSlider expands horizontally by itself; the API defaults to no
       * expansion for everything but the spacer (ui.md). */
      clutter_actor_set_x_expand (slider, FALSE);
      return slider;
    }
  if (kind == &kind_spacer)
    return g_object_new (ST_TYPE_WIDGET, "x-expand", TRUE, "y-expand", TRUE, NULL);

  {
    ClutterActor *scroll = CLUTTER_ACTOR (st_scroll_view_new ());

    st_scroll_view_set_policy (ST_SCROLL_VIEW (scroll), ST_POLICY_NEVER, ST_POLICY_AUTOMATIC);
    return scroll;
  }
}

/* Argument checks that cannot be left to the property setters. They run
 * before anything is allocated. */
static void
check_kind_fields (lua_State        *L,
                   const WidgetKind *kind,
                   int               opts)
{
  if (kind == &kind_button)
    {
      gboolean label = field_present (L, opts, "label");
      gboolean icon = field_present (L, opts, "icon_name") || field_present (L, opts, "icon_file");
      gboolean child = field_present (L, opts, "child");

      if (field_present (L, opts, "icon_name") && field_present (L, opts, "icon_file"))
        luaL_error (L, "fields 'icon_name' and 'icon_file' are mutually exclusive");
      if ((label ? 1 : 0) + (icon ? 1 : 0) + (child ? 1 : 0) > 1)
        luaL_error (L, "fields 'label', 'icon_name'/'icon_file' and 'child' are mutually exclusive");
      if (icon)
        {
          const char *name = shell_lua_opt_string (L, opts, "accessible_name", NULL);

          if (name == NULL || name[0] == '\0')
            luaL_error (L, "an icon-only button requires the field 'accessible_name'");
        }
    }
  else if (kind == &kind_icon)
    {
      if (field_present (L, opts, "icon_name") && field_present (L, opts, "icon_file"))
        luaL_error (L, "fields 'icon_name' and 'icon_file' are mutually exclusive");
    }
}

/* upvalue 1: the WidgetKind. Arguments: handle, options. Assigns the fields
 * through the handle (so they are validated like later assignments), adds the
 * children, connects the on_<event> fields. */
static int
apply_fields (lua_State *L)
{
  const WidgetKind *kind = lua_touserdata (L, lua_upvalueindex (1));
  const EventDef *event;

  luaL_checkstack (L, 8, "widget fields");

  lua_pushnil (L);
  while (lua_next (L, 2) != 0)
    {
      const char *key = lua_tostring (L, -2);

      if (strcmp (key, "children") != 0 && strcmp (key, "toggle_mode") != 0 &&
          strncmp (key, "on_", 3) != 0)
        {
          lua_pushvalue (L, 1);
          lua_pushvalue (L, -3);
          lua_pushvalue (L, -3);
          lua_settable (L, -3);
          lua_pop (L, 1);   /* the handle */
        }
      lua_pop (L, 1);       /* the value; the key stays for lua_next */
    }

  if (lua_getfield (L, 2, "children") != LUA_TNIL)
    {
      ClutterActor *box = CLUTTER_ACTOR (shell_lua_handle_check (L, 1, "shell.Box"));
      lua_Unsigned n, i;

      if (!lua_istable (L, -1))
        luaL_error (L, "field 'children' must be a table, got %s", luaL_typename (L, -1));
      n = lua_rawlen (L, -1);
      for (i = 1; i <= n; i++)
        {
          lua_rawgeti (L, -1, (lua_Integer) i);
          box_add (L, box, lua_gettop (L), 0);
          lua_pop (L, 1);
        }
    }
  lua_pop (L, 1);

  for (event = kind->events; event->event != NULL; event++)
    {
      g_autofree char *field = g_strconcat ("on_", event->event, NULL);
      GObject *object = shell_lua_handle_check (L, 1, kind->type_name);
      int ref = shell_lua_opt_callback (L, 2, field, FALSE);
      gpointer instance;
      gulong id;

      if (ref != LUA_NOREF)
        connect_event (L, object, event, ref, &instance, &id);
    }
  return 0;
}

static int
widget_new (lua_State        *L,
            const WidgetKind *kind)
{
  ClutterActor *actor;
  GObject *object;
  int status;

  if (!lua_istable (L, 1))
    return luaL_error (L, "shell.ui.%s expects an options table, got %s",
                       kind->constructor, luaL_typename (L, 1));
  lua_settop (L, 1);

  shell_lua_check_fields (L, 1, kind->fields);
  check_kind_fields (L, kind, 1);
  shell_lua_check_owned_capacity (L);

  actor = new_actor (L, kind, 1);
  object = G_OBJECT (actor);
  set_kind (object, kind->type_name);
  shell_lua_own_object (L, object, &actor_ops);
  shell_lua_api_ui_decorate (L, actor);

  shell_lua_handle_push (L, kind->type_name, object);   /* 2: the handle */
  lua_pushlightuserdata (L, (gpointer) kind);
  lua_pushcclosure (L, apply_fields, 1);
  lua_pushvalue (L, 2);
  lua_pushvalue (L, 1);
  status = lua_pcall (L, 2, 0, 0);
  if (status != LUA_OK)
    {
      /* Do not leave a half built widget behind. */
      shell_lua_destroy_object (L, object);
      return lua_error (L);
    }
  return 1;
}

#define CONSTRUCTOR(fn, kind) \
  static int fn (lua_State *L) { return widget_new (L, &kind); }

CONSTRUCTOR (l_ui_box, kind_box)
CONSTRUCTOR (l_ui_label, kind_label)
CONSTRUCTOR (l_ui_icon, kind_icon)
CONSTRUCTOR (l_ui_button, kind_button)
CONSTRUCTOR (l_ui_entry, kind_entry)
CONSTRUCTOR (l_ui_switch, kind_switch)
CONSTRUCTOR (l_ui_slider, kind_slider)
CONSTRUCTOR (l_ui_spacer, kind_spacer)
CONSTRUCTOR (l_ui_scroll, kind_scroll)

/* ------------------------------------------------------------------------ */
/* Module                                                                    */
/* ------------------------------------------------------------------------ */

static void
shell_lua_api_ui_populate (ShellLuaState *state,
                           lua_State     *L)
{
  static const luaL_Reg constructors[] = {
    { "box", l_ui_box }, { "label", l_ui_label }, { "icon", l_ui_icon },
    { "button", l_ui_button }, { "entry", l_ui_entry }, { "switch", l_ui_switch },
    { "slider", l_ui_slider }, { "spacer", l_ui_spacer }, { "scroll", l_ui_scroll },
    { NULL, NULL }
  };

  shell_lua_api_ui_register_types (L);
  shell_lua_set_contains_func (actor_contains);

  luaL_newlib (L, constructors);
  lua_setfield (L, -2, "ui");
}

static void __attribute__ ((constructor))
shell_lua_api_ui_init (void)
{
  shell_lua_api_register ("ui", shell_lua_api_ui_populate);
}
