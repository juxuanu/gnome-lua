/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * history.c: The history of a text entry, navigated with the Up and Down
 * keys and optionally stored in a GSettings key of the shell's settings.
 * Port of js/misc/history.js.
 *
 * The array of items keeps the JavaScript semantics on purpose: while the
 * user walks through the history, the text they were editing is stored in the
 * slot of the item being left (appending a slot when leaving the "new item"
 * position at the end), exactly like `this._history[this._historyIndex] = text`
 * did.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include "misc/history.h"
#include "shell-global.h"

#define DEFAULT_LIMIT 512

enum
{
  PROP_0,

  PROP_GSETTINGS_KEY,
  PROP_LIMIT,
  PROP_ENTRY,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  CHANGED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellHistoryManager
{
  GObject parent;

  char *key;                  /* nullable: in-memory history */
  uint32_t limit;

  GPtrArray *history;         /* of char *, owned */
  uint32_t history_index;

  ClutterText *entry;         /* nullable, strong */
  ClutterAction *key_controller;
  GSettings *settings;        /* the shell's settings, only with @key */
};

G_DEFINE_FINAL_TYPE (ShellHistoryManager, shell_history_manager, G_TYPE_OBJECT)

/* String.prototype.trim (): white space and line terminators, which is what
 * g_unichar_isspace () covers plus the vertical tab and the byte order mark */
static gboolean
is_js_space (gunichar c)
{
  return g_unichar_isspace (c) || c == '\v' || c == 0xFEFF;
}

static char *
js_trim (const char *str)
{
  const char *start = str;
  const char *end = str + strlen (str);

  while (*start != '\0')
    {
      gunichar c = g_utf8_get_char_validated (start, -1);

      if (c == (gunichar) -1 || c == (gunichar) -2 || !is_js_space (c))
        break;

      start = g_utf8_next_char (start);
    }

  while (end > start)
    {
      const char *prev = g_utf8_find_prev_char (start, end);
      gunichar c;

      if (prev == NULL)
        break;

      c = g_utf8_get_char_validated (prev, end - prev);
      if (c == (gunichar) -1 || c == (gunichar) -2 || !is_js_space (c))
        break;

      end = prev;
    }

  return g_strndup (start, end - start);
}

static GPtrArray *
history_new (void)
{
  return g_ptr_array_new_with_free_func (g_free);
}

static GPtrArray *
history_from_strv (char **strv)
{
  GPtrArray *history = history_new ();
  uint32_t i;

  for (i = 0; strv[i] != NULL; i++)
    g_ptr_array_add (history, g_strdup (strv[i]));

  return history;
}

/* this._history[index] = text */
static void
history_set (GPtrArray  *history,
             uint32_t    index,
             const char *text)
{
  if (index < history->len)
    {
      g_free (g_ptr_array_index (history, index));
      g_ptr_array_index (history, index) = g_strdup (text);
    }
  else
    {
      g_return_if_fail (index == history->len);
      g_ptr_array_add (history, g_strdup (text));
    }
}

static void
index_changed (ShellHistoryManager *self)
{
  const char *current = "";

  /* this._history[this._historyIndex] || '' */
  if (self->history_index < self->history->len)
    {
      const char *item = g_ptr_array_index (self->history, self->history_index);

      if (item != NULL)
        current = item;
    }

  g_signal_emit (self, signals[CHANGED], 0, current);

  if (self->entry != NULL)
    clutter_text_set_text (self->entry, current);
}

static gboolean
set_prev_item (ShellHistoryManager *self,
               const char          *text)
{
  if (self->history_index == 0)
    return FALSE;

  if (text[0] != '\0')
    history_set (self->history, self->history_index, text);

  self->history_index--;
  index_changed (self);
  return TRUE;
}

static gboolean
set_next_item (ShellHistoryManager *self,
               const char          *text)
{
  if (self->history_index >= self->history->len)
    return FALSE;

  if (text[0] != '\0')
    history_set (self->history, self->history_index, text);

  self->history_index++;
  index_changed (self);
  return TRUE;
}

static void
history_changed_cb (GSettings           *settings,
                    const char          *key,
                    ShellHistoryManager *self)
{
  g_auto (GStrv) strv = g_settings_get_strv (settings, self->key);

  g_clear_pointer (&self->history, g_ptr_array_unref);
  self->history = history_from_strv (strv);
  self->history_index = self->history->len;
}

static gboolean
key_press_cb (ClutterKeyController *key_controller,
              ShellHistoryManager  *self)
{
  g_autoptr (ShellHistoryManager) self_ref = g_object_ref (self);
  g_autofree char *text = NULL;
  uint32_t symbol = 0;

  if (self->entry == NULL)
    return CLUTTER_EVENT_PROPAGATE;

  clutter_key_controller_get_key (key_controller, &symbol, NULL, NULL);

  if (symbol == CLUTTER_KEY_Up)
    {
      text = js_trim (clutter_text_get_text (self->entry));
      return set_prev_item (self, text);
    }
  else if (symbol == CLUTTER_KEY_Down)
    {
      text = js_trim (clutter_text_get_text (self->entry));
      return set_next_item (self, text);
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static void
save (ShellHistoryManager *self)
{
  if (self->history->len > self->limit)
    g_ptr_array_remove_range (self->history, 0, self->history->len - self->limit);

  if (self->key != NULL)
    {
      g_autofree const char **strv = NULL;
      uint32_t i;

      strv = g_new0 (const char *, self->history->len + 1);
      for (i = 0; i < self->history->len; i++)
        strv[i] = g_ptr_array_index (self->history, i);

      g_settings_set_strv (self->settings, self->key, strv);
    }
}

/**
 * shell_history_manager_add_item:
 * @self: a #ShellHistoryManager
 * @input: the text to remember
 *
 * Trims @input and, if it is not empty and not already the last item, appends
 * it to the history (removing older copies of it) and stores the history.
 * Resets the navigation to the end of the history.
 *
 * Returns: (transfer full): @input, trimmed
 */
char *
shell_history_manager_add_item (ShellHistoryManager *self,
                                const char          *input)
{
  g_autofree char *trimmed = NULL;

  g_return_val_if_fail (SHELL_IS_HISTORY_MANAGER (self), NULL);
  g_return_val_if_fail (input != NULL, NULL);

  trimmed = js_trim (input);

  if (trimmed[0] != '\0' &&
      (self->history->len == 0 ||
       g_strcmp0 (g_ptr_array_index (self->history, self->history->len - 1),
                  trimmed) != 0))
    {
      uint32_t i = 0;

      while (i < self->history->len)
        {
          if (g_strcmp0 (g_ptr_array_index (self->history, i), trimmed) == 0)
            g_ptr_array_remove_index (self->history, i);
          else
            i++;
        }

      g_ptr_array_add (self->history, g_strdup (trimmed));
      save (self);
    }

  self->history_index = self->history->len;
  return g_steal_pointer (&trimmed);
}

/**
 * shell_history_manager_last_item:
 * @self: a #ShellHistoryManager
 *
 * Moves the navigation to the end of the history, emitting
 * #ShellHistoryManager::changed and updating the entry if it was somewhere
 * else, and returns the most recent item.
 *
 * Returns: (nullable) (transfer none): the most recent item, or %NULL if the
 *   history is empty
 */
const char *
shell_history_manager_last_item (ShellHistoryManager *self)
{
  g_return_val_if_fail (SHELL_IS_HISTORY_MANAGER (self), NULL);

  if (self->history_index != self->history->len)
    {
      self->history_index = self->history->len;
      index_changed (self);
    }

  if (self->history_index == 0)
    return NULL;

  return g_ptr_array_index (self->history, self->history_index - 1);
}

/**
 * shell_history_manager_new:
 * @gsettings_key: (nullable): the key of the shell's settings holding the
 *   history as an array of strings, or %NULL to keep it in memory only
 * @entry: (nullable): the text actor whose Up and Down keys walk through the
 *   history; it shows the item being navigated to
 *
 * Returns: (transfer full): a new #ShellHistoryManager
 */
ShellHistoryManager *
shell_history_manager_new (const char  *gsettings_key,
                           ClutterText *entry)
{
  return g_object_new (SHELL_TYPE_HISTORY_MANAGER,
                       "gsettings-key", gsettings_key,
                       "entry", entry,
                       NULL);
}

static void
shell_history_manager_constructed (GObject *object)
{
  ShellHistoryManager *self = SHELL_HISTORY_MANAGER (object);

  G_OBJECT_CLASS (shell_history_manager_parent_class)->constructed (object);

  self->history_index = 0;

  if (self->key != NULL)
    {
      g_autofree char *detailed_signal = NULL;
      g_auto (GStrv) strv = NULL;

      self->settings = g_object_ref (shell_global_get_settings (shell_global_get ()));

      strv = g_settings_get_strv (self->settings, self->key);
      self->history = history_from_strv (strv);

      detailed_signal = g_strdup_printf ("changed::%s", self->key);
      g_signal_connect_object (self->settings, detailed_signal,
                               G_CALLBACK (history_changed_cb), self, 0);
    }
  else
    {
      self->history = history_new ();
    }

  if (self->entry != NULL)
    {
      self->key_controller = g_object_ref_sink (clutter_key_controller_new (NULL));
      g_signal_connect_object (self->key_controller, "key-press",
                               G_CALLBACK (key_press_cb), self, 0);
      clutter_actor_add_action (CLUTTER_ACTOR (self->entry), self->key_controller);
    }
}

static void
shell_history_manager_dispose (GObject *object)
{
  ShellHistoryManager *self = SHELL_HISTORY_MANAGER (object);

  /* A destroyed entry keeps its address, not its actions */
  if (self->key_controller != NULL && self->entry != NULL &&
      clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self->key_controller)) ==
      CLUTTER_ACTOR (self->entry))
    clutter_actor_remove_action (CLUTTER_ACTOR (self->entry), self->key_controller);

  g_clear_object (&self->key_controller);
  g_clear_object (&self->entry);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (shell_history_manager_parent_class)->dispose (object);
}

static void
shell_history_manager_finalize (GObject *object)
{
  ShellHistoryManager *self = SHELL_HISTORY_MANAGER (object);

  g_clear_pointer (&self->history, g_ptr_array_unref);
  g_clear_pointer (&self->key, g_free);

  G_OBJECT_CLASS (shell_history_manager_parent_class)->finalize (object);
}

static void
shell_history_manager_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  ShellHistoryManager *self = SHELL_HISTORY_MANAGER (object);

  switch (prop_id)
    {
    case PROP_GSETTINGS_KEY:
      g_value_set_string (value, self->key);
      break;

    case PROP_LIMIT:
      g_value_set_uint (value, self->limit);
      break;

    case PROP_ENTRY:
      g_value_set_object (value, self->entry);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_history_manager_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  ShellHistoryManager *self = SHELL_HISTORY_MANAGER (object);

  switch (prop_id)
    {
    case PROP_GSETTINGS_KEY:
      g_free (self->key);
      self->key = g_value_dup_string (value);
      break;

    case PROP_LIMIT:
      self->limit = g_value_get_uint (value);
      break;

    case PROP_ENTRY:
      g_clear_object (&self->entry);
      self->entry = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_history_manager_class_init (ShellHistoryManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_history_manager_constructed;
  object_class->dispose = shell_history_manager_dispose;
  object_class->finalize = shell_history_manager_finalize;
  object_class->get_property = shell_history_manager_get_property;
  object_class->set_property = shell_history_manager_set_property;

  /**
   * ShellHistoryManager:gsettings-key:
   *
   * The key of the shell's settings (`global.settings`) that stores the
   * history, or %NULL to keep it in memory only.
   */
  props[PROP_GSETTINGS_KEY] =
    g_param_spec_string ("gsettings-key", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellHistoryManager:limit:
   *
   * The maximum number of items that are kept when the history is stored.
   */
  props[PROP_LIMIT] =
    g_param_spec_uint ("limit", NULL, NULL,
                       0, G_MAXUINT, DEFAULT_LIMIT,
                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellHistoryManager:entry:
   *
   * The text actor that is navigated with the Up and Down keys. Pass
   * `entry.clutter_text` for an #StEntry.
   */
  props[PROP_ENTRY] =
    g_param_spec_object ("entry", NULL, NULL,
                         CLUTTER_TYPE_TEXT,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellHistoryManager::changed:
   * @self: the #ShellHistoryManager
   * @current: the item that is now current, or the empty string
   *
   * Emitted when the navigation moves to another item.
   */
  signals[CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
}

static void
shell_history_manager_init (ShellHistoryManager *self)
{
}
