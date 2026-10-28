/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * bar-level.h: A horizontal bar showing a value between 0 and a maximum,
 * with an optional "overdrive" zone. Port of js/ui/barLevel.js.
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

#define SHELL_TYPE_BAR_LEVEL (shell_bar_level_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellBarLevel, shell_bar_level,
                          SHELL, BAR_LEVEL, StDrawingArea)

/**
 * ShellBarLevelClass:
 * @parent_class: the parent class
 * @get_preferred_bar_height: virtual function returning the content height of
 *   the bar, before the theme node's padding and borders are added. The
 *   default implementation returns the `-barlevel-height` CSS length.
 * @get_preferred_bar_width: virtual function returning the content width of
 *   the bar, before the theme node's padding and borders are added. The
 *   default implementation returns the `-barlevel-overdrive-separator-width`
 *   CSS length.
 */
struct _ShellBarLevelClass
{
  StDrawingAreaClass parent_class;

  double (* get_preferred_bar_height) (ShellBarLevel *self);
  double (* get_preferred_bar_width)  (ShellBarLevel *self);

  /*< private >*/
  gpointer padding[6];
};

double shell_bar_level_get_value           (ShellBarLevel *self);
void   shell_bar_level_set_value           (ShellBarLevel *self,
                                            double         value);

double shell_bar_level_get_maximum_value   (ShellBarLevel *self);
void   shell_bar_level_set_maximum_value   (ShellBarLevel *self,
                                            double         maximum_value);

double shell_bar_level_get_overdrive_start (ShellBarLevel *self);
void   shell_bar_level_set_overdrive_start (ShellBarLevel *self,
                                            double         overdrive_start);

G_END_DECLS
