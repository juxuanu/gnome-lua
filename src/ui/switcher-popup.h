/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * switcher-popup.h: The popup of Alt+Tab and its relatives: a row of items in
 * the middle of the primary monitor, a selection that moves with the keys, the
 * scroll wheel or the pointer, and a modal grab that ends when the modifier
 * keys of the shortcut are released. Port of js/ui/switcherPopup.js.
 *
 * #ShellSwitcherPopup is the popup (abstract) and #ShellSwitcherList the row
 * of items it shows. Both are derivable: the input source popup of
 * status/keyboard.js is still a JS class registered with
 * GObject.registerClass() that extends them, and so are the C subclasses
 * #ShellAppSwitcherPopup and the other popups of alt-tab.c,
 * #ShellCtrlAltTabPopup and #ShellSwitchMonitorPopup. The methods the
 * subclasses of the JS classes used to override (`_initialSelection`,
 * `_keyPressHandler`, `_scrollHandler`, `_itemActivatedHandler`, `_itemEnteredHandler`,
 * `_itemRemovedHandler`, `_isActorOutside`, `_select`, `_finish`, `show`, and
 * `highlight` and `_onItemMotion` of the list) are virtual functions of the
 * classes; JS subclasses override them with `vfunc_<name> ()` and chain up with
 * `super.vfunc_<name> ()`.
 */

#pragma once

#include <atk/atk.h>

#include "st.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellSwitcherList                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SWITCHER_LIST (shell_switcher_list_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellSwitcherList, shell_switcher_list,
                          SHELL, SWITCHER_LIST, StWidget)

/**
 * ShellSwitcherListClass:
 * @parent_class: the parent class
 * @highlight: highlights the item at an index, `highlight ()` of the old JS
 *   class. Chain up to scroll the item into view
 * @item_motion: the pointer moved over an item, `_onItemMotion ()` of the
 *   old JS class. The default emits #ShellSwitcherList::item-entered unless the
 *   item is the highlighted one
 */
struct _ShellSwitcherListClass
{
  StWidgetClass parent_class;

  void (* highlight)   (ShellSwitcherList *self,
                        int                index,
                        gboolean           just_outline);
  void (* item_motion) (ShellSwitcherList *self,
                        StButton          *item);

  /*< private >*/
  gpointer padding[8];
};

StButton     *shell_switcher_list_add_item               (ShellSwitcherList *self,
                                                          ClutterActor      *item,
                                                          ClutterActor      *label);
void          shell_switcher_list_remove_item            (ShellSwitcherList *self,
                                                          int                index);
void          shell_switcher_list_add_accessible_state   (ShellSwitcherList *self,
                                                          int                index,
                                                          AtkStateType       state);
void          shell_switcher_list_remove_accessible_state (ShellSwitcherList *self,
                                                           int                index,
                                                           AtkStateType       state);
void          shell_switcher_list_highlight              (ShellSwitcherList *self,
                                                          int                index,
                                                          gboolean           just_outline);

void          shell_switcher_list_item_activated         (ShellSwitcherList *self,
                                                          int                index);
void          shell_switcher_list_item_entered           (ShellSwitcherList *self,
                                                          int                index);

int           shell_switcher_list_get_n_items            (ShellSwitcherList *self);
StButton     *shell_switcher_list_get_item               (ShellSwitcherList *self,
                                                          int                index);
int           shell_switcher_list_get_item_index         (ShellSwitcherList *self,
                                                          StButton          *item);
int           shell_switcher_list_get_highlighted        (ShellSwitcherList *self);
StBoxLayout  *shell_switcher_list_get_list               (ShellSwitcherList *self);

void          shell_switcher_draw_arrow                  (StDrawingArea *area,
                                                          StSide         side);

/* ------------------------------------------------------------------------ */
/* ShellSwitcherPopup                                                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SWITCHER_POPUP (shell_switcher_popup_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellSwitcherPopup, shell_switcher_popup,
                          SHELL, SWITCHER_POPUP, StWidget)

/**
 * ShellSwitcherPopupClass:
 * @parent_class: the parent class
 * @show_switcher: grabs the input and shows the popup (`show ()` of the old JS
 *   class). Returns %FALSE if there is nothing to show
 * @get_n_items: the number of items of the popup. The JS classes kept the
 *   items in the array `_items`; subclasses that do override this and return
 *   its length
 * @initial_selection: selects the first item (`_initialSelection ()`)
 * @key_press_handler: handles a key press, returns %CLUTTER_EVENT_STOP if the
 *   key was used (`_keyPressHandler ()`)
 * @scroll_handler: handles a scroll step (`_scrollHandler ()`)
 * @item_activated_handler: an item was clicked (`_itemActivatedHandler ()`)
 * @item_entered_handler: the pointer entered an item (`_itemEnteredHandler ()`)
 * @item_removed_handler: an item was removed from the list
 *   (`_itemRemovedHandler ()`)
 * @is_actor_outside: whether a click on an actor means a click outside of the
 *   popup (`_isActorOutside ()`)
 * @select: selects an item (`_select ()`)
 * @finish: ends the popup with the selected item (`_finish ()`)
 */
struct _ShellSwitcherPopupClass
{
  StWidgetClass parent_class;

  gboolean (* show_switcher)          (ShellSwitcherPopup *self,
                                       gboolean            backward,
                                       const char         *binding,
                                       uint32_t            mask);
  int      (* get_n_items)            (ShellSwitcherPopup *self);
  void     (* initial_selection)      (ShellSwitcherPopup *self,
                                       gboolean            backward,
                                       const char         *binding);
  gboolean (* key_press_handler)      (ShellSwitcherPopup *self,
                                       uint32_t            keysym,
                                       uint32_t            action);
  void     (* scroll_handler)         (ShellSwitcherPopup *self,
                                       ClutterScrollDirection direction);
  void     (* item_activated_handler) (ShellSwitcherPopup *self,
                                       int                 index);
  void     (* item_entered_handler)   (ShellSwitcherPopup *self,
                                       int                 index);
  void     (* item_removed_handler)   (ShellSwitcherPopup *self,
                                       int                 index);
  gboolean (* is_actor_outside)       (ShellSwitcherPopup *self,
                                       ClutterActor       *actor);
  void     (* select)                 (ShellSwitcherPopup *self,
                                       int                 index);
  void     (* finish)                 (ShellSwitcherPopup *self,
                                       uint32_t            timestamp);

  /*< private >*/
  gpointer padding[8];
};

int       shell_switcher_popup_mod                   (int a,
                                                      int b);

gboolean  shell_switcher_popup_show_switcher         (ShellSwitcherPopup *self,
                                                      gboolean            backward,
                                                      const char         *binding,
                                                      uint32_t            mask);
void      shell_switcher_popup_fade_and_destroy      (ShellSwitcherPopup *self);

int       shell_switcher_popup_get_n_items           (ShellSwitcherPopup *self);
void      shell_switcher_popup_select                (ShellSwitcherPopup *self,
                                                      int                 index);
void      shell_switcher_popup_finish                (ShellSwitcherPopup *self,
                                                      uint32_t            timestamp);
gboolean  shell_switcher_popup_is_actor_outside      (ShellSwitcherPopup *self,
                                                      ClutterActor       *actor);

int       shell_switcher_popup_get_next_index        (ShellSwitcherPopup *self);
int       shell_switcher_popup_get_previous_index    (ShellSwitcherPopup *self);

int       shell_switcher_popup_get_selected_index    (ShellSwitcherPopup *self);
void      shell_switcher_popup_set_selected_index    (ShellSwitcherPopup *self,
                                                      int                 index);
StWidget *shell_switcher_popup_get_switcher_list     (ShellSwitcherPopup *self);
void      shell_switcher_popup_set_switcher_list     (ShellSwitcherPopup *self,
                                                      StWidget           *switcher_list);
gboolean  shell_switcher_popup_get_mouse_active      (ShellSwitcherPopup *self);

G_END_DECLS
