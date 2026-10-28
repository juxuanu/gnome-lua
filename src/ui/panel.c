/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * panel.c: The top bar. Port of js/ui/panel.js.
 *
 * What stayed in JS (src/ui/panel-items.c): the indicators of the shell
 * (`ActivitiesButton`, `QuickSettings`, the date menu, the accessibility,
 * keyboard, dwell click and screen recording or sharing indicators) and the
 * `PANEL_ITEM_IMPLEMENTATIONS` table that maps their roles to classes. The
 * panel does not know those classes: when the session mode asks for a role
 * that has no indicator yet it emits #ShellPanel::create-indicator and takes
 * what the handler returns. ShellMain connects that signal to
 * `PanelItems.createPanelItem ()`. The roles themselves are known here too
 * (see shell_panel_is_builtin_role ()), as the panel hides the indicators of
 * the shell that the session mode does not list, and keeps extensions from
 * taking their names.
 *
 * Differences from the JS class, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The panel is created inactive: ShellMain connects #ShellPanel::create-indicator
 *    and calls shell_panel_begin_startup (), which creates the indicators. The
 *    constructor of the JS class did it all at once.
 *  - `Main.overview.connectObject ('showing' / 'hiding')` is the property
 *    #ShellPanel:overview-visible, which ShellMain sets from the overview
 *    signals.
 *  - `Main.messageTray.bannerAlignment` and `bannerBlocked` are the read-only
 *    properties #ShellPanel:banner-alignment and #ShellPanel:banner-blocked,
 *    which ShellMain forwards to the message tray.
 *  - `Main.ctrlAltTabManager.addGroup (panel, ...)` is done by ShellMain.
 *  - `statusArea`, the JS object that maps roles to indicators, is
 *    shell_panel_get_status_area_indicator () and
 *    shell_panel_list_status_area_roles (). The panel keeps a reference on its
 *    indicators until they are destroyed.
 *  - `addToStatusArea ()` reports a conflict or a type error through a
 *    #GError, and also refuses the roles of the shell that are not in use in
 *    the current session mode.
 *  - The hook "panel.indicator-added" is emitted once per indicator, when it
 *    is first registered (the JS `_addToPanelBox ()` ran at every session mode
 *    change, and connected its handlers every time).
 *  - Every box is [indicators of extensions at the start] [the indicators of
 *    the shell] [indicators of extensions at the end]
 *    (shell_panel_add_extension_indicator (), docs/api/panel.md). The JS
 *    panel put the indicators of the shell at the end of the box at every
 *    session mode update, which would have moved them left of the extensions
 *    at the end; the boxes are laid out again after the update instead.
 */

#include "config.h"

#include <string.h>

#include <clutter/clutter.h>
#include <meta/display.h>
#include <meta/meta-workspace-manager.h>
#include <meta/window.h>
#include <meta/workspace.h>

#include "shell-global.h"
#include "shell-hooks.h"
#include "ui/extension-order.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/panel.h"
#include "ui/session-mode.h"

/* Roles of PANEL_ITEM_IMPLEMENTATIONS (src/ui/panel-items.c) */
static const char * const builtin_roles[] = {
  "activities",
  "quickSettings",
  "dateMenu",
  "a11y",
  "keyboard",
  "dwellClick",
  "screenRecording",
  "screenSharing",
  NULL
};

enum
{
  PROP_0,

  PROP_MENU_MANAGER,
  PROP_LEFT_BOX,
  PROP_CENTER_BOX,
  PROP_RIGHT_BOX,
  PROP_OVERVIEW_VISIBLE,
  PROP_BOX_OPACITY,
  PROP_BANNER_ALIGNMENT,
  PROP_BANNER_BLOCKED,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  CREATE_INDICATOR,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellPanel
{
  StWidget parent;

  ShellPopupMenuManager *menu_manager; /* strong */

  StBoxLayout *left_box;   /* weak */
  StBoxLayout *center_box; /* weak */
  StBoxLayout *right_box;  /* weak */

  GHashTable *status_area; /* role -> ShellPanelButtonBox, both strong */
  GPtrArray *ext_items;    /* ExtItem, grouped by owner, ordered by the rank of the owners */

  char *session_style;
  ShellSessionMode *session_mode; /* weak */
  gulong session_updated_id;
  gboolean started;

  gboolean overview_visible;
  ClutterActorAlign banner_alignment;
  gboolean banner_blocked;
};

G_DEFINE_FINAL_TYPE (ShellPanel, shell_panel, ST_TYPE_WIDGET)

/* An indicator added by an extension. The status area holds the reference;
 * the item is removed when the indicator is destroyed. */
typedef struct
{
  char *owner;
  ShellPanelButtonBox *indicator;
  ShellPanelBox box;
  ShellPanelEdge edge;
} ExtItem;

static void
ext_item_free (gpointer data)
{
  ExtItem *item = data;

  g_free (item->owner);
  g_free (item);
}

static const char *
ext_item_get_owner (gpointer data)
{
  return ((ExtItem *) data)->owner;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
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
      return "right";
    default:
      return "right";
    }
}

static StBoxLayout *
get_box (ShellPanel    *self,
         ShellPanelBox  box)
{
  switch (box)
    {
    case SHELL_PANEL_BOX_LEFT:
      return self->left_box;
    case SHELL_PANEL_BOX_CENTER:
      return self->center_box;
    case SHELL_PANEL_BOX_RIGHT:
    default:
      return self->right_box;
    }
}

static gboolean
find_box_of_indicator (ShellPanel          *self,
                       ShellPanelButtonBox *indicator,
                       ShellPanelBox       *box_out)
{
  ClutterActor *container =
    CLUTTER_ACTOR (shell_panel_button_box_get_container (indicator));
  ShellPanelBox box;

  for (box = SHELL_PANEL_BOX_LEFT; box <= SHELL_PANEL_BOX_RIGHT; box++)
    {
      StBoxLayout *box_actor = get_box (self, box);

      if (box_actor != NULL &&
          clutter_actor_contains (CLUTTER_ACTOR (box_actor), container))
        {
          *box_out = box;
          return TRUE;
        }
    }

  return FALSE;
}

static const char *
find_role_of_indicator (ShellPanel          *self,
                        ShellPanelButtonBox *indicator)
{
  GHashTableIter iter;
  gpointer key, value;

  if (self->status_area == NULL)
    return NULL;

  g_hash_table_iter_init (&iter, self->status_area);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (value == indicator)
        return key;
    }

  return NULL;
}

static void
emit_hook (const char *name,
           const char *role,
           const char *box,
           const char *owner)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  GVariantBuilder builder;

  if (!shell_hook_registry_is_declared (registry, name))
    return;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "name", g_variant_new_string (role));
  g_variant_builder_add (&builder, "{sv}", "box", g_variant_new_string (box));
  if (owner != NULL)
    g_variant_builder_add (&builder, "{sv}", "owner", g_variant_new_string (owner));

  shell_hook_registry_emit_after (registry, name, g_variant_builder_end (&builder));
}

/* ------------------------------------------------------------------------ */
/* Banner state                                                             */
/* ------------------------------------------------------------------------ */

static void
set_banner_alignment (ShellPanel       *self,
                      ClutterActorAlign alignment)
{
  if (self->banner_alignment == alignment)
    return;

  self->banner_alignment = alignment;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_BANNER_ALIGNMENT]);
}

static void
set_banner_blocked (ShellPanel *self,
                    gboolean    blocked)
{
  if (self->banner_blocked == blocked)
    return;

  self->banner_blocked = blocked;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_BANNER_BLOCKED]);
}

/* ------------------------------------------------------------------------ */
/* Menus of the indicators                                                  */
/* ------------------------------------------------------------------------ */

#define MENU_WATCHED_KEY "shell-panel-open-state-watched"

typedef struct
{
  GWeakRef panel;
  GWeakRef indicator;
} MenuWatch;

static void
menu_watch_free (MenuWatch *watch,
                 GClosure  *closure)
{
  g_weak_ref_clear (&watch->panel);
  g_weak_ref_clear (&watch->indicator);
  g_free (watch);
}

static void
menu_open_state_changed_cb (GObject   *menu,
                            gboolean   is_open,
                            MenuWatch *watch)
{
  g_autoptr (ShellPanel) self = g_weak_ref_get (&watch->panel);
  g_autoptr (ShellPanelButtonBox) indicator = g_weak_ref_get (&watch->indicator);
  ShellPanelBox box;
  ClutterActorAlign box_alignment;
  const char *role;

  if (self == NULL || indicator == NULL)
    return;

  if (!find_box_of_indicator (self, indicator, &box))
    return;

  switch (box)
    {
    case SHELL_PANEL_BOX_LEFT:
      box_alignment = CLUTTER_ACTOR_ALIGN_START;
      break;
    case SHELL_PANEL_BOX_CENTER:
      box_alignment = CLUTTER_ACTOR_ALIGN_CENTER;
      break;
    case SHELL_PANEL_BOX_RIGHT:
    default:
      box_alignment = CLUTTER_ACTOR_ALIGN_END;
      break;
    }

  if (box_alignment == self->banner_alignment)
    set_banner_blocked (self, is_open);

  role = find_role_of_indicator (self, indicator);
  if (is_open && role != NULL)
    emit_hook ("panel.menu-opened", role, box_to_string (box), NULL);
}

/* The `_onMenuSet ()` of the JS panel: the menu joins the menu manager (so
 * that moving the pointer to another indicator switches menus) and the panel
 * learns when it opens and closes, once per menu. */
static void
indicator_menu_set_cb (ShellPanelMenuButton *indicator,
                       ShellPanel           *self)
{
  GObject *menu = shell_panel_menu_button_get_menu (indicator);
  MenuWatch *watch;

  if (menu == NULL || g_object_get_data (menu, MENU_WATCHED_KEY) != NULL)
    return;

  shell_popup_menu_manager_add_menu (self->menu_manager, menu);

  g_object_set_data (menu, MENU_WATCHED_KEY, GINT_TO_POINTER (TRUE));

  watch = g_new0 (MenuWatch, 1);
  g_weak_ref_init (&watch->panel, self);
  g_weak_ref_init (&watch->indicator, indicator);
  g_signal_connect_data (menu, "open-state-changed",
                         G_CALLBACK (menu_open_state_changed_cb),
                         watch, (GClosureNotify) menu_watch_free, 0);
}

static void
indicator_destroyed_cb (ClutterActor *actor,
                        ShellPanel   *self)
{
  GHashTableIter iter;
  gpointer value;

  guint i;

  /* An extension indicator leaves its slot */
  if (self->ext_items != NULL)
    {
      for (i = 0; i < self->ext_items->len; i++)
        {
          ExtItem *item = g_ptr_array_index (self->ext_items, i);

          if (item->indicator == (ShellPanelButtonBox *) actor)
            {
              g_ptr_array_remove_index (self->ext_items, i);
              break;
            }
        }
    }

  /* delete this.statusArea[role]; the indicators are destroyed with the
   * panel after dispose already dropped the table */
  if (self->status_area == NULL)
    return;

  g_hash_table_iter_init (&iter, self->status_area);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      if (value == actor)
        g_hash_table_iter_remove (&iter);
    }
}

/* ------------------------------------------------------------------------ */
/* The status area                                                          */
/* ------------------------------------------------------------------------ */

/**
 * shell_panel_is_builtin_role:
 * @role: a status area role
 *
 * Whether @role is the role of one of the indicators of the shell itself:
 * `activities`, `quickSettings`, `dateMenu`, `a11y`, `keyboard`, `dwellClick`,
 * `screenRecording` and `screenSharing` (the keys of
 * `PANEL_ITEM_IMPLEMENTATIONS`).
 *
 * Returns: %TRUE if the shell owns @role
 */
gboolean
shell_panel_is_builtin_role (const char *role)
{
  g_return_val_if_fail (role != NULL, FALSE);

  return g_strv_contains (builtin_roles, role);
}

/**
 * shell_panel_get_status_area_indicator:
 * @self: a #ShellPanel
 * @role: the role of an indicator
 *
 * Gets an indicator of the top bar by its role (`statusArea[role]`), for
 * instance `quickSettings`, `dateMenu` or `activities`.
 *
 * Returns: (transfer none) (nullable): the indicator, or %NULL if there is
 *   none with that role (yet, or any more)
 */
ShellPanelButtonBox *
shell_panel_get_status_area_indicator (ShellPanel *self,
                                       const char *role)
{
  g_return_val_if_fail (SHELL_IS_PANEL (self), NULL);
  g_return_val_if_fail (role != NULL, NULL);

  return g_hash_table_lookup (self->status_area, role);
}

static int
compare_roles (gconstpointer a,
               gconstpointer b)
{
  return strcmp (*(const char * const *) a, *(const char * const *) b);
}

/**
 * shell_panel_list_status_area_roles:
 * @self: a #ShellPanel
 *
 * Lists the roles of the indicators of the top bar (`Object.keys (statusArea)`),
 * sorted alphabetically.
 *
 * Returns: (transfer full) (array zero-terminated=1) (element-type utf8): the
 *   roles
 */
char **
shell_panel_list_status_area_roles (ShellPanel *self)
{
  g_autoptr (GPtrArray) roles = g_ptr_array_new ();
  GHashTableIter iter;
  gpointer key;

  g_return_val_if_fail (SHELL_IS_PANEL (self), NULL);

  g_hash_table_iter_init (&iter, self->status_area);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_ptr_array_add (roles, g_strdup (key));

  g_ptr_array_sort (roles, compare_roles);
  g_ptr_array_add (roles, NULL);

  return (char **) g_ptr_array_free (g_steal_pointer (&roles), FALSE);
}

/* The part of adding an indicator which does not depend on where it goes:
 * the role is taken, the panel learns when the indicator is destroyed or
 * gets a menu, and the hook is emitted. */
static void
register_indicator (ShellPanel          *self,
                    const char          *role,
                    ShellPanelButtonBox *indicator,
                    ShellPanelBox        box,
                    const char          *owner)
{
  gboolean is_new = g_hash_table_lookup (self->status_area, role) != indicator;

  if (is_new)
    {
      g_hash_table_replace (self->status_area, g_strdup (role), g_object_ref (indicator));

      g_signal_connect_object (indicator, "destroy",
                               G_CALLBACK (indicator_destroyed_cb), self, 0);
      if (SHELL_IS_PANEL_MENU_BUTTON (indicator))
        g_signal_connect_object (indicator, "menu-set",
                                 G_CALLBACK (indicator_menu_set_cb), self, 0);
    }

  if (SHELL_IS_PANEL_MENU_BUTTON (indicator))
    indicator_menu_set_cb (SHELL_PANEL_MENU_BUTTON (indicator), self);

  if (is_new)
    emit_hook ("panel.indicator-added", role, box_to_string (box), owner != NULL ? owner : "");
}

static void
add_to_panel_box (ShellPanel          *self,
                  const char          *role,
                  ShellPanelButtonBox *indicator,
                  int                  position,
                  ShellPanelBox        box,
                  const char          *owner)
{
  ClutterActor *container =
    CLUTTER_ACTOR (shell_panel_button_box_get_container (indicator));
  StBoxLayout *box_actor = get_box (self, box);
  ClutterActor *parent;

  if (box_actor == NULL)
    return;

  clutter_actor_show (container);

  parent = clutter_actor_get_parent (container);
  if (parent != NULL)
    clutter_actor_remove_child (parent, container);

  clutter_actor_insert_child_at_index (CLUTTER_ACTOR (box_actor), container, position);

  register_indicator (self, role, indicator, box, owner);
}

/**
 * shell_panel_add_to_status_area_with_owner:
 * @self: a #ShellPanel
 * @role: the role of the indicator, which must be unique
 * @indicator: the indicator, a button with a menu
 * @position: the index among the children of the box, or -1 to append
 * @box: the box to add the indicator to
 * @owner: (nullable): the uuid of the extension which adds the indicator, for
 *   the "panel.indicator-added" hook; %NULL or empty for the shell
 * @error: return location for a #GError
 *
 * Like shell_panel_add_to_status_area (), and tells the hook who is adding.
 *
 * Returns: %TRUE on success, %FALSE with %G_IO_ERROR_EXISTS if there already
 *   is an indicator with that role
 */
gboolean
shell_panel_add_to_status_area_with_owner (ShellPanel           *self,
                                           const char           *role,
                                           ShellPanelMenuButton *indicator,
                                           int                   position,
                                           ShellPanelBox         box,
                                           const char           *owner,
                                           GError              **error)
{
  g_return_val_if_fail (SHELL_IS_PANEL (self), FALSE);
  g_return_val_if_fail (role != NULL, FALSE);
  g_return_val_if_fail (SHELL_IS_PANEL_MENU_BUTTON (indicator), FALSE);

  if (g_hash_table_contains (self->status_area, role))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                   "Extension point conflict: there is already a status indicator for role %s",
                   role);
      return FALSE;
    }

  add_to_panel_box (self, role, SHELL_PANEL_BUTTON_BOX (indicator), position, box, owner);
  return TRUE;
}

/**
 * shell_panel_add_to_status_area:
 * @self: a #ShellPanel
 * @role: the role of the indicator, which must be unique
 * @indicator: the indicator, a button with a menu
 * @position: the index among the children of the box, or -1 to append
 *   (`position ?? 0` of old: pass 0 for the start of the box)
 * @box: the box to add the indicator to
 * @error: return location for a #GError
 *
 * Adds an indicator to the top bar and registers it under @role
 * (`addToStatusArea ()`). The indicator's menu joins the menu manager of the
 * panel. When the indicator is destroyed the role is free again. Emits the
 * "panel.indicator-added" hook.
 *
 * Returns: %TRUE on success, %FALSE with %G_IO_ERROR_EXISTS if there already
 *   is an indicator with that role or if it is a role of the shell
 */
gboolean
shell_panel_add_to_status_area (ShellPanel           *self,
                                const char           *role,
                                ShellPanelMenuButton *indicator,
                                int                   position,
                                ShellPanelBox         box,
                                GError              **error)
{
  return shell_panel_add_to_status_area_with_owner (self, role, indicator,
                                                    position, box, NULL, error);
}

/* ------------------------------------------------------------------------ */
/* Extension indicators                                                     */
/* ------------------------------------------------------------------------ */

static ExtItem *
find_ext_item_of_container (ShellPanel   *self,
                            ShellPanelBox box,
                            ClutterActor *actor)
{
  guint i;

  for (i = 0; i < self->ext_items->len; i++)
    {
      ExtItem *item = g_ptr_array_index (self->ext_items, i);

      if (item->box == box && item->indicator != NULL &&
          CLUTTER_ACTOR (shell_panel_button_box_get_container (item->indicator)) == actor)
        return item;
    }

  return NULL;
}

/* Appends the containers of the extension indicators of a slot, in order */
static void
append_ext_group (ShellPanel     *self,
                  ShellPanelBox   box,
                  ShellPanelEdge  edge,
                  GPtrArray      *desired)
{
  ClutterActor *box_actor = CLUTTER_ACTOR (get_box (self, box));
  guint i;

  for (i = 0; i < self->ext_items->len; i++)
    {
      ExtItem *item = g_ptr_array_index (self->ext_items, i);
      ClutterActor *container;

      if (item->box != box || item->edge != edge || item->indicator == NULL)
        continue;

      container = CLUTTER_ACTOR (shell_panel_button_box_get_container (item->indicator));
      if (container != NULL && clutter_actor_get_parent (container) == box_actor)
        g_ptr_array_add (desired, container);
    }
}

/* Puts the children of a box in the order [extensions at the start] [the
 * indicators of the shell, in the order they already have] [extensions at the
 * end]. Only the children which are out of place are moved. */
static void
relayout_box (ShellPanel    *self,
              ShellPanelBox  box)
{
  StBoxLayout *box_actor = get_box (self, box);
  g_autoptr (GPtrArray) desired = g_ptr_array_new ();
  ClutterActor *child;
  guint i;

  if (box_actor == NULL || self->ext_items == NULL)
    return;

  append_ext_group (self, box, SHELL_PANEL_EDGE_START, desired);

  for (child = clutter_actor_get_first_child (CLUTTER_ACTOR (box_actor));
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      if (find_ext_item_of_container (self, box, child) == NULL)
        g_ptr_array_add (desired, child);
    }

  append_ext_group (self, box, SHELL_PANEL_EDGE_END, desired);

  for (i = 0; i < desired->len; i++)
    {
      ClutterActor *actor = g_ptr_array_index (desired, i);

      if (clutter_actor_get_child_at_index (CLUTTER_ACTOR (box_actor), i) != actor)
        clutter_actor_set_child_at_index (CLUTTER_ACTOR (box_actor), actor, i);
    }
}

typedef struct
{
  ShellPanelBox box;
  ShellPanelEdge edge;
} ExtSlot;

static gboolean
ext_item_in_slot (gpointer item_data,
                  gpointer slot_data)
{
  ExtItem *item = item_data;
  ExtSlot *slot = slot_data;

  return item->box == slot->box && item->edge == slot->edge;
}

static void
enabled_extensions_changed_cb (GSettings  *settings,
                               const char *key,
                               ShellPanel *self)
{
  ShellPanelBox box;

  if (self->ext_items == NULL)
    return;

  shell_extension_order_sort (self->ext_items, ext_item_get_owner);
  for (box = SHELL_PANEL_BOX_LEFT; box <= SHELL_PANEL_BOX_RIGHT; box++)
    relayout_box (self, box);
}

/**
 * shell_panel_add_extension_indicator:
 * @self: a #ShellPanel
 * @role: the role of the indicator, which must be unique and not the role
 *   of an indicator of the shell
 * @indicator: the indicator, a button with a menu
 * @owner: the uuid of the extension which adds the indicator
 * @box: the box to add the indicator to
 * @edge: which end of the box
 * @index: the 1-based position among the indicators of @owner at that end of
 *   that box, or 0 or less to go after the last one; values past the end
 *   clamp
 * @error: return location for a #GError
 *
 * Adds an indicator of an extension to the top bar and registers it under
 * @role. Every box is [indicators of extensions at %SHELL_PANEL_EDGE_START]
 * [indicators of the shell] [indicators of extensions at
 * %SHELL_PANEL_EDGE_END]; inside a group the indicators of the extensions are
 * ordered by the position of the extension in the `enabled-extensions`
 * setting, then by @index (docs/api/README.md, "Placement and ordering").
 * The placement is kept when the session mode changes. Emits the
 * "panel.indicator-added" hook with @owner.
 *
 * Returns: %TRUE on success, %FALSE with %G_IO_ERROR_EXISTS if the role is
 *   taken or is a role of the shell
 */
gboolean
shell_panel_add_extension_indicator (ShellPanel           *self,
                                     const char           *role,
                                     ShellPanelMenuButton *indicator,
                                     const char           *owner,
                                     ShellPanelBox         box,
                                     ShellPanelEdge        edge,
                                     int                   index,
                                     GError              **error)
{
  ShellPanelButtonBox *button_box;
  ClutterActor *container;
  StBoxLayout *box_actor;
  ExtSlot slot = { box, edge };
  ExtItem *item;
  guint position;

  g_return_val_if_fail (SHELL_IS_PANEL (self), FALSE);
  g_return_val_if_fail (role != NULL, FALSE);
  g_return_val_if_fail (SHELL_IS_PANEL_MENU_BUTTON (indicator), FALSE);
  g_return_val_if_fail (owner != NULL && owner[0] != '\0', FALSE);

  button_box = SHELL_PANEL_BUTTON_BOX (indicator);
  box_actor = get_box (self, box);

  if (shell_panel_is_builtin_role (role) || g_hash_table_contains (self->status_area, role))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                   "Extension point conflict: there is already a status indicator for role %s",
                   role);
      return FALSE;
    }

  if (box_actor == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "The top bar has no %s box", box_to_string (box));
      return FALSE;
    }

  container = CLUTTER_ACTOR (shell_panel_button_box_get_container (button_box));
  clutter_actor_show (container);
  if (clutter_actor_get_parent (container) != NULL)
    clutter_actor_remove_child (clutter_actor_get_parent (container), container);
  clutter_actor_add_child (CLUTTER_ACTOR (box_actor), container);

  item = g_new0 (ExtItem, 1);
  item->owner = g_strdup (owner);
  item->indicator = button_box;
  item->box = box;
  item->edge = edge;

  position = shell_extension_order_insert_position (self->ext_items, ext_item_get_owner,
                                                    ext_item_in_slot, &slot, owner, index);
  g_ptr_array_insert (self->ext_items, MIN (position, self->ext_items->len), item);

  /* In place before the hook runs */
  relayout_box (self, box);
  register_indicator (self, role, button_box, box, owner);

  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Session mode                                                             */
/* ------------------------------------------------------------------------ */

static gboolean
accumulate_first_indicator (GSignalInvocationHint *hint,
                            GValue                *return_accu,
                            const GValue          *handler_return,
                            gpointer               data)
{
  GObject *indicator = g_value_get_object (handler_return);

  if (indicator == NULL)
    return TRUE;

  g_value_set_object (return_accu, indicator);
  return FALSE;
}

/* Returns a new reference */
static ShellPanelButtonBox *
ensure_indicator (ShellPanel *self,
                  const char *role)
{
  ShellPanelButtonBox *indicator = g_hash_table_lookup (self->status_area, role);
  ShellPanelButtonBox *created = NULL;

  if (indicator != NULL)
    return g_object_ref (indicator);

  /* A role nobody implements is a bug, but not worth more than ignoring it */
  g_signal_emit (self, signals[CREATE_INDICATOR], 0, role, &created);
  return created;
}

static void
hide_indicators (ShellPanel *self)
{
  const char * const *role;

  for (role = builtin_roles; *role != NULL; role++)
    {
      ShellPanelButtonBox *indicator = g_hash_table_lookup (self->status_area, *role);

      if (indicator != NULL)
        clutter_actor_hide (CLUTTER_ACTOR (shell_panel_button_box_get_container (indicator)));
    }
}

static void
update_box (ShellPanel          *self,
            const char * const  *elements,
            ShellPanelBox        box)
{
  StBoxLayout *box_actor = get_box (self, box);
  int n_children;
  int i;

  if (box_actor == NULL || elements == NULL)
    return;

  n_children = clutter_actor_get_n_children (CLUTTER_ACTOR (box_actor));

  for (i = 0; elements[i] != NULL; i++)
    {
      g_autoptr (ShellPanelButtonBox) indicator = ensure_indicator (self, elements[i]);

      if (indicator == NULL)
        continue;

      add_to_panel_box (self, elements[i], indicator, i + n_children, box, NULL);
    }

  /* The indicators of the shell go between the groups of the extensions */
  relayout_box (self, box);
}

static void
update_panel (ShellPanel *self)
{
  g_auto (GStrv) left = NULL;
  g_auto (GStrv) center = NULL;
  g_auto (GStrv) right = NULL;
  g_autofree char *style = NULL;

  if (self->session_mode == NULL)
    return;

  g_object_get (self->session_mode,
                "panel-left", &left,
                "panel-center", &center,
                "panel-right", &right,
                "panel-style", &style,
                NULL);

  hide_indicators (self);
  update_box (self, (const char * const *) left, SHELL_PANEL_BOX_LEFT);
  update_box (self, (const char * const *) center, SHELL_PANEL_BOX_CENTER);
  update_box (self, (const char * const *) right, SHELL_PANEL_BOX_RIGHT);

  if (left != NULL && g_strv_contains ((const char * const *) left, "dateMenu"))
    set_banner_alignment (self, CLUTTER_ACTOR_ALIGN_START);
  else if (right != NULL && g_strv_contains ((const char * const *) right, "dateMenu"))
    set_banner_alignment (self, CLUTTER_ACTOR_ALIGN_END);
  /* Default to center if there is no dateMenu */
  else
    set_banner_alignment (self, CLUTTER_ACTOR_ALIGN_CENTER);

  if (self->session_style != NULL)
    st_widget_remove_style_class_name (ST_WIDGET (self), self->session_style);

  g_free (self->session_style);
  self->session_style = g_steal_pointer (&style);
  if (self->session_style != NULL && *self->session_style != '\0')
    st_widget_add_style_class_name (ST_WIDGET (self), self->session_style);
}

static void
session_updated_cb (ShellSessionMode *session_mode,
                    ShellPanel       *self)
{
  update_panel (self);
}

/**
 * shell_panel_begin_startup:
 * @self: a #ShellPanel
 *
 * Fills the top bar for the current session mode, creating the indicators
 * through #ShellPanel::create-indicator, and keeps it up to date when the
 * session mode changes. Call it once, after connecting the signal. (This is
 * the second half of the constructor of the JS class.)
 */
void
shell_panel_begin_startup (ShellPanel *self)
{
  ShellSessionMode *session_mode;

  g_return_if_fail (SHELL_IS_PANEL (self));

  if (self->started)
    return;

  session_mode = shell_main_get_session_mode (shell_main_get_default ());
  if (session_mode == NULL)
    {
      g_warning ("%s: no session mode", G_STRFUNC);
      return;
    }

  self->started = TRUE;

  g_set_weak_pointer (&self->session_mode, session_mode);
  self->session_updated_id =
    g_signal_connect (session_mode, "updated",
                      G_CALLBACK (session_updated_cb), self);

  update_panel (self);
}

/* ------------------------------------------------------------------------ */
/* Menus: keybindings and shortcuts                                         */
/* ------------------------------------------------------------------------ */

static ShellPanelMenuButton *
get_openable_indicator (ShellPanel *self,
                        const char *role)
{
  ShellPanelButtonBox *indicator = g_hash_table_lookup (self->status_area, role);

  /* menu not supported by current session mode */
  if (indicator == NULL ||
      !SHELL_IS_PANEL_MENU_BUTTON (indicator) ||
      !clutter_actor_is_mapped (CLUTTER_ACTOR (indicator)))
    return NULL;

  if (!clutter_actor_get_reactive (CLUTTER_ACTOR (indicator)))
    return NULL;

  return SHELL_PANEL_MENU_BUTTON (indicator);
}

static void
toggle_menu (ShellPanel *self,
             const char *role)
{
  ShellPanelMenuButton *indicator = get_openable_indicator (self, role);

  if (indicator != NULL)
    shell_panel_menu_button_toggle_menu (indicator);
}

static void
close_menu (ShellPanel *self,
            const char *role)
{
  ShellPanelMenuButton *indicator = get_openable_indicator (self, role);

  if (indicator != NULL)
    shell_panel_menu_button_close_menu (indicator, SHELL_POPUP_ANIMATION_FULL);
}

/**
 * shell_panel_toggle_calendar:
 * @self: a #ShellPanel
 *
 * Opens or closes the menu of the date menu, if the top bar shows one
 * (`toggleCalendar ()`).
 */
void
shell_panel_toggle_calendar (ShellPanel *self)
{
  g_return_if_fail (SHELL_IS_PANEL (self));

  toggle_menu (self, "dateMenu");
}

/**
 * shell_panel_toggle_quick_settings:
 * @self: a #ShellPanel
 *
 * Opens or closes the quick settings menu, if the top bar shows it
 * (`toggleQuickSettings ()`).
 */
void
shell_panel_toggle_quick_settings (ShellPanel *self)
{
  g_return_if_fail (SHELL_IS_PANEL (self));

  toggle_menu (self, "quickSettings");
}

/**
 * shell_panel_close_calendar:
 * @self: a #ShellPanel
 *
 * Closes the menu of the date menu (`closeCalendar ()`).
 */
void
shell_panel_close_calendar (ShellPanel *self)
{
  g_return_if_fail (SHELL_IS_PANEL (self));

  close_menu (self, "dateMenu");
}

/**
 * shell_panel_close_quick_settings:
 * @self: a #ShellPanel
 *
 * Closes the quick settings menu (`closeQuickSettings ()`).
 */
void
shell_panel_close_quick_settings (ShellPanel *self)
{
  g_return_if_fail (SHELL_IS_PANEL (self));

  close_menu (self, "quickSettings");
}

/* ------------------------------------------------------------------------ */
/* Dragging windows by the top bar                                          */
/* ------------------------------------------------------------------------ */

static MetaWindow *
get_draggable_window_for_position (float stage_x)
{
  ShellGlobal *global = shell_global_get ();
  MetaWorkspace *workspace =
    meta_workspace_manager_get_active_workspace (shell_global_get_workspace_manager (global));
  g_autoptr (GList) windows = meta_workspace_list_windows (workspace);
  GSList *unsorted = NULL;
  GSList *sorted;
  GList *wl;
  GSList *l;
  MetaWindow *result = NULL;

  for (wl = windows; wl != NULL; wl = wl->next)
    unsorted = g_slist_prepend (unsorted, wl->data);

  /* The sort goes from the bottom to the top: reverse it */
  sorted = meta_display_sort_windows_by_stacking (shell_global_get_display (global), unsorted);
  sorted = g_slist_reverse (sorted);
  g_slist_free (unsorted);

  for (l = sorted; l != NULL; l = l->next)
    {
      MetaWindow *window = l->data;
      MtkRectangle rect;
      gboolean maximized_vertically = FALSE;

      meta_window_get_frame_rect (window, &rect);
      g_object_get (window, "maximized-vertically", &maximized_vertically, NULL);

      if (meta_window_is_on_primary_monitor (window) &&
          meta_window_showing_on_its_workspace (window) &&
          meta_window_get_window_type (window) != META_WINDOW_DESKTOP &&
          maximized_vertically &&
          stage_x > rect.x && stage_x < rect.x + rect.width)
        {
          result = window;
          break;
        }
    }

  g_slist_free (sorted);

  return result;
}

static void
window_drag_recognize_cb (ClutterGesture *gesture,
                          ShellPanel     *self)
{
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());
  ClutterBackend *backend;
  const ClutterEvent *event;
  ClutterSprite *sprite;
  graphene_point_t coords;
  MetaWindow *drag_window;

  if (shell_main_get_modal_count (shell_main_get_default ()) > 0)
    return;

  event = clutter_gesture_get_point_event (gesture, 0);
  backend = clutter_context_get_backend (clutter_actor_get_context (CLUTTER_ACTOR (stage)));
  sprite = clutter_backend_get_sprite (backend, stage, event);

  clutter_press_gesture_get_coords_abs (CLUTTER_PRESS_GESTURE (gesture), &coords);
  drag_window = get_draggable_window_for_position (coords.x);

  if (drag_window == NULL)
    return;

  meta_window_begin_grab_op (drag_window,
                             META_GRAB_OP_MOVING,
                             sprite,
                             clutter_event_get_time (event),
                             &coords);
}

static gboolean
unfocus_binding_cb (GObject             *object,
                    const char          *action_name,
                    guint                key_val,
                    ClutterModifierType  modifiers,
                    gpointer             user_data)
{
  meta_display_focus_default_window (shell_global_get_display (shell_global_get ()),
                                     CLUTTER_CURRENT_TIME);
  return CLUTTER_EVENT_STOP;
}

/* ------------------------------------------------------------------------ */
/* Actor                                                                    */
/* ------------------------------------------------------------------------ */

static void
shell_panel_get_preferred_width (ClutterActor *actor,
                                 float         for_height,
                                 float        *min_width_p,
                                 float        *nat_width_p)
{
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  ShellMonitor *primary_monitor = NULL;

  if (layout_manager != NULL)
    primary_monitor = shell_layout_manager_get_primary_monitor (layout_manager);

  if (min_width_p != NULL)
    *min_width_p = 0;
  if (nat_width_p != NULL)
    *nat_width_p = primary_monitor != NULL ? primary_monitor->width : 0;

  g_clear_pointer (&primary_monitor, shell_monitor_free);
}

static void
shell_panel_allocate (ClutterActor          *actor,
                      const ClutterActorBox *box)
{
  ShellPanel *self = SHELL_PANEL (actor);
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  ShellMonitor *monitor = NULL;
  ClutterActorBox child_box;
  float left_natural_width = 0;
  float center_natural_width = 0;
  float right_natural_width = 0;
  double alloc_width, alloc_height;
  double center_width;
  double side_width;
  int center_offset = 0;
  gboolean rtl;

  /* Not chained up, like the vfunc_allocate () of the JS class */
  clutter_actor_set_allocation (actor, box);

  if (self->left_box == NULL || self->center_box == NULL || self->right_box == NULL)
    return;

  alloc_width = box->x2 - box->x1;
  alloc_height = box->y2 - box->y1;

  clutter_actor_get_preferred_width (CLUTTER_ACTOR (self->left_box), -1,
                                     NULL, &left_natural_width);
  clutter_actor_get_preferred_width (CLUTTER_ACTOR (self->center_box), -1,
                                     NULL, &center_natural_width);
  clutter_actor_get_preferred_width (CLUTTER_ACTOR (self->right_box), -1,
                                     NULL, &right_natural_width);

  center_width = center_natural_width;

  /* get workspace area and center date entry relative to it */
  if (layout_manager != NULL)
    monitor = shell_layout_manager_find_monitor_for_actor (layout_manager, actor);
  if (monitor != NULL)
    {
      MtkRectangle work_area;

      shell_layout_manager_get_work_area_for_monitor (layout_manager, monitor->index, &work_area);
      center_offset = 2 * (work_area.x - monitor->x) + work_area.width - monitor->width;
    }
  g_clear_pointer (&monitor, shell_monitor_free);

  side_width = MAX (0, (alloc_width - center_width + center_offset) / 2);
  rtl = clutter_actor_get_text_direction (actor) == CLUTTER_TEXT_DIRECTION_RTL;

  child_box.y1 = 0;
  child_box.y2 = alloc_height;
  if (rtl)
    {
      child_box.x1 = MAX (alloc_width - MIN (floor (side_width), left_natural_width), 0);
      child_box.x2 = alloc_width;
    }
  else
    {
      child_box.x1 = 0;
      child_box.x2 = MIN (floor (side_width), left_natural_width);
    }
  clutter_actor_allocate (CLUTTER_ACTOR (self->left_box), &child_box);

  child_box.x1 = ceil (side_width);
  child_box.y1 = 0;
  child_box.x2 = child_box.x1 + center_width;
  child_box.y2 = alloc_height;
  clutter_actor_allocate (CLUTTER_ACTOR (self->center_box), &child_box);

  child_box.y1 = 0;
  child_box.y2 = alloc_height;
  if (rtl)
    {
      child_box.x1 = 0;
      child_box.x2 = MIN (floor (side_width), right_natural_width);
    }
  else
    {
      child_box.x1 = MAX (alloc_width - MIN (floor (side_width), right_natural_width), 0);
      child_box.x2 = alloc_width;
    }
  clutter_actor_allocate (CLUTTER_ACTOR (self->right_box), &child_box);
}

/* ------------------------------------------------------------------------ */
/* Properties                                                               */
/* ------------------------------------------------------------------------ */

/**
 * shell_panel_get_menu_manager:
 * @self: a #ShellPanel
 *
 * Returns: (transfer none): the manager of the menus of the indicators
 *   (`menuManager`)
 */
ShellPopupMenuManager *
shell_panel_get_menu_manager (ShellPanel *self)
{
  g_return_val_if_fail (SHELL_IS_PANEL (self), NULL);

  return self->menu_manager;
}

/**
 * shell_panel_get_left_box:
 * @self: a #ShellPanel
 *
 * Returns: (transfer none) (nullable): the box at the start of the top bar
 *   (`_leftBox`)
 */
StBoxLayout *
shell_panel_get_left_box (ShellPanel *self)
{
  g_return_val_if_fail (SHELL_IS_PANEL (self), NULL);

  return self->left_box;
}

/**
 * shell_panel_get_center_box:
 * @self: a #ShellPanel
 *
 * Returns: (transfer none) (nullable): the box in the middle of the top bar
 *   (`_centerBox`)
 */
StBoxLayout *
shell_panel_get_center_box (ShellPanel *self)
{
  g_return_val_if_fail (SHELL_IS_PANEL (self), NULL);

  return self->center_box;
}

/**
 * shell_panel_get_right_box:
 * @self: a #ShellPanel
 *
 * Returns: (transfer none) (nullable): the box at the end of the top bar
 *   (`_rightBox`)
 */
StBoxLayout *
shell_panel_get_right_box (ShellPanel *self)
{
  g_return_val_if_fail (SHELL_IS_PANEL (self), NULL);

  return self->right_box;
}

/**
 * shell_panel_get_overview_visible:
 * @self: a #ShellPanel
 *
 * Returns: #ShellPanel:overview-visible
 */
gboolean
shell_panel_get_overview_visible (ShellPanel *self)
{
  g_return_val_if_fail (SHELL_IS_PANEL (self), FALSE);

  return self->overview_visible;
}

/**
 * shell_panel_set_overview_visible:
 * @self: a #ShellPanel
 * @visible: whether the overview is showing
 *
 * Gives the panel the `overview` pseudo class while the overview is shown.
 * ShellMain calls it with %TRUE when the overview starts showing and with
 * %FALSE when it starts hiding.
 */
void
shell_panel_set_overview_visible (ShellPanel *self,
                                  gboolean    visible)
{
  g_return_if_fail (SHELL_IS_PANEL (self));

  visible = !!visible;
  if (self->overview_visible == visible)
    return;

  self->overview_visible = visible;

  if (visible)
    st_widget_add_style_pseudo_class (ST_WIDGET (self), "overview");
  else
    st_widget_remove_style_pseudo_class (ST_WIDGET (self), "overview");

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OVERVIEW_VISIBLE]);
}

/**
 * shell_panel_get_box_opacity:
 * @self: a #ShellPanel
 *
 * Returns: #ShellPanel:box-opacity
 */
guint
shell_panel_get_box_opacity (ShellPanel *self)
{
  g_return_val_if_fail (SHELL_IS_PANEL (self), 0);

  if (self->left_box == NULL)
    return 0;

  return clutter_actor_get_opacity (CLUTTER_ACTOR (self->left_box));
}

/**
 * shell_panel_set_box_opacity:
 * @self: a #ShellPanel
 * @opacity: the opacity, from 0 to 255
 *
 * Sets the opacity of the three boxes (but not of the panel itself), which
 * are only reactive while it is not 0 (`boxOpacity`).
 */
void
shell_panel_set_box_opacity (ShellPanel *self,
                             guint       opacity)
{
  StBoxLayout *boxes[3];
  gboolean is_reactive = opacity > 0;
  guint i;

  g_return_if_fail (SHELL_IS_PANEL (self));

  boxes[0] = self->left_box;
  boxes[1] = self->center_box;
  boxes[2] = self->right_box;

  for (i = 0; i < G_N_ELEMENTS (boxes); i++)
    {
      if (boxes[i] == NULL)
        continue;

      clutter_actor_set_opacity (CLUTTER_ACTOR (boxes[i]), MIN (opacity, 255));
      clutter_actor_set_reactive (CLUTTER_ACTOR (boxes[i]), is_reactive);
    }

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_BOX_OPACITY]);
}

/**
 * shell_panel_get_banner_alignment:
 * @self: a #ShellPanel
 *
 * Returns: #ShellPanel:banner-alignment
 */
ClutterActorAlign
shell_panel_get_banner_alignment (ShellPanel *self)
{
  g_return_val_if_fail (SHELL_IS_PANEL (self), CLUTTER_ACTOR_ALIGN_CENTER);

  return self->banner_alignment;
}

/**
 * shell_panel_get_banner_blocked:
 * @self: a #ShellPanel
 *
 * Returns: #ShellPanel:banner-blocked
 */
gboolean
shell_panel_get_banner_blocked (ShellPanel *self)
{
  g_return_val_if_fail (SHELL_IS_PANEL (self), FALSE);

  return self->banner_blocked;
}

static void
shell_panel_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  ShellPanel *self = SHELL_PANEL (object);

  switch (prop_id)
    {
    case PROP_MENU_MANAGER:
      g_value_set_object (value, self->menu_manager);
      break;

    case PROP_LEFT_BOX:
      g_value_set_object (value, self->left_box);
      break;

    case PROP_CENTER_BOX:
      g_value_set_object (value, self->center_box);
      break;

    case PROP_RIGHT_BOX:
      g_value_set_object (value, self->right_box);
      break;

    case PROP_OVERVIEW_VISIBLE:
      g_value_set_boolean (value, self->overview_visible);
      break;

    case PROP_BOX_OPACITY:
      g_value_set_uint (value, shell_panel_get_box_opacity (self));
      break;

    case PROP_BANNER_ALIGNMENT:
      g_value_set_enum (value, self->banner_alignment);
      break;

    case PROP_BANNER_BLOCKED:
      g_value_set_boolean (value, self->banner_blocked);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_panel_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  ShellPanel *self = SHELL_PANEL (object);

  switch (prop_id)
    {
    case PROP_OVERVIEW_VISIBLE:
      shell_panel_set_overview_visible (self, g_value_get_boolean (value));
      break;

    case PROP_BOX_OPACITY:
      shell_panel_set_box_opacity (self, g_value_get_uint (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

/* ------------------------------------------------------------------------ */
/* Object                                                                   */
/* ------------------------------------------------------------------------ */

static StBoxLayout *
create_box (ShellPanel *self,
            const char *name)
{
  StBoxLayout *box = g_object_new (ST_TYPE_BOX_LAYOUT, "name", name, NULL);

  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (box));
  return box;
}

static void
shell_panel_constructed (GObject *object)
{
  ShellPanel *self = SHELL_PANEL (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellLayoutManager *layout_manager;
  ClutterAction *click_gesture;
  MetaDisplay *display;

  G_OBJECT_CLASS (shell_panel_parent_class)->constructed (object);

  clutter_actor_set_offscreen_redirect (actor, CLUTTER_OFFSCREEN_REDIRECT_ALWAYS);

  g_set_weak_pointer (&self->left_box, create_box (self, "panelLeft"));
  g_set_weak_pointer (&self->center_box, create_box (self, "panelCenter"));
  g_set_weak_pointer (&self->right_box, create_box (self, "panelRight"));

  click_gesture = clutter_click_gesture_new ();
  clutter_click_gesture_set_recognize_on_press (CLUTTER_CLICK_GESTURE (click_gesture), TRUE);
  g_signal_connect_object (click_gesture, "recognize",
                           G_CALLBACK (window_drag_recognize_cb), self, 0);
  clutter_actor_add_action_full (actor, "window-drag", CLUTTER_PHASE_TARGET, click_gesture);

  layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  if (layout_manager != NULL)
    clutter_actor_add_child (CLUTTER_ACTOR (shell_layout_manager_get_panel_box (layout_manager)),
                             actor);
  else
    g_warning ("%s: the layout manager does not exist yet", G_STRFUNC);

  /* The order of the extensions' indicators follows enabled-extensions */
  g_signal_connect_object (shell_global_get_settings (shell_global_get ()),
                           "changed::enabled-extensions",
                           G_CALLBACK (enabled_extensions_changed_cb), self, 0);

  display = shell_global_get_display (shell_global_get ());
  g_signal_connect_object (display, "workareas-changed",
                           G_CALLBACK (clutter_actor_queue_relayout), self,
                           G_CONNECT_SWAPPED);
}

static void
shell_panel_dispose (GObject *object)
{
  ShellPanel *self = SHELL_PANEL (object);

  if (self->session_mode != NULL)
    g_clear_signal_handler (&self->session_updated_id, self->session_mode);
  g_clear_weak_pointer (&self->session_mode);

  g_clear_weak_pointer (&self->left_box);
  g_clear_weak_pointer (&self->center_box);
  g_clear_weak_pointer (&self->right_box);

  g_clear_pointer (&self->ext_items, g_ptr_array_unref);
  g_clear_pointer (&self->status_area, g_hash_table_unref);
  g_clear_object (&self->menu_manager);

  G_OBJECT_CLASS (shell_panel_parent_class)->dispose (object);
}

static void
shell_panel_finalize (GObject *object)
{
  ShellPanel *self = SHELL_PANEL (object);

  g_clear_pointer (&self->session_style, g_free);

  G_OBJECT_CLASS (shell_panel_parent_class)->finalize (object);
}

static void
shell_panel_class_init (ShellPanelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  ClutterBindingPool *binding_pool;
  ShellHookRegistry *registry;

  object_class->constructed = shell_panel_constructed;
  object_class->dispose = shell_panel_dispose;
  object_class->finalize = shell_panel_finalize;
  object_class->get_property = shell_panel_get_property;
  object_class->set_property = shell_panel_set_property;

  actor_class->get_preferred_width = shell_panel_get_preferred_width;
  actor_class->allocate = shell_panel_allocate;

  /**
   * ShellPanel:menu-manager:
   *
   * The manager of the menus of the indicators (`menuManager`): it lets one
   * of them be open at a time and switches menus when the pointer moves to
   * another indicator.
   */
  props[PROP_MENU_MANAGER] =
    g_param_spec_object ("menu-manager", NULL, NULL,
                         SHELL_TYPE_POPUP_MENU_MANAGER,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPanel:left-box:
   *
   * The box at the start of the top bar (`_leftBox`).
   */
  props[PROP_LEFT_BOX] =
    g_param_spec_object ("left-box", NULL, NULL,
                         ST_TYPE_BOX_LAYOUT,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPanel:center-box:
   *
   * The box in the middle of the top bar (`_centerBox`).
   */
  props[PROP_CENTER_BOX] =
    g_param_spec_object ("center-box", NULL, NULL,
                         ST_TYPE_BOX_LAYOUT,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPanel:right-box:
   *
   * The box at the end of the top bar (`_rightBox`).
   */
  props[PROP_RIGHT_BOX] =
    g_param_spec_object ("right-box", NULL, NULL,
                         ST_TYPE_BOX_LAYOUT,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellPanel:overview-visible:
   *
   * Whether the overview is shown: the panel has the `overview` pseudo
   * class meanwhile. Set by ShellMain from the `showing` and `hiding` signals
   * of the overview.
   */
  props[PROP_OVERVIEW_VISIBLE] =
    g_param_spec_boolean ("overview-visible", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellPanel:box-opacity:
   *
   * The opacity of the three boxes, from 0 to 255 (`boxOpacity`). The boxes
   * are only reactive while it is not 0.
   */
  props[PROP_BOX_OPACITY] =
    g_param_spec_uint ("box-opacity", NULL, NULL,
                       0, 255, 255,
                       G_PARAM_READWRITE |
                       G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellPanel:banner-alignment:
   *
   * Where notification banners are aligned: the side of the screen the date
   * menu is on, or the center if the session mode has none. ShellMain sets the
   * `bannerAlignment` of the message tray from it.
   */
  props[PROP_BANNER_ALIGNMENT] =
    g_param_spec_enum ("banner-alignment", NULL, NULL,
                       CLUTTER_TYPE_ACTOR_ALIGN,
                       CLUTTER_ACTOR_ALIGN_CENTER,
                       G_PARAM_READABLE |
                       G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellPanel:banner-blocked:
   *
   * Whether the banners should be hidden: a menu is open in the box of the top
   * bar the banners are aligned with. ShellMain sets the `bannerBlocked` of the
   * message tray from it.
   */
  props[PROP_BANNER_BLOCKED] =
    g_param_spec_boolean ("banner-blocked", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellPanel::create-indicator:
   * @self: the #ShellPanel
   * @role: the role the session mode asks for, such as `dateMenu`
   *
   * Emitted when the session mode lists a role that has no indicator yet. A
   * handler creates the indicator of the shell for that role and returns
   * it; the first handler to return one wins. The panel adds it to the
   * top bar and registers it itself.
   *
   * Returns: (transfer none) (nullable): the new indicator, or %NULL if the
   *   handler has no indicator for @role
   */
  signals[CREATE_INDICATOR] =
    g_signal_new ("create-indicator",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  accumulate_first_indicator, NULL, NULL,
                  SHELL_TYPE_PANEL_BUTTON_BOX, 1, G_TYPE_STRING);

  binding_pool = clutter_binding_pool_get_for_class (klass);
  clutter_binding_pool_install_action (binding_pool,
                                       "unfocus",
                                       CLUTTER_KEY_Escape,
                                       0,
                                       G_CALLBACK (unfocus_binding_cb),
                                       NULL,
                                       NULL);

  /* Lua extensions (gnome-lua): the hook points of the top bar */
  registry = shell_hook_registry_get_default ();
  shell_hook_registry_declare (registry, "panel.indicator-added",
                               "name:s box:s owner:s", FALSE,
                               "An indicator was added to the top bar");
  shell_hook_registry_declare (registry, "panel.menu-opened",
                               "name:s box:s", FALSE,
                               "The menu of an indicator of the top bar opened");
}

static void
shell_panel_init (ShellPanel *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  clutter_actor_set_name (actor, "panel");
  clutter_actor_set_reactive (actor, TRUE);

  self->menu_manager = shell_popup_menu_manager_new (SHELL_ACTION_MODE_POPUP);
  self->status_area = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, g_object_unref);
  self->ext_items = g_ptr_array_new_with_free_func (ext_item_free);
  self->banner_alignment = CLUTTER_ACTOR_ALIGN_CENTER;
}
