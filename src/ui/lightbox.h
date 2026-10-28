/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * lightbox.h: A dark translucent "shade" covering the contents of a
 * container, with optional highlighted children. Port of js/ui/lightbox.js.
 *
 * Unlike most ports this type is derivable: screenshot-ui.c subclasses it
 * (ShellFlashspot).
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

#define SHELL_TYPE_LIGHTBOX (shell_lightbox_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellLightbox, shell_lightbox,
                          SHELL, LIGHTBOX, StBin)

struct _ShellLightboxClass
{
  StBinClass parent_class;

  /*< private >*/
  gpointer padding[6];
};

void     shell_lightbox_light_on   (ShellLightbox *self,
                                    uint32_t       fade_in_time);
void     shell_lightbox_light_off  (ShellLightbox *self,
                                    uint32_t       fade_out_time);

void     shell_lightbox_highlight  (ShellLightbox *self,
                                    ClutterActor  *window);

gboolean shell_lightbox_get_active (ShellLightbox *self);

G_END_DECLS
