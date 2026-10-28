/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * screenshot-ui.h: The screenshot and screen recording UI, the
 * org.gnome.Shell.Screenshot D-Bus service and the helpers they share (the
 * flash, the area selector and the color picker). Port of js/ui/screenshot.js;
 * the type names ShellScreenshot (the backend that grabs the pixels) and
 * ShellScreenshotService are not those of the JS module, which called its
 * classes ScreenshotUI, ScreenshotService, SelectArea, PickPixel and Flashspot.
 *
 * What the JS reached through the `Main` module is reached like this:
 *
 *  - `Main.layoutManager`, `Main.sessionMode`, `Main.messageTray` and
 *    `Main.panel` are the properties of #ShellMain;
 *  - `Main.wm` (the key bindings of the UI, `Main.overview.hide ()`) is the
 *    #ShellScreenshotUI:window-manager property, which main.js sets, and the
 *    #ShellWindowManager::overview-hide-requested signal;
 *  - `Main.screenshotUI` (InteractiveScreenshot) is the
 *    #ShellScreenshotService:screenshot-ui property.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <mtk/mtk.h>

#include "shell-screenshot.h"
#include "st.h"
#include "ui/lightbox.h"
#include "ui/window-manager.h"

G_BEGIN_DECLS

/**
 * ShellScreenshotUIMode:
 * @SHELL_SCREENSHOT_UI_MODE_SCREENSHOT: open in screenshot mode
 * @SHELL_SCREENSHOT_UI_MODE_SCREENCAST: open in screen recording mode
 * @SHELL_SCREENSHOT_UI_MODE_SCREENSHOT_ONLY: open in screenshot mode, with the
 *   screen recording button disabled (the InteractiveScreenshot D-Bus call)
 *
 * How shell_screenshot_ui_open () opens the UI (`UIMode`).
 */
typedef enum
{
  SHELL_SCREENSHOT_UI_MODE_SCREENSHOT      = 0,
  SHELL_SCREENSHOT_UI_MODE_SCREENCAST      = 1,
  SHELL_SCREENSHOT_UI_MODE_SCREENSHOT_ONLY = 2,
} ShellScreenshotUIMode;

/**
 * ShellScreenshotKind:
 * @SHELL_SCREENSHOT_KIND_SCREEN: a screenshot of a whole monitor, or of the
 *   whole stage
 * @SHELL_SCREENSHOT_KIND_AREA: a screenshot of a part of the screen
 * @SHELL_SCREENSHOT_KIND_WINDOW: a screenshot of a window
 *
 * What a screenshot is a screenshot of, for the `screenshot.taken` hook
 * (the JS module did not tell).
 */
typedef enum
{
  SHELL_SCREENSHOT_KIND_SCREEN = 0,
  SHELL_SCREENSHOT_KIND_AREA   = 1,
  SHELL_SCREENSHOT_KIND_WINDOW = 2,
} ShellScreenshotKind;

/* ------------------------------------------------------------------------ */
/* captureScreenshot ()                                                      */
/* ------------------------------------------------------------------------ */

void   shell_screenshot_capture_screenshot_async  (CoglTexture         *texture,
                                                   const MtkRectangle  *geometry,
                                                   float                scale,
                                                   CoglTexture         *cursor_texture,
                                                   int                  cursor_x,
                                                   int                  cursor_y,
                                                   float                cursor_scale,
                                                   ShellScreenshotKind  kind,
                                                   GCancellable        *cancellable,
                                                   GAsyncReadyCallback  callback,
                                                   gpointer             user_data);
GFile *shell_screenshot_capture_screenshot_finish (GAsyncResult        *result,
                                                   GError             **error);

/* ------------------------------------------------------------------------ */
/* ShellFlashspot                                                            */
/* ------------------------------------------------------------------------ */

/**
 * ShellFlashspotDoneFunc:
 * @user_data: (closure): the data given to shell_flashspot_fire ()
 *
 * Called when the flash is over, right before the flashspot is destroyed.
 */
typedef void (* ShellFlashspotDoneFunc) (gpointer user_data);

#define SHELL_TYPE_FLASHSPOT (shell_flashspot_get_type ())
G_DECLARE_FINAL_TYPE (ShellFlashspot, shell_flashspot,
                      SHELL, FLASHSPOT, ShellLightbox)

ShellFlashspot *shell_flashspot_new  (const MtkRectangle     *area);
void            shell_flashspot_fire (ShellFlashspot         *self,
                                      ShellFlashspotDoneFunc  done,
                                      gpointer                user_data,
                                      GDestroyNotify          destroy);

/* ------------------------------------------------------------------------ */
/* ShellSelectArea                                                           */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SELECT_AREA (shell_select_area_get_type ())
G_DECLARE_FINAL_TYPE (ShellSelectArea, shell_select_area,
                      SHELL, SELECT_AREA, StWidget)

void          shell_select_area_select_async  (ShellSelectArea     *self,
                                               GCancellable        *cancellable,
                                               GAsyncReadyCallback  callback,
                                               gpointer             user_data);
MtkRectangle *shell_select_area_select_finish (ShellSelectArea     *self,
                                               GAsyncResult        *result,
                                               GError             **error);

/* ------------------------------------------------------------------------ */
/* ShellPickPixel                                                            */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_PICK_PIXEL (shell_pick_pixel_get_type ())
G_DECLARE_FINAL_TYPE (ShellPickPixel, shell_pick_pixel,
                      SHELL, PICK_PIXEL, StWidget)

void     shell_pick_pixel_pick_async  (ShellPickPixel       *self,
                                       GCancellable         *cancellable,
                                       GAsyncReadyCallback   callback,
                                       gpointer              user_data);
gboolean shell_pick_pixel_pick_finish (ShellPickPixel       *self,
                                       GAsyncResult         *result,
                                       CoglColor            *color,
                                       GError              **error);

/* ------------------------------------------------------------------------ */
/* ShellScreenshotUI                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SCREENSHOT_UI (shell_screenshot_ui_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenshotUI, shell_screenshot_ui,
                      SHELL, SCREENSHOT_UI, StWidget)

ShellScreenshotUI *shell_screenshot_ui_new     (ShellWindowManager    *window_manager);

void               shell_screenshot_ui_open    (ShellScreenshotUI     *self,
                                                ShellScreenshotUIMode  mode);
void               shell_screenshot_ui_close   (ShellScreenshotUI     *self,
                                                gboolean               instantly);
void               shell_screenshot_ui_stop_screencast (ShellScreenshotUI *self);
gboolean           shell_screenshot_ui_get_screencast_in_progress (ShellScreenshotUI *self);

/* ------------------------------------------------------------------------ */
/* ShellScreenshotService                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SCREENSHOT_SERVICE (shell_screenshot_service_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenshotService, shell_screenshot_service,
                      SHELL, SCREENSHOT_SERVICE, GObject)

ShellScreenshotService *shell_screenshot_service_new (ShellScreenshotUI *screenshot_ui);

G_END_DECLS
