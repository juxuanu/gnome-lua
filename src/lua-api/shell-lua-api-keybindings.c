/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.keybindings and the shell.Keybinding handle type
 * (docs/api/keybindings.md, API 1.1): global shortcuts of extensions.
 *
 * A shortcut is a #ShellLuaKeybinding (private, below), an object that the
 * context of the extension owns and that emits "pressed". Two kinds:
 *
 *  - settings-backed (`name`): a key binding of Mutter added with
 *    shell_window_manager_add_keybinding (), like the shell's own shortcuts;
 *    Mutter watches the `as` key of the #GSettings, so a rebinding takes
 *    effect at once. The shell's action modes come from the `modes` of the
 *    call (shell_window_manager_add_keybinding () records them for the
 *    filter of ShellWM::filter-keybinding).
 *  - literal (`accelerators`): one meta_display_grab_accelerator () per
 *    combination, with the action ids dispatched from the display's
 *    "accelerator-activated" signal (one connection for the whole module, a
 *    table action id -> shortcut). The modes are recorded for the external
 *    binding name of each action, as the D-Bus grabs of shell-dbus.c do.
 *
 * Mutter keeps a raw pointer to the shortcut as the user data of a
 * settings-backed binding: it is removed from Mutter before the object is
 * finalized (unregister (), from the destroy op and dispose ()). A shortcut
 * removed from inside its own key handler is released from an idle, because
 * Mutter still holds the binding it is invoking.
 */

#include "config.h"

#include <string.h>

#include <clutter/clutter.h>
#include <meta/display.h>
#include <meta/keybindings.h>
#include <meta/prefs.h>
#include <meta/util.h>

#include "shell-action-modes.h"
#include "shell-global.h"
#include "shell-lua-api-private.h"
#include "ui/main.h"
#include "ui/window-manager.h"

#define KEYBINDING_TYPE "shell.Keybinding"

#define MAX_KEYBINDINGS 16
#define MAX_ACCELERATORS 8
#define MAX_ACCELERATOR_LENGTH 100

/* ------------------------------------------------------------------------ */
/* ShellLuaKeybinding                                                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LUA_KEYBINDING (shell_lua_keybinding_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaKeybinding, shell_lua_keybinding,
                      SHELL, LUA_KEYBINDING, GObject)

enum
{
  PRESSED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0, };

struct _ShellLuaKeybinding
{
  GObject parent;

  char *owner;               /* uuid of the extension, "" for an inspector */
  char *name;                /* settings key; NULL for a literal shortcut */
  GSettings *settings;       /* settings-backed: strong */
  char **accelerators;       /* literal: as given */
  GArray *actions;           /* literal: the guint action ids that are grabbed */
  ShellActionMode modes;

  /* Weak: set when the shortcut is registered, so that giving it back never
   * needs the singletons (dispose runs at shutdown) */
  MetaDisplay *display;
  ShellWindowManager *wm;

  gboolean registered;       /* counts against the limit, known to Mutter */
  gboolean dead;             /* destroyed: never emits */
  guint in_handler;
  guint release_idle;
};

G_DEFINE_FINAL_TYPE (ShellLuaKeybinding, shell_lua_keybinding, G_TYPE_OBJECT)

/* action id -> ShellLuaKeybinding (no reference), literal shortcuts */
static GHashTable *by_action = NULL;
/* lower-cased accelerator -> ShellLuaKeybinding (no reference) */
static GHashTable *by_accelerator = NULL;
/* uuid -> GINT_TO_POINTER (number of registered shortcuts) */
static GHashTable *counts = NULL;

static MetaDisplay *connected_display = NULL;

static void
count_change (const char *owner,
              int         delta)
{
  int n;

  if (counts == NULL)
    counts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  n = GPOINTER_TO_INT (g_hash_table_lookup (counts, owner)) + delta;
  if (n > 0)
    g_hash_table_replace (counts, g_strdup (owner), GINT_TO_POINTER (n));
  else
    g_hash_table_remove (counts, owner);
}

static int
count_get (const char *owner)
{
  return counts != NULL ? GPOINTER_TO_INT (g_hash_table_lookup (counts, owner)) : 0;
}

static ShellWindowManager *
get_window_manager (void)
{
  ShellMain *main_object = shell_main_get_default ();

  return main_object != NULL ? shell_main_get_window_manager (main_object) : NULL;
}

static void
keybinding_press (ShellLuaKeybinding *self)
{
  if (self->dead)
    return;

  g_object_ref (self);
  self->in_handler++;
  g_signal_emit (self, signals[PRESSED], 0);
  self->in_handler--;
  g_object_unref (self);
}

static void
settings_key_handler (MetaDisplay        *display,
                      MetaWindow         *window,
                      const ClutterEvent *event,
                      MetaKeyBinding     *binding,
                      gpointer            user_data)
{
  keybinding_press (user_data);
}

static void
on_accelerator_activated (MetaDisplay        *display,
                          guint               action,
                          ClutterInputDevice *device,
                          guint               timestamp,
                          gpointer            user_data)
{
  ShellLuaKeybinding *self;

  if (by_action == NULL)
    return;

  self = g_hash_table_lookup (by_action, GUINT_TO_POINTER (action));
  if (self != NULL)
    keybinding_press (self);
}

/* One connection to the display for the whole module */
static void
ensure_dispatch (MetaDisplay *display)
{
  if (connected_display == display)
    return;

  g_signal_connect (display, "accelerator-activated",
                    G_CALLBACK (on_accelerator_activated), NULL);
  g_set_weak_pointer (&connected_display, display);
}

/* Gives the combinations or the name back to Mutter; idempotent */
static void
keybinding_unregister (ShellLuaKeybinding *self)
{
  ShellWindowManager *wm = self->wm;
  MetaDisplay *display = self->display;

  if (!self->registered)
    return;
  self->registered = FALSE;

  if (self->name != NULL)
    {
      if (wm != NULL)
        shell_window_manager_remove_keybinding (wm, self->name);
    }
  else if (self->actions != NULL)
    {
      guint i;

      for (i = 0; i < self->actions->len; i++)
        {
          guint action = g_array_index (self->actions, guint, i);
          g_autofree char *binding_name = NULL;

          if (by_action != NULL)
            g_hash_table_remove (by_action, GUINT_TO_POINTER (action));

          if (display == NULL)
            continue;

          binding_name = meta_external_binding_name_for_action (action);
          meta_display_ungrab_accelerator (display, action);
          if (wm != NULL && binding_name != NULL)
            shell_window_manager_allow_keybinding (wm, binding_name, SHELL_ACTION_MODE_NONE);
        }
      g_array_set_size (self->actions, 0);
    }

  if (self->accelerators != NULL && by_accelerator != NULL)
    {
      guint i;

      for (i = 0; self->accelerators[i] != NULL; i++)
        {
          g_autofree char *key = g_ascii_strdown (self->accelerators[i], -1);

          if (g_hash_table_lookup (by_accelerator, key) == self)
            g_hash_table_remove (by_accelerator, key);
        }
    }

  count_change (self->owner, -1);
}

static gboolean
keybinding_release_idle (gpointer data)
{
  ShellLuaKeybinding *self = data;

  self->release_idle = 0;
  keybinding_unregister (self);
  return G_SOURCE_REMOVE;
}

/* ShellLuaOwnOps.destroy: `binding:destroy ()` and disable */
static void
keybinding_destroy (GObject *object)
{
  ShellLuaKeybinding *self = SHELL_LUA_KEYBINDING (object);

  self->dead = TRUE;

  /* Mutter is invoking the handler of this binding right now */
  if (self->in_handler > 0 && self->name != NULL && self->registered)
    {
      if (self->release_idle == 0)
        self->release_idle = g_idle_add_full (G_PRIORITY_DEFAULT,
                                              keybinding_release_idle,
                                              g_object_ref (self), g_object_unref);
      return;
    }

  keybinding_unregister (self);
}

static const ShellLuaOwnOps keybinding_ops = {
  .destroy = keybinding_destroy,
};

static void
shell_lua_keybinding_dispose (GObject *object)
{
  ShellLuaKeybinding *self = SHELL_LUA_KEYBINDING (object);

  self->dead = TRUE;
  keybinding_unregister (self);

  G_OBJECT_CLASS (shell_lua_keybinding_parent_class)->dispose (object);
}

static void
shell_lua_keybinding_finalize (GObject *object)
{
  ShellLuaKeybinding *self = SHELL_LUA_KEYBINDING (object);

  g_clear_weak_pointer (&self->display);
  g_clear_weak_pointer (&self->wm);
  g_clear_pointer (&self->owner, g_free);
  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->accelerators, g_strfreev);
  g_clear_pointer (&self->actions, g_array_unref);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (shell_lua_keybinding_parent_class)->finalize (object);
}

static void
shell_lua_keybinding_class_init (ShellLuaKeybindingClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_lua_keybinding_dispose;
  object_class->finalize = shell_lua_keybinding_finalize;

  signals[PRESSED] = g_signal_new ("pressed",
                                   G_TYPE_FROM_CLASS (klass),
                                   G_SIGNAL_RUN_LAST,
                                   0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
shell_lua_keybinding_init (ShellLuaKeybinding *self)
{
  self->actions = g_array_new (FALSE, FALSE, sizeof (guint));
}

/* ------------------------------------------------------------------------ */
/* Registration with Mutter                                                  */
/* ------------------------------------------------------------------------ */

/* Settings-backed. Returns NULL, or a message (g_malloc'ed) when Mutter or
 * the shell cannot have the shortcut. */
static char *
register_settings_backed (ShellLuaKeybinding *self)
{
  ShellWindowManager *wm = get_window_manager ();
  guint action;

  if (wm == NULL)
    return g_strdup ("the window manager is not available");

  g_set_weak_pointer (&self->wm, wm);

  /* The names of the key bindings are one namespace for Mutter (and it
   * warns when one is added twice) */
  if (meta_prefs_get_keybinding_action (self->name) != META_KEYBINDING_ACTION_NONE)
    return g_strdup_printf ("the key binding name '%s' is already in use", self->name);

  action = shell_window_manager_add_keybinding (wm, self->name, self->settings,
                                                META_KEY_BINDING_IGNORE_AUTOREPEAT,
                                                self->modes,
                                                settings_key_handler, self, NULL);
  if (action == META_KEYBINDING_ACTION_NONE)
    return g_strdup_printf ("the key binding '%s' cannot be added", self->name);

  return NULL;
}

/* Literal. All or nothing. */
static char *
register_literal (ShellLuaKeybinding *self)
{
  ShellWindowManager *wm = get_window_manager ();
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  char *message = NULL;
  guint i;

  if (wm == NULL || display == NULL)
    return g_strdup ("the window manager is not available");

  g_set_weak_pointer (&self->wm, wm);
  g_set_weak_pointer (&self->display, display);

  if (by_action == NULL)
    by_action = g_hash_table_new (g_direct_hash, g_direct_equal);
  if (by_accelerator == NULL)
    by_accelerator = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  ensure_dispatch (display);

  for (i = 0; self->accelerators[i] != NULL; i++)
    {
      const char *accelerator = self->accelerators[i];
      g_autofree char *key = g_ascii_strdown (accelerator, -1);
      g_autofree char *binding_name = NULL;
      guint action;

      if (g_hash_table_contains (by_accelerator, key))
        {
          message = g_strdup_printf ("the accelerator '%s' is already used by a key binding", accelerator);
          break;
        }

      action = meta_display_grab_accelerator (display, accelerator,
                                              META_KEY_BINDING_IGNORE_AUTOREPEAT);
      if (action == META_KEYBINDING_ACTION_NONE)
        {
          message = g_strdup_printf ("the accelerator '%s' cannot be grabbed (it is already taken or not a key)",
                                     accelerator);
          break;
        }

      binding_name = meta_external_binding_name_for_action (action);
      if (binding_name != NULL)
        shell_window_manager_allow_keybinding (wm, binding_name, self->modes);

      g_array_append_val (self->actions, action);
      g_hash_table_insert (by_action, GUINT_TO_POINTER (action), self);
      g_hash_table_insert (by_accelerator, g_steal_pointer (&key), self);
    }

  if (message != NULL)
    {
      /* Release what was taken: unregister () looks at `registered` */
      self->registered = TRUE;
      count_change (self->owner, 1);
      keybinding_unregister (self);
    }

  return message;
}

/* ------------------------------------------------------------------------ */
/* shell.Keybinding                                                          */
/* ------------------------------------------------------------------------ */

static void
get_name (lua_State *L,
          GObject   *object)
{
  ShellLuaKeybinding *self = SHELL_LUA_KEYBINDING (object);

  if (self->name != NULL)
    lua_pushstring (L, self->name);
  else
    lua_pushnil (L);
}

static void
get_accelerators (lua_State *L,
                  GObject   *object)
{
  ShellLuaKeybinding *self = SHELL_LUA_KEYBINDING (object);
  char **accelerators;
  guint i;

  if (self->name != NULL)
    accelerators = g_settings_get_strv (self->settings, self->name);
  else
    accelerators = g_strdupv (self->accelerators);

  lua_newtable (L);
  for (i = 0; accelerators != NULL && accelerators[i] != NULL; i++)
    {
      lua_pushstring (L, accelerators[i]);
      lua_rawseti (L, -2, (lua_Integer) i + 1);
    }
  g_strfreev (accelerators);
}

static void
pressed_cb (ShellLuaKeybinding *self,
            ShellLuaCallback   *cb)
{
  shell_lua_callback_emit (cb, 0, NULL, NULL);
}

static int
l_binding_on_pressed (lua_State *L)
{
  ShellLuaKeybinding *self = SHELL_LUA_KEYBINDING (shell_lua_handle_check (L, 1, KEYBINDING_TYPE));
  ShellLuaCallback *cb;
  gulong id;
  int ref;

  luaL_checktype (L, 2, LUA_TFUNCTION);
  lua_pushvalue (L, 2);
  ref = luaL_ref (L, LUA_REGISTRYINDEX);
  cb = shell_lua_callback_new (L, G_OBJECT (self), ref);
  id = shell_lua_callback_connect (cb, self, "pressed", G_CALLBACK (pressed_cb));

  return shell_lua_callback_push_connection (L, cb, self, id);
}

static const ShellLuaHandleMethod binding_methods[] = {
  { "destroy",    shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { "on_pressed", l_binding_on_pressed, 0 },
  { NULL }
};

static const ShellLuaHandleVirtual binding_virtuals[] = {
  { "name",         get_name,         NULL },
  { "accelerators", get_accelerators, NULL },
  { NULL }
};

static ShellLuaHandleType binding_type = {
  .name = KEYBINDING_TYPE,
  .methods_ex = binding_methods,
  .virtuals = binding_virtuals,
};

/* ------------------------------------------------------------------------ */
/* shell.keybindings.add                                                     */
/* ------------------------------------------------------------------------ */

static const char * const add_fields[] = {
  "name", "settings", "accelerators", "modes", "on_pressed", NULL
};

/* GTK syntax: <Modifier>... then a key name */
static gboolean
accelerator_is_valid (const char *accelerator)
{
  const char *p = accelerator;

  if (accelerator[0] == '\0' || strlen (accelerator) > MAX_ACCELERATOR_LENGTH)
    return FALSE;

  while (*p == '<')
    {
      const char *end = strchr (p, '>');
      const char *q;

      if (end == NULL || end == p + 1)
        return FALSE;
      for (q = p + 1; q < end; q++)
        {
          if (!g_ascii_isalnum (*q))
            return FALSE;
        }
      p = end + 1;
    }

  if (*p == '\0')
    return FALSE;
  for (; *p != '\0'; p++)
    {
      if (*p == '<' || *p == '>' || (guchar) *p <= ' ' || (guchar) *p == 0x7f)
        return FALSE;
    }

  return TRUE;
}

/* The sequence of strings at table[@field], @tbl being an absolute index.
 * Raises on a bad shape. Returns the number of elements; the strings stay on
 * the Lua side. */
static int
check_string_sequence (lua_State  *L,
                       int         tbl,
                       const char *field)
{
  lua_Integer n, i;

  if (lua_getfield (L, tbl, field) != LUA_TTABLE)
    return luaL_error (L, "field '%s' must be a sequence of strings, got %s",
                       field, luaL_typename (L, -1));

  n = luaL_len (L, -1);
  for (i = 1; i <= n; i++)
    {
      if (lua_geti (L, -1, i) != LUA_TSTRING)
        return luaL_error (L, "field '%s': element %d must be a string, got %s",
                           field, (int) i, luaL_typename (L, -1));
      lua_pop (L, 1);
    }
  lua_pop (L, 1);

  return (int) n;
}

static ShellActionMode
check_modes (lua_State *L,
             int        tbl)
{
  ShellActionMode modes = SHELL_ACTION_MODE_NONE;
  lua_Integer i, n;

  if (lua_getfield (L, tbl, "modes") == LUA_TNIL)
    {
      lua_pop (L, 1);
      return SHELL_ACTION_MODE_NORMAL | SHELL_ACTION_MODE_OVERVIEW;
    }
  lua_pop (L, 1);

  n = check_string_sequence (L, tbl, "modes");
  if (n == 0)
    luaL_error (L, "field 'modes' must not be empty");

  lua_getfield (L, tbl, "modes");
  for (i = 1; i <= n; i++)
    {
      const char *mode;

      lua_geti (L, -1, i);
      mode = lua_tostring (L, -1);
      if (strcmp (mode, "normal") == 0)
        modes |= SHELL_ACTION_MODE_NORMAL;
      else if (strcmp (mode, "overview") == 0)
        modes |= SHELL_ACTION_MODE_OVERVIEW;
      else
        luaL_error (L, "field 'modes': unknown value '%s' (expected \"normal\" or \"overview\")", mode);
      lua_pop (L, 1);
    }
  lua_pop (L, 1);

  return modes;
}

/* The settings the call uses, raising when there are none; @tbl is the
 * options table. The result is borrowed (the handle or the extension keeps
 * it alive during the call). */
static GSettings *
check_settings (lua_State *L,
                int        tbl)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);
  GSettings *settings = NULL;

  if (lua_getfield (L, tbl, "settings") != LUA_TNIL)
    {
      settings = G_SETTINGS (shell_lua_handle_check (L, -1, "shell.Settings"));
      lua_pop (L, 1);
      return settings;
    }
  lua_pop (L, 1);

  if (extension != NULL)
    settings = shell_lua_extension_get_settings (extension);
  if (settings == NULL)
    luaL_error (L, "missing required field 'settings' (the extension has no \"settings-schema\")");

  return settings;
}

/* Raises unless @name is a key of type `as` of @settings */
static void
check_settings_key (lua_State  *L,
                    GSettings  *settings,
                    const char *name)
{
  GSettingsSchema *schema = NULL;
  const char *problem = NULL;
  const char *schema_id = "?";
  char *copy = NULL;

  g_object_get (settings, "settings-schema", &schema, NULL);
  if (schema == NULL || !g_settings_schema_has_key (schema, name))
    {
      problem = "no key '%s' in settings schema '%s'";
    }
  else
    {
      GSettingsSchemaKey *key = g_settings_schema_get_key (schema, name);
      gboolean ok = g_variant_type_equal (g_settings_schema_key_get_value_type (key),
                                          G_VARIANT_TYPE ("as"));

      g_settings_schema_key_unref (key);
      if (!ok)
        problem = "key '%s' of settings schema '%s' must be of type 'as'";
    }

  if (problem == NULL)
    {
      g_settings_schema_unref (schema);
      return;
    }

  if (schema != NULL)
    copy = g_strdup (g_settings_schema_get_id (schema));
  schema_id = copy != NULL ? copy : "?";
  lua_pushfstring (L, problem, name, schema_id);
  g_free (copy);
  g_clear_pointer (&schema, g_settings_schema_unref);
  lua_error (L);
}

static int
l_add (lua_State *L)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);
  const char *owner = extension != NULL ? shell_lua_extension_get_uuid (extension) : "";
  ShellLuaKeybinding *self;
  GSettings *settings = NULL;
  ShellActionMode modes;
  const char *name;
  char **accelerators = NULL;
  gboolean has_name, has_accelerators;
  char *message;
  int on_pressed_type, n = 0, ref;

  luaL_checktype (L, 1, LUA_TTABLE);
  shell_lua_check_fields (L, 1, add_fields);

  name = shell_lua_opt_string (L, 1, "name", NULL);
  has_name = name != NULL;
  has_accelerators = lua_getfield (L, 1, "accelerators") != LUA_TNIL;
  lua_pop (L, 1);

  if (has_name == has_accelerators)
    return luaL_error (L, has_name ? "fields 'name' and 'accelerators' are mutually exclusive"
                                   : "one of 'name' and 'accelerators' is required");

  if (has_name)
    {
      if (name[0] == '\0')
        return luaL_error (L, "field 'name' must not be empty");
      settings = check_settings (L, 1);
      check_settings_key (L, settings, name);
    }
  else
    {
      int i;

      if (lua_getfield (L, 1, "settings") != LUA_TNIL)
        return luaL_error (L, "field 'settings' is only valid with 'name'");
      lua_pop (L, 1);

      n = check_string_sequence (L, 1, "accelerators");
      if (n < 1 || n > MAX_ACCELERATORS)
        return luaL_error (L, "field 'accelerators' must hold 1 to %d accelerators, got %d",
                           MAX_ACCELERATORS, n);

      lua_getfield (L, 1, "accelerators");
      for (i = 1; i <= n; i++)
        {
          lua_geti (L, -1, i);
          if (!accelerator_is_valid (lua_tostring (L, -1)))
            return luaL_error (L, "field 'accelerators': '%s' is not an accelerator (e.g. \"<Super>x\")",
                               lua_tostring (L, -1));
          lua_pop (L, 1);
        }
      lua_pop (L, 1);
    }

  modes = check_modes (L, 1);

  lua_getfield (L, 1, "on_pressed");
  on_pressed_type = lua_type (L, -1);
  lua_pop (L, 1);
  if (on_pressed_type == LUA_TNIL)
    return luaL_error (L, "missing required field 'on_pressed' (a function)");
  if (on_pressed_type != LUA_TFUNCTION)
    return luaL_error (L, "field 'on_pressed' must be a function, got %s",
                       lua_typename (L, on_pressed_type));

  shell_lua_check_owned_capacity (L);
  if (count_get (owner) >= MAX_KEYBINDINGS)
    return luaL_error (L, "too many key bindings (limit %d)", MAX_KEYBINDINGS);

  self = g_object_new (SHELL_TYPE_LUA_KEYBINDING, NULL);
  self->owner = g_strdup (owner);
  self->modes = modes;

  if (has_name)
    {
      self->name = g_strdup (name);
      self->settings = g_object_ref (settings);
    }
  else
    {
      int i;

      accelerators = g_new0 (char *, n + 1);
      lua_getfield (L, 1, "accelerators");
      for (i = 0; i < n; i++)
        {
          lua_geti (L, -1, i + 1);
          accelerators[i] = g_strdup (lua_tostring (L, -1));
          lua_pop (L, 1);
        }
      lua_pop (L, 1);
      self->accelerators = accelerators;
    }

  /* From here the Mutter side holds a pointer to @self until it is
   * unregistered */
  if (has_name)
    {
      self->registered = TRUE;
      message = register_settings_backed (self);
      if (message != NULL)
        self->registered = FALSE;
      else
        count_change (self->owner, 1);
    }
  else
    {
      message = register_literal (self);
      if (message == NULL)
        {
          self->registered = TRUE;
          count_change (self->owner, 1);
        }
    }

  if (message != NULL)
    {
      lua_pushnil (L);
      lua_pushstring (L, message);
      g_free (message);
      g_object_unref (self);
      return 2;
    }

  shell_lua_own_object (L, G_OBJECT (self), &keybinding_ops);
  g_object_unref (self);   /* the context holds it */

  lua_getfield (L, 1, "on_pressed");
  ref = luaL_ref (L, LUA_REGISTRYINDEX);
  {
    ShellLuaCallback *cb = shell_lua_callback_new (L, G_OBJECT (self), ref);

    shell_lua_callback_connect (cb, self, "pressed", G_CALLBACK (pressed_cb));
  }

  shell_lua_handle_push (L, KEYBINDING_TYPE, G_OBJECT (self));
  return 1;
}

static const luaL_Reg keybindings_funcs[] = {
  { "add", l_add },
  { NULL, NULL }
};

static void
shell_lua_api_keybindings_register (ShellLuaState *state,
                                    lua_State     *L)
{
  binding_type.gtype = SHELL_TYPE_LUA_KEYBINDING;
  shell_lua_handle_register_type (L, &binding_type);

  lua_newtable (L);
  luaL_setfuncs (L, keybindings_funcs, 0);
  lua_setfield (L, -2, "keybindings");
}

static void __attribute__ ((constructor))
shell_lua_api_keybindings_init (void)
{
  shell_lua_api_register ("keybindings", shell_lua_api_keybindings_register);
}
