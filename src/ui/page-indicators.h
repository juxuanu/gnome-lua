/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * page-indicators.h: A row (or column) of dots showing which page of a
 * paged view is visible. Port of js/ui/pageIndicators.js.
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

#define SHELL_TYPE_PAGE_INDICATORS (shell_page_indicators_get_type ())
G_DECLARE_FINAL_TYPE (ShellPageIndicators, shell_page_indicators,
                      SHELL, PAGE_INDICATORS, StBoxLayout)

ShellPageIndicators *shell_page_indicators_new                  (ClutterOrientation   orientation);

void                 shell_page_indicators_set_indicators_reactive (ShellPageIndicators *self,
                                                                    gboolean             reactive);

void                 shell_page_indicators_set_n_pages          (ShellPageIndicators *self,
                                                                 int                  n_pages);
int                  shell_page_indicators_get_n_pages          (ShellPageIndicators *self);

void                 shell_page_indicators_set_current_position (ShellPageIndicators *self,
                                                                 double               position);

G_END_DECLS
