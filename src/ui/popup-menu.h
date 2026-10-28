/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * popup-menu.h: The shell's menu library: menu items, the menu classes
 * (popup menu with a box pointer, submenu, section, dummy) and the menu
 * manager that grabs input while a menu is open. Port of js/ui/popupMenu.js.
 *
 * Every class is derivable (except the dummy menu, the switch and the
 * manager) because most panel, status and dialog modules still subclass them
 * through GObject.registerClass (). What a JS subclass used to override
 * (`open`, `close`, `destroy`, `isEmpty`, `activate`, `syncSensitive`,
 * `_setParent`, `_updateOrnamentStyle`) is a vfunc in the class structs below
 * (`vfunc_open`, `vfunc_close`, ...).
 */

#pragma once

#include <gio/gio.h>

#include "shell-action-modes.h"
#include "st.h"
#include "ui/box-pointer.h"
#include "ui/session-mode.h"

G_BEGIN_DECLS

/**
 * ShellOrnament:
 * @SHELL_ORNAMENT_NONE: no ornament, but space is reserved for one
 * @SHELL_ORNAMENT_DOT: a checked radio dot
 * @SHELL_ORNAMENT_CHECK: a check mark
 * @SHELL_ORNAMENT_HIDDEN: no ornament and no space reserved for it
 * @SHELL_ORNAMENT_NO_DOT: an unchecked radio dot
 *
 * The decoration at the start of a #ShellPopupBaseMenuItem. Same values as
 * the former `PopupMenu.Ornament`.
 */
typedef enum
{
  SHELL_ORNAMENT_NONE   = 0,
  SHELL_ORNAMENT_DOT    = 1,
  SHELL_ORNAMENT_CHECK  = 2,
  SHELL_ORNAMENT_HIDDEN = 3,
  SHELL_ORNAMENT_NO_DOT = 4,
} ShellOrnament;

/**
 * ShellPopupMenuActionFunc:
 * @event: (nullable): the event which activated the item
 * @user_data: (closure): the user data given along with the callback
 *
 * Called when an item added by shell_popup_menu_base_add_action () is
 * activated.
 */
typedef void (* ShellPopupMenuActionFunc) (const ClutterEvent *event,
                                           gpointer            user_data);

#define SHELL_TYPE_POPUP_BASE_MENU_ITEM (shell_popup_base_menu_item_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellPopupBaseMenuItem, shell_popup_base_menu_item,
                          SHELL, POPUP_BASE_MENU_ITEM, StBoxLayout)

/* ------------------------------------------------------------------------ */
/* ShellSwitch                                                              */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SWITCH (shell_switch_get_type ())
G_DECLARE_FINAL_TYPE (ShellSwitch, shell_switch, SHELL, SWITCH, StWidget)

ShellSwitch *shell_switch_new       (gboolean     state);
gboolean     shell_switch_get_state (ShellSwitch *self);
void         shell_switch_set_state (ShellSwitch *self,
                                     gboolean     state);
void         shell_switch_toggle    (ShellSwitch *self);

/* ------------------------------------------------------------------------ */
/* ShellPopupMenuBase                                                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_POPUP_MENU_BASE (shell_popup_menu_base_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellPopupMenuBase, shell_popup_menu_base,
                          SHELL, POPUP_MENU_BASE, GObject)

/**
 * ShellPopupMenuBaseClass:
 * @parent_class: the parent class
 * @open: opens the menu. Returns whether the open state changed
 * @close: closes the menu. Returns whether the open state changed
 * @destroy: destroys the menu, its actor and its items
 * @is_empty: whether the menu has no visible item
 * @get_sensitive: whether the menu is sensitive, taking its parent into account
 */
struct _ShellPopupMenuBaseClass
{
  GObjectClass parent_class;

  gboolean (* open)          (ShellPopupMenuBase *self,
                              ShellPopupAnimation animate);
  gboolean (* close)         (ShellPopupMenuBase *self,
                              ShellPopupAnimation animate);
  void     (* destroy)       (ShellPopupMenuBase *self);
  gboolean (* is_empty)      (ShellPopupMenuBase *self);
  gboolean (* get_sensitive) (ShellPopupMenuBase *self);

  /*< private >*/
  gpointer padding[8];
};

ShellPopupMenuBase     *shell_popup_menu_base_get_from_actor   (ClutterActor        *actor);

ClutterActor           *shell_popup_menu_base_get_actor        (ShellPopupMenuBase  *self);
void                    shell_popup_menu_base_set_actor        (ShellPopupMenuBase  *self,
                                                                ClutterActor        *actor);
StBoxLayout            *shell_popup_menu_base_get_box          (ShellPopupMenuBase  *self);
ClutterActor           *shell_popup_menu_base_get_source_actor (ShellPopupMenuBase  *self);
ClutterActor           *shell_popup_menu_base_get_focus_actor  (ShellPopupMenuBase  *self);
void                    shell_popup_menu_base_set_focus_actor  (ShellPopupMenuBase  *self,
                                                                ClutterActor        *actor);
gboolean                shell_popup_menu_base_get_is_open      (ShellPopupMenuBase  *self);
guint                   shell_popup_menu_base_get_length       (ShellPopupMenuBase  *self);

gboolean                shell_popup_menu_base_get_sensitive    (ShellPopupMenuBase  *self);
void                    shell_popup_menu_base_set_sensitive    (ShellPopupMenuBase  *self,
                                                                gboolean             sensitive);

ShellPopupMenuBase     *shell_popup_menu_base_get_parent_menu  (ShellPopupMenuBase  *self);
void                    shell_popup_menu_base_set_parent_menu  (ShellPopupMenuBase  *self,
                                                                ShellPopupMenuBase  *parent);
ShellPopupMenuBase     *shell_popup_menu_base_get_top_menu     (ShellPopupMenuBase  *self);

gboolean                shell_popup_menu_base_open             (ShellPopupMenuBase  *self,
                                                                ShellPopupAnimation  animate);
gboolean                shell_popup_menu_base_close            (ShellPopupMenuBase  *self,
                                                                ShellPopupAnimation  animate);
void                    shell_popup_menu_base_toggle           (ShellPopupMenuBase  *self);
void                    shell_popup_menu_base_destroy          (ShellPopupMenuBase  *self);
gboolean                shell_popup_menu_base_is_empty         (ShellPopupMenuBase  *self);

void                    shell_popup_menu_base_item_activated   (ShellPopupMenuBase  *self);
void                    shell_popup_menu_base_set_opened_submenu (ShellPopupMenuBase *self,
                                                                  ShellPopupMenuBase *submenu);

void                    shell_popup_menu_base_add_menu_item    (ShellPopupMenuBase  *self,
                                                                GObject             *menu_item);
void                    shell_popup_menu_base_insert_menu_item (ShellPopupMenuBase  *self,
                                                                GObject             *menu_item,
                                                                int                  position);
void                    shell_popup_menu_base_move_menu_item   (ShellPopupMenuBase  *self,
                                                                GObject             *menu_item,
                                                                int                  position);
void                    shell_popup_menu_base_remove_all       (ShellPopupMenuBase  *self);
GList                  *shell_popup_menu_base_get_menu_items   (ShellPopupMenuBase  *self);
GObject                *shell_popup_menu_base_get_first_menu_item (ShellPopupMenuBase *self);
guint                   shell_popup_menu_base_get_n_menu_items (ShellPopupMenuBase  *self);

ShellPopupBaseMenuItem *shell_popup_menu_base_add_action       (ShellPopupMenuBase       *self,
                                                                const char               *title,
                                                                ShellPopupMenuActionFunc  callback,
                                                                gpointer                  user_data,
                                                                GDestroyNotify            destroy);
ShellPopupBaseMenuItem *shell_popup_menu_base_add_action_with_icon (ShellPopupMenuBase       *self,
                                                                    const char               *title,
                                                                    GIcon                    *gicon,
                                                                    const char               *icon_name,
                                                                    ShellPopupMenuActionFunc  callback,
                                                                    gpointer                  user_data,
                                                                    GDestroyNotify            destroy);
ShellPopupBaseMenuItem *shell_popup_menu_base_add_settings_action (ShellPopupMenuBase *self,
                                                                   const char         *title,
                                                                   const char         *desktop_file);
void                    shell_popup_menu_base_set_settings_visibility (ShellPopupMenuBase *self,
                                                                       gboolean            visible);

/* ------------------------------------------------------------------------ */
/* ShellPopupBaseMenuItem                                                   */
/* ------------------------------------------------------------------------ */

/**
 * ShellPopupBaseMenuItemClass:
 * @parent_class: the parent class
 * @activate: activates the item; the default emits #ShellPopupBaseMenuItem::activate
 * @sync_sensitive: updates the actor from the sensitivity; returns the new sensitivity
 * @set_parent_menu: called when the item is added to a menu
 * @update_ornament_style: updates the style class which makes room for the ornament
 */
struct _ShellPopupBaseMenuItemClass
{
  StBoxLayoutClass parent_class;

  void     (* activate)              (ShellPopupBaseMenuItem *self,
                                      const ClutterEvent     *event);
  gboolean (* sync_sensitive)        (ShellPopupBaseMenuItem *self);
  void     (* set_parent_menu)       (ShellPopupBaseMenuItem *self,
                                      ShellPopupMenuBase     *parent);
  void     (* update_ornament_style) (ShellPopupBaseMenuItem *self);

  /*< private >*/
  gpointer padding[8];
};

gboolean            shell_popup_base_menu_item_get_active      (ShellPopupBaseMenuItem *self);
void                shell_popup_base_menu_item_set_active      (ShellPopupBaseMenuItem *self,
                                                                gboolean                active);
gboolean            shell_popup_base_menu_item_get_sensitive   (ShellPopupBaseMenuItem *self);
void                shell_popup_base_menu_item_set_sensitive   (ShellPopupBaseMenuItem *self,
                                                                gboolean                sensitive);
gboolean            shell_popup_base_menu_item_sync_sensitive  (ShellPopupBaseMenuItem *self);
void                shell_popup_base_menu_item_activate        (ShellPopupBaseMenuItem *self,
                                                                const ClutterEvent     *event);
ShellOrnament       shell_popup_base_menu_item_get_ornament    (ShellPopupBaseMenuItem *self);
void                shell_popup_base_menu_item_set_ornament    (ShellPopupBaseMenuItem *self,
                                                                ShellOrnament           ornament);
StIcon             *shell_popup_base_menu_item_get_ornament_icon (ShellPopupBaseMenuItem *self);
ShellPopupMenuBase *shell_popup_base_menu_item_get_parent_menu (ShellPopupBaseMenuItem *self);
void                shell_popup_base_menu_item_set_parent_menu (ShellPopupBaseMenuItem *self,
                                                                ShellPopupMenuBase     *parent);
ShellPopupMenuBase *shell_popup_base_menu_item_get_top_menu    (ShellPopupBaseMenuItem *self);

StIcon             *shell_popup_menu_arrow_icon                (StSide                  side);

/* ------------------------------------------------------------------------ */
/* ShellPopupMenuItem                                                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_POPUP_MENU_ITEM (shell_popup_menu_item_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellPopupMenuItem, shell_popup_menu_item,
                          SHELL, POPUP_MENU_ITEM, ShellPopupBaseMenuItem)

struct _ShellPopupMenuItemClass
{
  ShellPopupBaseMenuItemClass parent_class;

  /*< private >*/
  gpointer padding[6];
};

StLabel    *shell_popup_menu_item_get_label (ShellPopupMenuItem *self);
const char *shell_popup_menu_item_get_text  (ShellPopupMenuItem *self);
void        shell_popup_menu_item_set_text  (ShellPopupMenuItem *self,
                                             const char         *text);

/* ------------------------------------------------------------------------ */
/* ShellPopupSeparatorMenuItem                                              */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM (shell_popup_separator_menu_item_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellPopupSeparatorMenuItem, shell_popup_separator_menu_item,
                          SHELL, POPUP_SEPARATOR_MENU_ITEM, ShellPopupBaseMenuItem)

struct _ShellPopupSeparatorMenuItemClass
{
  ShellPopupBaseMenuItemClass parent_class;

  /*< private >*/
  gpointer padding[6];
};

StLabel    *shell_popup_separator_menu_item_get_label (ShellPopupSeparatorMenuItem *self);
const char *shell_popup_separator_menu_item_get_text  (ShellPopupSeparatorMenuItem *self);
void        shell_popup_separator_menu_item_set_text  (ShellPopupSeparatorMenuItem *self,
                                                       const char                  *text);

/* ------------------------------------------------------------------------ */
/* ShellPopupSwitchMenuItem                                                 */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_POPUP_SWITCH_MENU_ITEM (shell_popup_switch_menu_item_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellPopupSwitchMenuItem, shell_popup_switch_menu_item,
                          SHELL, POPUP_SWITCH_MENU_ITEM, ShellPopupBaseMenuItem)

struct _ShellPopupSwitchMenuItemClass
{
  ShellPopupBaseMenuItemClass parent_class;

  /*< private >*/
  gpointer padding[6];
};

StLabel    *shell_popup_switch_menu_item_get_label        (ShellPopupSwitchMenuItem *self);
const char *shell_popup_switch_menu_item_get_text         (ShellPopupSwitchMenuItem *self);
void        shell_popup_switch_menu_item_set_text         (ShellPopupSwitchMenuItem *self,
                                                           const char               *text);
gboolean    shell_popup_switch_menu_item_get_state        (ShellPopupSwitchMenuItem *self);
void        shell_popup_switch_menu_item_set_state        (ShellPopupSwitchMenuItem *self,
                                                           gboolean                  state);
void        shell_popup_switch_menu_item_set_toggle_state (ShellPopupSwitchMenuItem *self,
                                                           gboolean                  state);
void        shell_popup_switch_menu_item_set_status       (ShellPopupSwitchMenuItem *self,
                                                           const char               *text);
void        shell_popup_switch_menu_item_toggle           (ShellPopupSwitchMenuItem *self);

/* ------------------------------------------------------------------------ */
/* ShellPopupImageMenuItem                                                  */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_POPUP_IMAGE_MENU_ITEM (shell_popup_image_menu_item_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellPopupImageMenuItem, shell_popup_image_menu_item,
                          SHELL, POPUP_IMAGE_MENU_ITEM, ShellPopupBaseMenuItem)

struct _ShellPopupImageMenuItemClass
{
  ShellPopupBaseMenuItemClass parent_class;

  /*< private >*/
  gpointer padding[6];
};

StLabel    *shell_popup_image_menu_item_get_label     (ShellPopupImageMenuItem *self);
StIcon     *shell_popup_image_menu_item_get_icon      (ShellPopupImageMenuItem *self);
const char *shell_popup_image_menu_item_get_text      (ShellPopupImageMenuItem *self);
void        shell_popup_image_menu_item_set_text      (ShellPopupImageMenuItem *self,
                                                       const char              *text);
GIcon      *shell_popup_image_menu_item_get_gicon     (ShellPopupImageMenuItem *self);
void        shell_popup_image_menu_item_set_gicon     (ShellPopupImageMenuItem *self,
                                                       GIcon                   *gicon);
const char *shell_popup_image_menu_item_get_icon_name (ShellPopupImageMenuItem *self);
void        shell_popup_image_menu_item_set_icon_name (ShellPopupImageMenuItem *self,
                                                       const char              *icon_name);

/* ------------------------------------------------------------------------ */
/* ShellPopupMenu                                                           */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_POPUP_MENU (shell_popup_menu_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellPopupMenu, shell_popup_menu,
                          SHELL, POPUP_MENU, ShellPopupMenuBase)

struct _ShellPopupMenuClass
{
  ShellPopupMenuBaseClass parent_class;

  /*< private >*/
  gpointer padding[6];
};

ShellPopupMenu  *shell_popup_menu_new                  (ClutterActor   *source_actor,
                                                        double          arrow_alignment,
                                                        StSide          arrow_side);
ShellBoxPointer *shell_popup_menu_get_box_pointer      (ShellPopupMenu *self);
StSide           shell_popup_menu_get_arrow_side       (ShellPopupMenu *self);
double           shell_popup_menu_get_arrow_alignment  (ShellPopupMenu *self);
void             shell_popup_menu_set_arrow_origin     (ShellPopupMenu *self,
                                                        double          origin);
void             shell_popup_menu_set_source_alignment (ShellPopupMenu *self,
                                                        double          alignment);

/* ------------------------------------------------------------------------ */
/* ShellPopupDummyMenu                                                      */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_POPUP_DUMMY_MENU (shell_popup_dummy_menu_get_type ())
G_DECLARE_FINAL_TYPE (ShellPopupDummyMenu, shell_popup_dummy_menu,
                      SHELL, POPUP_DUMMY_MENU, GObject)

ShellPopupDummyMenu *shell_popup_dummy_menu_new        (ClutterActor        *source_actor);
ClutterActor        *shell_popup_dummy_menu_get_actor  (ShellPopupDummyMenu *self);
gboolean             shell_popup_dummy_menu_get_is_open (ShellPopupDummyMenu *self);
gboolean             shell_popup_dummy_menu_get_sensitive (ShellPopupDummyMenu *self);
void                 shell_popup_dummy_menu_open       (ShellPopupDummyMenu *self,
                                                        ShellPopupAnimation  animate);
void                 shell_popup_dummy_menu_close      (ShellPopupDummyMenu *self,
                                                        ShellPopupAnimation  animate);
void                 shell_popup_dummy_menu_toggle     (ShellPopupDummyMenu *self);
void                 shell_popup_dummy_menu_destroy    (ShellPopupDummyMenu *self);

/* ------------------------------------------------------------------------ */
/* ShellPopupSubMenu                                                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_POPUP_SUB_MENU (shell_popup_sub_menu_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellPopupSubMenu, shell_popup_sub_menu,
                          SHELL, POPUP_SUB_MENU, ShellPopupMenuBase)

struct _ShellPopupSubMenuClass
{
  ShellPopupMenuBaseClass parent_class;

  /*< private >*/
  gpointer padding[6];
};

ClutterActor *shell_popup_sub_menu_get_source_arrow (ShellPopupSubMenu *self);

/* ------------------------------------------------------------------------ */
/* ShellPopupMenuSection                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_POPUP_MENU_SECTION (shell_popup_menu_section_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellPopupMenuSection, shell_popup_menu_section,
                          SHELL, POPUP_MENU_SECTION, ShellPopupMenuBase)

struct _ShellPopupMenuSectionClass
{
  ShellPopupMenuBaseClass parent_class;

  /*< private >*/
  gpointer padding[6];
};

ShellPopupMenuSection *shell_popup_menu_section_new (void);

/* ------------------------------------------------------------------------ */
/* ShellPopupSubMenuMenuItem                                                */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_POPUP_SUB_MENU_MENU_ITEM (shell_popup_sub_menu_menu_item_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellPopupSubMenuMenuItem, shell_popup_sub_menu_menu_item,
                          SHELL, POPUP_SUB_MENU_MENU_ITEM, ShellPopupBaseMenuItem)

struct _ShellPopupSubMenuMenuItemClass
{
  ShellPopupBaseMenuItemClass parent_class;

  /*< private >*/
  gpointer padding[6];
};

ShellPopupSubMenu *shell_popup_sub_menu_menu_item_get_menu          (ShellPopupSubMenuMenuItem *self);
StLabel           *shell_popup_sub_menu_menu_item_get_label         (ShellPopupSubMenuMenuItem *self);
StIcon            *shell_popup_sub_menu_menu_item_get_icon          (ShellPopupSubMenuMenuItem *self);
const char        *shell_popup_sub_menu_menu_item_get_text          (ShellPopupSubMenuMenuItem *self);
void               shell_popup_sub_menu_menu_item_set_text          (ShellPopupSubMenuMenuItem *self,
                                                                     const char                *text);
void               shell_popup_sub_menu_menu_item_set_submenu_shown (ShellPopupSubMenuMenuItem *self,
                                                                     gboolean                   open);

/* ------------------------------------------------------------------------ */
/* ShellPopupMenuManager                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_POPUP_MENU_MANAGER (shell_popup_menu_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellPopupMenuManager, shell_popup_menu_manager,
                      SHELL, POPUP_MENU_MANAGER, GObject)

ShellPopupMenuManager *shell_popup_menu_manager_new             (ShellActionMode        action_mode);
void                   shell_popup_menu_manager_add_menu        (ShellPopupMenuManager *self,
                                                                 GObject               *menu);
void                   shell_popup_menu_manager_insert_menu     (ShellPopupMenuManager *self,
                                                                 GObject               *menu,
                                                                 int                    position);
void                   shell_popup_menu_manager_remove_menu     (ShellPopupMenuManager *self,
                                                                 GObject               *menu);
GObject               *shell_popup_menu_manager_get_active_menu (ShellPopupMenuManager *self);
ShellActionMode        shell_popup_menu_manager_get_action_mode (ShellPopupMenuManager *self);

G_END_DECLS
