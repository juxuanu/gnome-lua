/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-entry.c: The context menu of text entries (copy, paste and, for
 * password entries, show or hide the text) and the label which warns that
 * caps lock is on. Port of js/ui/shellEntry.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `addContextMenu (entry, {actionMode})` is shell_entry_add_context_menu ()
 *    with the action mode as an argument, as GI methods have no optional
 *    arguments.
 *  - The `entry.menu` and `entry._menuManager` expandos are object data of
 *    the entry; the menu is shell_entry_get_menu ().
 *  - `Main.uiGroup` is read from #ShellMain.
 *  - Whether a menu has the "Show Text" item is the construct-only
 *    #ShellEntryMenu:is-password property (add_context_menu () sets it for a
 *    #StPasswordEntry) instead of an `instanceof` check of the menu.
 */

#include "config.h"

#include <atk/atk.h>
#include <clutter/clutter.h>
#include <clutter/clutter-pango.h>
#include <glib/gi18n-lib.h>
#include <pango/pango.h>

#include "shell-global.h"
#include "ui/environment.h"
#include "ui/main.h"
#include "ui/shell-entry.h"

#define MENU_DATA_KEY "shell-entry-menu"
#define MENU_MANAGER_DATA_KEY "shell-entry-menu-manager"

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static GWeakRef *
weak_ref_new (gpointer object)
{
  GWeakRef *ref = g_new0 (GWeakRef, 1);

  g_weak_ref_init (ref, object);
  return ref;
}

static void
weak_ref_free (GWeakRef *ref)
{
  g_weak_ref_clear (ref);
  g_free (ref);
}

/* ------------------------------------------------------------------------ */
/* ShellEntryMenu                                                           */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_IS_PASSWORD,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellEntryMenu
{
  ShellPopupMenu parent;

  ShellPopupMenuItem *copy_item;     /* weak */
  ShellPopupMenuItem *paste_item;    /* weak */
  ShellPopupMenuItem *password_item; /* weak */

  gboolean is_password;
};

G_DEFINE_FINAL_TYPE (ShellEntryMenu, shell_entry_menu, SHELL_TYPE_POPUP_MENU)

static StEntry *
menu_get_entry (ShellEntryMenu *self)
{
  ClutterActor *source = shell_popup_menu_base_get_source_actor (SHELL_POPUP_MENU_BASE (self));

  return ST_IS_ENTRY (source) ? ST_ENTRY (source) : NULL;
}

static void
set_item_sensitive (ShellPopupMenuItem *item,
                    gboolean            sensitive)
{
  if (item != NULL)
    shell_popup_base_menu_item_set_sensitive (SHELL_POPUP_BASE_MENU_ITEM (item),
                                              sensitive);
}

static void
update_paste_text_cb (StClipboard *clipboard,
                      const char  *text,
                      gpointer     user_data)
{
  g_autoptr (ShellEntryMenu) self = g_weak_ref_get (user_data);

  if (self != NULL)
    set_item_sensitive (self->paste_item, text != NULL && *text != '\0');

  weak_ref_free (user_data);
}

static void
update_paste_item (ShellEntryMenu *self)
{
  st_clipboard_get_text (st_clipboard_get_default (),
                         ST_CLIPBOARD_TYPE_CLIPBOARD,
                         update_paste_text_cb,
                         weak_ref_new (self));
}

static void
update_copy_item (ShellEntryMenu *self)
{
  StEntry *entry = menu_get_entry (self);
  ClutterText *text;
  g_autofree char *selection = NULL;

  if (entry == NULL)
    return;

  text = st_entry_get_clutter_text (entry);
  selection = clutter_text_get_selection (text);

  set_item_sensitive (self->copy_item,
                      clutter_text_get_password_char (text) == 0 &&
                      selection != NULL && *selection != '\0');
}

static void
update_password_item (ShellEntryMenu *self)
{
  StEntry *entry = menu_get_entry (self);
  StLabel *label;

  if (self->password_item == NULL || !ST_IS_PASSWORD_ENTRY (entry))
    return;

  label = shell_popup_menu_item_get_label (self->password_item);
  if (!st_password_entry_get_password_visible (ST_PASSWORD_ENTRY (entry)))
    st_label_set_text (label, _("Show Text"));
  else
    st_label_set_text (label, _("Hide Text"));
}

static void
copy_activated_cb (ShellPopupBaseMenuItem *item,
                   const ClutterEvent     *event,
                   ShellEntryMenu         *self)
{
  StEntry *entry = menu_get_entry (self);
  g_autofree char *selection = NULL;

  if (entry == NULL)
    return;

  selection = clutter_text_get_selection (st_entry_get_clutter_text (entry));
  st_clipboard_set_text (st_clipboard_get_default (),
                         ST_CLIPBOARD_TYPE_CLIPBOARD,
                         selection != NULL ? selection : "");
}

static void
paste_text_cb (StClipboard *clipboard,
               const char  *text,
               gpointer     user_data)
{
  g_autoptr (ShellEntryMenu) self = g_weak_ref_get (user_data);
  StEntry *entry;
  ClutterText *clutter_text;

  weak_ref_free (user_data);

  if (self == NULL || text == NULL || *text == '\0')
    return;

  entry = menu_get_entry (self);
  if (entry == NULL)
    return;

  clutter_text = st_entry_get_clutter_text (entry);
  clutter_text_delete_selection (clutter_text);
  clutter_text_insert_text (clutter_text, text,
                            clutter_text_get_cursor_position (clutter_text));
}

static void
paste_activated_cb (ShellPopupBaseMenuItem *item,
                    const ClutterEvent     *event,
                    ShellEntryMenu         *self)
{
  st_clipboard_get_text (st_clipboard_get_default (),
                         ST_CLIPBOARD_TYPE_CLIPBOARD,
                         paste_text_cb,
                         weak_ref_new (self));
}

static void
password_activated_cb (ShellPopupBaseMenuItem *item,
                       const ClutterEvent     *event,
                       ShellEntryMenu         *self)
{
  StEntry *entry = menu_get_entry (self);
  StPasswordEntry *password_entry;

  if (!ST_IS_PASSWORD_ENTRY (entry))
    return;

  password_entry = ST_PASSWORD_ENTRY (entry);
  st_password_entry_set_password_visible (password_entry,
                                          !st_password_entry_get_password_visible (password_entry));
}

static ShellPopupMenuItem *
add_item (ShellEntryMenu *self,
          const char     *text,
          GCallback       activate_cb)
{
  ShellPopupMenuItem *item = g_object_new (SHELL_TYPE_POPUP_MENU_ITEM,
                                           "text", text,
                                           NULL);

  g_signal_connect_object (item, "activate", activate_cb, self, 0);
  shell_popup_menu_base_add_menu_item (SHELL_POPUP_MENU_BASE (self), G_OBJECT (item));

  return item;
}

static gboolean
shell_entry_menu_open (ShellPopupMenuBase  *base,
                       ShellPopupAnimation  animate)
{
  ShellEntryMenu *self = SHELL_ENTRY_MENU (base);
  ShellPopupMenuBaseClass *parent_class =
    SHELL_POPUP_MENU_BASE_CLASS (shell_entry_menu_parent_class);
  StEntry *entry;
  ClutterActor *actor;

  if (!parent_class->open (base, animate))
    return FALSE;

  update_paste_item (self);
  update_copy_item (self);
  if (self->password_item != NULL)
    update_password_item (self);

  entry = menu_get_entry (self);
  if (entry != NULL)
    st_widget_add_style_pseudo_class (ST_WIDGET (entry), "focus");

  actor = shell_popup_menu_base_get_actor (base);
  if (!st_widget_navigate_focus (ST_WIDGET (actor), NULL, ST_DIR_TAB_FORWARD, FALSE))
    clutter_actor_grab_key_focus (actor);

  return TRUE;
}

static void
shell_entry_menu_constructed (GObject *object)
{
  ShellEntryMenu *self = SHELL_ENTRY_MENU (object);
  ShellLayoutManager *layout_manager;
  ClutterActor *actor;
  StEntry *entry;

  G_OBJECT_CLASS (shell_entry_menu_parent_class)->constructed (object);

  entry = menu_get_entry (self);

  /* Populate menu */
  g_set_weak_pointer (&self->copy_item,
                      add_item (self, _("Copy"), G_CALLBACK (copy_activated_cb)));
  g_set_weak_pointer (&self->paste_item,
                      add_item (self, _("Paste"), G_CALLBACK (paste_activated_cb)));

  if (self->is_password && ST_IS_PASSWORD_ENTRY (entry))
    {
      ShellPopupMenuItem *item = add_item (self, "", G_CALLBACK (password_activated_cb));

      g_set_weak_pointer (&self->password_item, item);
      g_object_bind_property (entry, "show-peek-icon",
                              item, "visible",
                              G_BINDING_SYNC_CREATE);
    }

  actor = shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (self));
  layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  if (layout_manager != NULL)
    clutter_actor_add_child (CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager)),
                             actor);
  else
    g_warning ("%s: no layout manager to add the entry menu to", G_STRFUNC);
  clutter_actor_hide (actor);
}

static void
shell_entry_menu_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  ShellEntryMenu *self = SHELL_ENTRY_MENU (object);

  switch (prop_id)
    {
    case PROP_IS_PASSWORD:
      g_value_set_boolean (value, self->is_password);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_entry_menu_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  ShellEntryMenu *self = SHELL_ENTRY_MENU (object);

  switch (prop_id)
    {
    case PROP_IS_PASSWORD:
      self->is_password = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_entry_menu_dispose (GObject *object)
{
  ShellEntryMenu *self = SHELL_ENTRY_MENU (object);

  g_clear_weak_pointer (&self->copy_item);
  g_clear_weak_pointer (&self->paste_item);
  g_clear_weak_pointer (&self->password_item);

  G_OBJECT_CLASS (shell_entry_menu_parent_class)->dispose (object);
}

static void
shell_entry_menu_class_init (ShellEntryMenuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupMenuBaseClass *base_class = SHELL_POPUP_MENU_BASE_CLASS (klass);

  object_class->constructed = shell_entry_menu_constructed;
  object_class->dispose = shell_entry_menu_dispose;
  object_class->get_property = shell_entry_menu_get_property;
  object_class->set_property = shell_entry_menu_set_property;

  base_class->open = shell_entry_menu_open;

  /**
   * ShellEntryMenu:is-password:
   *
   * Whether the menu has the item which shows and hides the text. It only
   * has it if the source actor is a #StPasswordEntry as well.
   */
  props[PROP_IS_PASSWORD] =
    g_param_spec_boolean ("is-password", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_entry_menu_init (ShellEntryMenu *self)
{
}

/**
 * shell_entry_menu_new:
 * @entry: the entry the menu is for
 * @is_password: whether to add the item which shows and hides the text; only
 *   has an effect if @entry is a #StPasswordEntry
 *
 * Creates the context menu of @entry (`new EntryMenu (entry)`) and adds it to
 * the UI group, hidden. It is not attached to @entry: use
 * shell_entry_add_context_menu () for that.
 *
 * Returns: (transfer full): a new #ShellEntryMenu
 */
ShellEntryMenu *
shell_entry_menu_new (StEntry  *entry,
                      gboolean  is_password)
{
  g_return_val_if_fail (ST_IS_ENTRY (entry), NULL);

  return g_object_new (SHELL_TYPE_ENTRY_MENU,
                       "source-actor", entry,
                       "arrow-alignment", 0.0,
                       "arrow-side", ST_SIDE_TOP,
                       "is-password", is_password,
                       NULL);
}

/**
 * shell_entry_menu_get_is_password:
 * @self: a #ShellEntryMenu
 *
 * Returns: whether the menu was made for a password entry
 */
gboolean
shell_entry_menu_get_is_password (ShellEntryMenu *self)
{
  g_return_val_if_fail (SHELL_IS_ENTRY_MENU (self), FALSE);

  return self->is_password;
}

/* ------------------------------------------------------------------------ */
/* Context menus of entries                                                 */
/* ------------------------------------------------------------------------ */

static void
set_menu_alignment (StEntry        *entry,
                    ShellEntryMenu *menu,
                    float           stage_x)
{
  float entry_x, entry_y;
  float width = clutter_actor_get_width (CLUTTER_ACTOR (entry));

  if (width > 0 &&
      clutter_actor_transform_stage_point (CLUTTER_ACTOR (entry),
                                           stage_x, 0, &entry_x, &entry_y))
    shell_popup_menu_set_source_alignment (SHELL_POPUP_MENU (menu), entry_x / width);
}

static void
menu_click_recognize_cb (ClutterClickGesture *gesture,
                         StEntry             *entry)
{
  ShellEntryMenu *menu = shell_entry_get_menu (entry);
  ShellPopupMenuBase *base;

  if (menu == NULL)
    return;

  base = SHELL_POPUP_MENU_BASE (menu);
  if (shell_popup_menu_base_get_is_open (base))
    {
      shell_popup_menu_base_close (base, SHELL_POPUP_ANIMATION_FULL);
    }
  else if (clutter_press_gesture_get_button (CLUTTER_PRESS_GESTURE (gesture)) ==
           CLUTTER_BUTTON_SECONDARY)
    {
      graphene_point_t coords;

      clutter_press_gesture_get_coords_abs (CLUTTER_PRESS_GESTURE (gesture), &coords);
      set_menu_alignment (entry, menu, coords.x);
      shell_popup_menu_base_open (base, SHELL_POPUP_ANIMATION_FULL);
    }
}

static void
entry_popup_menu_cb (StWidget *widget,
                     StEntry  *entry)
{
  ShellEntryMenu *menu = shell_entry_get_menu (entry);
  ClutterText *text;
  float text_x, text_y, line_height;
  int cursor_position;

  if (menu == NULL)
    return;

  text = st_entry_get_clutter_text (entry);
  cursor_position = clutter_text_get_cursor_position (text);
  if (clutter_text_position_to_coords (text, cursor_position,
                                       &text_x, &text_y, &line_height))
    {
      float width = clutter_actor_get_width (CLUTTER_ACTOR (entry));

      if (width > 0)
        shell_popup_menu_set_source_alignment (SHELL_POPUP_MENU (menu), text_x / width);
    }
  shell_popup_menu_base_open (SHELL_POPUP_MENU_BASE (menu), SHELL_POPUP_ANIMATION_FULL);
}

static void
entry_destroy_cb (ClutterActor *actor,
                  gpointer      user_data)
{
  ShellEntryMenu *menu = shell_entry_get_menu (ST_ENTRY (actor));

  if (menu != NULL)
    {
      g_autoptr (ShellEntryMenu) keep_alive = g_object_ref (menu);

      shell_popup_menu_base_destroy (SHELL_POPUP_MENU_BASE (menu));
      g_object_set_data (G_OBJECT (actor), MENU_DATA_KEY, NULL);
    }
  g_object_set_data (G_OBJECT (actor), MENU_MANAGER_DATA_KEY, NULL);
}

/**
 * shell_entry_add_context_menu:
 * @entry: the entry to add the menu to
 * @action_mode: the action mode while the menu is open (%SHELL_ACTION_MODE_POPUP
 *   usually, %SHELL_ACTION_MODE_NONE on the login screen)
 *
 * Gives @entry a right click and keyboard (Menu key) context menu with the
 * copy and paste items, and the show or hide text item for a #StPasswordEntry
 * (`addContextMenu (entry, {actionMode})`). Does nothing if @entry has one
 * already. The menu is destroyed with @entry; get it with
 * shell_entry_get_menu ().
 */
void
shell_entry_add_context_menu (StEntry         *entry,
                              ShellActionMode  action_mode)
{
  g_autoptr (ShellEntryMenu) menu = NULL;
  g_autoptr (ShellPopupMenuManager) manager = NULL;
  ClutterAction *click_gesture;

  g_return_if_fail (ST_IS_ENTRY (entry));

  if (shell_entry_get_menu (entry) != NULL)
    return;

  menu = shell_entry_menu_new (entry, ST_IS_PASSWORD_ENTRY (entry));
  manager = shell_popup_menu_manager_new (action_mode);
  shell_popup_menu_manager_add_menu (manager, G_OBJECT (menu));

  g_object_set_data_full (G_OBJECT (entry), MENU_DATA_KEY,
                          g_steal_pointer (&menu), g_object_unref);
  g_object_set_data_full (G_OBJECT (entry), MENU_MANAGER_DATA_KEY,
                          g_steal_pointer (&manager), g_object_unref);

  click_gesture = clutter_click_gesture_new ();
  clutter_click_gesture_set_recognize_on_press (CLUTTER_CLICK_GESTURE (click_gesture), TRUE);
  clutter_press_gesture_set_required_button (CLUTTER_PRESS_GESTURE (click_gesture),
                                             CLUTTER_BUTTON_SECONDARY);
  g_signal_connect (click_gesture, "recognize",
                    G_CALLBACK (menu_click_recognize_cb), entry);
  clutter_actor_add_action_full (CLUTTER_ACTOR (entry), "menu-click-gesture",
                                 CLUTTER_PHASE_CAPTURE, click_gesture);

  g_signal_connect (entry, "popup-menu", G_CALLBACK (entry_popup_menu_cb), entry);
  g_signal_connect (entry, "destroy", G_CALLBACK (entry_destroy_cb), NULL);
}

/**
 * shell_entry_get_menu:
 * @entry: a #StEntry
 *
 * Returns: (transfer none) (nullable): the context menu added with
 *   shell_entry_add_context_menu (), or %NULL (`entry.menu`)
 */
ShellEntryMenu *
shell_entry_get_menu (StEntry *entry)
{
  g_return_val_if_fail (ST_IS_ENTRY (entry), NULL);

  return g_object_get_data (G_OBJECT (entry), MENU_DATA_KEY);
}

/* ------------------------------------------------------------------------ */
/* ShellCapsLockWarning                                                     */
/* ------------------------------------------------------------------------ */

struct _ShellCapsLockWarning
{
  StLabel parent;

  ClutterKeymap *keymap; /* weak */
  gulong state_changed_id;
};

G_DEFINE_FINAL_TYPE (ShellCapsLockWarning, shell_caps_lock_warning, ST_TYPE_LABEL)

static void
sync_done_cb (gboolean finished,
              gpointer user_data)
{
  g_autoptr (ShellCapsLockWarning) self = NULL;

  if (!finished)
    return;

  self = g_weak_ref_get (user_data);
  if (self != NULL && self->keymap != NULL &&
      clutter_keymap_get_caps_lock_state (self->keymap))
    clutter_actor_set_height (CLUTTER_ACTOR (self), -1);
}

static void
sync_caps_lock (ShellCapsLockWarning *self,
                gboolean              animate)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  gboolean caps_lock_on = self->keymap != NULL &&
                          clutter_keymap_get_caps_lock_state (self->keymap);
  gboolean natural_height_set;
  float height;
  ShellEaseParams params = {
    .duration = animate ? 200 : 0,
    .delay = -1,
    .mode = 0,
  };

  clutter_actor_remove_all_transitions (actor);

  g_object_get (self, "natural-height-set", &natural_height_set, NULL);
  g_object_set (self, "natural-height-set", FALSE, NULL);
  clutter_actor_get_preferred_height (actor, -1, NULL, &height);
  g_object_set (self, "natural-height-set", natural_height_set, NULL);

  if (caps_lock_on)
    {
      clutter_actor_add_accessible_state (actor, ATK_STATE_SHOWING);
      clutter_actor_add_accessible_state (actor, ATK_STATE_VISIBLE);
    }
  else
    {
      clutter_actor_remove_accessible_state (actor, ATK_STATE_SHOWING);
      clutter_actor_remove_accessible_state (actor, ATK_STATE_VISIBLE);
    }

  shell_actor_ease (actor, &params,
                    sync_done_cb, weak_ref_new (self), (GDestroyNotify) weak_ref_free,
                    "height", caps_lock_on ? (double) height : 0.0,
                    "opacity", caps_lock_on ? 255u : 0u,
                    NULL);
}

static void
keymap_state_changed_cb (ClutterKeymap        *keymap,
                         ShellCapsLockWarning *self)
{
  sync_caps_lock (self, TRUE);
}

static void
mapped_changed_cb (GObject    *object,
                   GParamSpec *pspec,
                   gpointer    user_data)
{
  ShellCapsLockWarning *self = SHELL_CAPS_LOCK_WARNING (object);

  if (self->keymap != NULL)
    {
      if (clutter_actor_is_mapped (CLUTTER_ACTOR (self)))
        {
          if (self->state_changed_id == 0)
            self->state_changed_id =
              g_signal_connect (self->keymap, "state-changed",
                                G_CALLBACK (keymap_state_changed_cb), self);
        }
      else
        {
          g_clear_signal_handler (&self->state_changed_id, self->keymap);
        }
    }

  sync_caps_lock (self, FALSE);
}

static void
shell_caps_lock_warning_constructed (GObject *object)
{
  ShellCapsLockWarning *self = SHELL_CAPS_LOCK_WARNING (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterText *text;
  ClutterBackend *backend;
  ClutterSeat *seat;

  G_OBJECT_CLASS (shell_caps_lock_warning_parent_class)->constructed (object);

  st_widget_set_style_class_name (ST_WIDGET (self), "caps-lock-warning-label");
  st_label_set_text (ST_LABEL (self), _("Caps lock is on"));

  text = CLUTTER_TEXT (st_label_get_clutter_text (ST_LABEL (self)));
  clutter_text_set_ellipsize (text, PANGO_ELLIPSIZE_NONE);
  clutter_text_set_line_wrap (text, TRUE);

  backend = clutter_context_get_backend (clutter_actor_get_context (actor));
  seat = clutter_backend_get_default_seat (backend);
  g_set_weak_pointer (&self->keymap, clutter_seat_get_keymap (seat));

  g_signal_connect (self, "notify::mapped", G_CALLBACK (mapped_changed_cb), NULL);
}

static void
shell_caps_lock_warning_dispose (GObject *object)
{
  ShellCapsLockWarning *self = SHELL_CAPS_LOCK_WARNING (object);

  if (self->keymap != NULL)
    g_clear_signal_handler (&self->state_changed_id, self->keymap);
  self->state_changed_id = 0;
  g_clear_weak_pointer (&self->keymap);

  G_OBJECT_CLASS (shell_caps_lock_warning_parent_class)->dispose (object);
}

static void
shell_caps_lock_warning_class_init (ShellCapsLockWarningClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_caps_lock_warning_constructed;
  object_class->dispose = shell_caps_lock_warning_dispose;
}

static void
shell_caps_lock_warning_init (ShellCapsLockWarning *self)
{
}

/**
 * shell_caps_lock_warning_new:
 *
 * Creates a label which says that caps lock is on, and is folded away
 * (height and opacity 0) while it is off (`new CapsLockWarning ()`). Set
 * properties like the expansion with g_object_set () or from JS with
 * `new Shell.CapsLockWarning ({x_expand: true})`.
 *
 * Returns: (transfer full): a new #ShellCapsLockWarning
 */
ShellCapsLockWarning *
shell_caps_lock_warning_new (void)
{
  return g_object_new (SHELL_TYPE_CAPS_LOCK_WARNING, NULL);
}
