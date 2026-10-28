/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * box-pointer.h: An actor which displays a triangle "arrow" pointing to a
 * given side, around a container. Port of js/ui/boxpointer.js.
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

/**
 * ShellPopupAnimation:
 * @SHELL_POPUP_ANIMATION_NONE: no animation, the transition is instantaneous
 * @SHELL_POPUP_ANIMATION_SLIDE: slide (and scale) the popup in or out
 * @SHELL_POPUP_ANIMATION_FADE: fade the popup in or out
 * @SHELL_POPUP_ANIMATION_FULL: all of the above
 *
 * Flags controlling how a #ShellBoxPointer is opened and closed. Same values
 * as the former `BoxPointer.PopupAnimation` of boxpointer.js.
 */
typedef enum
{
  SHELL_POPUP_ANIMATION_NONE  = 0,
  SHELL_POPUP_ANIMATION_SLIDE = 1 << 0,
  SHELL_POPUP_ANIMATION_FADE  = 1 << 1,
  SHELL_POPUP_ANIMATION_FULL  = ~0,
} ShellPopupAnimation;

/**
 * SHELL_POPUP_ANIMATION_TIME:
 *
 * Duration in milliseconds of the open and close animations.
 */
#define SHELL_POPUP_ANIMATION_TIME 150

#define SHELL_TYPE_BOX_POINTER (shell_box_pointer_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellBoxPointer, shell_box_pointer,
                          SHELL, BOX_POINTER, StWidget)

/**
 * ShellBoxPointerClass:
 * @parent_class: the parent class
 * @arrow_side_changed: class handler for the #ShellBoxPointer::arrow-side-changed
 *   signal
 */
struct _ShellBoxPointerClass
{
  StWidgetClass parent_class;

  void (* arrow_side_changed) (ShellBoxPointer *self);

  /*< private >*/
  gpointer padding[6];
};

/**
 * ShellBoxPointerDoneFunc:
 * @user_data: (closure): the user data given along with the callback
 *
 * Called when the open or close animation of a #ShellBoxPointer ran to
 * completion.
 */
typedef void (* ShellBoxPointerDoneFunc) (gpointer user_data);

ShellBoxPointer *shell_box_pointer_new               (StSide                   arrow_side);

StBin           *shell_box_pointer_get_bin           (ShellBoxPointer         *self);

StSide           shell_box_pointer_get_arrow_side    (ShellBoxPointer         *self);
void             shell_box_pointer_update_arrow_side (ShellBoxPointer         *self,
                                                      StSide                   side);

double           shell_box_pointer_get_arrow_alignment (ShellBoxPointer       *self);
void             shell_box_pointer_set_arrow_alignment (ShellBoxPointer       *self,
                                                        double                 alignment);

ClutterActor    *shell_box_pointer_get_arrow_actor   (ShellBoxPointer         *self);
void             shell_box_pointer_set_arrow_actor   (ShellBoxPointer         *self,
                                                      ClutterActor            *actor);

void             shell_box_pointer_set_arrow_origin  (ShellBoxPointer         *self,
                                                      double                   origin);

void             shell_box_pointer_set_position      (ShellBoxPointer         *self,
                                                      ClutterActor            *source_actor,
                                                      double                   alignment);
void             shell_box_pointer_set_source_alignment (ShellBoxPointer      *self,
                                                         double                alignment);

void             shell_box_pointer_open              (ShellBoxPointer         *self,
                                                      ShellPopupAnimation      animate,
                                                      ShellBoxPointerDoneFunc  on_complete,
                                                      gpointer                 user_data,
                                                      GDestroyNotify           destroy);
void             shell_box_pointer_close             (ShellBoxPointer         *self,
                                                      ShellPopupAnimation      animate,
                                                      ShellBoxPointerDoneFunc  on_complete,
                                                      gpointer                 user_data,
                                                      GDestroyNotify           destroy);

double           shell_box_pointer_get_padding       (ShellBoxPointer         *self,
                                                      StSide                   side);
double           shell_box_pointer_get_arrow_height  (ShellBoxPointer         *self);

G_END_DECLS
