/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * quick-settings.h: The items of the quick settings menu of the top bar
 * (toggles, toggles with a menu, sliders), the menu which lays them out in a
 * grid and the menu which an item can open below itself. Port of
 * js/ui/quickSettings.js (`SystemIndicator`, which lived there as well, is
 * #ShellSystemIndicator in panel-menu.h).
 *
 * The items are derivable: the status modules (the files of js/ui/status) and the
 * extensions extend them with GObject.registerClass (). What the JS
 * constructors took as `params` are construct properties (`title`,
 * `icon-name`, `toggle-mode`, `has-menu`, ...).
 */

#pragma once

#include <clutter/clutter.h>
#include <gio/gio.h>

#include "st.h"
#include "ui/box-pointer.h"
#include "ui/popup-menu.h"
#include "ui/slider.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellQuickToggleMenu                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_QUICK_TOGGLE_MENU (shell_quick_toggle_menu_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellQuickToggleMenu, shell_quick_toggle_menu,
                          SHELL, QUICK_TOGGLE_MENU, ShellPopupMenuBase)

/**
 * ShellQuickToggleMenuClass:
 * @parent_class: the parent class
 */
struct _ShellQuickToggleMenuClass
{
  ShellPopupMenuBaseClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

ShellQuickToggleMenu *shell_quick_toggle_menu_new              (ClutterActor         *source_actor);
void                  shell_quick_toggle_menu_set_header       (ShellQuickToggleMenu *self,
                                                                const char           *icon_name,
                                                                const char           *title,
                                                                const char           *subtitle);
void                  shell_quick_toggle_menu_set_header_gicon (ShellQuickToggleMenu *self,
                                                                GIcon                *gicon,
                                                                const char           *title,
                                                                const char           *subtitle);
void                  shell_quick_toggle_menu_add_header_suffix (ShellQuickToggleMenu *self,
                                                                 ClutterActor         *actor);

/* ------------------------------------------------------------------------ */
/* ShellQuickSettingsItem                                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_QUICK_SETTINGS_ITEM (shell_quick_settings_item_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellQuickSettingsItem, shell_quick_settings_item,
                          SHELL, QUICK_SETTINGS_ITEM, StButton)

/**
 * ShellQuickSettingsItemClass:
 * @parent_class: the parent class
 */
struct _ShellQuickSettingsItemClass
{
  StButtonClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

gboolean              shell_quick_settings_item_get_has_menu (ShellQuickSettingsItem *self);
ShellQuickToggleMenu *shell_quick_settings_item_get_menu     (ShellQuickSettingsItem *self);
void                  shell_quick_settings_item_set_menu     (ShellQuickSettingsItem *self,
                                                              ShellQuickToggleMenu   *menu);

/* ------------------------------------------------------------------------ */
/* ShellQuickToggle                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_QUICK_TOGGLE (shell_quick_toggle_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellQuickToggle, shell_quick_toggle,
                          SHELL, QUICK_TOGGLE, ShellQuickSettingsItem)

/**
 * ShellQuickToggleClass:
 * @parent_class: the parent class
 */
struct _ShellQuickToggleClass
{
  ShellQuickSettingsItemClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

const char  *shell_quick_toggle_get_title          (ShellQuickToggle *self);
void         shell_quick_toggle_set_title          (ShellQuickToggle *self,
                                                    const char       *title);
const char  *shell_quick_toggle_get_subtitle       (ShellQuickToggle *self);
void         shell_quick_toggle_set_subtitle       (ShellQuickToggle *self,
                                                    const char       *subtitle);
const char  *shell_quick_toggle_get_icon_name      (ShellQuickToggle *self);
void         shell_quick_toggle_set_icon_name      (ShellQuickToggle *self,
                                                    const char       *icon_name);
GIcon       *shell_quick_toggle_get_gicon          (ShellQuickToggle *self);
void         shell_quick_toggle_set_gicon          (ShellQuickToggle *self,
                                                    GIcon            *gicon);
StBoxLayout *shell_quick_toggle_get_box            (ShellQuickToggle *self);
StIcon      *shell_quick_toggle_get_icon           (ShellQuickToggle *self);
StLabel     *shell_quick_toggle_get_title_label    (ShellQuickToggle *self);
StLabel     *shell_quick_toggle_get_subtitle_label (ShellQuickToggle *self);

/* ------------------------------------------------------------------------ */
/* ShellQuickMenuToggle                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_QUICK_MENU_TOGGLE (shell_quick_menu_toggle_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellQuickMenuToggle, shell_quick_menu_toggle,
                          SHELL, QUICK_MENU_TOGGLE, ShellQuickSettingsItem)

/**
 * ShellQuickMenuToggleClass:
 * @parent_class: the parent class
 */
struct _ShellQuickMenuToggleClass
{
  ShellQuickSettingsItemClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

const char *shell_quick_menu_toggle_get_title        (ShellQuickMenuToggle *self);
void        shell_quick_menu_toggle_set_title        (ShellQuickMenuToggle *self,
                                                      const char           *title);
const char *shell_quick_menu_toggle_get_subtitle     (ShellQuickMenuToggle *self);
void        shell_quick_menu_toggle_set_subtitle     (ShellQuickMenuToggle *self,
                                                      const char           *subtitle);
const char *shell_quick_menu_toggle_get_icon_name    (ShellQuickMenuToggle *self);
void        shell_quick_menu_toggle_set_icon_name    (ShellQuickMenuToggle *self,
                                                      const char           *icon_name);
GIcon      *shell_quick_menu_toggle_get_gicon        (ShellQuickMenuToggle *self);
void        shell_quick_menu_toggle_set_gicon        (ShellQuickMenuToggle *self,
                                                      GIcon                *gicon);
gboolean    shell_quick_menu_toggle_get_menu_enabled (ShellQuickMenuToggle *self);
void        shell_quick_menu_toggle_set_menu_enabled (ShellQuickMenuToggle *self,
                                                      gboolean              enabled);
StButton   *shell_quick_menu_toggle_get_menu_button  (ShellQuickMenuToggle *self);

/* ------------------------------------------------------------------------ */
/* ShellQuickSlider                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_QUICK_SLIDER (shell_quick_slider_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellQuickSlider, shell_quick_slider,
                          SHELL, QUICK_SLIDER, ShellQuickSettingsItem)

/**
 * ShellQuickSliderClass:
 * @parent_class: the parent class
 */
struct _ShellQuickSliderClass
{
  ShellQuickSettingsItemClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

ShellSlider *shell_quick_slider_get_slider        (ShellQuickSlider *self);
const char  *shell_quick_slider_get_icon_name     (ShellQuickSlider *self);
void         shell_quick_slider_set_icon_name     (ShellQuickSlider *self,
                                                   const char       *icon_name);
GIcon       *shell_quick_slider_get_gicon         (ShellQuickSlider *self);
void         shell_quick_slider_set_gicon         (ShellQuickSlider *self,
                                                   GIcon            *gicon);
gboolean     shell_quick_slider_get_icon_reactive (ShellQuickSlider *self);
void         shell_quick_slider_set_icon_reactive (ShellQuickSlider *self,
                                                   gboolean          reactive);
gboolean     shell_quick_slider_get_sensitive     (ShellQuickSlider *self);
void         shell_quick_slider_set_sensitive     (ShellQuickSlider *self,
                                                   gboolean          sensitive);
const char  *shell_quick_slider_get_icon_label    (ShellQuickSlider *self);
void         shell_quick_slider_set_icon_label    (ShellQuickSlider *self,
                                                   const char       *label);
gboolean     shell_quick_slider_get_menu_enabled  (ShellQuickSlider *self);
void         shell_quick_slider_set_menu_enabled  (ShellQuickSlider *self,
                                                   gboolean          enabled);

/* ------------------------------------------------------------------------ */
/* ShellQuickSettingsLayout                                                 */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_QUICK_SETTINGS_LAYOUT_META (shell_quick_settings_layout_meta_get_type ())
G_DECLARE_FINAL_TYPE (ShellQuickSettingsLayoutMeta, shell_quick_settings_layout_meta,
                      SHELL, QUICK_SETTINGS_LAYOUT_META, ClutterLayoutMeta)

#define SHELL_TYPE_QUICK_SETTINGS_LAYOUT (shell_quick_settings_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellQuickSettingsLayout, shell_quick_settings_layout,
                      SHELL, QUICK_SETTINGS_LAYOUT, ClutterLayoutManager)

ShellQuickSettingsLayout *shell_quick_settings_layout_new (ClutterActor *overlay,
                                                           int           n_columns);

/* ------------------------------------------------------------------------ */
/* ShellQuickSettingsMenu                                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_QUICK_SETTINGS_MENU (shell_quick_settings_menu_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellQuickSettingsMenu, shell_quick_settings_menu,
                          SHELL, QUICK_SETTINGS_MENU, ShellPopupMenu)

/**
 * ShellQuickSettingsMenuClass:
 * @parent_class: the parent class
 */
struct _ShellQuickSettingsMenuClass
{
  ShellPopupMenuClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

ShellQuickSettingsMenu *shell_quick_settings_menu_new               (ClutterActor           *source_actor,
                                                                     int                     n_columns);
void                    shell_quick_settings_menu_add_item          (ShellQuickSettingsMenu *self,
                                                                     ClutterActor           *item,
                                                                     int                     col_span);
void                    shell_quick_settings_menu_insert_item_before (ShellQuickSettingsMenu *self,
                                                                      ClutterActor           *item,
                                                                      ClutterActor           *sibling,
                                                                      int                     col_span);
int                     shell_quick_settings_menu_get_item_col_span (ShellQuickSettingsMenu *self,
                                                                     ClutterActor           *item);
void                    shell_quick_settings_menu_set_item_col_span (ShellQuickSettingsMenu *self,
                                                                     ClutterActor           *item,
                                                                     int                     col_span);
ClutterActor           *shell_quick_settings_menu_get_first_item    (ShellQuickSettingsMenu *self);
ClutterActor           *shell_quick_settings_menu_get_grid          (ShellQuickSettingsMenu *self);
ClutterActor           *shell_quick_settings_menu_get_overlay       (ShellQuickSettingsMenu *self);

G_END_DECLS
