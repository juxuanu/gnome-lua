/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * pad-osd.c: The on-screen display which shows what the buttons, rings,
 * strips and dials of a tablet pad do, lets the user change it, and the
 * D-Bus service which asks for it. Port of js/ui/padOsd.js.
 *
 * #ShellPadOsd (the former `PadOsd.PadOsd`) covers a monitor and holds a
 * title with an optional pad chooser, the diagram of the pad and an
 * "Edit..." button. The diagram, the pad chooser and the action editor are
 * private classes of this file (`PadDiagram`, `PadChooser`, `ActionEditor`,
 * `ActionComboBox` and `KeybindingEntry` in JS).
 * #ShellPadOsdService (the former `PadOsdService`) exports
 * org.gnome.Shell.Wacom.PadOsd and asks mutter for the OSD.
 *
 * Differences from the JS classes, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The popup menus of the pad chooser and of the action combo box are
 *    destroyed with their widgets. The JS combo box left its menu, hidden,
 *    in Main.uiGroup after the OSD was closed, and the JS chooser created a
 *    new menu manager every time it rebuilt its menu.
 *  - An image which cannot be loaded logs a warning and the diagram stays
 *    empty, where the JS constructor threw.
 *  - A missing pad button schema or key logs a warning, where GJS threw.
 */

#include "config.h"

#include <atk/atk.h>
#include <clutter/clutter-pango.h>
#include <gdesktop-enums.h>
#include <glib/gi18n-lib.h>
#include <librsvg/rsvg.h>
#include <meta/display.h>
#include <meta/util.h>

#include "pad-osd-dbus.h"
#include "shell-global.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/pad-osd.h"
#include "ui/popup-menu.h"

#define PAD_OSD_BUS_NAME "org.gnome.Shell.Wacom.PadOsd"
#define PAD_OSD_OBJECT_PATH "/org/gnome/Shell/Wacom"

#define PAD_BUTTON_SCHEMA "org.gnome.desktop.peripherals.tablet.pad-button"
#define PAD_DIAGRAM_CSS_URI "resource:///org/gnome/shell/theme/pad-osd.css"

#define ACTIVE_COLOR "st-lighten(-st-accent-color, 15%)"

/* The "feature" of an edited label or action which is a button, as opposed
 * to the #MetaPadFeatureType values (`null` in the JS) */
#define PAD_ACTION_BUTTON (-1)

/* The direction of the features which have none, as opposed to the
 * #MetaPadDirection values, which start at 1 (`undefined` in the JS) */
#define PAD_DIRECTION_NONE 0

typedef enum
{
  PAD_ARRANGEMENT_LTR = 0,
  PAD_ARRANGEMENT_RTL = 1,
} PadArrangement;

/* The container the popup menus of the widgets go to (`Main.uiGroup`) */
static ClutterActor *
get_ui_group (void)
{
  ShellLayoutManager *layout_manager =
    shell_main_get_layout_manager (shell_main_get_default ());

  if (layout_manager == NULL)
    return NULL;

  return CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager));
}

/* A popup menu for @source_actor, hidden and added to the ui group the way
 * the JS widgets did it, and the manager which grabs the input while it is
 * open */
static ShellPopupMenu *
create_popup_menu (ClutterActor *source_actor,
                   double        arrow_alignment)
{
  ShellPopupMenu *menu;
  ClutterActor *menu_actor;
  ClutterActor *ui_group;

  menu = shell_popup_menu_new (source_actor, arrow_alignment, ST_SIDE_TOP);
  menu_actor = shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (menu));
  clutter_actor_hide (menu_actor);

  ui_group = get_ui_group ();
  if (ui_group != NULL)
    clutter_actor_add_child (ui_group, menu_actor);
  else
    g_warning ("%s: no layout manager to add the menu to", G_STRFUNC);

  return menu;
}

static void
destroy_popup_menu (ShellPopupMenu **menu)
{
  if (*menu == NULL)
    return;

  shell_popup_menu_base_destroy (SHELL_POPUP_MENU_BASE (*menu));
  g_clear_object (menu);
}

static ClutterActor *
create_arrow_icon (void)
{
  return g_object_new (ST_TYPE_ICON,
                       "style-class", "popup-menu-arrow",
                       "icon-name", "pan-down-symbolic",
                       "accessible-role", ATK_ROLE_ARROW,
                       "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       NULL);
}

/* ======================================================================== */
/* ShellPadChooser                                                          */
/* ======================================================================== */

#define SHELL_TYPE_PAD_CHOOSER (shell_pad_chooser_get_type ())
G_DECLARE_FINAL_TYPE (ShellPadChooser, shell_pad_chooser,
                      SHELL, PAD_CHOOSER, StButton)

enum
{
  CHOOSER_PAD_SELECTED,
  CHOOSER_LAST_SIGNAL
};

static guint chooser_signals[CHOOSER_LAST_SIGNAL] = { 0 };

struct _ShellPadChooser
{
  StButton parent;

  ClutterInputDevice *current_device;
  ShellPopupMenu *menu;
  ShellPopupMenuManager *menu_manager;
};

G_DEFINE_FINAL_TYPE (ShellPadChooser, shell_pad_chooser, ST_TYPE_BUTTON)

typedef struct
{
  ShellPadChooser *chooser; /* the menu is destroyed with the chooser */
  ClutterInputDevice *device;
} PadChooserItem;

static void
pad_chooser_item_free (gpointer data)
{
  PadChooserItem *item = data;

  g_object_unref (item->device);
  g_free (item);
}

static void
pad_chooser_item_activated_cb (const ClutterEvent *event,
                               gpointer            user_data)
{
  PadChooserItem *item = user_data;

  g_signal_emit (item->chooser, chooser_signals[CHOOSER_PAD_SELECTED], 0,
                 item->device);
}

static void
pad_chooser_menu_closed_cb (ShellPopupMenuBase *menu,
                            ShellPadChooser    *self)
{
  st_button_set_checked (ST_BUTTON (self), FALSE);
}

static void
pad_chooser_ensure_menu (ShellPadChooser *self,
                         GPtrArray       *devices)
{
  guint i;

  self->menu = create_popup_menu (CLUTTER_ACTOR (self), 0.5);
  g_signal_connect_object (self->menu, "menu-closed",
                           G_CALLBACK (pad_chooser_menu_closed_cb), self, 0);

  if (self->menu_manager == NULL)
    self->menu_manager = shell_popup_menu_manager_new (SHELL_ACTION_MODE_POPUP);
  shell_popup_menu_manager_add_menu (self->menu_manager, G_OBJECT (self->menu));

  for (i = 0; i < devices->len; i++)
    {
      ClutterInputDevice *device = g_ptr_array_index (devices, i);
      PadChooserItem *item;

      if (device == self->current_device)
        continue;

      item = g_new0 (PadChooserItem, 1);
      item->chooser = self;
      item->device = g_object_ref (device);
      shell_popup_menu_base_add_action (SHELL_POPUP_MENU_BASE (self->menu),
                                        clutter_input_device_get_device_name (device),
                                        pad_chooser_item_activated_cb,
                                        item, pad_chooser_item_free);
    }
}

static void
shell_pad_chooser_clicked (StButton *button,
                           int       clicked_button)
{
  ShellPadChooser *self = SHELL_PAD_CHOOSER (button);

  if (st_button_get_checked (button))
    {
      if (self->menu != NULL)
        shell_popup_menu_base_open (SHELL_POPUP_MENU_BASE (self->menu),
                                    SHELL_POPUP_ANIMATION_FULL);
      else
        st_button_set_checked (button, FALSE);
    }
  else if (self->menu != NULL)
    {
      shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (self->menu),
                                   SHELL_POPUP_ANIMATION_FULL);
    }
}

static void
shell_pad_chooser_destroy (ClutterActor *actor)
{
  ShellPadChooser *self = SHELL_PAD_CHOOSER (actor);

  destroy_popup_menu (&self->menu);

  CLUTTER_ACTOR_CLASS (shell_pad_chooser_parent_class)->destroy (actor);
}

static void
shell_pad_chooser_dispose (GObject *object)
{
  ShellPadChooser *self = SHELL_PAD_CHOOSER (object);

  destroy_popup_menu (&self->menu);
  g_clear_object (&self->menu_manager);
  g_clear_object (&self->current_device);

  G_OBJECT_CLASS (shell_pad_chooser_parent_class)->dispose (object);
}

static void
shell_pad_chooser_class_init (ShellPadChooserClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->dispose = shell_pad_chooser_dispose;
  actor_class->destroy = shell_pad_chooser_destroy;
  button_class->clicked = shell_pad_chooser_clicked;

  chooser_signals[CHOOSER_PAD_SELECTED] =
    g_signal_new ("pad-selected",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, CLUTTER_TYPE_INPUT_DEVICE);
}

static void
shell_pad_chooser_init (ShellPadChooser *self)
{
}

static ShellPadChooser *
shell_pad_chooser_new (ClutterInputDevice *device,
                       GPtrArray          *group_devices)
{
  ShellPadChooser *self;

  self = g_object_new (SHELL_TYPE_PAD_CHOOSER,
                       "style-class", "pad-chooser-button",
                       "toggle-mode", TRUE,
                       NULL);
  self->current_device = g_object_ref (device);

  st_bin_set_child (ST_BIN (self), create_arrow_icon ());
  pad_chooser_ensure_menu (self, group_devices);

  return self;
}

static void
shell_pad_chooser_update (ShellPadChooser *self,
                          GPtrArray       *devices)
{
  destroy_popup_menu (&self->menu);
  st_button_set_checked (ST_BUTTON (self), FALSE);
  pad_chooser_ensure_menu (self, devices);
}

/* ======================================================================== */
/* ShellPadKeybindingEntry                                                  */
/* ======================================================================== */

#define SHELL_TYPE_PAD_KEYBINDING_ENTRY (shell_pad_keybinding_entry_get_type ())
G_DECLARE_FINAL_TYPE (ShellPadKeybindingEntry, shell_pad_keybinding_entry,
                      SHELL, PAD_KEYBINDING_ENTRY, StEntry)

enum
{
  ENTRY_KEYBINDING_EDITED,
  ENTRY_LAST_SIGNAL
};

static guint entry_signals[ENTRY_LAST_SIGNAL] = { 0 };

struct _ShellPadKeybindingEntry
{
  StEntry parent;
};

G_DEFINE_FINAL_TYPE (ShellPadKeybindingEntry, shell_pad_keybinding_entry, ST_TYPE_ENTRY)

static gboolean
shell_pad_keybinding_entry_captured_event (ClutterActor *actor,
                                           ClutterEvent *event)
{
  g_autofree char *accelerator = NULL;

  if (clutter_event_type (event) != CLUTTER_KEY_PRESS)
    return CLUTTER_EVENT_PROPAGATE;

  accelerator = meta_accelerator_name (clutter_event_get_state (event),
                                       clutter_event_get_key_symbol (event));

  st_entry_set_text (ST_ENTRY (actor), accelerator);
  g_signal_emit (actor, entry_signals[ENTRY_KEYBINDING_EDITED], 0, accelerator);

  return CLUTTER_EVENT_STOP;
}

static void
shell_pad_keybinding_entry_class_init (ShellPadKeybindingEntryClass *klass)
{
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  actor_class->captured_event = shell_pad_keybinding_entry_captured_event;

  entry_signals[ENTRY_KEYBINDING_EDITED] =
    g_signal_new ("keybinding-edited",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
shell_pad_keybinding_entry_init (ShellPadKeybindingEntry *self)
{
}

static ShellPadKeybindingEntry *
shell_pad_keybinding_entry_new (void)
{
  return g_object_new (SHELL_TYPE_PAD_KEYBINDING_ENTRY,
                       "hint-text", _("New shortcut…"),
                       "style", "width: 10em",
                       NULL);
}

/* ======================================================================== */
/* ShellPadActionComboBox                                                   */
/* ======================================================================== */

#define SHELL_TYPE_PAD_ACTION_COMBO_BOX (shell_pad_action_combo_box_get_type ())
G_DECLARE_FINAL_TYPE (ShellPadActionComboBox, shell_pad_action_combo_box,
                      SHELL, PAD_ACTION_COMBO_BOX, StButton)

enum
{
  COMBO_ACTION_SELECTED,
  COMBO_LAST_SIGNAL
};

static guint combo_signals[COMBO_LAST_SIGNAL] = { 0 };

struct _ShellPadActionComboBox
{
  StButton parent;

  StLabel *label; /* weak, a descendant */

  ShellPopupMenu *edit_menu;
  ShellPopupMenuManager *edit_menu_manager;

  /* The items which only apply to pad buttons; owned by the menu, cleared
   * when the menu is destroyed */
  GPtrArray *button_items;
};

G_DEFINE_FINAL_TYPE (ShellPadActionComboBox, shell_pad_action_combo_box, ST_TYPE_BUTTON)

typedef struct
{
  ShellPadActionComboBox *combo_box; /* the menu is destroyed with the combo box */
  GDesktopPadButtonAction action;
} ComboBoxItem;

static const char *
get_action_label (GDesktopPadButtonAction action)
{
  switch (action)
    {
    case G_DESKTOP_PAD_BUTTON_ACTION_NONE:
      return _("App defined");

    case G_DESKTOP_PAD_BUTTON_ACTION_HELP:
      return _("Show on-screen help");

    case G_DESKTOP_PAD_BUTTON_ACTION_SWITCH_MONITOR:
      return _("Switch monitor");

    case G_DESKTOP_PAD_BUTTON_ACTION_KEYBINDING:
      return _("Assign keystroke");

    default:
      return "";
    }
}

static void
shell_pad_action_combo_box_set_action (ShellPadActionComboBox *self,
                                       GDesktopPadButtonAction action)
{
  if (self->label != NULL)
    st_label_set_text (self->label, get_action_label (action));
}

static void
shell_pad_action_combo_box_popup (ShellPadActionComboBox *self)
{
  if (self->edit_menu != NULL)
    shell_popup_menu_base_open (SHELL_POPUP_MENU_BASE (self->edit_menu),
                                SHELL_POPUP_ANIMATION_FULL);
}

static void
shell_pad_action_combo_box_popdown (ShellPadActionComboBox *self)
{
  if (self->edit_menu != NULL)
    shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (self->edit_menu),
                                 SHELL_POPUP_ANIMATION_FULL);
}

static void
shell_pad_action_combo_box_set_button_actions_active (ShellPadActionComboBox *self,
                                                      gboolean                active)
{
  guint i;

  for (i = 0; i < self->button_items->len; i++)
    shell_popup_base_menu_item_set_sensitive (g_ptr_array_index (self->button_items, i),
                                              active);
}

static void
combo_box_item_activated_cb (const ClutterEvent *event,
                             gpointer            user_data)
{
  ComboBoxItem *item = user_data;
  ShellPadActionComboBox *self = item->combo_box;

  shell_pad_action_combo_box_set_action (self, item->action);
  shell_pad_action_combo_box_popdown (self);
  g_signal_emit (self, combo_signals[COMBO_ACTION_SELECTED], 0, (int) item->action);
}

static void
combo_box_menu_closed_cb (ShellPopupMenuBase      *menu,
                          ShellPadActionComboBox *self)
{
  st_button_set_checked (ST_BUTTON (self), FALSE);
}

static void
shell_pad_action_combo_box_clicked (StButton *button,
                                    int       clicked_button)
{
  ShellPadActionComboBox *self = SHELL_PAD_ACTION_COMBO_BOX (button);

  if (st_button_get_checked (button))
    shell_pad_action_combo_box_popup (self);
  else
    shell_pad_action_combo_box_popdown (self);
}

static void
shell_pad_action_combo_box_destroy (ClutterActor *actor)
{
  ShellPadActionComboBox *self = SHELL_PAD_ACTION_COMBO_BOX (actor);

  g_ptr_array_set_size (self->button_items, 0);
  destroy_popup_menu (&self->edit_menu);

  CLUTTER_ACTOR_CLASS (shell_pad_action_combo_box_parent_class)->destroy (actor);
}

static void
shell_pad_action_combo_box_dispose (GObject *object)
{
  ShellPadActionComboBox *self = SHELL_PAD_ACTION_COMBO_BOX (object);

  g_ptr_array_set_size (self->button_items, 0);
  destroy_popup_menu (&self->edit_menu);
  g_clear_object (&self->edit_menu_manager);

  G_OBJECT_CLASS (shell_pad_action_combo_box_parent_class)->dispose (object);

  g_clear_weak_pointer (&self->label);
}

static void
shell_pad_action_combo_box_finalize (GObject *object)
{
  ShellPadActionComboBox *self = SHELL_PAD_ACTION_COMBO_BOX (object);

  g_clear_pointer (&self->button_items, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_pad_action_combo_box_parent_class)->finalize (object);
}

static void
shell_pad_action_combo_box_class_init (ShellPadActionComboBoxClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->dispose = shell_pad_action_combo_box_dispose;
  object_class->finalize = shell_pad_action_combo_box_finalize;
  actor_class->destroy = shell_pad_action_combo_box_destroy;
  button_class->clicked = shell_pad_action_combo_box_clicked;

  combo_signals[COMBO_ACTION_SELECTED] =
    g_signal_new ("action-selected",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
shell_pad_action_combo_box_init (ShellPadActionComboBox *self)
{
  static const GDesktopPadButtonAction actions[] = {
    G_DESKTOP_PAD_BUTTON_ACTION_NONE,
    G_DESKTOP_PAD_BUTTON_ACTION_HELP,
    G_DESKTOP_PAD_BUTTON_ACTION_SWITCH_MONITOR,
    G_DESKTOP_PAD_BUTTON_ACTION_KEYBINDING,
  };
  ClutterLayoutManager *box_layout;
  ClutterActor *box;
  ClutterActor *label;
  guint i;

  self->button_items = g_ptr_array_new ();

  st_widget_set_style_class_name (ST_WIDGET (self), "button");
  st_button_set_toggle_mode (ST_BUTTON (self), TRUE);

  box_layout = clutter_box_layout_new ();
  clutter_box_layout_set_orientation (CLUTTER_BOX_LAYOUT (box_layout),
                                      CLUTTER_ORIENTATION_HORIZONTAL);
  clutter_box_layout_set_spacing (CLUTTER_BOX_LAYOUT (box_layout), 6);
  box = g_object_new (ST_TYPE_WIDGET, "layout-manager", box_layout, NULL);
  st_bin_set_child (ST_BIN (self), box);

  label = g_object_new (ST_TYPE_LABEL, "style-class", "combo-box-label", NULL);
  clutter_actor_add_child (box, label);
  g_set_weak_pointer (&self->label, ST_LABEL (label));

  clutter_actor_add_child (box,
                           g_object_new (ST_TYPE_ICON,
                                         "style-class", "popup-menu-arrow",
                                         "icon-name", "pan-down-symbolic",
                                         "accessible-role", ATK_ROLE_ARROW,
                                         "y-expand", TRUE,
                                         "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                         NULL));

  self->edit_menu = create_popup_menu (CLUTTER_ACTOR (self), 0);
  g_signal_connect_object (self->edit_menu, "menu-closed",
                           G_CALLBACK (combo_box_menu_closed_cb), self, 0);

  self->edit_menu_manager = shell_popup_menu_manager_new (SHELL_ACTION_MODE_POPUP);
  shell_popup_menu_manager_add_menu (self->edit_menu_manager,
                                     G_OBJECT (self->edit_menu));

  for (i = 0; i < G_N_ELEMENTS (actions); i++)
    {
      ComboBoxItem *item = g_new0 (ComboBoxItem, 1);
      ShellPopupBaseMenuItem *menu_item;

      item->combo_box = self;
      item->action = actions[i];
      menu_item = shell_popup_menu_base_add_action (SHELL_POPUP_MENU_BASE (self->edit_menu),
                                                    get_action_label (actions[i]),
                                                    combo_box_item_activated_cb,
                                                    item, g_free);

      /* These actions only apply to pad buttons */
      if (actions[i] == G_DESKTOP_PAD_BUTTON_ACTION_HELP ||
          actions[i] == G_DESKTOP_PAD_BUTTON_ACTION_SWITCH_MONITOR)
        g_ptr_array_add (self->button_items, menu_item);
    }

  shell_pad_action_combo_box_set_action (self, G_DESKTOP_PAD_BUTTON_ACTION_NONE);
}

/* ======================================================================== */
/* ShellPadActionEditor                                                     */
/* ======================================================================== */

#define SHELL_TYPE_PAD_ACTION_EDITOR (shell_pad_action_editor_get_type ())
G_DECLARE_FINAL_TYPE (ShellPadActionEditor, shell_pad_action_editor,
                      SHELL, PAD_ACTION_EDITOR, StWidget)

enum
{
  EDITOR_DONE,
  EDITOR_LAST_SIGNAL
};

static guint editor_signals[EDITOR_LAST_SIGNAL] = { 0 };

struct _ShellPadActionEditor
{
  StWidget parent;

  ShellPadActionComboBox *action_combo_box; /* weak, a child */
  ShellPadKeybindingEntry *keybinding_edit; /* weak, a child */
  StButton *done_button; /* weak, a child */

  GSettings *button_settings;
  GDesktopPadButtonAction current_action;
  char *current_keybinding;
};

G_DEFINE_FINAL_TYPE (ShellPadActionEditor, shell_pad_action_editor, ST_TYPE_WIDGET)

static void
action_editor_update_keybinding_entry_state (ShellPadActionEditor *self)
{
  if (self->keybinding_edit == NULL)
    return;

  if (self->current_action == G_DESKTOP_PAD_BUTTON_ACTION_KEYBINDING)
    {
      ClutterActor *entry = CLUTTER_ACTOR (self->keybinding_edit);

      st_entry_set_text (ST_ENTRY (entry),
                         self->current_keybinding != NULL ? self->current_keybinding : "");
      clutter_actor_show (entry);
      clutter_actor_grab_key_focus (entry);
    }
  else
    {
      clutter_actor_hide (CLUTTER_ACTOR (self->keybinding_edit));
    }
}

/* @action is the feature the settings are for, PAD_ACTION_BUTTON for a button */
static void
shell_pad_action_editor_set_settings (ShellPadActionEditor *self,
                                      GSettings            *settings,
                                      int                   action)
{
  g_set_object (&self->button_settings, settings);

  self->current_action = g_settings_get_enum (self->button_settings, "action");
  g_free (self->current_keybinding);
  self->current_keybinding = g_settings_get_string (self->button_settings, "keybinding");
  shell_pad_action_combo_box_set_action (self->action_combo_box, self->current_action);
  action_editor_update_keybinding_entry_state (self);

  shell_pad_action_combo_box_set_button_actions_active (self->action_combo_box,
                                                        action == PAD_ACTION_BUTTON);
}

static void
shell_pad_action_editor_close (ShellPadActionEditor *self)
{
  if (self->action_combo_box != NULL)
    shell_pad_action_combo_box_popdown (self->action_combo_box);
  clutter_actor_hide (CLUTTER_ACTOR (self));
}

static void
action_editor_on_keybinding_edited (ShellPadKeybindingEntry *entry,
                                    const char              *keybinding,
                                    ShellPadActionEditor    *self)
{
  g_free (self->current_keybinding);
  self->current_keybinding = g_strdup (keybinding);
}

static void
action_editor_on_action_selected (ShellPadActionComboBox *combo_box,
                                  int                     action,
                                  ShellPadActionEditor   *self)
{
  self->current_action = action;
  action_editor_update_keybinding_entry_state (self);
}

static void
action_editor_store_settings (ShellPadActionEditor *self)
{
  const char *keybinding = NULL;

  if (self->button_settings == NULL)
    return;

  if (self->current_action == G_DESKTOP_PAD_BUTTON_ACTION_KEYBINDING)
    keybinding = self->current_keybinding;

  g_settings_set_enum (self->button_settings, "action", self->current_action);

  if (keybinding != NULL && keybinding[0] != '\0')
    g_settings_set_string (self->button_settings, "keybinding", keybinding);
  else
    g_settings_reset (self->button_settings, "keybinding");
}

static void
action_editor_on_editing_done (ShellPadActionEditor *self)
{
  g_object_ref (self);

  action_editor_store_settings (self);
  shell_pad_action_editor_close (self);
  g_signal_emit (self, editor_signals[EDITOR_DONE], 0);

  g_object_unref (self);
}

static void
shell_pad_action_editor_dispose (GObject *object)
{
  ShellPadActionEditor *self = SHELL_PAD_ACTION_EDITOR (object);

  g_clear_object (&self->button_settings);

  G_OBJECT_CLASS (shell_pad_action_editor_parent_class)->dispose (object);

  g_clear_weak_pointer (&self->action_combo_box);
  g_clear_weak_pointer (&self->keybinding_edit);
  g_clear_weak_pointer (&self->done_button);
}

static void
shell_pad_action_editor_finalize (GObject *object)
{
  ShellPadActionEditor *self = SHELL_PAD_ACTION_EDITOR (object);

  g_clear_pointer (&self->current_keybinding, g_free);

  G_OBJECT_CLASS (shell_pad_action_editor_parent_class)->finalize (object);
}

static void
shell_pad_action_editor_class_init (ShellPadActionEditorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_pad_action_editor_dispose;
  object_class->finalize = shell_pad_action_editor_finalize;

  editor_signals[EDITOR_DONE] =
    g_signal_new ("done",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_pad_action_editor_init (ShellPadActionEditor *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterLayoutManager *box_layout;
  ShellPadActionComboBox *action_combo_box;
  ShellPadKeybindingEntry *keybinding_edit;
  ClutterActor *done_button;

  box_layout = clutter_box_layout_new ();
  clutter_box_layout_set_orientation (CLUTTER_BOX_LAYOUT (box_layout),
                                      CLUTTER_ORIENTATION_HORIZONTAL);
  clutter_box_layout_set_spacing (CLUTTER_BOX_LAYOUT (box_layout), 12);
  clutter_actor_set_layout_manager (actor, box_layout);

  action_combo_box = g_object_new (SHELL_TYPE_PAD_ACTION_COMBO_BOX, NULL);
  g_signal_connect (action_combo_box, "action-selected",
                    G_CALLBACK (action_editor_on_action_selected), self);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (action_combo_box));
  g_set_weak_pointer (&self->action_combo_box, action_combo_box);

  keybinding_edit = shell_pad_keybinding_entry_new ();
  g_signal_connect (keybinding_edit, "keybinding-edited",
                    G_CALLBACK (action_editor_on_keybinding_edited), self);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (keybinding_edit));
  g_set_weak_pointer (&self->keybinding_edit, keybinding_edit);

  done_button = g_object_new (ST_TYPE_BUTTON,
                              "label", _("Done"),
                              "style-class", "button",
                              "x-expand", FALSE,
                              NULL);
  g_signal_connect_swapped (done_button, "clicked",
                            G_CALLBACK (action_editor_on_editing_done), self);
  clutter_actor_add_child (actor, done_button);
  g_set_weak_pointer (&self->done_button, ST_BUTTON (done_button));
}

static ShellPadActionEditor *
shell_pad_action_editor_new (void)
{
  return g_object_new (SHELL_TYPE_PAD_ACTION_EDITOR, NULL);
}

/* ======================================================================== */
/* ShellPadDiagram                                                          */
/* ======================================================================== */

#define SHELL_TYPE_PAD_DIAGRAM (shell_pad_diagram_get_type ())
G_DECLARE_FINAL_TYPE (ShellPadDiagram, shell_pad_diagram,
                      SHELL, PAD_DIAGRAM, StDrawingArea)

/* Gives the text of the label of a feature: @action is a #MetaPadFeatureType,
 * or PAD_ACTION_BUTTON for a button; @idx is the number of the button or of
 * the feature; @dir is a #MetaPadDirection, or PAD_DIRECTION_NONE for a
 * button. The result is freed with g_free (). */
typedef char * (* PadDiagramTextFunc) (int      action,
                                       int      idx,
                                       int      dir,
                                       gpointer user_data);

typedef struct
{
  StLabel *label; /* weak, a child of the diagram */
  int action;
  int idx;
  int dir;
  int x;
  int y;
  PadArrangement arrangement;
} PadLabel;

struct _ShellPadDiagram
{
  StDrawingArea parent;

  gboolean left_handed;
  char *image_path;
  int image_width;
  int image_height;
  char *css;
  RsvgHandle *handle; /* the image wrapped with the css; NULL if it cannot be shown */

  GPtrArray *labels; /* PadLabel */
  GArray *active_buttons; /* int */
  PadLabel *cur_edited; /* an element of labels */
  ClutterActor *editor_actor; /* weak */

  gboolean has_scale;
  double scale;
  float actor_width;
  float actor_height;
};

G_DEFINE_FINAL_TYPE (ShellPadDiagram, shell_pad_diagram, ST_TYPE_DRAWING_AREA)

static void
pad_label_free (gpointer data)
{
  PadLabel *label = data;

  g_clear_weak_pointer (&label->label);
  g_free (label);
}

/* The librsvg functions which tell where an element is are deprecated in
 * favour of ones which return a different geometry, and the diagram is laid
 * out with the former. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

static void
get_handle_dimensions (RsvgHandle *handle,
                       int        *width,
                       int        *height)
{
  RsvgDimensionData dimensions = { 0, };

  rsvg_handle_get_dimensions (handle, &dimensions);
  *width = dimensions.width;
  *height = dimensions.height;
}

static gboolean
get_element_geometry (RsvgHandle *handle,
                      const char *name,
                      int        *x,
                      int        *y,
                      int        *width,
                      int        *height)
{
  g_autofree char *id = g_strdup_printf ("#%s", name);
  RsvgPositionData position = { 0, };
  RsvgDimensionData size = { 0, };
  gboolean found;

  found = rsvg_handle_get_position_sub (handle, &position, id);
  rsvg_handle_get_dimensions_sub (handle, &size, id);

  *x = position.x;
  *y = position.y;
  *width = size.width;
  *height = size.height;

  return found;
}

static void
render_handle (RsvgHandle *handle,
               cairo_t    *cr)
{
  rsvg_handle_render_cairo (handle, cr);
}

G_GNUC_END_IGNORE_DEPRECATIONS

static char *
letter_for_number (int number)
{
  GString *str = g_string_new (NULL);

  g_string_append_unichar (str, 'A' + number);

  return g_string_free (str, FALSE);
}

static char *
number_suffix (int number)
{
  return number > 0 ? g_strdup_printf ("%d", number + 1) : g_strdup ("");
}

static void
get_label_names (int    action,
                 int    idx,
                 int    dir,
                 char **label_name,
                 char **leader_name)
{
  g_autofree char *num_str = NULL;
  const char *dir_str;

  switch (action)
    {
    case META_PAD_FEATURE_RING:
      num_str = number_suffix (idx);
      dir_str = dir == META_PAD_DIRECTION_CW ? "CW" : "CCW";
      *label_name = g_strdup_printf ("LabelRing%s%s", num_str, dir_str);
      *leader_name = g_strdup_printf ("LeaderRing%s%s", num_str, dir_str);
      break;

    case META_PAD_FEATURE_STRIP:
      num_str = number_suffix (idx);
      dir_str = dir == META_PAD_DIRECTION_UP ? "Up" : "Down";
      *label_name = g_strdup_printf ("LabelStrip%s%s", num_str, dir_str);
      *leader_name = g_strdup_printf ("LeaderStrip%s%s", num_str, dir_str);
      break;

    case META_PAD_FEATURE_DIAL:
      num_str = number_suffix (idx);
      dir_str = dir == META_PAD_DIRECTION_CW ? "CW" : "CCW";
      *label_name = g_strdup_printf ("LabelDial%s%s", num_str, dir_str);
      *leader_name = g_strdup_printf ("LeaderDial%s%s", num_str, dir_str);
      break;

    default:
      {
        g_autofree char *ch = letter_for_number (idx);

        *label_name = g_strdup_printf ("Label%s", ch);
        *leader_name = g_strdup_printf ("Leader%s", ch);
      }
      break;
    }
}

/* Where the label of the feature goes, and which way the text grows */
static gboolean
get_item_label_coords (ShellPadDiagram *self,
                       const char      *label_name,
                       const char      *leader_name,
                       int             *x,
                       int             *y,
                       PadArrangement  *arrangement)
{
  int label_x, label_y, label_width, label_height;
  int leader_x, leader_y, leader_width, leader_height;
  PadArrangement direction;

  if (self->handle == NULL)
    return FALSE;

  if (!get_element_geometry (self->handle, label_name,
                             &label_x, &label_y, &label_width, &label_height))
    return FALSE;

  if (!get_element_geometry (self->handle, leader_name,
                             &leader_x, &leader_y, &leader_width, &leader_height))
    return FALSE;

  if (label_x > leader_x + leader_width)
    direction = PAD_ARRANGEMENT_LTR;
  else
    direction = PAD_ARRANGEMENT_RTL;

  *x = label_x;
  *y = label_y + label_height;
  if (self->left_handed)
    {
      direction = 1 - direction;
      *x = self->image_width - *x;
      *y = self->image_height - *y;
    }

  *arrangement = direction;

  return TRUE;
}

static gboolean
shell_pad_diagram_add_label (ShellPadDiagram *self,
                             int              action,
                             int              idx,
                             int              dir)
{
  g_autofree char *label_name = NULL;
  g_autofree char *leader_name = NULL;
  PadArrangement arrangement;
  PadLabel *pad_label;
  ClutterActor *label;
  int x, y;

  get_label_names (action, idx, dir, &label_name, &leader_name);
  if (!get_item_label_coords (self, label_name, leader_name, &x, &y, &arrangement))
    return FALSE;

  label = g_object_new (ST_TYPE_LABEL, NULL);

  pad_label = g_new0 (PadLabel, 1);
  g_set_weak_pointer (&pad_label->label, ST_LABEL (label));
  pad_label->action = action;
  pad_label->idx = idx;
  pad_label->dir = dir;
  pad_label->x = x;
  pad_label->y = y;
  pad_label->arrangement = arrangement;
  g_ptr_array_add (self->labels, pad_label);

  clutter_actor_add_child (CLUTTER_ACTOR (self), label);

  return TRUE;
}

static void
shell_pad_diagram_init_labels (ShellPadDiagram *self)
{
  int i;

  for (i = 0; ; i++)
    {
      if (!shell_pad_diagram_add_label (self, PAD_ACTION_BUTTON, i, PAD_DIRECTION_NONE))
        break;
    }

  for (i = 0; ; i++)
    {
      if (!shell_pad_diagram_add_label (self, META_PAD_FEATURE_RING, i, META_PAD_DIRECTION_CW) ||
          !shell_pad_diagram_add_label (self, META_PAD_FEATURE_RING, i, META_PAD_DIRECTION_CCW))
        break;
    }

  for (i = 0; ; i++)
    {
      if (!shell_pad_diagram_add_label (self, META_PAD_FEATURE_STRIP, i, META_PAD_DIRECTION_UP) ||
          !shell_pad_diagram_add_label (self, META_PAD_FEATURE_STRIP, i, META_PAD_DIRECTION_DOWN))
        break;
    }

  for (i = 0; ; i++)
    {
      if (!shell_pad_diagram_add_label (self, META_PAD_FEATURE_DIAL, i, META_PAD_DIRECTION_CW) ||
          !shell_pad_diagram_add_label (self, META_PAD_FEATURE_DIAL, i, META_PAD_DIRECTION_CCW))
        break;
    }
}

/* The image wrapped in a document which carries the css of the pad OSD and
 * highlights the active buttons */
static RsvgHandle *
shell_pad_diagram_compose_styled (ShellPadDiagram *self)
{
  g_autoptr (GString) svg_data = NULL;
  g_autoptr (GInputStream) istream = NULL;
  g_autoptr (GFile) base_file = NULL;
  g_autoptr (GError) error = NULL;
  RsvgHandle *handle;
  guint i;

  if (!g_file_test (self->image_path, G_FILE_TEST_EXISTS))
    return NULL;

  svg_data = g_string_new (NULL);

  /* header */
  g_string_append_printf (svg_data,
                          "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>"
                          "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\" "
                          "xmlns:xi=\"http://www.w3.org/2001/XInclude\" "
                          "width=\"%d\" height=\"%d\"> "
                          "<style type=\"text/css\">",
                          self->image_width, self->image_height);

  /* css */
  g_string_append (svg_data, self->css != NULL ? self->css : "");
  for (i = 0; i < self->active_buttons->len; i++)
    {
      g_autofree char *ch = letter_for_number (g_array_index (self->active_buttons, int, i));

      g_string_append_printf (svg_data,
                              ".%s.Leader { stroke: %s !important; }",
                              ch, ACTIVE_COLOR);
      g_string_append_printf (svg_data,
                              ".%s.Button { stroke: %s !important; fill: %s !important; }",
                              ch, ACTIVE_COLOR, ACTIVE_COLOR);
    }

  /* footer */
  g_string_append_printf (svg_data,
                          "</style><xi:include href=\"%s\" /></svg>",
                          self->image_path);

  istream = g_memory_input_stream_new_from_data (g_strdup (svg_data->str),
                                                 svg_data->len, g_free);
  base_file = g_file_new_for_path (self->image_path);

  handle = rsvg_handle_new_from_stream_sync (istream, base_file,
                                             RSVG_HANDLE_FLAGS_NONE,
                                             NULL, &error);
  if (handle == NULL)
    g_warning ("%s: failed to load %s: %s", G_STRFUNC, self->image_path, error->message);

  return handle;
}

static void
shell_pad_diagram_invalidate_svg (ShellPadDiagram *self)
{
  if (self->handle == NULL)
    return;

  g_clear_object (&self->handle);
  self->handle = shell_pad_diagram_compose_styled (self);
  st_drawing_area_queue_repaint (ST_DRAWING_AREA (self));
}

static void
shell_pad_diagram_set_image (ShellPadDiagram *self,
                             const char      *image_path)
{
  g_autoptr (GError) error = NULL;
  RsvgHandle *original;

  original = rsvg_handle_new_from_file (image_path, &error);
  if (original == NULL)
    {
      g_warning ("%s: failed to load %s: %s", G_STRFUNC, image_path, error->message);
      return;
    }

  get_handle_dimensions (original, &self->image_width, &self->image_height);
  g_object_unref (original);

  g_free (self->image_path);
  self->image_path = g_strdup (image_path);
  g_clear_object (&self->handle);
  self->handle = shell_pad_diagram_compose_styled (self);
  shell_pad_diagram_init_labels (self);
}

static void
shell_pad_diagram_set_editor_actor (ShellPadDiagram *self,
                                    ClutterActor    *editor_actor)
{
  clutter_actor_hide (editor_actor);
  g_set_weak_pointer (&self->editor_actor, editor_actor);
  clutter_actor_add_child (CLUTTER_ACTOR (self), editor_actor);
}

static void
shell_pad_diagram_update_scale (ShellPadDiagram *self)
{
  int width, height;

  clutter_actor_get_size (CLUTTER_ACTOR (self), &self->actor_width, &self->actor_height);
  get_handle_dimensions (self->handle, &width, &height);
  self->scale = MIN ((double) self->actor_width / width,
                     (double) self->actor_height / height);
  self->has_scale = TRUE;
}

static void
shell_pad_diagram_allocate_child (ShellPadDiagram *self,
                                  ClutterActor    *child,
                                  int              x,
                                  int              y,
                                  PadArrangement   direction)
{
  ClutterActorBox child_box;
  float nat_width, nat_height;
  double child_x, child_y;
  int width, height;

  clutter_actor_get_preferred_height (child, -1, NULL, &nat_height);
  clutter_actor_get_preferred_width (child, nat_height, NULL, &nat_width);

  get_handle_dimensions (self->handle, &width, &height);
  child_x = x * self->scale + self->actor_width / 2.0 - width / 2.0 * self->scale;
  child_y = y * self->scale + self->actor_height / 2.0 - height / 2.0 * self->scale;

  if (direction == PAD_ARRANGEMENT_LTR)
    {
      child_box.x1 = child_x;
      child_box.x2 = child_x + nat_width;
    }
  else
    {
      child_box.x1 = child_x - nat_width;
      child_box.x2 = child_x;
    }

  child_box.y1 = child_y - nat_height / 2;
  child_box.y2 = child_y + nat_height / 2;
  clutter_actor_allocate (child, &child_box);
}

static void
shell_pad_diagram_allocate (ClutterActor          *actor,
                            const ClutterActorBox *box)
{
  ShellPadDiagram *self = SHELL_PAD_DIAGRAM (actor);
  guint i;

  CLUTTER_ACTOR_CLASS (shell_pad_diagram_parent_class)->allocate (actor, box);

  if (self->handle == NULL)
    return;

  shell_pad_diagram_update_scale (self);

  for (i = 0; i < self->labels->len; i++)
    {
      PadLabel *pad_label = g_ptr_array_index (self->labels, i);

      if (pad_label->label != NULL)
        shell_pad_diagram_allocate_child (self, CLUTTER_ACTOR (pad_label->label),
                                          pad_label->x, pad_label->y,
                                          pad_label->arrangement);
    }

  if (self->editor_actor != NULL && self->cur_edited != NULL)
    shell_pad_diagram_allocate_child (self, self->editor_actor,
                                      self->cur_edited->x, self->cur_edited->y,
                                      self->cur_edited->arrangement);
}

static void
shell_pad_diagram_repaint (StDrawingArea *area)
{
  ShellPadDiagram *self = SHELL_PAD_DIAGRAM (area);
  guint width, height;
  int image_width, image_height;
  cairo_t *cr;

  if (self->handle == NULL)
    return;

  if (!self->has_scale)
    shell_pad_diagram_update_scale (self);

  st_drawing_area_get_surface_size (area, &width, &height);
  get_handle_dimensions (self->handle, &image_width, &image_height);
  cr = st_drawing_area_get_context (area);

  cairo_save (cr);
  cairo_translate (cr, width / 2.0, height / 2.0);
  cairo_scale (cr, self->scale, self->scale);
  if (self->left_handed)
    cairo_rotate (cr, G_PI);
  cairo_translate (cr, -image_width / 2.0, -image_height / 2.0);
  render_handle (self->handle, cr);
  cairo_restore (cr);
  cairo_destroy (cr);
}

static void
shell_pad_diagram_activate_button (ShellPadDiagram *self,
                                   int              button)
{
  g_array_append_val (self->active_buttons, button);
  shell_pad_diagram_invalidate_svg (self);
}

static void
shell_pad_diagram_deactivate_button (ShellPadDiagram *self,
                                     int              button)
{
  guint i;

  for (i = 0; i < self->active_buttons->len; )
    {
      if (g_array_index (self->active_buttons, int, i) == button)
        g_array_remove_index (self->active_buttons, i);
      else
        i++;
    }

  shell_pad_diagram_invalidate_svg (self);
}

static void
shell_pad_diagram_update_labels (ShellPadDiagram    *self,
                                 PadDiagramTextFunc  get_text,
                                 gpointer            user_data)
{
  guint i;

  for (i = 0; i < self->labels->len; i++)
    {
      PadLabel *pad_label = g_ptr_array_index (self->labels, i);
      g_autofree char *str = get_text (pad_label->action, pad_label->idx,
                                       pad_label->dir, user_data);

      if (pad_label->label != NULL)
        st_label_set_text (pad_label->label, str);
    }

  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

static void
shell_pad_diagram_stop_edition (ShellPadDiagram *self,
                                const char      *str)
{
  if (self->editor_actor != NULL)
    clutter_actor_hide (self->editor_actor);

  if (self->cur_edited != NULL)
    {
      if (self->cur_edited->label != NULL)
        {
          if (str != NULL)
            st_label_set_text (self->cur_edited->label, str);
          clutter_actor_show (CLUTTER_ACTOR (self->cur_edited->label));
        }

      self->cur_edited = NULL;
    }

  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

static void
shell_pad_diagram_start_edition (ShellPadDiagram *self,
                                 int              action,
                                 int              idx,
                                 int              dir)
{
  guint i;

  if (self->cur_edited != NULL)
    return;

  for (i = 0; i < self->labels->len; i++)
    {
      PadLabel *pad_label = g_ptr_array_index (self->labels, i);

      if (action == pad_label->action &&
          idx == pad_label->idx &&
          dir == pad_label->dir)
        {
          self->cur_edited = pad_label;
          break;
        }
    }

  if (self->cur_edited == NULL)
    return;

  if (self->editor_actor != NULL)
    clutter_actor_show (self->editor_actor);
  if (self->cur_edited->label != NULL)
    clutter_actor_hide (CLUTTER_ACTOR (self->cur_edited->label));
  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

static void
shell_pad_diagram_dispose (GObject *object)
{
  ShellPadDiagram *self = SHELL_PAD_DIAGRAM (object);

  self->cur_edited = NULL;
  g_clear_object (&self->handle);

  G_OBJECT_CLASS (shell_pad_diagram_parent_class)->dispose (object);

  g_clear_weak_pointer (&self->editor_actor);
}

static void
shell_pad_diagram_finalize (GObject *object)
{
  ShellPadDiagram *self = SHELL_PAD_DIAGRAM (object);

  g_clear_pointer (&self->labels, g_ptr_array_unref);
  g_clear_pointer (&self->active_buttons, g_array_unref);
  g_clear_pointer (&self->image_path, g_free);
  g_clear_pointer (&self->css, g_free);

  G_OBJECT_CLASS (shell_pad_diagram_parent_class)->finalize (object);
}

static void
shell_pad_diagram_class_init (ShellPadDiagramClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StDrawingAreaClass *drawing_area_class = ST_DRAWING_AREA_CLASS (klass);

  object_class->dispose = shell_pad_diagram_dispose;
  object_class->finalize = shell_pad_diagram_finalize;
  actor_class->allocate = shell_pad_diagram_allocate;
  drawing_area_class->repaint = shell_pad_diagram_repaint;
}

static void
shell_pad_diagram_init (ShellPadDiagram *self)
{
  g_autoptr (GFile) file = g_file_new_for_uri (PAD_DIAGRAM_CSS_URI);
  g_autoptr (GError) error = NULL;
  char *contents = NULL;

  self->labels = g_ptr_array_new_with_free_func (pad_label_free);
  self->active_buttons = g_array_new (FALSE, FALSE, sizeof (int));

  if (g_file_load_contents (file, NULL, &contents, NULL, NULL, &error))
    self->css = contents;
  else
    g_warning ("%s: failed to load %s: %s", G_STRFUNC, PAD_DIAGRAM_CSS_URI, error->message);
}

/* The labels depend on the handedness, so it comes first, and the editor
 * goes above the labels */
static ShellPadDiagram *
shell_pad_diagram_new (const char   *image_path,
                       gboolean      left_handed,
                       ClutterActor *editor_actor)
{
  ShellPadDiagram *self;

  self = g_object_new (SHELL_TYPE_PAD_DIAGRAM,
                       "x-expand", TRUE,
                       "y-expand", TRUE,
                       NULL);
  self->left_handed = left_handed;
  shell_pad_diagram_set_image (self, image_path);
  shell_pad_diagram_set_editor_actor (self, editor_actor);

  return self;
}

/* ======================================================================== */
/* ShellPadOsd                                                              */
/* ======================================================================== */

enum
{
  PROP_0,

  PROP_PAD_DEVICE,
  PROP_SETTINGS,
  PROP_IMAGE_PATH,
  PROP_EDITION_MODE,
  PROP_MONITOR_INDEX,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  PAD_SELECTED,
  CLOSED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

typedef struct
{
  int type; /* a #MetaPadFeatureType or PAD_ACTION_BUTTON */
  int number;
  int dir;
  guint mode;
} EditedAction;

struct _ShellPadOsd
{
  StBoxLayout parent;

  ClutterInputDevice *pad_device;
  GPtrArray *group_pads; /* ClutterInputDevice, including pad_device */
  GSettings *settings;
  char *image_path;
  gboolean edition_mode;
  int monitor_index;

  ClutterSeat *seat; /* weak */
  gulong device_added_id;
  gulong device_removed_id;

  ShellPadChooser *pad_chooser; /* weak, a child of title_box */
  StBoxLayout *title_box; /* weak */
  StLabel *title_label; /* weak */
  StLabel *tip_label; /* weak */
  ShellPadActionEditor *action_editor; /* weak */
  ShellPadDiagram *pad_diagram; /* weak */
  StButton *edit_button; /* weak */

  gboolean has_edited_action;
  EditedAction edited_action;
  GSettings *edited_action_settings;

  ClutterGrab *grab;
};

G_DEFINE_FINAL_TYPE (ShellPadOsd, shell_pad_osd, ST_TYPE_BOX_LAYOUT)

static void start_ring_action_edition   (ShellPadOsd *self, int ring,  int dir, guint mode);
static void start_strip_action_edition  (ShellPadOsd *self, int strip, int dir, guint mode);
static void start_dial_action_edition   (ShellPadOsd *self, int dial,  int dir, guint mode);

static gboolean
group_pads_includes (ShellPadOsd        *self,
                     ClutterInputDevice *device)
{
  return device != NULL && g_ptr_array_find (self->group_pads, device, NULL);
}


static void request_for_other_pad (ShellPadOsd        *self,
                                   ClutterInputDevice *pad);

static void
pad_chooser_pad_selected_cb (ShellPadChooser    *chooser,
                             ClutterInputDevice *pad,
                             ShellPadOsd        *self)
{
  request_for_other_pad (self, pad);
}

static void
update_pad_chooser (ShellPadOsd *self)
{
  if (self->group_pads->len > 1)
    {
      if (self->pad_chooser == NULL)
        {
          ShellPadChooser *chooser = shell_pad_chooser_new (self->pad_device,
                                                            self->group_pads);

          g_signal_connect (chooser, "pad-selected",
                            G_CALLBACK (pad_chooser_pad_selected_cb), self);
          g_set_weak_pointer (&self->pad_chooser, chooser);
          clutter_actor_add_child (CLUTTER_ACTOR (self->title_box),
                                   CLUTTER_ACTOR (chooser));
        }
      else
        {
          shell_pad_chooser_update (self->pad_chooser, self->group_pads);
        }
    }
  else if (self->pad_chooser != NULL)
    {
      clutter_actor_destroy (CLUTTER_ACTOR (self->pad_chooser));
      g_clear_weak_pointer (&self->pad_chooser);
    }
}

/* Replaces this OSD with the one of another pad of the same group */
static void
request_for_other_pad (ShellPadOsd        *self,
                       ClutterInputDevice *pad)
{
  g_autoptr (ShellPadOsd) keep_alive = NULL;
  ClutterInputDevice *pad_ref;
  MetaDisplay *display;
  gboolean edition_mode;

  if (pad == self->pad_device || !group_pads_includes (self, pad))
    return;

  keep_alive = g_object_ref (self);
  pad_ref = g_object_ref (pad);
  edition_mode = self->edition_mode;
  display = shell_global_get_display (shell_global_get ());

  clutter_actor_destroy (CLUTTER_ACTOR (self));
  meta_display_request_pad_osd (display, pad_ref, edition_mode);

  g_object_unref (pad_ref);
}

static char *
get_action_text (ShellPadOsd *self,
                 int          type,
                 int          number,
                 int          dir)
{
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  char *str;

  if (type == META_PAD_FEATURE_RING ||
      type == META_PAD_FEATURE_STRIP ||
      type == META_PAD_FEATURE_DIAL)
    str = meta_display_get_pad_feature_label (display, self->pad_device,
                                              type, dir, number);
  else
    str = meta_display_get_pad_button_label (display, self->pad_device, number);

  return str != NULL ? str : g_strdup (_("None"));
}

static char *
get_action_text_cb (int      action,
                    int      idx,
                    int      dir,
                    gpointer user_data)
{
  return get_action_text (user_data, action, idx, dir);
}

static void
update_action_labels (ShellPadOsd *self)
{
  if (self->pad_diagram != NULL)
    shell_pad_diagram_update_labels (self->pad_diagram, get_action_text_cb, self);
}

static void
sync_edition_mode (ShellPadOsd *self)
{
  ClutterActor *edit_button = CLUTTER_ACTOR (self->edit_button);
  const char *title;

  if (self->edit_button == NULL || self->title_label == NULL || self->tip_label == NULL)
    return;

  clutter_actor_set_reactive (edit_button, !self->edition_mode);
  clutter_actor_save_easing_state (edit_button);
  clutter_actor_set_easing_duration (edit_button, 200);
  clutter_actor_set_opacity (edit_button, self->edition_mode ? 128 : 255);
  clutter_actor_restore_easing_state (edit_button);

  if (self->edition_mode)
    {
      title = _("Press a button to configure");
      st_label_set_text (self->tip_label, _("Press Esc to exit"));
    }
  else
    {
      title = clutter_input_device_get_device_name (self->pad_device);
      st_label_set_text (self->tip_label, _("Press any key to exit"));
    }

  st_label_set_text (self->title_label, title);
}

static gboolean
is_edited_action (ShellPadOsd *self,
                  int          type,
                  int          number,
                  int          dir)
{
  if (!self->has_edited_action)
    return FALSE;

  return self->edited_action.type == type &&
         self->edited_action.number == number &&
         self->edited_action.dir == dir;
}

static void
end_action_edition (ShellPadOsd *self)
{
  if (self->action_editor != NULL)
    shell_pad_action_editor_close (self->action_editor);

  if (self->has_edited_action)
    {
      EditedAction edited = self->edited_action;
      g_autofree char *str = get_action_text (self, edited.type, edited.number, edited.dir);
      gboolean has_next_action =
        (edited.type == META_PAD_FEATURE_RING && edited.dir == META_PAD_DIRECTION_CCW) ||
        (edited.type == META_PAD_FEATURE_STRIP && edited.dir == META_PAD_DIRECTION_UP) ||
        (edited.type == META_PAD_FEATURE_DIAL && edited.dir == META_PAD_DIRECTION_CCW);

      if (self->pad_diagram != NULL)
        shell_pad_diagram_stop_edition (self->pad_diagram, str);
      self->has_edited_action = FALSE;

      /* Maybe follow up on next ring/strip direction */
      if (has_next_action)
        {
          if (edited.type == META_PAD_FEATURE_RING)
            start_ring_action_edition (self, edited.number, META_PAD_DIRECTION_CW, edited.mode);
          else if (edited.type == META_PAD_FEATURE_DIAL)
            start_dial_action_edition (self, edited.number, META_PAD_DIRECTION_CW, edited.mode);
          else
            start_strip_action_edition (self, edited.number, META_PAD_DIRECTION_DOWN, edited.mode);
        }
    }

  g_clear_object (&self->edited_action_settings);
}

static void
start_action_edition (ShellPadOsd *self,
                      const char  *key,
                      int          type,
                      int          number,
                      int          dir,
                      guint        mode)
{
  GSettingsSchemaSource *schema_source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = NULL;
  g_autofree char *path = NULL;
  g_autofree char *settings_path = NULL;

  if (self->action_editor == NULL || self->pad_diagram == NULL)
    return;

  if (is_edited_action (self, type, number, dir))
    return;

  if (schema_source != NULL)
    schema = g_settings_schema_source_lookup (schema_source, PAD_BUTTON_SCHEMA, TRUE);
  g_object_get (self->settings, "path", &path, NULL);
  if (schema == NULL || path == NULL)
    {
      g_warning ("%s: cannot edit the actions of %s", G_STRFUNC,
                 clutter_input_device_get_device_name (self->pad_device));
      return;
    }

  end_action_edition (self);
  self->has_edited_action = TRUE;
  self->edited_action.type = type;
  self->edited_action.number = number;
  self->edited_action.dir = dir;
  self->edited_action.mode = mode;

  settings_path = g_strdup_printf ("%s%s/", path, key);
  self->edited_action_settings = g_settings_new_full (schema, NULL, settings_path);
  shell_pad_action_editor_set_settings (self->action_editor,
                                        self->edited_action_settings, type);
  shell_pad_diagram_start_edition (self->pad_diagram, type, number, dir);
}

static void
start_button_action_edition (ShellPadOsd *self,
                             int          button)
{
  g_autofree char *ch = letter_for_number (button);
  g_autofree char *key = g_strdup_printf ("button%s", ch);

  start_action_edition (self, key, PAD_ACTION_BUTTON, button, PAD_DIRECTION_NONE, 0);
}

static void
start_ring_action_edition (ShellPadOsd *self,
                           int          ring,
                           int          dir,
                           guint        mode)
{
  g_autofree char *ch = letter_for_number (ring);
  g_autofree char *key = g_strdup_printf ("ring%s-%s-mode-%u", ch,
                                          dir == META_PAD_DIRECTION_CCW ? "ccw" : "cw",
                                          mode);

  start_action_edition (self, key, META_PAD_FEATURE_RING, ring, dir, mode);
}

static void
start_strip_action_edition (ShellPadOsd *self,
                            int          strip,
                            int          dir,
                            guint        mode)
{
  g_autofree char *ch = letter_for_number (strip);
  g_autofree char *key = g_strdup_printf ("strip%s-%s-mode-%u", ch,
                                          dir == META_PAD_DIRECTION_UP ? "up" : "down",
                                          mode);

  start_action_edition (self, key, META_PAD_FEATURE_STRIP, strip, dir, mode);
}

static void
start_dial_action_edition (ShellPadOsd *self,
                           int          dial,
                           int          dir,
                           guint        mode)
{
  g_autofree char *ch = letter_for_number (dial);
  g_autofree char *key = g_strdup_printf ("dial%s-%s-mode-%u", ch,
                                          dir == META_PAD_DIRECTION_CCW ? "ccw" : "cw",
                                          mode);

  start_action_edition (self, key, META_PAD_FEATURE_DIAL, dial, dir, mode);
}

static void
get_pad_details (ClutterEvent *event,
                 guint        *number,
                 guint        *mode)
{
  ClutterInputDevicePadSource source;
  double value;

  clutter_event_get_pad_details (event, number, mode, &source, &value);
}

static gboolean
shell_pad_osd_captured_event (ClutterActor *actor,
                              ClutterEvent *event)
{
  ShellPadOsd *self = SHELL_PAD_OSD (actor);
  ClutterEventType type = clutter_event_type (event);
  ClutterInputDevice *source_device = clutter_event_get_source_device (event);
  gboolean is_mode_switch = FALSE;
  guint number = 0, mode = 0;

  if (type == CLUTTER_PAD_BUTTON_PRESS || type == CLUTTER_PAD_BUTTON_RELEASE)
    is_mode_switch =
      clutter_input_device_get_mode_switch_button_group (self->pad_device,
                                                         clutter_event_get_button (event)) >= 0;

  if (type == CLUTTER_PAD_BUTTON_PRESS && source_device == self->pad_device)
    {
      int button = clutter_event_get_button (event);

      if (self->pad_diagram != NULL)
        shell_pad_diagram_activate_button (self->pad_diagram, button);

      /* Buttons that switch between modes cannot be edited */
      if (self->edition_mode && !is_mode_switch)
        start_button_action_edition (self, button);
      return CLUTTER_EVENT_STOP;
    }
  else if (type == CLUTTER_PAD_BUTTON_RELEASE && source_device == self->pad_device)
    {
      if (self->pad_diagram != NULL)
        shell_pad_diagram_deactivate_button (self->pad_diagram,
                                             clutter_event_get_button (event));

      if (is_mode_switch)
        {
          end_action_edition (self);
          update_action_labels (self);
        }
      return CLUTTER_EVENT_STOP;
    }
  else if (type == CLUTTER_KEY_PRESS &&
           (!self->edition_mode ||
            clutter_event_get_key_symbol (event) == CLUTTER_KEY_Escape))
    {
      if (self->has_edited_action)
        end_action_edition (self);
      else
        clutter_actor_destroy (actor);
      return CLUTTER_EVENT_STOP;
    }
  else if (source_device == self->pad_device && type == CLUTTER_PAD_STRIP)
    {
      if (self->edition_mode)
        {
          get_pad_details (event, &number, &mode);
          start_strip_action_edition (self, number, META_PAD_DIRECTION_UP, mode);
        }
    }
  else if (source_device == self->pad_device && type == CLUTTER_PAD_RING)
    {
      if (self->edition_mode)
        {
          get_pad_details (event, &number, &mode);
          start_ring_action_edition (self, number, META_PAD_DIRECTION_CCW, mode);
        }
    }
  else if (source_device == self->pad_device && type == CLUTTER_PAD_DIAL)
    {
      if (self->edition_mode)
        {
          get_pad_details (event, &number, &mode);
          start_dial_action_edition (self, number, META_PAD_DIRECTION_CCW, mode);
        }
    }

  /* If the event comes from another pad in the same group,
   * show the OSD for it. */
  if (group_pads_includes (self, source_device))
    {
      request_for_other_pad (self, source_device);
      return CLUTTER_EVENT_STOP;
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static void
seat_device_added_cb (ClutterSeat        *seat,
                      ClutterInputDevice *device,
                      ShellPadOsd        *self)
{
  if (clutter_input_device_get_device_type (device) == CLUTTER_PAD_DEVICE &&
      clutter_input_device_is_grouped (self->pad_device, device))
    {
      g_ptr_array_add (self->group_pads, g_object_ref (device));
      update_pad_chooser (self);
    }
}

static void
seat_device_removed_cb (ClutterSeat        *seat,
                        ClutterInputDevice *device,
                        ShellPadOsd        *self)
{
  /* If the device is being removed, destroy the padOsd. */
  if (device == self->pad_device)
    {
      clutter_actor_destroy (CLUTTER_ACTOR (self));
    }
  else if (group_pads_includes (self, device))
    {
      /* Or update the pad chooser if the device belongs to
       * the same group. */
      g_ptr_array_remove (self->group_pads, device);
      update_pad_chooser (self);
    }
}

static void
disconnect_seat (ShellPadOsd *self)
{
  if (self->seat != NULL)
    {
      g_clear_signal_handler (&self->device_added_id, self->seat);
      g_clear_signal_handler (&self->device_removed_id, self->seat);
    }

  g_clear_weak_pointer (&self->seat);
}

/* Connected to ::destroy before the modal is pushed, so that it runs before
 * the handler of the modal stack and pops the modal itself */
static void
pad_osd_destroyed_cb (ClutterActor *actor,
                      ShellPadOsd  *self)
{
  disconnect_seat (self);

  if (self->grab != NULL)
    {
      g_autoptr (GError) error = NULL;

      /* This fails when the shell is going down and the modal stack is
       * already gone: nothing to restore then, and nothing to warn about */
      shell_main_pop_modal (shell_main_get_default (), self->grab, &error);
      g_clear_object (&self->grab);
    }

  if (self->action_editor != NULL)
    shell_pad_action_editor_close (self->action_editor);

  g_signal_emit (self, signals[CLOSED], 0);
}

static void
edit_button_clicked_cb (ShellPadOsd *self)
{
  shell_pad_osd_set_edition_mode (self, TRUE);
}

static void
shell_pad_osd_constructed (GObject *object)
{
  ShellPadOsd *self = SHELL_PAD_OSD (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *ui_group;
  ClutterBackend *backend;
  ClutterSeat *seat;
  ClutterActor *label_box;
  ClutterActor *button_box;
  ClutterActor *editor;
  ClutterActor *title_label;
  ClutterActor *tip_label;
  ClutterActor *edit_button;
  g_autoptr (GList) devices = NULL;
  g_autoptr (GSettingsSchema) schema = NULL;
  gboolean left_handed = FALSE;
  GList *l;

  G_OBJECT_CLASS (shell_pad_osd_parent_class)->constructed (object);

  /* The layout manager of an StBoxLayout is only there after chaining up */
  st_box_layout_set_orientation (ST_BOX_LAYOUT (self), CLUTTER_ORIENTATION_VERTICAL);

  g_return_if_fail (CLUTTER_IS_INPUT_DEVICE (self->pad_device));
  g_return_if_fail (G_IS_SETTINGS (self->settings));
  g_return_if_fail (self->image_path != NULL);

  self->group_pads = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (self->group_pads, g_object_ref (self->pad_device));

  backend = clutter_context_get_backend (clutter_actor_get_context (actor));
  seat = clutter_backend_get_default_seat (backend);
  g_set_weak_pointer (&self->seat, seat);
  self->device_added_id = g_signal_connect (seat, "device-added",
                                            G_CALLBACK (seat_device_added_cb), self);
  self->device_removed_id = g_signal_connect (seat, "device-removed",
                                              G_CALLBACK (seat_device_removed_cb), self);

  devices = clutter_seat_list_devices (seat);
  for (l = devices; l != NULL; l = l->next)
    {
      ClutterInputDevice *device = l->data;

      if (device != self->pad_device &&
          clutter_input_device_get_device_type (device) == CLUTTER_PAD_DEVICE &&
          clutter_input_device_is_grouped (self->pad_device, device))
        g_ptr_array_add (self->group_pads, g_object_ref (device));
    }

  g_signal_connect (self, "destroy", G_CALLBACK (pad_osd_destroyed_cb), self);

  ui_group = get_ui_group ();
  if (ui_group != NULL)
    clutter_actor_add_child (ui_group, actor);
  else
    g_warning ("%s: no layout manager to add the pad OSD to", G_STRFUNC);

  clutter_actor_add_constraint (actor,
                                g_object_new (SHELL_TYPE_MONITOR_CONSTRAINT,
                                              "index", self->monitor_index,
                                              NULL));

  self->title_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                  "style-class", "pad-osd-title-box",
                                  "orientation", CLUTTER_ORIENTATION_HORIZONTAL,
                                  "x-expand", FALSE,
                                  "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                  NULL);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (self->title_box));
  g_object_add_weak_pointer (G_OBJECT (self->title_box), (gpointer *) &self->title_box);

  label_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "style-class", "pad-osd-title-menu-box",
                            "orientation", CLUTTER_ORIENTATION_VERTICAL,
                            NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self->title_box), label_box);

  title_label = g_object_new (ST_TYPE_LABEL,
                              "style", "font-side: larger; font-weight: bold;",
                              "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                              NULL);
  clutter_text_set_ellipsize (st_label_get_clutter_text (ST_LABEL (title_label)),
                              PANGO_ELLIPSIZE_NONE);
  clutter_text_set_text (st_label_get_clutter_text (ST_LABEL (title_label)),
                         clutter_input_device_get_device_name (self->pad_device));
  clutter_actor_add_child (label_box, title_label);
  g_set_weak_pointer (&self->title_label, ST_LABEL (title_label));

  tip_label = g_object_new (ST_TYPE_LABEL,
                            "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                            NULL);
  clutter_actor_add_child (label_box, tip_label);
  g_set_weak_pointer (&self->tip_label, ST_LABEL (tip_label));

  update_pad_chooser (self);

  self->action_editor = shell_pad_action_editor_new ();
  editor = CLUTTER_ACTOR (self->action_editor);
  g_object_add_weak_pointer (G_OBJECT (self->action_editor),
                             (gpointer *) &self->action_editor);
  g_signal_connect_swapped (self->action_editor, "done",
                            G_CALLBACK (end_action_edition), self);

  g_object_get (self->settings, "settings-schema", &schema, NULL);
  if (schema != NULL && g_settings_schema_has_key (schema, "left-handed"))
    left_handed = g_settings_get_boolean (self->settings, "left-handed");

  self->pad_diagram = shell_pad_diagram_new (self->image_path, left_handed, editor);
  g_object_add_weak_pointer (G_OBJECT (self->pad_diagram),
                             (gpointer *) &self->pad_diagram);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (self->pad_diagram));
  update_action_labels (self);

  button_box = g_object_new (ST_TYPE_WIDGET,
                             "layout-manager", clutter_bin_layout_new (),
                             "x-expand", TRUE,
                             "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             NULL);
  clutter_actor_add_child (actor, button_box);

  edit_button = g_object_new (ST_TYPE_BUTTON,
                              "label", _("Edit…"),
                              "style-class", "button",
                              "can-focus", TRUE,
                              "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                              NULL);
  g_signal_connect_swapped (edit_button, "clicked",
                            G_CALLBACK (edit_button_clicked_cb), self);
  clutter_actor_add_child (button_box, edit_button);
  g_set_weak_pointer (&self->edit_button, ST_BUTTON (edit_button));

  sync_edition_mode (self);

  self->grab = shell_main_push_modal (shell_main_get_default (), actor,
                                      SHELL_ACTION_MODE_NONE);
  if (self->grab != NULL)
    g_object_ref (self->grab);
}

static void
shell_pad_osd_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  ShellPadOsd *self = SHELL_PAD_OSD (object);

  switch (prop_id)
    {
    case PROP_PAD_DEVICE:
      g_value_set_object (value, self->pad_device);
      break;

    case PROP_SETTINGS:
      g_value_set_object (value, self->settings);
      break;

    case PROP_IMAGE_PATH:
      g_value_set_string (value, self->image_path);
      break;

    case PROP_EDITION_MODE:
      g_value_set_boolean (value, self->edition_mode);
      break;

    case PROP_MONITOR_INDEX:
      g_value_set_int (value, self->monitor_index);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_pad_osd_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  ShellPadOsd *self = SHELL_PAD_OSD (object);

  switch (prop_id)
    {
    case PROP_PAD_DEVICE:
      g_set_object (&self->pad_device, g_value_get_object (value));
      break;

    case PROP_SETTINGS:
      g_set_object (&self->settings, g_value_get_object (value));
      break;

    case PROP_IMAGE_PATH:
      g_free (self->image_path);
      self->image_path = g_value_dup_string (value);
      break;

    case PROP_EDITION_MODE:
      shell_pad_osd_set_edition_mode (self, g_value_get_boolean (value));
      break;

    case PROP_MONITOR_INDEX:
      self->monitor_index = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_pad_osd_dispose (GObject *object)
{
  ShellPadOsd *self = SHELL_PAD_OSD (object);

  /* Disposing an actor destroys it first, which runs pad_osd_destroyed_cb () */
  G_OBJECT_CLASS (shell_pad_osd_parent_class)->dispose (object);

  disconnect_seat (self);
  g_clear_object (&self->grab);
  g_clear_object (&self->edited_action_settings);
  g_clear_object (&self->settings);
  g_clear_object (&self->pad_device);
  g_clear_pointer (&self->group_pads, g_ptr_array_unref);

  g_clear_weak_pointer (&self->pad_chooser);
  g_clear_weak_pointer (&self->title_box);
  g_clear_weak_pointer (&self->title_label);
  g_clear_weak_pointer (&self->tip_label);
  g_clear_weak_pointer (&self->action_editor);
  g_clear_weak_pointer (&self->pad_diagram);
  g_clear_weak_pointer (&self->edit_button);
}

static void
shell_pad_osd_finalize (GObject *object)
{
  ShellPadOsd *self = SHELL_PAD_OSD (object);

  g_clear_pointer (&self->image_path, g_free);

  G_OBJECT_CLASS (shell_pad_osd_parent_class)->finalize (object);
}

static void
shell_pad_osd_class_init (ShellPadOsdClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_pad_osd_constructed;
  object_class->get_property = shell_pad_osd_get_property;
  object_class->set_property = shell_pad_osd_set_property;
  object_class->dispose = shell_pad_osd_dispose;
  object_class->finalize = shell_pad_osd_finalize;

  actor_class->captured_event = shell_pad_osd_captured_event;

  /**
   * ShellPadOsd:pad-device:
   *
   * The pad the OSD is for (`padDevice`).
   */
  props[PROP_PAD_DEVICE] =
    g_param_spec_object ("pad-device", NULL, NULL,
                         CLUTTER_TYPE_INPUT_DEVICE,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPadOsd:settings:
   *
   * The settings of the pad. The settings of its buttons, rings, strips and
   * dials are found below its path.
   */
  props[PROP_SETTINGS] =
    g_param_spec_object ("settings", NULL, NULL,
                         G_TYPE_SETTINGS,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPadOsd:image-path:
   *
   * The path of the SVG of the diagram of the pad.
   */
  props[PROP_IMAGE_PATH] =
    g_param_spec_string ("image-path", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPadOsd:edition-mode:
   *
   * Whether pressing a button, or using a ring, a strip or a dial, lets the
   * user change what it does (`setEditionMode ()`).
   */
  props[PROP_EDITION_MODE] =
    g_param_spec_boolean ("edition-mode", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellPadOsd:monitor-index:
   *
   * The index of the monitor the OSD covers.
   */
  props[PROP_MONITOR_INDEX] =
    g_param_spec_int ("monitor-index", NULL, NULL,
                      -1, G_MAXINT, 0,
                      G_PARAM_READWRITE |
                      G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellPadOsd::pad-selected:
   * @self: the #ShellPadOsd
   * @pad: the pad
   *
   * Declared like the signal of the JS class; the OSD handles the choice of
   * the user in the pad chooser itself and never emits it.
   */
  signals[PAD_SELECTED] =
    g_signal_new ("pad-selected",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, CLUTTER_TYPE_INPUT_DEVICE);

  /**
   * ShellPadOsd::closed:
   * @self: the #ShellPadOsd
   *
   * Emitted when the OSD is destroyed.
   */
  signals[CLOSED] =
    g_signal_new ("closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_pad_osd_init (ShellPadOsd *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  st_widget_set_style_class_name (ST_WIDGET (self), "pad-osd-window");
  clutter_actor_set_x_expand (actor, TRUE);
  clutter_actor_set_y_expand (actor, TRUE);
  clutter_actor_set_reactive (actor, TRUE);
}

/**
 * shell_pad_osd_new:
 * @pad: the pad to show the OSD of
 * @settings: the settings of @pad
 * @image_path: the path of the SVG of the diagram of the pad
 * @edition_mode: whether to start in edition mode
 * @monitor_index: the index of the monitor to show the OSD on
 *
 * Creates the OSD of a pad, adds it to `Main.uiGroup` and grabs the input
 * (`new PadOsd.PadOsd (padDevice, settings, imagePath, editionMode,
 * monitorIndex)`). It destroys itself when a key is pressed (outside of the
 * edition mode, or Escape), and emits #ShellPadOsd::closed. The arguments are
 * the ones of the `show-pad-osd` signal of the display, and the OSD is what the
 * handler of that signal returns.
 *
 * Returns: (transfer none): a new #ShellPadOsd, which is owned by the ui
 *   group until it is destroyed
 */
ShellPadOsd *
shell_pad_osd_new (ClutterInputDevice *pad,
                   GSettings          *settings,
                   const char         *image_path,
                   gboolean            edition_mode,
                   int                 monitor_index)
{
  g_return_val_if_fail (CLUTTER_IS_INPUT_DEVICE (pad), NULL);
  g_return_val_if_fail (G_IS_SETTINGS (settings), NULL);
  g_return_val_if_fail (image_path != NULL, NULL);

  return g_object_new (SHELL_TYPE_PAD_OSD,
                       "pad-device", pad,
                       "settings", settings,
                       "image-path", image_path,
                       "edition-mode", edition_mode,
                       "monitor-index", monitor_index,
                       NULL);
}

/**
 * shell_pad_osd_get_pad_device:
 * @self: a #ShellPadOsd
 *
 * Returns: (transfer none): the pad the OSD is for
 */
ClutterInputDevice *
shell_pad_osd_get_pad_device (ShellPadOsd *self)
{
  g_return_val_if_fail (SHELL_IS_PAD_OSD (self), NULL);

  return self->pad_device;
}

/**
 * shell_pad_osd_get_edition_mode:
 * @self: a #ShellPadOsd
 *
 * Returns: whether the OSD is in edition mode
 */
gboolean
shell_pad_osd_get_edition_mode (ShellPadOsd *self)
{
  g_return_val_if_fail (SHELL_IS_PAD_OSD (self), FALSE);

  return self->edition_mode;
}

/**
 * shell_pad_osd_set_edition_mode:
 * @self: a #ShellPadOsd
 * @edition_mode: whether to let the user change the actions
 *
 * Switches the edition mode on or off (`setEditionMode ()`).
 */
void
shell_pad_osd_set_edition_mode (ShellPadOsd *self,
                                gboolean     edition_mode)
{
  g_return_if_fail (SHELL_IS_PAD_OSD (self));

  if (self->edition_mode == edition_mode)
    return;

  self->edition_mode = edition_mode;
  sync_edition_mode (self);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_EDITION_MODE]);
}

/* ======================================================================== */
/* ShellPadOsdService                                                       */
/* ======================================================================== */

struct _ShellPadOsdService
{
  GObject parent;

  ShellWacomPadOsd *skeleton;
  guint name_id;
};

G_DEFINE_FINAL_TYPE (ShellPadOsdService, shell_pad_osd_service, G_TYPE_OBJECT)

static gboolean
pad_osd_service_handle_show (ShellWacomPadOsd      *skeleton,
                             GDBusMethodInvocation *invocation,
                             const char            *device_node,
                             gboolean               edition_mode,
                             ShellPadOsdService    *self)
{
  ShellGlobal *global = shell_global_get ();
  ClutterContext *context = clutter_actor_get_context (CLUTTER_ACTOR (shell_global_get_stage (global)));
  ClutterSeat *seat = clutter_backend_get_default_seat (clutter_context_get_backend (context));
  g_autoptr (GList) devices = clutter_seat_list_devices (seat);
  ClutterInputDevice *pad_device = NULL;
  GList *l;

  for (l = devices; l != NULL; l = l->next)
    {
      ClutterInputDevice *device = l->data;

      if (g_strcmp0 (device_node, clutter_input_device_get_device_node (device)) == 0 &&
          clutter_input_device_get_device_type (device) == CLUTTER_PAD_DEVICE)
        pad_device = device;
    }

  if (pad_device == NULL)
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_IO_ERROR,
                                                     G_IO_ERROR_CANCELLED,
                                                     "Invalid params");
      return TRUE;
    }

  meta_display_request_pad_osd (shell_global_get_display (global),
                                pad_device, edition_mode);
  shell_wacom_pad_osd_complete_show (skeleton, invocation);

  return TRUE;
}

static void
export_and_own_name (ShellPadOsdService *self)
{
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GError) error = NULL;

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (connection == NULL)
    {
      g_message ("%s: %s", G_STRFUNC, error->message);
      return;
    }

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                         connection, PAD_OSD_OBJECT_PATH,
                                         &error))
    {
      g_message ("%s: %s", G_STRFUNC, error->message);
      return;
    }

  self->name_id = g_bus_own_name_on_connection (connection, PAD_OSD_BUS_NAME,
                                                G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                                NULL, NULL, NULL, NULL);
}

static void
shell_pad_osd_service_constructed (GObject *object)
{
  ShellPadOsdService *self = SHELL_PAD_OSD_SERVICE (object);

  G_OBJECT_CLASS (shell_pad_osd_service_parent_class)->constructed (object);

  self->skeleton = shell_wacom_pad_osd_skeleton_new ();
  g_signal_connect (self->skeleton, "handle-show",
                    G_CALLBACK (pad_osd_service_handle_show), self);

  export_and_own_name (self);
}

static void
shell_pad_osd_service_dispose (GObject *object)
{
  ShellPadOsdService *self = SHELL_PAD_OSD_SERVICE (object);

  if (self->name_id != 0)
    {
      g_bus_unown_name (self->name_id);
      self->name_id = 0;
    }

  if (self->skeleton != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->skeleton, self);
      if (g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (self->skeleton)) != NULL)
        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->skeleton));
      g_clear_object (&self->skeleton);
    }

  G_OBJECT_CLASS (shell_pad_osd_service_parent_class)->dispose (object);
}

static void
shell_pad_osd_service_class_init (ShellPadOsdServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_pad_osd_service_constructed;
  object_class->dispose = shell_pad_osd_service_dispose;
}

static void
shell_pad_osd_service_init (ShellPadOsdService *self)
{
}

/**
 * shell_pad_osd_service_new:
 *
 * Creates the service, exports org.gnome.Shell.Wacom.PadOsd at
 * /org/gnome/Shell/Wacom on the session bus and owns the bus name of the
 * interface (`new PadOsdService ()`). `Show (device_node, edition_mode)`
 * asks the display for the OSD of the pad with that device node. js/ui/main.js
 * creates the only instance.
 *
 * Returns: (transfer full): a new #ShellPadOsdService
 */
ShellPadOsdService *
shell_pad_osd_service_new (void)
{
  return g_object_new (SHELL_TYPE_PAD_OSD_SERVICE, NULL);
}
