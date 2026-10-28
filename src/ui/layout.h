/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * layout.h: The stage hierarchy of the shell (the "chrome"), the monitors, the
 * hot corners and the startup animation. Port of js/ui/layout.js.
 *
 * #ShellLayoutManager is created by ShellMain (`new Shell.LayoutManager ()`)
 * and kept in `Main.layoutManager`; there is no global accessor. The ported
 * UI code that needs the monitors reads them from #MetaDisplay, like the
 * Layout.MonitorConstraint of the JS did.
 *
 * The other objects of the JS file are private to layout.c (HotCorner,
 * PressureBarrier) or have a type of their own: #ShellMonitorConstraint (was
 * Layout.MonitorConstraint), #ShellUiActor (the actor of `uiGroup`),
 * #ShellScreenTransition (`screenTransition`) and the boxed #ShellMonitor.
 */

#pragma once

#include <clutter/clutter.h>
#include <mtk/mtk.h>

#include "st.h"

G_BEGIN_DECLS

/**
 * SHELL_STARTUP_ANIMATION_TIME:
 *
 * Duration in milliseconds of each step of the startup animation
 * (`Layout.STARTUP_ANIMATION_TIME`).
 */
#define SHELL_STARTUP_ANIMATION_TIME 500

/**
 * ShellChromeFlags:
 * @SHELL_CHROME_FLAGS_NONE: the actor only affects the stacking
 * @SHELL_CHROME_FLAGS_AFFECTS_STRUTS: the size and position of the actor
 *   (which is along a screen edge) affect the struts of the window manager.
 *   Changes of its visibility do not affect whether the strut is present
 * @SHELL_CHROME_FLAGS_TRACK_FULLSCREEN: the visibility of the actor is bound
 *   to the presence of a fullscreen window on the same monitor: it is hidden
 *   whenever a fullscreen window is visible, and shown otherwise
 *
 * How the layout manager treats a chrome actor. Replaces the `{affectsStruts,
 * trackFullscreen}` parameters of `addChrome ()` and friends.
 */
typedef enum
{
  SHELL_CHROME_FLAGS_NONE             = 0,
  SHELL_CHROME_FLAGS_AFFECTS_STRUTS   = 1 << 0,
  SHELL_CHROME_FLAGS_TRACK_FULLSCREEN = 1 << 1,
} ShellChromeFlags;

/* ------------------------------------------------------------------------ */
/* ShellMonitor                                                              */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MONITOR (shell_monitor_get_type ())

typedef struct _ShellMonitor ShellMonitor;

/**
 * ShellMonitor:
 * @index: the index of the monitor in #MetaDisplay
 * @x: the x coordinate of the monitor in the stage
 * @y: the y coordinate of the monitor in the stage
 * @width: the width of the monitor in the stage
 * @height: the height of the monitor in the stage
 * @geometry_scale: the scale of the monitor
 *
 * A snapshot of the geometry of a monitor, taken when the layout manager last
 * saw the monitors change (the `Monitor` objects of `layoutManager.monitors`).
 * Whether the monitor shows a fullscreen window is not part of the snapshot,
 * see shell_monitor_get_in_fullscreen ().
 */
struct _ShellMonitor
{
  int   index;
  int   x;
  int   y;
  int   width;
  int   height;
  float geometry_scale;
};

GType         shell_monitor_get_type          (void);
ShellMonitor *shell_monitor_copy              (const ShellMonitor *self);
void          shell_monitor_free              (ShellMonitor       *self);
gboolean      shell_monitor_get_in_fullscreen (const ShellMonitor *self);

/* ------------------------------------------------------------------------ */
/* ShellMonitorConstraint                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MONITOR_CONSTRAINT (shell_monitor_constraint_get_type ())
G_DECLARE_FINAL_TYPE (ShellMonitorConstraint, shell_monitor_constraint,
                      SHELL, MONITOR_CONSTRAINT, ClutterConstraint)

ShellMonitorConstraint *shell_monitor_constraint_new      (void);

gboolean                shell_monitor_constraint_get_primary   (ShellMonitorConstraint *self);
void                    shell_monitor_constraint_set_primary   (ShellMonitorConstraint *self,
                                                                gboolean                primary);
int                     shell_monitor_constraint_get_index     (ShellMonitorConstraint *self);
void                    shell_monitor_constraint_set_index     (ShellMonitorConstraint *self,
                                                                int                     index);
gboolean                shell_monitor_constraint_get_work_area (ShellMonitorConstraint *self);
void                    shell_monitor_constraint_set_work_area (ShellMonitorConstraint *self,
                                                                gboolean                work_area);

/* ------------------------------------------------------------------------ */
/* ShellUiActor, ShellScreenTransition                                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_UI_ACTOR (shell_ui_actor_get_type ())
G_DECLARE_FINAL_TYPE (ShellUiActor, shell_ui_actor, SHELL, UI_ACTOR, StWidget)

#define SHELL_TYPE_SCREEN_TRANSITION (shell_screen_transition_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenTransition, shell_screen_transition,
                      SHELL, SCREEN_TRANSITION, ClutterActor)

void shell_screen_transition_run (ShellScreenTransition *self);

/* ------------------------------------------------------------------------ */
/* ShellLayoutManager                                                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LAYOUT_MANAGER (shell_layout_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellLayoutManager, shell_layout_manager,
                      SHELL, LAYOUT_MANAGER, GObject)

ShellLayoutManager *shell_layout_manager_new                   (void);

void                shell_layout_manager_begin_startup         (ShellLayoutManager *self);
void                shell_layout_manager_startup_animation_finished (ShellLayoutManager *self);

void                shell_layout_manager_show_overview         (ShellLayoutManager *self);
void                shell_layout_manager_hide_overview         (ShellLayoutManager *self);
void                shell_layout_manager_modal_ended           (ShellLayoutManager *self);
void                shell_layout_manager_system_modal_opened   (ShellLayoutManager *self);

/* Actors of the stage hierarchy */
ShellUiActor          *shell_layout_manager_get_ui_group           (ShellLayoutManager *self);
StWidget              *shell_layout_manager_get_overview_group     (ShellLayoutManager *self);
StWidget              *shell_layout_manager_get_screen_shield_group (ShellLayoutManager *self);
StBoxLayout           *shell_layout_manager_get_panel_box          (ShellLayoutManager *self);
StWidget              *shell_layout_manager_get_modal_dialog_group (ShellLayoutManager *self);
StBoxLayout           *shell_layout_manager_get_keyboard_box       (ShellLayoutManager *self);
StWidget              *shell_layout_manager_get_screenshot_ui_group (ShellLayoutManager *self);
StWidget              *shell_layout_manager_get_dummy_cursor       (ShellLayoutManager *self);
ShellScreenTransition *shell_layout_manager_get_screen_transition  (ShellLayoutManager *self);

/* Monitors */
int                 shell_layout_manager_get_n_monitors        (ShellLayoutManager *self);
ShellMonitor       *shell_layout_manager_get_monitor           (ShellLayoutManager *self,
                                                                int                 index);
int                 shell_layout_manager_get_primary_index     (ShellLayoutManager *self);
ShellMonitor       *shell_layout_manager_get_primary_monitor   (ShellLayoutManager *self);
int                 shell_layout_manager_get_bottom_index      (ShellLayoutManager *self);
ShellMonitor       *shell_layout_manager_get_bottom_monitor    (ShellLayoutManager *self);
int                 shell_layout_manager_get_focus_index       (ShellLayoutManager *self);
ShellMonitor       *shell_layout_manager_get_focus_monitor     (ShellLayoutManager *self);
ShellMonitor       *shell_layout_manager_get_current_monitor   (ShellLayoutManager *self);
int                 shell_layout_manager_get_keyboard_index    (ShellLayoutManager *self);
void                shell_layout_manager_set_keyboard_index    (ShellLayoutManager *self,
                                                                int                 index);
ShellMonitor       *shell_layout_manager_get_keyboard_monitor  (ShellLayoutManager *self);

int                 shell_layout_manager_find_index_for_actor  (ShellLayoutManager *self,
                                                                ClutterActor       *actor);
ShellMonitor       *shell_layout_manager_find_monitor_for_actor (ShellLayoutManager *self,
                                                                 ClutterActor       *actor);
ShellMonitor       *shell_layout_manager_find_monitor_for_point (ShellLayoutManager *self,
                                                                 double              x,
                                                                 double              y);
void                shell_layout_manager_get_work_area_for_monitor (ShellLayoutManager *self,
                                                                    int                 monitor_index,
                                                                    MtkRectangle       *work_area);

/* Chrome */
void                shell_layout_manager_add_chrome            (ShellLayoutManager *self,
                                                                ClutterActor       *actor,
                                                                ShellChromeFlags    flags);
void                shell_layout_manager_add_top_chrome        (ShellLayoutManager *self,
                                                                ClutterActor       *actor,
                                                                ShellChromeFlags    flags);
void                shell_layout_manager_track_chrome          (ShellLayoutManager *self,
                                                                ClutterActor       *actor);
void                shell_layout_manager_track_chrome_with_flags (ShellLayoutManager *self,
                                                                  ClutterActor       *actor,
                                                                  ShellChromeFlags    flags);
void                shell_layout_manager_untrack_chrome        (ShellLayoutManager *self,
                                                                ClutterActor       *actor);
void                shell_layout_manager_remove_chrome         (ShellLayoutManager *self,
                                                                ClutterActor       *actor);

void                shell_layout_manager_set_dummy_cursor_geometry (ShellLayoutManager *self,
                                                                    double              x,
                                                                    double              y,
                                                                    double              width,
                                                                    double              height);

int                 shell_layout_manager_get_n_background_managers (ShellLayoutManager *self);

G_END_DECLS
