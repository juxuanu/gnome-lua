/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * input-method.c: The input method of the compositor, backed by an IBus input
 * context. Port of js/misc/inputMethod.js.
 *
 * The input method needs two facts that live in the still-JS part of the
 * shell, which pushes them instead of being pulled: whether an input source
 * is current (#ShellInputMethod:has-input-source, set by the input source
 * manager) and whether the on-screen keyboard is visible
 * (#ShellInputMethod:osk-visible, set by the on-screen keyboard manager).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <ibus.h>
#include <meta/display.h>

#include "misc/input-method.h"
#include "shell-global.h"

#define HIDE_PANEL_TIME 50

enum
{
  PROP_0,

  PROP_CURRENT_FOCUS,
  PROP_HAS_INPUT_SOURCE,
  PROP_OSK_VISIBLE,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  SURROUNDING_TEXT_SET,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellInputMethod
{
  ClutterInputMethod parent;

  uint32_t hints;     /* IBusInputHints */
  uint32_t purpose;   /* IBusInputPurpose */
  ClutterInputFocus *current_focus;

  char *preedit_str;
  unsigned int preedit_pos;
  unsigned int preedit_anchor;
  gboolean preedit_visible;
  ClutterPreeditResetMode preedit_commit_mode;
  GArray *preedit_hints;   /* ClutterPreeditAttribute */

  guint hide_panel_id;

  /* NULL when the focus has no surrounding text support */
  char *surrounding_text;
  unsigned int surrounding_text_cursor;
  unsigned int surrounding_text_anchor;

  IBusBus *ibus;
  IBusInputContext *context;
  GCancellable *cancellable;

  gboolean has_cursor_rect;
  int cursor_x;
  int cursor_y;
  int cursor_width;
  int cursor_height;

  gboolean has_input_source;
  gboolean osk_visible;
  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellInputMethod, shell_input_method, CLUTTER_TYPE_INPUT_METHOD)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (IBusText, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (IBusInputContext, g_object_unref)

static void update_capabilities (ShellInputMethod *self);
static void clear (ShellInputMethod *self);

static gboolean
have_require_surrounding_text (void)
{
  static gsize have = 0;

  if (g_once_init_enter (&have))
    {
      guint id = g_signal_lookup ("require-surrounding-text", IBUS_TYPE_INPUT_CONTEXT);

      g_once_init_leave (&have, id != 0 ? 1 : 2);
    }

  return have == 1;
}

static void
update_capabilities (ShellInputMethod *self)
{
  uint32_t caps = IBUS_CAP_PREEDIT_TEXT | IBUS_CAP_FOCUS;

  if (self->surrounding_text != NULL)
    caps |= IBUS_CAP_SURROUNDING_TEXT;

  if (self->osk_visible)
    caps |= IBUS_CAP_OSK;

  if (self->context != NULL)
    ibus_input_context_set_capabilities (self->context, caps);
}

static void
on_can_show_preedit_changed (GObject    *object,
                             GParamSpec *pspec,
                             gpointer    user_data)
{
  update_capabilities (SHELL_INPUT_METHOD (object));
}

static void
emit_request_surrounding (ShellInputMethod *self)
{
  if (ibus_input_context_needs_surrounding_text (self->context))
    g_signal_emit_by_name (self, "request-surrounding");
}

static ClutterPreeditAttribute *
translate_preedit_attributes (IBusAttrList *ibus_attributes,
                              unsigned int *n_attributes)
{
  g_autoptr (GArray) attrs =
    g_array_new (FALSE, FALSE, sizeof (ClutterPreeditAttribute));
  IBusAttribute *ibus_attr;
  unsigned int i;

  for (i = 0;
       ibus_attributes != NULL &&
       (ibus_attr = ibus_attr_list_get (ibus_attributes, i)) != NULL;
       i++)
    {
      ClutterPreeditAttribute attr;

      /* Ignore all explicit style attributes */
      if (ibus_attribute_get_attr_type (ibus_attr) != IBUS_ATTR_TYPE_HINT)
        continue;

      switch (ibus_attribute_get_value (ibus_attr))
        {
        case IBUS_ATTR_PREEDIT_DEFAULT:
          attr.hint = CLUTTER_PREEDIT_STYLE_NONE;
          break;
        case IBUS_ATTR_PREEDIT_WHOLE:
          attr.hint = CLUTTER_PREEDIT_STYLE_WHOLE;
          break;
        case IBUS_ATTR_PREEDIT_SELECTION:
          attr.hint = CLUTTER_PREEDIT_STYLE_SELECTION;
          break;
        case IBUS_ATTR_PREEDIT_PREDICTION:
          attr.hint = CLUTTER_PREEDIT_STYLE_PREDICTION;
          break;
        case IBUS_ATTR_PREEDIT_PREFIX:
          attr.hint = CLUTTER_PREEDIT_STYLE_PREFIX;
          break;
        case IBUS_ATTR_PREEDIT_SUFFIX:
          attr.hint = CLUTTER_PREEDIT_STYLE_SUFFIX;
          break;
        case IBUS_ATTR_PREEDIT_ERROR_SPELLING:
          attr.hint = CLUTTER_PREEDIT_STYLE_SPELLING_ERROR;
          break;
        case IBUS_ATTR_PREEDIT_ERROR_COMPOSE:
          attr.hint = CLUTTER_PREEDIT_STYLE_COMPOSE_ERROR;
          break;
        default:
          continue;
        }

      attr.start = ibus_attribute_get_start_index (ibus_attr);
      attr.end = ibus_attribute_get_end_index (ibus_attr);
      g_array_append_val (attrs, attr);
    }

  *n_attributes = attrs->len;
  return (ClutterPreeditAttribute *) g_array_free (g_steal_pointer (&attrs), FALSE);
}

static void
on_commit_text (IBusInputContext *context,
                IBusText         *text,
                ShellInputMethod *self)
{
  clutter_input_method_commit (CLUTTER_INPUT_METHOD (self),
                               ibus_text_get_text (text));
}

static void
on_require_surrounding_text (IBusInputContext *context,
                             ShellInputMethod *self)
{
  clutter_input_method_request_surrounding (CLUTTER_INPUT_METHOD (self));
}

static void
on_delete_surrounding_text (IBusInputContext *context,
                            int               offset,
                            unsigned int      nchars,
                            ShellInputMethod *self)
{
  if (self->surrounding_text == NULL)
    {
      g_message ("input-method engines should not call the "
                 "delete-surrounding-text API in case the input context has "
                 "no SURROUNDING_TEXT capability.");
      return;
    }

  clutter_input_method_delete_surrounding (CLUTTER_INPUT_METHOD (self),
                                           offset, nchars);
}

static void
on_update_preedit_text (IBusInputContext *context,
                        IBusText         *text,
                        unsigned int      pos,
                        gboolean          visible,
                        unsigned int      mode,
                        ShellInputMethod *self)
{
  ClutterInputMethod *im = CLUTTER_INPUT_METHOD (self);
  g_autoptr (GArray) hints = NULL;
  g_autofree ClutterPreeditAttribute *attrs = NULL;
  const char *preedit;
  unsigned int n_attrs = 0;
  unsigned int anchor = pos;

  if (text == NULL)
    return;

  preedit = ibus_text_get_text (text);
  if (preedit != NULL && preedit[0] == '\0')
    preedit = NULL;

  attrs = translate_preedit_attributes (ibus_text_get_attributes (text), &n_attrs);

  if (visible)
    {
      clutter_input_method_set_preedit_text_with_attrs (im, preedit, pos, anchor,
                                                        mode, attrs, n_attrs);
    }
  else if (self->preedit_visible)
    {
      clutter_input_method_set_preedit_text (im, NULL, pos, anchor, mode);
    }

  g_free (self->preedit_str);
  self->preedit_str = g_strdup (preedit);
  self->preedit_pos = pos;
  self->preedit_anchor = anchor;
  self->preedit_visible = visible;
  self->preedit_commit_mode = mode;

  g_clear_pointer (&self->preedit_hints, g_array_unref);
  hints = g_array_sized_new (FALSE, FALSE, sizeof (ClutterPreeditAttribute), n_attrs);
  if (n_attrs > 0)
    g_array_append_vals (hints, attrs, n_attrs);
  self->preedit_hints = g_steal_pointer (&hints);
}

static void
on_show_preedit_text (IBusInputContext *context,
                      ShellInputMethod *self)
{
  self->preedit_visible = TRUE;

  clutter_input_method_set_preedit_text_with_attrs (
    CLUTTER_INPUT_METHOD (self),
    self->preedit_str, self->preedit_pos, self->preedit_anchor,
    self->preedit_commit_mode,
    self->preedit_hints != NULL ?
    (ClutterPreeditAttribute *) self->preedit_hints->data : NULL,
    self->preedit_hints != NULL ? self->preedit_hints->len : 0);
}

static void
on_hide_preedit_text (IBusInputContext *context,
                      ShellInputMethod *self)
{
  clutter_input_method_set_preedit_text (CLUTTER_INPUT_METHOD (self),
                                         NULL,
                                         self->preedit_pos,
                                         self->preedit_anchor,
                                         self->preedit_commit_mode);
  self->preedit_visible = FALSE;
}

static void
on_forward_key_event (IBusInputContext *context,
                      unsigned int      keyval,
                      unsigned int      keycode,
                      unsigned int      state,
                      ShellInputMethod *self)
{
  gboolean press = (state & IBUS_RELEASE_MASK) == 0;
  const ClutterEvent *current_event = clutter_get_current_event ();
  uint32_t time;

  state &= ~IBUS_RELEASE_MASK;

  if (current_event != NULL)
    time = clutter_event_get_time (current_event);
  else
    time = meta_display_get_current_time_roundtrip (shell_global_get_display (shell_global_get ()));

  clutter_input_method_forward_key (CLUTTER_INPUT_METHOD (self),
                                    keyval, keycode + 8,
                                    state & CLUTTER_MODIFIER_MASK,
                                    time, press);
}

static void
on_context_destroy (IBusInputContext *context,
                    ShellInputMethod *self)
{
  clear (self);
}

static void
on_input_context_created (GObject      *source,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  g_autoptr (ShellInputMethod) self = user_data;
  g_autoptr (GError) error = NULL;
  IBusInputContext *context;

  context = ibus_bus_create_input_context_async_finish (IBUS_BUS (source), res, &error);
  if (context == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          if (g_dbus_error_is_remote_error (error))
            g_dbus_error_strip_remote_error (error);
          g_warning ("%s", error->message);
          clear (self);
        }
      return;
    }

  if (self->shutting_down)
    {
      g_object_unref (context);
      return;
    }

  if (self->context != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->context, self);
      g_clear_object (&self->context);
    }
  self->context = context;

  ibus_input_context_set_client_commit_preedit (context, TRUE);
  ibus_input_context_set_preedit_format (context, IBUS_PREEDIT_FORMAT_HINT);

  g_signal_connect_object (context, "commit-text",
                           G_CALLBACK (on_commit_text), self, 0);
  g_signal_connect_object (context, "delete-surrounding-text",
                           G_CALLBACK (on_delete_surrounding_text), self, 0);
  g_signal_connect_object (context, "update-preedit-text-with-mode",
                           G_CALLBACK (on_update_preedit_text), self, 0);
  g_signal_connect_object (context, "show-preedit-text",
                           G_CALLBACK (on_show_preedit_text), self, 0);
  g_signal_connect_object (context, "hide-preedit-text",
                           G_CALLBACK (on_hide_preedit_text), self, 0);
  g_signal_connect_object (context, "forward-key-event",
                           G_CALLBACK (on_forward_key_event), self, 0);
  g_signal_connect_object (context, "destroy",
                           G_CALLBACK (on_context_destroy), self, 0);

  if (have_require_surrounding_text ())
    g_signal_connect_object (context, "require-surrounding-text",
                             G_CALLBACK (on_require_surrounding_text), self, 0);

  update_capabilities (self);
}

static void
on_disconnected (IBusBus          *bus,
                 ShellInputMethod *self)
{
  clear (self);
}

static void
on_connected (IBusBus          *bus,
              ShellInputMethod *self)
{
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  ibus_bus_create_input_context_async (self->ibus, "gnome-shell", -1,
                                       self->cancellable,
                                       on_input_context_created,
                                       g_object_ref (self));
}

static void
clear (ShellInputMethod *self)
{
  g_autoptr (IBusInputContext) context = g_steal_pointer (&self->context);

  if (context != NULL)
    g_signal_handlers_disconnect_by_data (context, self);

  if (self->cancellable != NULL)
    {
      g_cancellable_cancel (self->cancellable);
      g_clear_object (&self->cancellable);
    }

  self->hints = 0;
  self->purpose = 0;
  g_free (self->preedit_str);
  self->preedit_str = g_strdup ("");
  self->preedit_pos = 0;
  self->preedit_anchor = 0;
  self->preedit_visible = FALSE;
}

static void
full_reset (ShellInputMethod *self)
{
  ibus_input_context_set_content_type (self->context, 0, 0);
  ibus_input_context_set_cursor_location (self->context, 0, 0, 0, 0);
  ibus_input_context_reset (self->context);
}

/**
 * shell_input_method_update:
 * @self: a #ShellInputMethod
 *
 * Sends the capabilities, content type, cursor location and a surrounding
 * text request to the input context again.
 */
void
shell_input_method_update (ShellInputMethod *self)
{
  g_return_if_fail (SHELL_IS_INPUT_METHOD (self));

  if (self->context == NULL)
    return;

  update_capabilities (self);
  ibus_input_context_set_content_type (self->context, self->purpose, self->hints);
  if (self->has_cursor_rect)
    ibus_input_context_set_cursor_location (self->context,
                                            self->cursor_x, self->cursor_y,
                                            self->cursor_width, self->cursor_height);
  emit_request_surrounding (self);
}

static gboolean
hide_panel_timeout (gpointer user_data)
{
  ShellInputMethod *self = user_data;

  self->hide_panel_id = 0;
  clutter_input_method_set_input_panel_state (CLUTTER_INPUT_METHOD (self),
                                              CLUTTER_INPUT_PANEL_STATE_OFF);

  return G_SOURCE_REMOVE;
}

static void
shell_input_method_focus_in (ClutterInputMethod *im,
                             ClutterInputFocus  *focus)
{
  ShellInputMethod *self = SHELL_INPUT_METHOD (im);

  if (g_set_object (&self->current_focus, focus))
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CURRENT_FOCUS]);

  if (self->context != NULL)
    {
      shell_input_method_update (self);
      ibus_input_context_focus_in (self->context);
      emit_request_surrounding (self);
    }

  g_clear_handle_id (&self->hide_panel_id, g_source_remove);
}

static void
shell_input_method_focus_out (ClutterInputMethod *im)
{
  ShellInputMethod *self = SHELL_INPUT_METHOD (im);

  if (self->current_focus != NULL)
    {
      g_clear_object (&self->current_focus);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CURRENT_FOCUS]);
    }

  if (self->context != NULL)
    {
      full_reset (self);
      ibus_input_context_focus_out (self->context);
    }

  if (self->preedit_str != NULL && self->preedit_str[0] != '\0' &&
      self->preedit_visible)
    {
      /* Unset any preedit text */
      clutter_input_method_set_preedit_text (im, NULL, 0, 0,
                                             self->preedit_commit_mode);
      g_clear_pointer (&self->preedit_str, g_free);
    }

  g_clear_handle_id (&self->hide_panel_id, g_source_remove);
  if (!self->shutting_down)
    self->hide_panel_id = g_timeout_add (HIDE_PANEL_TIME, hide_panel_timeout, self);
}

static void
shell_input_method_reset (ClutterInputMethod *im)
{
  ShellInputMethod *self = SHELL_INPUT_METHOD (im);

  if (self->context != NULL)
    {
      ibus_input_context_reset (self->context);
      emit_request_surrounding (self);
    }

  g_clear_pointer (&self->surrounding_text, g_free);
  self->surrounding_text_cursor = 0;
  self->surrounding_text_anchor = 0;
  g_clear_pointer (&self->preedit_str, g_free);
}

static void
shell_input_method_set_cursor_location (ClutterInputMethod    *im,
                                        const graphene_rect_t *rect)
{
  ShellInputMethod *self = SHELL_INPUT_METHOD (im);

  if (self->context == NULL)
    return;

  self->has_cursor_rect = TRUE;
  self->cursor_x = (int) rect->origin.x;
  self->cursor_y = (int) rect->origin.y;
  self->cursor_width = (int) rect->size.width;
  self->cursor_height = (int) rect->size.height;

  ibus_input_context_set_cursor_location (self->context,
                                          self->cursor_x, self->cursor_y,
                                          self->cursor_width, self->cursor_height);
  emit_request_surrounding (self);
}

static void
shell_input_method_set_surrounding (ClutterInputMethod *im,
                                    const char         *text,
                                    unsigned int        cursor,
                                    unsigned int        anchor)
{
  ShellInputMethod *self = SHELL_INPUT_METHOD (im);
  g_autoptr (IBusText) ibus_text = NULL;
  /* If the previous input context supports the surrounding-text feature */
  gboolean prev_has_surrounding = self->surrounding_text != NULL;
  /* If the current input context supports the surrounding-text feature */
  gboolean now_has_surrounding = text != NULL;
  /* If the SURROUNDING_TEXT capability is changed */
  gboolean update_caps = prev_has_surrounding != now_has_surrounding;

  g_free (self->surrounding_text);
  self->surrounding_text = g_strdup (text);
  self->surrounding_text_cursor = cursor;
  self->surrounding_text_anchor = anchor;
  g_signal_emit (self, signals[SURROUNDING_TEXT_SET], 0);

  if (self->context == NULL || text == NULL)
    return;

  ibus_text = ibus_text_new_from_string (text);
  if (g_object_is_floating (ibus_text))
    g_object_ref_sink (ibus_text);

  if (update_caps)
    update_capabilities (self);

  /* Call set_surrounding_text () after set_capabilities () */
  ibus_input_context_set_surrounding_text (self->context, ibus_text, cursor, anchor);
}

static void
shell_input_method_update_content_hints (ClutterInputMethod           *im,
                                         ClutterInputContentHintFlags  hints)
{
  ShellInputMethod *self = SHELL_INPUT_METHOD (im);
  uint32_t ibus_hints = 0;

  if (hints & CLUTTER_INPUT_CONTENT_HINT_COMPLETION)
    ibus_hints |= IBUS_INPUT_HINT_WORD_COMPLETION;
  if (hints & CLUTTER_INPUT_CONTENT_HINT_SPELLCHECK)
    ibus_hints |= IBUS_INPUT_HINT_SPELLCHECK;
  if (hints & CLUTTER_INPUT_CONTENT_HINT_AUTO_CAPITALIZATION)
    ibus_hints |= IBUS_INPUT_HINT_UPPERCASE_SENTENCES;
  if (hints & CLUTTER_INPUT_CONTENT_HINT_LOWERCASE)
    ibus_hints |= IBUS_INPUT_HINT_LOWERCASE;
  if (hints & CLUTTER_INPUT_CONTENT_HINT_UPPERCASE)
    ibus_hints |= IBUS_INPUT_HINT_UPPERCASE_CHARS;
  if (hints & CLUTTER_INPUT_CONTENT_HINT_TITLECASE)
    ibus_hints |= IBUS_INPUT_HINT_UPPERCASE_WORDS;
  if (hints & CLUTTER_INPUT_CONTENT_HINT_SENSITIVE_DATA)
    ibus_hints |= IBUS_INPUT_HINT_PRIVATE;
  if (hints & CLUTTER_INPUT_CONTENT_HINT_HIDDEN_TEXT)
    ibus_hints |= IBUS_INPUT_HINT_HIDDEN_TEXT;
  if (hints & CLUTTER_INPUT_CONTENT_HINT_NO_EMOJI)
    ibus_hints |= IBUS_INPUT_HINT_NO_EMOJI;

  self->hints = ibus_hints;
  if (self->context != NULL)
    ibus_input_context_set_content_type (self->context, self->purpose, self->hints);
}

static void
shell_input_method_update_content_purpose (ClutterInputMethod         *im,
                                           ClutterInputContentPurpose  purpose)
{
  ShellInputMethod *self = SHELL_INPUT_METHOD (im);
  uint32_t ibus_purpose = 0;

  switch (purpose)
    {
    case CLUTTER_INPUT_CONTENT_PURPOSE_NORMAL:
      ibus_purpose = IBUS_INPUT_PURPOSE_FREE_FORM;
      break;
    case CLUTTER_INPUT_CONTENT_PURPOSE_ALPHA:
      ibus_purpose = IBUS_INPUT_PURPOSE_ALPHA;
      break;
    case CLUTTER_INPUT_CONTENT_PURPOSE_DIGITS:
      ibus_purpose = IBUS_INPUT_PURPOSE_DIGITS;
      break;
    case CLUTTER_INPUT_CONTENT_PURPOSE_NUMBER:
      ibus_purpose = IBUS_INPUT_PURPOSE_NUMBER;
      break;
    case CLUTTER_INPUT_CONTENT_PURPOSE_PHONE:
      ibus_purpose = IBUS_INPUT_PURPOSE_PHONE;
      break;
    case CLUTTER_INPUT_CONTENT_PURPOSE_URL:
      ibus_purpose = IBUS_INPUT_PURPOSE_URL;
      break;
    case CLUTTER_INPUT_CONTENT_PURPOSE_EMAIL:
      ibus_purpose = IBUS_INPUT_PURPOSE_EMAIL;
      break;
    case CLUTTER_INPUT_CONTENT_PURPOSE_NAME:
      ibus_purpose = IBUS_INPUT_PURPOSE_NAME;
      break;
    case CLUTTER_INPUT_CONTENT_PURPOSE_PASSWORD:
      ibus_purpose = IBUS_INPUT_PURPOSE_PASSWORD;
      break;
    case CLUTTER_INPUT_CONTENT_PURPOSE_TERMINAL:
      ibus_purpose = IBUS_INPUT_PURPOSE_TERMINAL;
      break;
    default:
      break;
    }

  self->purpose = ibus_purpose;
  if (self->context != NULL)
    ibus_input_context_set_content_type (self->context, self->purpose, self->hints);
}

typedef struct
{
  ShellInputMethod *self;
  ClutterEvent *event;
} KeyEventData;

static void
key_event_data_free (KeyEventData *data)
{
  g_clear_pointer (&data->event, clutter_event_free);
  g_object_unref (data->self);
  g_free (data);
}

static void
on_process_key_event_done (GObject      *source,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  KeyEventData *data = user_data;
  ShellInputMethod *self = data->self;
  g_autoptr (GError) error = NULL;
  gboolean retval;

  if (source != (GObject *) self->context)
    {
      key_event_data_free (data);
      return;
    }

  retval = ibus_input_context_process_key_event_async_finish (IBUS_INPUT_CONTEXT (source),
                                                              res, &error);
  if (error == NULL)
    {
      clutter_input_method_notify_key_event (CLUTTER_INPUT_METHOD (self),
                                             data->event, retval);
    }
  else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_message ("Error processing key on IM: %s", error->message);
    }

  key_event_data_free (data);
}

static gboolean
shell_input_method_filter_key_event (ClutterInputMethod *im,
                                     const ClutterEvent *event)
{
  ShellInputMethod *self = SHELL_INPUT_METHOD (im);
  KeyEventData *data;
  uint32_t state;

  if (self->context == NULL)
    return FALSE;
  if (!self->has_input_source)
    return FALSE;

  state = clutter_event_get_state (event);
  if (state & IBUS_IGNORED_MASK)
    return FALSE;

  if (clutter_event_type (event) == CLUTTER_KEY_RELEASE)
    state |= IBUS_RELEASE_MASK;

  data = g_new0 (KeyEventData, 1);
  data->self = g_object_ref (self);
  data->event = clutter_event_copy (event);

  ibus_input_context_process_key_event_async (
    self->context,
    clutter_event_get_key_symbol (event),
    clutter_event_get_key_code (event) - 8, /* Convert XKB keycodes to evcodes */
    state, -1, self->cancellable,
    on_process_key_event_done, data);

  return TRUE;
}

/**
 * shell_input_method_get_current_focus:
 * @self: a #ShellInputMethod
 *
 * Returns: (transfer none) (nullable): the #ClutterInputFocus that the input
 *   method is currently attached to
 */
ClutterInputFocus *
shell_input_method_get_current_focus (ShellInputMethod *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_METHOD (self), NULL);

  return self->current_focus;
}

/**
 * shell_input_method_get_has_input_source:
 * @self: a #ShellInputMethod
 *
 * Returns: whether an input source was made current
 */
gboolean
shell_input_method_get_has_input_source (ShellInputMethod *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_METHOD (self), FALSE);

  return self->has_input_source;
}

/**
 * shell_input_method_set_has_input_source:
 * @self: a #ShellInputMethod
 * @has_input_source: whether an input source is current
 *
 * Keys are only filtered through IBus once an input source is current.
 */
void
shell_input_method_set_has_input_source (ShellInputMethod *self,
                                         gboolean          has_input_source)
{
  g_return_if_fail (SHELL_IS_INPUT_METHOD (self));

  has_input_source = !!has_input_source;
  if (self->has_input_source == has_input_source)
    return;

  self->has_input_source = has_input_source;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HAS_INPUT_SOURCE]);
}

/**
 * shell_input_method_get_osk_visible:
 * @self: a #ShellInputMethod
 *
 * Returns: whether the on-screen keyboard is visible
 */
gboolean
shell_input_method_get_osk_visible (ShellInputMethod *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_METHOD (self), FALSE);

  return self->osk_visible;
}

/**
 * shell_input_method_set_osk_visible:
 * @self: a #ShellInputMethod
 * @osk_visible: whether the on-screen keyboard is visible
 *
 * Tells the input context whether the on-screen keyboard owns the UI. The
 * capabilities are sent again even if the value did not change.
 */
void
shell_input_method_set_osk_visible (ShellInputMethod *self,
                                    gboolean          osk_visible)
{
  g_return_if_fail (SHELL_IS_INPUT_METHOD (self));

  osk_visible = !!osk_visible;
  if (self->osk_visible != osk_visible)
    {
      self->osk_visible = osk_visible;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OSK_VISIBLE]);
    }

  update_capabilities (self);
}

/**
 * shell_input_method_get_surrounding_text:
 * @self: a #ShellInputMethod
 * @text: (out) (optional) (nullable) (transfer none): return location for the
 *   surrounding text, or %NULL if the focus has none
 * @cursor: (out) (optional): return location for the cursor position, in
 *   characters
 * @anchor: (out) (optional): return location for the selection anchor, in
 *   characters
 *
 * Gets the surrounding text last set by the focus.
 */
void
shell_input_method_get_surrounding_text (ShellInputMethod  *self,
                                         char             **text,
                                         unsigned int      *cursor,
                                         unsigned int      *anchor)
{
  g_return_if_fail (SHELL_IS_INPUT_METHOD (self));

  if (text != NULL)
    *text = self->surrounding_text;
  if (cursor != NULL)
    *cursor = self->surrounding_text_cursor;
  if (anchor != NULL)
    *anchor = self->surrounding_text_anchor;
}

/**
 * shell_input_method_has_preedit:
 * @self: a #ShellInputMethod
 *
 * Returns: whether a non-empty preedit text is currently shown
 */
gboolean
shell_input_method_has_preedit (ShellInputMethod *self)
{
  g_return_val_if_fail (SHELL_IS_INPUT_METHOD (self), FALSE);

  return self->preedit_visible &&
         self->preedit_str != NULL &&
         self->preedit_str[0] != '\0';
}

static void
on_virtual_key_release_done (GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  GTask *task = user_data;
  ShellInputMethod *self = g_task_get_source_object (task);
  g_autoptr (GError) error = NULL;

  ibus_input_context_process_key_event_async_finish (IBUS_INPUT_CONTEXT (source),
                                                     res, &error);
  if (self->shutting_down)
    {
      g_object_unref (task);
      return;
    }

  g_task_return_boolean (task, error == NULL);
  g_object_unref (task);
}

static void
on_virtual_key_press_done (GObject      *source,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  GTask *task = user_data;
  ShellInputMethod *self = g_task_get_source_object (task);
  unsigned int keyval = GPOINTER_TO_UINT (g_task_get_task_data (task));
  g_autoptr (GError) error = NULL;
  gboolean handled;

  handled = ibus_input_context_process_key_event_async_finish (IBUS_INPUT_CONTEXT (source),
                                                               res, &error);

  /* Nothing may complete into JS while the shell is going away */
  if (self->shutting_down)
    {
      g_object_unref (task);
      return;
    }

  if (error != NULL || !handled || self->context == NULL)
    {
      g_task_return_boolean (task, FALSE);
      g_object_unref (task);
      return;
    }

  ibus_input_context_process_key_event_async (self->context, keyval, 0,
                                              IBUS_RELEASE_MASK, -1,
                                              g_task_get_cancellable (task),
                                              on_virtual_key_release_done, task);
}

/**
 * shell_input_method_handle_virtual_key_async:
 * @self: a #ShellInputMethod
 * @keyval: the keyval to press and release
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the key was processed
 * @user_data: (closure): data for @callback
 *
 * Sends a key press and release to the input method engine without going
 * through the compositor, e.g. from the on-screen keyboard.
 */
void
shell_input_method_handle_virtual_key_async (ShellInputMethod    *self,
                                             unsigned int         keyval,
                                             GCancellable        *cancellable,
                                             GAsyncReadyCallback  callback,
                                             gpointer             user_data)
{
  GTask *task;

  g_return_if_fail (SHELL_IS_INPUT_METHOD (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, shell_input_method_handle_virtual_key_async);
  g_task_set_check_cancellable (task, FALSE);
  g_task_set_task_data (task, GUINT_TO_POINTER (keyval), NULL);

  if (self->context == NULL)
    {
      g_task_return_boolean (task, FALSE);
      g_object_unref (task);
      return;
    }

  ibus_input_context_process_key_event_async (self->context, keyval, 0, 0, -1,
                                              cancellable,
                                              on_virtual_key_press_done, task);
}

/**
 * shell_input_method_handle_virtual_key_finish:
 * @self: a #ShellInputMethod
 * @res: the #GAsyncResult passed to the callback
 *
 * Returns: %TRUE if the engine handled the key, %FALSE if it did not or if
 *   there is no input context
 */
gboolean
shell_input_method_handle_virtual_key_finish (ShellInputMethod *self,
                                              GAsyncResult     *res)
{
  g_return_val_if_fail (SHELL_IS_INPUT_METHOD (self), FALSE);
  g_return_val_if_fail (g_task_is_valid (res, self), FALSE);

  return g_task_propagate_boolean (G_TASK (res), NULL);
}

/**
 * shell_input_method_new:
 *
 * Creates the input method, which connects to IBus. It still has to be
 * installed with clutter_backend_set_input_method ().
 *
 * Returns: (transfer full): a new #ShellInputMethod
 */
ShellInputMethod *
shell_input_method_new (void)
{
  return g_object_new (SHELL_TYPE_INPUT_METHOD, NULL);
}

static void
shell_input_method_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellInputMethod *self = SHELL_INPUT_METHOD (object);

  switch (prop_id)
    {
    case PROP_CURRENT_FOCUS:
      g_value_set_object (value, self->current_focus);
      break;

    case PROP_HAS_INPUT_SOURCE:
      g_value_set_boolean (value, self->has_input_source);
      break;

    case PROP_OSK_VISIBLE:
      g_value_set_boolean (value, self->osk_visible);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_input_method_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellInputMethod *self = SHELL_INPUT_METHOD (object);

  switch (prop_id)
    {
    case PROP_HAS_INPUT_SOURCE:
      shell_input_method_set_has_input_source (self, g_value_get_boolean (value));
      break;

    case PROP_OSK_VISIBLE:
      shell_input_method_set_osk_visible (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
on_shutdown (ShellGlobal      *global,
             ShellInputMethod *self)
{
  /* Nothing may be sent to IBus or come back from it (and reach the
   * compositor and the on-screen keyboard) while the shell is going away. */
  self->shutting_down = TRUE;

  g_clear_handle_id (&self->hide_panel_id, g_source_remove);
  clear (self);

  if (self->ibus != NULL)
    g_signal_handlers_disconnect_by_data (self->ibus, self);
}

static void
shell_input_method_constructed (GObject *object)
{
  ShellInputMethod *self = SHELL_INPUT_METHOD (object);

  G_OBJECT_CLASS (shell_input_method_parent_class)->constructed (object);

  self->ibus = ibus_bus_new_async ();
  g_signal_connect_object (self->ibus, "connected",
                           G_CALLBACK (on_connected), self, 0);
  g_signal_connect_object (self->ibus, "disconnected",
                           G_CALLBACK (on_disconnected), self, 0);
  g_signal_connect (self, "notify::can-show-preedit",
                    G_CALLBACK (on_can_show_preedit_changed), NULL);
  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  if (ibus_bus_is_connected (self->ibus))
    on_connected (self->ibus, self);
}

static void
shell_input_method_dispose (GObject *object)
{
  ShellInputMethod *self = SHELL_INPUT_METHOD (object);

  g_clear_handle_id (&self->hide_panel_id, g_source_remove);

  clear (self);

  g_clear_object (&self->current_focus);
  g_clear_object (&self->ibus);

  G_OBJECT_CLASS (shell_input_method_parent_class)->dispose (object);

  /* The parent may have unset the focus, which arms the timeout again */
  g_clear_handle_id (&self->hide_panel_id, g_source_remove);
}

static void
shell_input_method_finalize (GObject *object)
{
  ShellInputMethod *self = SHELL_INPUT_METHOD (object);

  g_free (self->preedit_str);
  g_clear_pointer (&self->preedit_hints, g_array_unref);
  g_free (self->surrounding_text);

  G_OBJECT_CLASS (shell_input_method_parent_class)->finalize (object);
}

static void
shell_input_method_class_init (ShellInputMethodClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterInputMethodClass *im_class = CLUTTER_INPUT_METHOD_CLASS (klass);

  object_class->constructed = shell_input_method_constructed;
  object_class->get_property = shell_input_method_get_property;
  object_class->set_property = shell_input_method_set_property;
  object_class->dispose = shell_input_method_dispose;
  object_class->finalize = shell_input_method_finalize;

  im_class->focus_in = shell_input_method_focus_in;
  im_class->focus_out = shell_input_method_focus_out;
  im_class->reset = shell_input_method_reset;
  im_class->set_cursor_location = shell_input_method_set_cursor_location;
  im_class->set_surrounding = shell_input_method_set_surrounding;
  im_class->update_content_hints = shell_input_method_update_content_hints;
  im_class->update_content_purpose = shell_input_method_update_content_purpose;
  im_class->filter_key_event = shell_input_method_filter_key_event;

  /**
   * ShellInputMethod:current-focus:
   *
   * The #ClutterInputFocus that the input method is attached to, or %NULL.
   */
  props[PROP_CURRENT_FOCUS] =
    g_param_spec_object ("current-focus", NULL, NULL,
                         CLUTTER_TYPE_INPUT_FOCUS,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellInputMethod:has-input-source:
   *
   * Whether an input source is current. Without one, key events are not
   * filtered through IBus.
   */
  props[PROP_HAS_INPUT_SOURCE] =
    g_param_spec_boolean ("has-input-source", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellInputMethod:osk-visible:
   *
   * Whether the on-screen keyboard is visible, which is told to IBus as the
   * `OSK` capability.
   */
  props[PROP_OSK_VISIBLE] =
    g_param_spec_boolean ("osk-visible", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellInputMethod::surrounding-text-set:
   * @self: the #ShellInputMethod
   *
   * Emitted when the focus set its surrounding text. Use
   * shell_input_method_get_surrounding_text () to read it.
   */
  signals[SURROUNDING_TEXT_SET] =
    g_signal_new ("surrounding-text-set",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_input_method_init (ShellInputMethod *self)
{
  self->preedit_str = g_strdup ("");
}
