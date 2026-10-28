/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * extension-prefs-window.c: The window of the preferences of a Lua extension.
 * Replaces js/dbusServices/extensions/extensionPrefsDialog.js and the
 * GTK-built windows of the prefs.js of JS extensions.
 *
 * The window is a header bar over a stack of AdwPreferencesPage, one for each
 * page of `prefs.lua` (see docs/api/prefs.md for the rows and for how each
 * binds to its key). When the file cannot be shown the window holds the
 * error page of the old dialog (ExtensionPrefsErrorPage).
 *
 * Differences from the JS dialog: the window is an AdwWindow and not an
 * AdwPreferencesWindow, so that it can have a menu ("Reset to Defaults");
 * the code of the extension never runs here (the file is data), so there is
 * no `set_titlebar ()` or `destroy ()` to intercept.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <math.h>
#include <string.h>

#include <glib/gi18n.h>

#include "extension-prefs-window.h"

/* The schemas whose accelerators a shortcut of an extension is checked against */
static const char * const conflict_schemas[] = {
  "org.gnome.desktop.wm.keybindings",
  "org.gnome.shell.keybindings",
  "org.gnome.mutter.keybindings",
  "org.gnome.settings-daemon.plugins.media-keys",
  NULL
};

#define ROW_CTX_KEY "ext-prefs-row-ctx"

enum
{
  PROP_0,
  PROP_EXTENSION,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ExtPrefsWindow
{
  AdwWindow parent;

  ShellLuaExtension *extension;
  ExtPrefsSchema *schema;           /* nullable: the error page is shown */

  AdwToastOverlay *toast_overlay;
  AdwHeaderBar *header_bar;
  AdwViewStack *stack;
  GtkWidget *menu_button;
  GPtrArray *keys;                  /* char *, the keys of the rows (borrowed from schema) */
};

G_DEFINE_FINAL_TYPE (ExtPrefsWindow, ext_prefs_window, ADW_TYPE_WINDOW)

/* What the signal handlers of one row need */
typedef struct
{
  const ExtPrefsRow *spec;
  GSettings *settings;      /* borrowed from the schema, which outlives the rows */
  gboolean updating;        /* the widget is being updated from the settings */
  gboolean captured;        /* keybinding: a capture dialog is open */
} RowCtx;

static RowCtx *
row_get_ctx (GtkWidget *row)
{
  return g_object_get_data (G_OBJECT (row), ROW_CTX_KEY);
}

/* ------------------------------------------------------------------------ */
/* Sensitivity: a key that is not writable, or a dependency that is not met   */
/* ------------------------------------------------------------------------ */

static void
update_sensitivity (GtkWidget *row)
{
  RowCtx *ctx = row_get_ctx (row);
  gboolean sensitive = g_settings_is_writable (ctx->settings, ctx->spec->key);

  if (sensitive && ctx->spec->depends_on != NULL)
    {
      g_autoptr (GVariant) value = g_settings_get_value (ctx->settings, ctx->spec->depends_on->key);

      sensitive = g_variant_equal (value, ctx->spec->depends_on->value);
    }

  gtk_widget_set_sensitive (row, sensitive);
}

static void
on_sensitivity_changed (GSettings *settings,
                        const char *key,
                        GtkWidget  *row)
{
  update_sensitivity (row);
}

static void
row_attach_ctx (GtkWidget         *row,
                const ExtPrefsRow *spec,
                GSettings         *settings)
{
  RowCtx *ctx = g_new0 (RowCtx, 1);
  g_autofree char *writable_signal = g_strdup_printf ("writable-changed::%s", spec->key);

  ctx->spec = spec;
  ctx->settings = settings;
  g_object_set_data_full (G_OBJECT (row), ROW_CTX_KEY, ctx, g_free);

  g_signal_connect_object (settings, writable_signal,
                           G_CALLBACK (on_sensitivity_changed), row, 0);
  if (spec->depends_on != NULL)
    {
      g_autofree char *changed_signal = g_strdup_printf ("changed::%s", spec->depends_on->key);

      g_signal_connect_object (settings, changed_signal,
                               G_CALLBACK (on_sensitivity_changed), row, 0);
    }

  update_sensitivity (row);
}

static void
row_set_texts (AdwPreferencesRow *row,
               const ExtPrefsRow *spec)
{
  /* The strings come from the extension: they are not markup */
  adw_preferences_row_set_use_markup (row, FALSE);
  adw_preferences_row_set_title (row, spec->title);
  if (spec->subtitle == NULL)
    return;

  /* An entry row has no subtitle: it is the tooltip */
  if (ADW_IS_ACTION_ROW (row))
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), spec->subtitle);
  else
    gtk_widget_set_tooltip_text (GTK_WIDGET (row), spec->subtitle);
}

/* ------------------------------------------------------------------------ */
/* switch, spin                                                              */
/* ------------------------------------------------------------------------ */

static GtkWidget *
create_switch_row (const ExtPrefsRow *spec,
                   GSettings         *settings)
{
  GtkWidget *row = adw_switch_row_new ();

  row_set_texts (ADW_PREFERENCES_ROW (row), spec);
  g_settings_bind (settings, spec->key, row, "active", G_SETTINGS_BIND_DEFAULT);
  row_attach_ctx (row, spec, settings);

  return row;
}

static GtkWidget *
create_spin_row (const ExtPrefsRow *spec,
                 GSettings         *settings)
{
  GtkAdjustment *adjustment = gtk_adjustment_new (spec->min, spec->min, spec->max,
                                                  spec->step, spec->step * 10, 0);
  GtkWidget *row = adw_spin_row_new (adjustment, spec->step, spec->digits);

  row_set_texts (ADW_PREFERENCES_ROW (row), spec);
  g_settings_bind (settings, spec->key, row, "value", G_SETTINGS_BIND_DEFAULT);
  row_attach_ctx (row, spec, settings);

  return row;
}

/* ------------------------------------------------------------------------ */
/* combo                                                                     */
/* ------------------------------------------------------------------------ */

static guint
combo_find_choice (const ExtPrefsRow *spec,
                   GVariant          *value)
{
  guint i;

  for (i = 0; i < spec->choices->len; i++)
    {
      ExtPrefsChoice *choice = g_ptr_array_index (spec->choices, i);

      if (g_variant_equal (choice->value, value))
        return i;
    }

  return GTK_INVALID_LIST_POSITION;
}

/* A value that is none of the choices shows no selection */
static void
combo_sync (GtkWidget *row)
{
  RowCtx *ctx = row_get_ctx (row);
  g_autoptr (GVariant) value = g_settings_get_value (ctx->settings, ctx->spec->key);
  guint selected = combo_find_choice (ctx->spec, value);

  ctx->updating = TRUE;
  adw_combo_row_set_selected (ADW_COMBO_ROW (row), selected);
  ctx->updating = FALSE;
}

static void
on_combo_settings_changed (GSettings *settings,
                           const char *key,
                           GtkWidget  *row)
{
  combo_sync (row);
}

static void
on_combo_selected_notify (AdwComboRow *row,
                          GParamSpec  *pspec,
                          gpointer     user_data)
{
  RowCtx *ctx = row_get_ctx (GTK_WIDGET (row));
  guint selected = adw_combo_row_get_selected (row);
  ExtPrefsChoice *choice;
  g_autoptr (GVariant) current = NULL;

  if (ctx->updating || selected == GTK_INVALID_LIST_POSITION)
    return;

  choice = g_ptr_array_index (ctx->spec->choices, selected);
  current = g_settings_get_value (ctx->settings, ctx->spec->key);
  if (!g_variant_equal (current, choice->value))
    g_settings_set_value (ctx->settings, ctx->spec->key, choice->value);
}

static GtkWidget *
create_combo_row (const ExtPrefsRow *spec,
                  GSettings         *settings)
{
  g_autoptr (GtkStringList) labels = gtk_string_list_new (NULL);
  g_autofree char *changed_signal = g_strdup_printf ("changed::%s", spec->key);
  GtkWidget *row = adw_combo_row_new ();
  guint i;

  for (i = 0; i < spec->choices->len; i++)
    {
      ExtPrefsChoice *choice = g_ptr_array_index (spec->choices, i);

      gtk_string_list_append (labels, choice->label);
    }

  row_set_texts (ADW_PREFERENCES_ROW (row), spec);
  adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (labels));
  adw_combo_row_set_use_subtitle (ADW_COMBO_ROW (row), spec->subtitle == NULL);
  row_attach_ctx (row, spec, settings);

  combo_sync (row);
  g_signal_connect (row, "notify::selected", G_CALLBACK (on_combo_selected_notify), NULL);
  g_signal_connect_object (settings, changed_signal, G_CALLBACK (on_combo_settings_changed), row, 0);

  return row;
}

/* ------------------------------------------------------------------------ */
/* entry                                                                     */
/* ------------------------------------------------------------------------ */

static void
entry_sync (GtkWidget *row)
{
  RowCtx *ctx = row_get_ctx (row);
  g_autofree char *value = g_settings_get_string (ctx->settings, ctx->spec->key);

  if (g_strcmp0 (gtk_editable_get_text (GTK_EDITABLE (row)), value) == 0)
    return;

  ctx->updating = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (row), value);
  ctx->updating = FALSE;
}

static void
entry_commit (GtkWidget *row)
{
  RowCtx *ctx = row_get_ctx (row);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (row));
  g_autofree char *current = NULL;

  if (ctx->updating)
    return;

  current = g_settings_get_string (ctx->settings, ctx->spec->key);
  if (g_strcmp0 (current, text) != 0)
    g_settings_set_string (ctx->settings, ctx->spec->key, text);
}

static void
on_entry_activated (AdwEntryRow *row,
                    gpointer     user_data)
{
  entry_commit (GTK_WIDGET (row));
}

static void
on_entry_focus_leave (GtkEventControllerFocus *controller,
                      GtkWidget               *row)
{
  entry_commit (row);
}

static void
on_entry_settings_changed (GSettings *settings,
                           const char *key,
                           GtkWidget  *row)
{
  entry_sync (row);
}

static GtkWidget *
create_entry_row (const ExtPrefsRow *spec,
                  GSettings         *settings)
{
  g_autofree char *changed_signal = g_strdup_printf ("changed::%s", spec->key);
  GtkWidget *row = adw_entry_row_new ();
  GtkEventController *focus = gtk_event_controller_focus_new ();

  row_set_texts (ADW_PREFERENCES_ROW (row), spec);
  row_attach_ctx (row, spec, settings);

  if (spec->max_length > 0)
    {
      GtkEditable *delegate = gtk_editable_get_delegate (GTK_EDITABLE (row));

      if (GTK_IS_TEXT (delegate))
        gtk_entry_buffer_set_max_length (gtk_text_get_buffer (GTK_TEXT (delegate)), spec->max_length);
    }

  entry_sync (row);
  g_signal_connect (row, "entry-activated", G_CALLBACK (on_entry_activated), NULL);
  g_signal_connect_object (focus, "leave", G_CALLBACK (on_entry_focus_leave), row, 0);
  gtk_widget_add_controller (row, focus);
  g_signal_connect_object (settings, changed_signal, G_CALLBACK (on_entry_settings_changed), row, 0);

  return row;
}

/* ------------------------------------------------------------------------ */
/* color                                                                     */
/* ------------------------------------------------------------------------ */

static char *
rgba_to_setting (const GdkRGBA *rgba,
                 gboolean       alpha)
{
  int red = CLAMP ((int) round (rgba->red * 255.0), 0, 255);
  int green = CLAMP ((int) round (rgba->green * 255.0), 0, 255);
  int blue = CLAMP ((int) round (rgba->blue * 255.0), 0, 255);
  char alpha_text[G_ASCII_DTOSTR_BUF_SIZE];

  if (!alpha)
    return g_strdup_printf ("#%02x%02x%02x", red, green, blue);

  g_ascii_formatd (alpha_text, sizeof (alpha_text), "%.3g", CLAMP (rgba->alpha, 0.0, 1.0));
  return g_strdup_printf ("rgba(%d, %d, %d, %s)", red, green, blue, alpha_text);
}

/* A value that cannot be parsed shows as unset (transparent black) and is
 * not written back */
static void
color_sync (GtkWidget *row)
{
  RowCtx *ctx = row_get_ctx (row);
  GtkColorDialogButton *button = g_object_get_data (G_OBJECT (row), "color-button");
  g_autofree char *value = g_settings_get_string (ctx->settings, ctx->spec->key);
  GdkRGBA rgba = { 0, 0, 0, 0 };

  if (!gdk_rgba_parse (&rgba, value))
    rgba = (GdkRGBA) { 0, 0, 0, 0 };

  ctx->updating = TRUE;
  gtk_color_dialog_button_set_rgba (button, &rgba);
  ctx->updating = FALSE;
}

static void
on_color_notify (GtkColorDialogButton *button,
                 GParamSpec           *pspec,
                 GtkWidget            *row)
{
  RowCtx *ctx = row_get_ctx (row);
  g_autofree char *text = NULL;
  g_autofree char *current = NULL;
  GdkRGBA parsed;

  if (ctx->updating)
    return;

  text = rgba_to_setting (gtk_color_dialog_button_get_rgba (button), ctx->spec->alpha);
  current = g_settings_get_string (ctx->settings, ctx->spec->key);

  /* The same colour in another notation is not a change */
  if (gdk_rgba_parse (&parsed, current))
    {
      g_autofree char *normalized = rgba_to_setting (&parsed, ctx->spec->alpha);

      if (g_strcmp0 (normalized, text) == 0)
        return;
    }

  g_settings_set_string (ctx->settings, ctx->spec->key, text);
}

static void
on_color_settings_changed (GSettings *settings,
                           const char *key,
                           GtkWidget  *row)
{
  color_sync (row);
}

static GtkWidget *
create_color_row (const ExtPrefsRow *spec,
                  GSettings         *settings)
{
  g_autofree char *changed_signal = g_strdup_printf ("changed::%s", spec->key);
  GtkColorDialog *dialog = gtk_color_dialog_new ();
  GtkWidget *row = adw_action_row_new ();
  GtkWidget *button;

  gtk_color_dialog_set_with_alpha (dialog, spec->alpha);
  gtk_color_dialog_set_title (dialog, spec->title);
  button = gtk_color_dialog_button_new (dialog);
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);

  row_set_texts (ADW_PREFERENCES_ROW (row), spec);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), button);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), button);
  g_object_set_data (G_OBJECT (row), "color-button", button);
  row_attach_ctx (row, spec, settings);

  color_sync (row);
  g_signal_connect_object (button, "notify::rgba", G_CALLBACK (on_color_notify), row, 0);
  g_signal_connect_object (settings, changed_signal, G_CALLBACK (on_color_settings_changed), row, 0);

  return row;
}

/* ------------------------------------------------------------------------ */
/* keybinding                                                                */
/* ------------------------------------------------------------------------ */

static void
keybinding_sync (GtkWidget *row)
{
  RowCtx *ctx = row_get_ctx (row);
  GtkShortcutLabel *label = g_object_get_data (G_OBJECT (row), "shortcut-label");
  g_auto (GStrv) accels = g_settings_get_strv (ctx->settings, ctx->spec->key);
  g_autofree char *text = g_strjoinv (" ", accels);

  gtk_shortcut_label_set_accelerator (label, text);
}

static void
on_keybinding_settings_changed (GSettings *settings,
                                const char *key,
                                GtkWidget  *row)
{
  keybinding_sync (row);
}

/* The first accelerator that another setting of the shell or of the settings
 * daemon uses, as "schema key", or NULL */
static char *
find_accelerator_conflict (const ExtPrefsRow *spec,
                           GSettings         *own_settings,
                           guint              keyval,
                           GdkModifierType    modifiers)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  gsize i;

  if (source == NULL)
    return NULL;

  for (i = 0; conflict_schemas[i] != NULL; i++)
    {
      g_autoptr (GSettingsSchema) schema = g_settings_schema_source_lookup (source, conflict_schemas[i], TRUE);
      g_autoptr (GSettings) settings = NULL;
      g_auto (GStrv) keys = NULL;
      gsize j;

      if (schema == NULL || g_settings_schema_get_path (schema) == NULL)
        continue;

      settings = g_settings_new_full (schema, NULL, NULL);
      keys = g_settings_schema_list_keys (schema);
      for (j = 0; keys[j] != NULL; j++)
        {
          g_autoptr (GSettingsSchemaKey) schema_key = g_settings_schema_get_key (schema, keys[j]);
          g_auto (GStrv) accels = NULL;
          gsize k;

          if (!g_variant_type_equal (g_settings_schema_key_get_value_type (schema_key),
                                     G_VARIANT_TYPE_STRING_ARRAY))
            continue;

          accels = g_settings_get_strv (settings, keys[j]);
          for (k = 0; accels[k] != NULL; k++)
            {
              guint other_key = 0;
              GdkModifierType other_modifiers = 0;

              if (gtk_accelerator_parse (accels[k], &other_key, &other_modifiers) &&
                  other_key == keyval && other_modifiers == modifiers)
                return g_strdup_printf ("%s %s", conflict_schemas[i], keys[j]);
            }
        }
    }

  return NULL;
}

static void
window_toast (GtkWidget  *widget,
              const char *message)
{
  ExtPrefsWindow *self = EXT_PREFS_WINDOW (gtk_widget_get_ancestor (widget, EXT_TYPE_PREFS_WINDOW));
  AdwToast *toast;

  if (self == NULL)
    return;

  toast = adw_toast_new (message);
  adw_toast_set_use_markup (toast, FALSE);
  adw_toast_overlay_add_toast (self->toast_overlay, toast);
}

static void
keybinding_store (GtkWidget  *row,
                  const char *accelerator)
{
  RowCtx *ctx = row_get_ctx (row);
  g_auto (GStrv) current = g_settings_get_strv (ctx->settings, ctx->spec->key);
  g_autoptr (GPtrArray) accels = g_ptr_array_new_with_free_func (g_free);
  guint i;

  if (accelerator == NULL)
    {
      /* Backspace: clear */
      g_settings_set_strv (ctx->settings, ctx->spec->key, (const char * const[]) { NULL });
      return;
    }

  if (ctx->spec->max_accels > 1)
    {
      for (i = 0; current[i] != NULL; i++)
        {
          if (g_strcmp0 (current[i], accelerator) != 0)
            g_ptr_array_add (accels, g_strdup (current[i]));
        }

      /* A full list loses its last shortcut */
      while (accels->len >= ctx->spec->max_accels)
        g_ptr_array_remove_index (accels, accels->len - 1);
    }

  g_ptr_array_add (accels, g_strdup (accelerator));
  g_ptr_array_add (accels, NULL);
  g_settings_set_strv (ctx->settings, ctx->spec->key, (const char * const *) accels->pdata);
}

static gboolean
on_capture_key_pressed (GtkEventControllerKey *controller,
                        guint                  keyval,
                        guint                  keycode,
                        GdkModifierType        state,
                        GtkWidget             *row)
{
  RowCtx *ctx = row_get_ctx (row);
  AdwDialog *dialog = ADW_DIALOG (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller)));
  GdkEvent *event = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (controller));
  GdkModifierType modifiers = state & gtk_accelerator_get_default_mod_mask ();
  g_autofree char *accelerator = NULL;
  g_autofree char *conflict = NULL;
  guint accel_key = 0;
  GdkModifierType accel_modifiers = 0;

  if (modifiers == 0 && keyval == GDK_KEY_Escape)
    {
      adw_dialog_close (dialog);
      return TRUE;
    }

  if (modifiers == 0 && keyval == GDK_KEY_BackSpace)
    {
      if (ctx->spec->allow_empty)
        {
          keybinding_store (row, NULL);
          adw_dialog_close (dialog);
        }
      return TRUE;
    }

  if (event == NULL || !gdk_key_event_get_match (event, &accel_key, &accel_modifiers))
    {
      accel_key = gdk_keyval_to_lower (keyval);
      accel_modifiers = modifiers;
    }

  /* Only a modifier so far: wait for the key */
  if (!gtk_accelerator_valid (accel_key, accel_modifiers))
    return TRUE;

  accelerator = gtk_accelerator_name (accel_key, accel_modifiers);
  conflict = find_accelerator_conflict (ctx->spec, ctx->settings, accel_key, accel_modifiers);
  keybinding_store (row, accelerator);
  adw_dialog_close (dialog);

  if (conflict != NULL)
    {
      /* Translators: %1$s is a keyboard shortcut, %2$s the name of the GSettings key that already uses it */
      g_autofree char *message = g_strdup_printf (_("%1$s is already used by %2$s"), accelerator, conflict);

      window_toast (row, message);
    }

  return TRUE;
}

static void
on_capture_closed (AdwDialog *dialog,
                   GtkWidget *row)
{
  row_get_ctx (row)->captured = FALSE;
}

static void
on_keybinding_activated (AdwActionRow *row,
                         gpointer      user_data)
{
  RowCtx *ctx = row_get_ctx (GTK_WIDGET (row));
  g_autofree char *body = NULL;
  AdwDialog *dialog;
  GtkEventController *controller;

  if (ctx->captured)
    return;

  if (ctx->spec->max_accels > 1)
    body = g_strdup (ctx->spec->allow_empty
                     ? _("Press the key combination to add. Press Esc to cancel or Backspace to clear.")
                     : _("Press the key combination to add. Press Esc to cancel."));
  else
    body = g_strdup (ctx->spec->allow_empty
                     ? _("Press the new key combination. Press Esc to cancel or Backspace to clear.")
                     : _("Press the new key combination. Press Esc to cancel."));

  dialog = adw_alert_dialog_new (_("Set Shortcut"), body);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel", _("_Cancel"));
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  adw_alert_dialog_set_body_use_markup (ADW_ALERT_DIALOG (dialog), FALSE);

  controller = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
  g_signal_connect_object (controller, "key-pressed", G_CALLBACK (on_capture_key_pressed), row, 0);
  gtk_widget_add_controller (GTK_WIDGET (dialog), controller);
  g_signal_connect_object (dialog, "closed", G_CALLBACK (on_capture_closed), row, 0);

  ctx->captured = TRUE;
  adw_dialog_present (dialog, GTK_WIDGET (row));
}

static GtkWidget *
create_keybinding_row (const ExtPrefsRow *spec,
                       GSettings         *settings)
{
  g_autofree char *changed_signal = g_strdup_printf ("changed::%s", spec->key);
  GtkWidget *row = adw_action_row_new ();
  GtkWidget *label = gtk_shortcut_label_new ("");

  gtk_shortcut_label_set_disabled_text (GTK_SHORTCUT_LABEL (label), _("Disabled"));
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);

  row_set_texts (ADW_PREFERENCES_ROW (row), spec);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), label);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  g_object_set_data (G_OBJECT (row), "shortcut-label", label);
  row_attach_ctx (row, spec, settings);

  keybinding_sync (row);
  g_signal_connect (row, "activated", G_CALLBACK (on_keybinding_activated), NULL);
  g_signal_connect_object (settings, changed_signal, G_CALLBACK (on_keybinding_settings_changed), row, 0);

  return row;
}

/* ------------------------------------------------------------------------ */
/* Pages                                                                     */
/* ------------------------------------------------------------------------ */

static GtkWidget *
create_row (const ExtPrefsRow *spec,
            GSettings         *settings)
{
  switch (spec->type)
    {
    case EXT_PREFS_ROW_SWITCH:
      return create_switch_row (spec, settings);
    case EXT_PREFS_ROW_SPIN:
      return create_spin_row (spec, settings);
    case EXT_PREFS_ROW_COMBO:
      return create_combo_row (spec, settings);
    case EXT_PREFS_ROW_ENTRY:
      return create_entry_row (spec, settings);
    case EXT_PREFS_ROW_COLOR:
      return create_color_row (spec, settings);
    case EXT_PREFS_ROW_KEYBINDING:
      return create_keybinding_row (spec, settings);
    default:
      g_assert_not_reached ();
    }
}

static GtkWidget *
create_page (ExtPrefsWindow *self,
             ExtPrefsPage   *page)
{
  GtkWidget *widget = adw_preferences_page_new ();
  guint i, j;

  for (i = 0; i < page->groups->len; i++)
    {
      ExtPrefsGroup *group = g_ptr_array_index (page->groups, i);
      GtkWidget *group_widget = adw_preferences_group_new ();

      if (group->title != NULL)
        adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group_widget), group->title);
      if (group->description != NULL)
        adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (group_widget), group->description);

      for (j = 0; j < group->rows->len; j++)
        {
          ExtPrefsRow *spec = g_ptr_array_index (group->rows, j);

          adw_preferences_group_add (ADW_PREFERENCES_GROUP (group_widget),
                                     create_row (spec, self->schema->settings));
          g_ptr_array_add (self->keys, spec->key);
        }

      adw_preferences_page_add (ADW_PREFERENCES_PAGE (widget), ADW_PREFERENCES_GROUP (group_widget));
    }

  return widget;
}

static void
build_pages (ExtPrefsWindow *self)
{
  guint i;

  for (i = 0; i < self->schema->pages->len; i++)
    {
      ExtPrefsPage *page = g_ptr_array_index (self->schema->pages, i);
      g_autofree char *name = g_strdup_printf ("page-%u", i + 1);
      GtkWidget *widget = create_page (self, page);

      adw_view_stack_add_titled_with_icon (self->stack, widget, name,
                                           page->title != NULL ? page->title : "",
                                           page->icon_name);
    }

  if (self->schema->pages->len > 1)
    {
      GtkWidget *switcher = adw_view_switcher_new ();
      GtkWidget *bar = adw_view_switcher_bar_new ();
      AdwBreakpoint *breakpoint = adw_breakpoint_new (adw_breakpoint_condition_parse ("max-width: 550sp"));
      GValue reveal = G_VALUE_INIT;
      GValue no_widget = G_VALUE_INIT;
      GtkWidget *toolbar_view = adw_toast_overlay_get_child (self->toast_overlay);

      adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher), self->stack);
      adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
      adw_header_bar_set_title_widget (self->header_bar, switcher);

      adw_view_switcher_bar_set_stack (ADW_VIEW_SWITCHER_BAR (bar), self->stack);
      adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar_view), bar);

      /* A narrow window gets the switcher at the bottom */
      g_value_init (&reveal, G_TYPE_BOOLEAN);
      g_value_set_boolean (&reveal, TRUE);
      adw_breakpoint_add_setter (breakpoint, G_OBJECT (bar), "reveal", &reveal);
      g_value_init (&no_widget, GTK_TYPE_WIDGET);
      g_value_set_object (&no_widget, NULL);
      adw_breakpoint_add_setter (breakpoint, G_OBJECT (self->header_bar), "title-widget", &no_widget);
      adw_window_add_breakpoint (ADW_WINDOW (self), breakpoint);
      g_value_unset (&reveal);
      g_value_unset (&no_widget);
    }
}

/* ------------------------------------------------------------------------ */
/* The error page (ExtensionPrefsErrorPage)                                  */
/* ------------------------------------------------------------------------ */

static void
on_copy_error_clicked (GtkButton *button,
                       gpointer   user_data)
{
  const char *markdown = g_object_get_data (G_OBJECT (button), "error-markdown");

  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (button)), markdown);
}

static GtkWidget *
create_error_page (ExtPrefsWindow *self,
                   char          **problems)
{
  JsonObject *metadata = shell_lua_extension_get_metadata (self->extension);
  const char *uuid = shell_lua_extension_get_uuid (self->extension);
  const char *name = json_object_get_string_member_with_default (metadata, "name", uuid);
  const char *url = json_object_get_string_member_with_default (metadata, "url", NULL);
  g_autofree char *details = NULL;
  g_autofree char *escaped_name = g_markup_escape_text (name, -1);
  g_autoptr (GString) description = g_string_new (NULL);
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *intro = adw_preferences_group_new ();
  GtkWidget *group = adw_preferences_group_new ();
  GtkWidget *label = gtk_label_new (NULL);
  GtkWidget *copy_button = gtk_button_new_from_icon_name ("edit-copy-symbolic");
  GtkWidget *view = gtk_text_view_new ();
  g_autoptr (GString) markdown = g_string_new (NULL);
  guint i;

  /* Translators: %s is an extension name */
  g_string_append_printf (description, _("Unable to display the settings for “%s”."), escaped_name);

  if (url != NULL && *url != '\0')
    {
      /* Translators: Link label in the phrase "Information about this
         problem may be available on the extension website" */
      g_autofree char *link_label = g_markup_escape_text (_("extension website"), -1);
      g_autofree char *escaped_url = g_markup_escape_text (url, -1);
      g_autofree char *link = g_strdup_printf ("<a href=\"%s\">%s</a>", escaped_url, link_label);

      g_string_append_c (description, ' ');
      /* Translators: %s is "extension website" */
      g_string_append_printf (description, _("Information about this problem may be available on the %s."), link);
    }

  gtk_label_set_markup (GTK_LABEL (label), description->str);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (intro), label);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (intro));

  details = g_strjoinv ("\n", problems);

  gtk_widget_add_css_class (copy_button, "flat");
  gtk_widget_set_tooltip_text (copy_button, _("Copy Error"));
  gtk_widget_set_valign (copy_button, GTK_ALIGN_CENTER);

  g_string_append_printf (markdown, "The settings of extension %s had an error:\n```\n%s\n```\n", uuid, details);
  g_object_set_data_full (G_OBJECT (copy_button), "error-markdown", g_string_free (g_steal_pointer (&markdown), FALSE), g_free);
  g_signal_connect (copy_button, "clicked", G_CALLBACK (on_copy_error_clicked), NULL);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Error Details"));
  adw_preferences_group_set_header_suffix (ADW_PREFERENCES_GROUP (group), copy_button);

  gtk_text_view_set_monospace (GTK_TEXT_VIEW (view), TRUE);
  gtk_text_view_set_editable (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (view), 12);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (view), 12);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (view), 12);
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (view), 12);
  gtk_widget_add_css_class (view, "card");
  {
    g_autofree char *shown = g_strconcat (details, "\n", NULL);

    gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)), shown, -1);
  }
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), view);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));

  for (i = 0; problems[i] != NULL; i++)
    g_debug ("%s: %s", uuid, problems[i]);

  return page;
}

/* ------------------------------------------------------------------------ */
/* Reset to defaults                                                         */
/* ------------------------------------------------------------------------ */

static void
on_reset_response (AdwAlertDialog *dialog,
                   const char     *response,
                   ExtPrefsWindow *self)
{
  guint i;

  if (strcmp (response, "reset") != 0 || self->schema == NULL)
    return;

  for (i = 0; i < self->keys->len; i++)
    g_settings_reset (self->schema->settings, g_ptr_array_index (self->keys, i));
}

static void
reset_defaults_action (GtkWidget  *widget,
                       const char *action_name,
                       GVariant   *parameter)
{
  ExtPrefsWindow *self = EXT_PREFS_WINDOW (widget);
  AdwDialog *dialog = adw_alert_dialog_new (_("Reset to Defaults?"),
                                            _("All the settings of this extension on this page will be set back to their default values."));

  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                  "cancel", _("_Cancel"),
                                  "reset", _("_Reset"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "reset",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_signal_connect_object (dialog, "response", G_CALLBACK (on_reset_response), self, 0);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

/* ------------------------------------------------------------------------ */
/* Object                                                                    */
/* ------------------------------------------------------------------------ */

static void
ext_prefs_window_constructed (GObject *object)
{
  ExtPrefsWindow *self = EXT_PREFS_WINDOW (object);
  JsonObject *metadata;
  const char *name;
  g_auto (GStrv) problems = NULL;
  GtkWidget *toolbar_view;
  g_autoptr (GMenu) menu = NULL;

  G_OBJECT_CLASS (ext_prefs_window_parent_class)->constructed (object);

  g_assert (self->extension != NULL);

  metadata = shell_lua_extension_get_metadata (self->extension);
  name = json_object_get_string_member_with_default (metadata, "name",
                                                     shell_lua_extension_get_uuid (self->extension));
  self->keys = g_ptr_array_new ();

  gtk_window_set_default_size (GTK_WINDOW (self), 640, 700);
  gtk_widget_set_size_request (GTK_WIDGET (self), 360, 294);

  self->stack = ADW_VIEW_STACK (adw_view_stack_new ());
  self->header_bar = ADW_HEADER_BAR (adw_header_bar_new ());
  toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), GTK_WIDGET (self->header_bar));
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), GTK_WIDGET (self->stack));

  self->toast_overlay = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (self->toast_overlay, toolbar_view);
  adw_window_set_content (ADW_WINDOW (self), GTK_WIDGET (self->toast_overlay));

  self->schema = ext_prefs_schema_load (self->extension, &problems);
  if (self->schema != NULL)
    {
      gtk_window_set_title (GTK_WINDOW (self),
                            self->schema->title != NULL ? self->schema->title : name);

      menu = g_menu_new ();
      g_menu_append (menu, _("Reset to Defaults"), "win.reset-defaults");
      self->menu_button = gtk_menu_button_new ();
      gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (self->menu_button), "open-menu-symbolic");
      gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (self->menu_button), G_MENU_MODEL (menu));
      gtk_widget_set_tooltip_text (self->menu_button, _("Main Menu"));
      adw_header_bar_pack_end (self->header_bar, self->menu_button);

      build_pages (self);
    }
  else
    {
      GtkWidget *page;
      static const char * const unknown[] = { "the preferences could not be loaded", NULL };

      gtk_window_set_title (GTK_WINDOW (self), _("Extension Error"));
      page = create_error_page (self, problems != NULL ? problems : (char **) unknown);
      adw_view_stack_add (self->stack, page);
    }
}

static void
ext_prefs_window_dispose (GObject *object)
{
  ExtPrefsWindow *self = EXT_PREFS_WINDOW (object);

  /* The rows hold the settings of the schema: drop them first */
  adw_window_set_content (ADW_WINDOW (self), NULL);
  self->toast_overlay = NULL;
  self->header_bar = NULL;
  self->stack = NULL;
  self->menu_button = NULL;
  g_clear_pointer (&self->keys, g_ptr_array_unref);

  G_OBJECT_CLASS (ext_prefs_window_parent_class)->dispose (object);
}

static void
ext_prefs_window_finalize (GObject *object)
{
  ExtPrefsWindow *self = EXT_PREFS_WINDOW (object);

  g_clear_pointer (&self->schema, ext_prefs_schema_free);
  g_clear_object (&self->extension);

  G_OBJECT_CLASS (ext_prefs_window_parent_class)->finalize (object);
}

static void
ext_prefs_window_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  ExtPrefsWindow *self = EXT_PREFS_WINDOW (object);

  switch (prop_id)
    {
    case PROP_EXTENSION:
      self->extension = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ext_prefs_window_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  ExtPrefsWindow *self = EXT_PREFS_WINDOW (object);

  switch (prop_id)
    {
    case PROP_EXTENSION:
      g_value_set_object (value, self->extension);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ext_prefs_window_class_init (ExtPrefsWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = ext_prefs_window_constructed;
  object_class->dispose = ext_prefs_window_dispose;
  object_class->finalize = ext_prefs_window_finalize;
  object_class->set_property = ext_prefs_window_set_property;
  object_class->get_property = ext_prefs_window_get_property;

  /**
   * ExtPrefsWindow:extension:
   *
   * The extension whose preferences the window shows.
   */
  props[PROP_EXTENSION] =
    g_param_spec_object ("extension", NULL, NULL,
                         SHELL_TYPE_LUA_EXTENSION,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  gtk_widget_class_install_action (widget_class, "win.reset-defaults", NULL,
                                   reset_defaults_action);
}

static void
ext_prefs_window_init (ExtPrefsWindow *self)
{
}

/**
 * ext_prefs_window_new:
 * @extension: the extension
 *
 * Creates the window of the preferences of @extension, running `prefs.lua`
 * to know what to show. Nothing is presented yet.
 *
 * Returns: (transfer floating): a new #ExtPrefsWindow
 */
ExtPrefsWindow *
ext_prefs_window_new (ShellLuaExtension *extension)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (extension), NULL);

  return g_object_new (EXT_TYPE_PREFS_WINDOW, "extension", extension, NULL);
}
