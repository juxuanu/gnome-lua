/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * magnifier.h: The screen magnifier of the accessibility menu: a list of zoom
 * regions showing a magnified clone of the `uiGroup`, the magnified mouse
 * pointer and the crosshairs. Port of js/ui/magnifier.js.
 *
 * #ShellMagnifier is created by ShellMain (`new Shell.Magnifier ()`) after the
 * layout manager and kept in `Main.magnifier`. It reads its settings from
 * `org.gnome.desktop.a11y.magnifier` and follows
 * #StSettings:magnifier-active by itself.
 *
 * The magnification, tracking and screen position modes are the values of
 * `GDesktopEnums.MagnifierMouseTrackingMode`, `MagnifierFocusTrackingMode`,
 * `MagnifierCaretTrackingMode` and `MagnifierScreenPosition`, passed as
 * integers so that the Shell GIR does not depend on GDesktopEnums.
 *
 * Copyright 2011 Red Hat, Inc.
 * Copyright 2012 Inclusive Design Research Centre, OCAD University.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>
#include <cogl/cogl.h>
#include <graphene.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellMagnifierCrosshairs                                                  */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MAGNIFIER_CROSSHAIRS (shell_magnifier_crosshairs_get_type ())
G_DECLARE_FINAL_TYPE (ShellMagnifierCrosshairs, shell_magnifier_crosshairs,
                      SHELL, MAGNIFIER_CROSSHAIRS, ClutterActor)

#define SHELL_TYPE_ZOOM_REGION (shell_zoom_region_get_type ())
G_DECLARE_FINAL_TYPE (ShellZoomRegion, shell_zoom_region,
                      SHELL, ZOOM_REGION, GObject)

ShellMagnifierCrosshairs *shell_magnifier_crosshairs_new   (void);

void          shell_magnifier_crosshairs_set_enabled       (ShellMagnifierCrosshairs *self,
                                                            gboolean                  enabled);
ClutterActor *shell_magnifier_crosshairs_add_to_zoom_region (ShellMagnifierCrosshairs *self,
                                                             ShellZoomRegion          *zoom_region,
                                                             ClutterActor             *magnified_mouse);
void          shell_magnifier_crosshairs_remove_from_parent (ShellMagnifierCrosshairs *self,
                                                             ClutterActor             *child_actor);
void          shell_magnifier_crosshairs_set_color         (ShellMagnifierCrosshairs *self,
                                                            const CoglColor          *color);
void          shell_magnifier_crosshairs_set_thickness     (ShellMagnifierCrosshairs *self,
                                                            double                    thickness);
double        shell_magnifier_crosshairs_get_thickness     (ShellMagnifierCrosshairs *self);
void          shell_magnifier_crosshairs_set_hair_opacity  (ShellMagnifierCrosshairs *self,
                                                            double                    opacity);
double        shell_magnifier_crosshairs_get_hair_opacity  (ShellMagnifierCrosshairs *self);
void          shell_magnifier_crosshairs_set_length        (ShellMagnifierCrosshairs *self,
                                                            double                    length);
double        shell_magnifier_crosshairs_get_length        (ShellMagnifierCrosshairs *self);
void          shell_magnifier_crosshairs_set_clip_size     (ShellMagnifierCrosshairs *self,
                                                            double                    width,
                                                            double                    height);
void          shell_magnifier_crosshairs_get_clip_size     (ShellMagnifierCrosshairs *self,
                                                            double                   *width,
                                                            double                   *height);
void          shell_magnifier_crosshairs_re_center         (ShellMagnifierCrosshairs *self);

/* ------------------------------------------------------------------------ */
/* ShellZoomRegion                                                           */
/* ------------------------------------------------------------------------ */

void     shell_zoom_region_set_active                (ShellZoomRegion *self,
                                                      gboolean         activate);
gboolean shell_zoom_region_is_active                 (ShellZoomRegion *self);

void     shell_zoom_region_set_mag_factor            (ShellZoomRegion *self,
                                                      double           x_mag_factor,
                                                      double           y_mag_factor);
void     shell_zoom_region_get_mag_factor            (ShellZoomRegion *self,
                                                      double          *x_mag_factor,
                                                      double          *y_mag_factor);

void     shell_zoom_region_set_mouse_tracking_mode   (ShellZoomRegion *self,
                                                      int              mode);
int      shell_zoom_region_get_mouse_tracking_mode   (ShellZoomRegion *self);
void     shell_zoom_region_set_focus_tracking_mode   (ShellZoomRegion *self,
                                                      int              mode);
void     shell_zoom_region_set_caret_tracking_mode   (ShellZoomRegion *self,
                                                      int              mode);

void     shell_zoom_region_set_view_port             (ShellZoomRegion       *self,
                                                      const graphene_rect_t *view_port);
void     shell_zoom_region_set_roi                   (ShellZoomRegion       *self,
                                                      const graphene_rect_t *roi);
void     shell_zoom_region_get_roi                   (ShellZoomRegion       *self,
                                                      graphene_rect_t       *roi);

void     shell_zoom_region_set_lens_mode             (ShellZoomRegion *self,
                                                      gboolean         lens_mode);
gboolean shell_zoom_region_is_lens_mode              (ShellZoomRegion *self);
void     shell_zoom_region_set_clamp_scrolling_at_edges (ShellZoomRegion *self,
                                                         gboolean         clamp);

void     shell_zoom_region_set_top_half              (ShellZoomRegion *self);
void     shell_zoom_region_set_bottom_half           (ShellZoomRegion *self);
void     shell_zoom_region_set_left_half             (ShellZoomRegion *self);
void     shell_zoom_region_set_right_half            (ShellZoomRegion *self);
void     shell_zoom_region_set_full_screen_mode      (ShellZoomRegion *self);
void     shell_zoom_region_set_screen_position       (ShellZoomRegion *self,
                                                      int              position);
int      shell_zoom_region_get_screen_position       (ShellZoomRegion *self);

gboolean shell_zoom_region_scroll_to_mouse_pos       (ShellZoomRegion *self);
void     shell_zoom_region_scroll_contents_to        (ShellZoomRegion *self,
                                                      double           x,
                                                      double           y);

void     shell_zoom_region_add_crosshairs            (ShellZoomRegion          *self,
                                                      ShellMagnifierCrosshairs *cross_hairs);

void     shell_zoom_region_set_invert_lightness      (ShellZoomRegion *self,
                                                      gboolean         flag);
gboolean shell_zoom_region_get_invert_lightness      (ShellZoomRegion *self);
void     shell_zoom_region_set_color_saturation      (ShellZoomRegion *self,
                                                      double           saturation);
double   shell_zoom_region_get_color_saturation      (ShellZoomRegion *self);
void     shell_zoom_region_set_brightness            (ShellZoomRegion *self,
                                                      double           red,
                                                      double           green,
                                                      double           blue);
void     shell_zoom_region_get_brightness            (ShellZoomRegion *self,
                                                      double          *red,
                                                      double          *green,
                                                      double          *blue);
void     shell_zoom_region_set_contrast              (ShellZoomRegion *self,
                                                      double           red,
                                                      double           green,
                                                      double           blue);
void     shell_zoom_region_get_contrast              (ShellZoomRegion *self,
                                                      double          *red,
                                                      double          *green,
                                                      double          *blue);

/* ------------------------------------------------------------------------ */
/* ShellMagnifier                                                            */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MAGNIFIER (shell_magnifier_get_type ())
G_DECLARE_FINAL_TYPE (ShellMagnifier, shell_magnifier,
                      SHELL, MAGNIFIER, GObject)

ShellMagnifier *shell_magnifier_new                    (void);

void            shell_magnifier_show_system_cursor     (ShellMagnifier *self);
void            shell_magnifier_hide_system_cursor     (ShellMagnifier *self);

void            shell_magnifier_set_active             (ShellMagnifier *self,
                                                        gboolean        activate);
gboolean        shell_magnifier_is_active              (ShellMagnifier *self);

void            shell_magnifier_start_tracking_mouse   (ShellMagnifier *self);
void            shell_magnifier_stop_tracking_mouse    (ShellMagnifier *self);
gboolean        shell_magnifier_is_tracking_mouse      (ShellMagnifier *self);
void            shell_magnifier_get_mouse_position     (ShellMagnifier *self,
                                                        double         *x_mouse,
                                                        double         *y_mouse);
void            shell_magnifier_scroll_to_mouse_pos    (ShellMagnifier *self);
void            shell_magnifier_scroll_to_point        (ShellMagnifier *self,
                                                        double          x_mouse,
                                                        double          y_mouse);

ShellZoomRegion *shell_magnifier_create_zoom_region    (ShellMagnifier        *self,
                                                        double                 x_mag_factor,
                                                        double                 y_mag_factor,
                                                        const graphene_rect_t *roi,
                                                        const graphene_rect_t *view_port);
void             shell_magnifier_add_zoom_region       (ShellMagnifier  *self,
                                                        ShellZoomRegion *zoom_region);
GList           *shell_magnifier_get_zoom_regions      (ShellMagnifier  *self);
void             shell_magnifier_clear_all_zoom_regions (ShellMagnifier *self);

void            shell_magnifier_add_crosshairs         (ShellMagnifier *self);
void            shell_magnifier_set_crosshairs_visible (ShellMagnifier *self,
                                                        gboolean        visible);
void            shell_magnifier_set_crosshairs_color   (ShellMagnifier *self,
                                                        const char     *color);
void            shell_magnifier_set_crosshairs_thickness (ShellMagnifier *self,
                                                          double          thickness);
double          shell_magnifier_get_crosshairs_thickness (ShellMagnifier *self);
void            shell_magnifier_set_crosshairs_opacity (ShellMagnifier *self,
                                                        double          opacity);
double          shell_magnifier_get_crosshairs_opacity (ShellMagnifier *self);
void            shell_magnifier_set_crosshairs_length  (ShellMagnifier *self,
                                                        double          length);
double          shell_magnifier_get_crosshairs_length  (ShellMagnifier *self);
void            shell_magnifier_set_crosshairs_clip    (ShellMagnifier *self,
                                                        gboolean        clip);
gboolean        shell_magnifier_get_crosshairs_clip    (ShellMagnifier *self);

G_END_DECLS
