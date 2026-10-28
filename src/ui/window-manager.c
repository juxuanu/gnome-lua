/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * window-manager.c: The shell's side of the window manager. Port of
 * js/ui/windowManager.js.
 *
 * #ShellWindowManager connects to the #ShellWM of #ShellGlobal (the effects
 * which mutter's plugin asks the shell to run), to #MetaDisplay and to the
 * stage, and owns the effect bookkeeping, the dynamic workspaces
 * (#ShellWorkspaceTracker), the tile preview, the resize popup, the window
 * menu manager and the #ShellWorkspaceAnimationController.
 *
 * What is still JS (the overview and the screenshot UI) is reached through the signals of
 * #ShellWindowManager, which main.js connects to; the state of the overview
 * is handed over with the #ShellWindowManager:overview-visible and
 * #ShellWindowManager:overview-closing properties.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The dimming effect, the tile preview, the resize popup, the display
 *    change dialog and the workspace tracker are the public types
 *    #ShellWindowDimmer, #ShellTilePreview, #ShellResizePopup,
 *    #ShellDisplayChangeDialog and #ShellWorkspaceTracker.
 *  - `_switchApp ()`, `_lookupIndex ()` and `_blockAnimations` of the JS were
 *    never used and are gone.
 *  - An animation which needs the monitor of a window that has none
 *    (`!monitor` in the minimize and unminimize handlers) completes the
 *    effect; the JS called `_minimizeWindowDone ()` and
 *    `_unminimizeWindowDone ()` without their arguments, which threw.
 *  - A map animation which was waiting for the overview to hide and whose
 *    effects were killed in the meantime (`kill-window-effects`) is not run
 *    when the overview is hidden; the JS ran it and ignored its end.
 *  - The same GSettings object is used for all the keybindings of the shell
 *    schema (the JS made one per keybinding).
 */

#include "config.h"

#include <math.h>
#include <string.h>

#include <glib/gi18n-lib.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/keybindings.h>
#include <meta/meta-backend.h>
#include <meta/meta-enum-types.h>
#include <meta/meta-later.h>
#include <meta/meta-monitor-manager.h>
#include <meta/meta-window-actor.h>
#include <meta/meta-workspace-manager.h>
#include <meta/prefs.h>

#include "misc/ibus-manager.h"
#include "shell-app.h"
#include "shell-edge-drag-gesture.h"
#include "shell-global.h"
#include "shell-util.h"
#include "shell-window-tracker.h"
#include "ui/alt-tab.h"
#include "ui/app-favorites.h"
#include "ui/close-dialog.h"
#include "ui/dialog.h"
#include "ui/environment.h"
#include "ui/inhibit-shortcuts-dialog.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/osd-window.h"
#include "ui/pad-osd.h"
#include "ui/panel.h"
#include "ui/switch-monitor.h"
#include "ui/window-manager.h"
#include "ui/window-menu.h"
#include "ui/workspace-animation.h"
#include "ui/workspace-switcher-popup.h"

#define SHELL_KEYBINDINGS_SCHEMA "org.gnome.shell.keybindings"

#define MINIMIZE_WINDOW_ANIMATION_TIME 400
#define MINIMIZE_WINDOW_ANIMATION_MODE CLUTTER_EASE_OUT_EXPO
#define SHOW_WINDOW_ANIMATION_TIME 150
#define DIALOG_SHOW_WINDOW_ANIMATION_TIME 100
#define DESTROY_WINDOW_ANIMATION_TIME 150
#define DIALOG_DESTROY_WINDOW_ANIMATION_TIME 100
#define WINDOW_ANIMATION_TIME 250
#define SCROLL_TIMEOUT_TIME 150
#define DIM_BRIGHTNESS (-0.3)
#define DIM_TIME 500
#define UNDIM_TIME 250

#define ONE_SECOND 1000 /* in ms */

#define MIN_NUM_WORKSPACES 2

#define WINDOW_DIMMER_EFFECT_NAME "gnome-shell-window-dimmer"

/*
 * When the last window closed on a workspace is a dialog or splash
 * screen, we assume that it might be an initial window shown before
 * the main window of an application, and give the app a grace period
 * where it can map another window before we remove the workspace.
 */
#define LAST_WINDOW_GRACE_TIME 1000

#define TYPE_BIT(type) (1u << (type))
#define TYPES_NORMAL_AND_DIALOGS (TYPE_BIT (META_WINDOW_NORMAL) | \
                                  TYPE_BIT (META_WINDOW_DIALOG) | \
                                  TYPE_BIT (META_WINDOW_MODAL_DIALOG))
#define TYPES_NORMAL (TYPE_BIT (META_WINDOW_NORMAL))

#define MODES_NORMAL_OVERVIEW (SHELL_ACTION_MODE_NORMAL | SHELL_ACTION_MODE_OVERVIEW)

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static gboolean
session_mode_get_flag (const char *property,
                       gboolean    fallback)
{
  ShellSessionMode *session_mode = shell_main_get_session_mode (shell_main_get_default ());
  gboolean value = FALSE;

  if (session_mode == NULL)
    return fallback;

  g_object_get (session_mode, property, &value, NULL);

  return value;
}

static gboolean
use_motion (void)
{
  return st_settings_get_reduced_motion (st_settings_get ()) != ST_REDUCED_MOTION_REDUCE;
}

static gboolean
is_rtl (void)
{
  return clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL;
}

static ShellLayoutManager *
get_layout_manager (void)
{
  return shell_main_get_layout_manager (shell_main_get_default ());
}

static void
init_ease_params (ShellEaseParams     *params,
                  guint                duration,
                  ClutterAnimationMode mode)
{
  params->duration = duration;
  params->delay = -1;
  params->mode = mode;
  params->repeat_count = 0;
  params->auto_reverse = FALSE;
  params->animation_required = FALSE;
}

/* ------------------------------------------------------------------------ */
/* ShellWindowDimmer                                                        */
/* ------------------------------------------------------------------------ */

struct _ShellWindowDimmer
{
  ClutterBrightnessContrastEffect parent;
};

G_DEFINE_FINAL_TYPE (ShellWindowDimmer, shell_window_dimmer,
                     CLUTTER_TYPE_BRIGHTNESS_CONTRAST_EFFECT)

static void
shell_window_dimmer_class_init (ShellWindowDimmerClass *klass)
{
}

static void
shell_window_dimmer_init (ShellWindowDimmer *self)
{
}

/**
 * shell_window_dimmer_new:
 *
 * Creates the effect which dims a window that has attached modal dialogs
 * (`new WindowDimmer ()`). It is disabled and named
 * `gnome-shell-window-dimmer`.
 *
 * Returns: (transfer full): a new #ShellWindowDimmer
 */
ShellWindowDimmer *
shell_window_dimmer_new (void)
{
  return g_object_new (SHELL_TYPE_WINDOW_DIMMER,
                       "name", WINDOW_DIMMER_EFFECT_NAME,
                       "enabled", FALSE,
                       NULL);
}

/**
 * shell_window_dimmer_get_for_actor:
 * @actor: the actor of a window
 *
 * Gets the dimming effect of @actor, adding it if the actor has none
 * (`getWindowDimmer ()`).
 *
 * Returns: (transfer none): the #ShellWindowDimmer of @actor
 */
ShellWindowDimmer *
shell_window_dimmer_get_for_actor (ClutterActor *actor)
{
  ClutterEffect *effect;

  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), NULL);

  effect = clutter_actor_get_effect (actor, WINDOW_DIMMER_EFFECT_NAME);
  if (effect == NULL)
    {
      effect = CLUTTER_EFFECT (shell_window_dimmer_new ());
      clutter_actor_add_effect (actor, effect);
    }

  return SHELL_WINDOW_DIMMER (effect);
}

static void
dimmer_sync_enabled (ShellWindowDimmer *self,
                     gboolean           dimmed)
{
  ClutterActor *actor = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self));
  gboolean animating;

  if (actor == NULL)
    return;

  animating = clutter_actor_get_transition (actor,
                                            "@effects." WINDOW_DIMMER_EFFECT_NAME ".brightness") != NULL;

  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self),
                                  meta_prefs_get_attach_modal_dialogs () &&
                                  (animating || dimmed));
}

typedef struct
{
  ShellWindowDimmer *dimmer;
  gboolean dimmed;
} DimmerData;

static void
dimmer_data_free (gpointer data)
{
  DimmerData *dimmer_data = data;

  g_object_unref (dimmer_data->dimmer);
  g_free (dimmer_data);
}

static void
dimmer_ease_stopped_cb (gboolean finished,
                        gpointer user_data)
{
  DimmerData *data = user_data;

  dimmer_sync_enabled (data->dimmer, data->dimmed);
}

/**
 * shell_window_dimmer_set_dimmed:
 * @self: a #ShellWindowDimmer
 * @dimmed: whether the window is dimmed
 * @animate: whether to animate the change
 *
 * Dims or undims the window (`setDimmed ()`).
 */
void
shell_window_dimmer_set_dimmed (ShellWindowDimmer *self,
                                gboolean           dimmed,
                                gboolean           animate)
{
  g_autoptr (GError) error = NULL;
  g_auto (GValue) value = G_VALUE_INIT;
  ClutterActor *actor;
  DimmerData *data;
  CoglColor color;
  ShellEaseParams params;
  guint8 val = 127 * (1 + (dimmed ? 1 : 0) * DIM_BRIGHTNESS);

  g_return_if_fail (SHELL_IS_WINDOW_DIMMER (self));

  actor = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self));
  if (actor == NULL)
    return;

  color = (CoglColor) COGL_COLOR_INIT (val, val, val, 255);
  g_value_init (&value, COGL_TYPE_COLOR);
  g_value_set_boxed (&value, &color);

  init_ease_params (&params, (dimmed ? DIM_TIME : UNDIM_TIME) * (animate ? 1 : 0),
                    CLUTTER_EASE_OUT_QUAD);

  data = g_new0 (DimmerData, 1);
  data->dimmer = g_object_ref (self);
  data->dimmed = dimmed;

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (actor),
                                       "@effects." WINDOW_DIMMER_EFFECT_NAME ".brightness",
                                       &value, &params,
                                       dimmer_ease_stopped_cb, data, dimmer_data_free,
                                       &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);

  dimmer_sync_enabled (self, dimmed);
}

/* ------------------------------------------------------------------------ */
/* ShellTilePreview                                                         */
/* ------------------------------------------------------------------------ */

struct _ShellTilePreview
{
  StWidget parent;

  ClutterActor *window_group;   /* weak */
  gboolean has_rect;
  MtkRectangle rect;
  int monitor_index;
  gboolean showing;
};

G_DEFINE_FINAL_TYPE (ShellTilePreview, shell_tile_preview, ST_TYPE_WIDGET)

static void
tile_preview_reset (ShellTilePreview *self)
{
  clutter_actor_hide (CLUTTER_ACTOR (self));
  self->has_rect = FALSE;
  self->monitor_index = -1;
}

static void
tile_preview_update_style (ShellTilePreview   *self,
                           const ShellMonitor *monitor)
{
  g_autoptr (GString) styles = g_string_new ("tile-preview");

  if (self->monitor_index == shell_layout_manager_get_primary_index (get_layout_manager ()))
    g_string_append (styles, " on-primary");
  if (self->rect.x == monitor->x)
    g_string_append (styles, " tile-preview-left");
  if (self->rect.x + self->rect.width == monitor->x + monitor->width)
    g_string_append (styles, " tile-preview-right");

  st_widget_set_style_class_name (ST_WIDGET (self), styles->str);
}

static void
shell_tile_preview_constructed (GObject *object)
{
  ShellTilePreview *self = SHELL_TILE_PREVIEW (object);
  ClutterActor *window_group = shell_global_get_window_group (shell_global_get ());

  G_OBJECT_CLASS (shell_tile_preview_parent_class)->constructed (object);

  g_set_weak_pointer (&self->window_group, window_group);
  clutter_actor_add_child (window_group, CLUTTER_ACTOR (self));

  tile_preview_reset (self);
  self->showing = FALSE;
}

static void
shell_tile_preview_dispose (GObject *object)
{
  ShellTilePreview *self = SHELL_TILE_PREVIEW (object);

  g_clear_weak_pointer (&self->window_group);

  G_OBJECT_CLASS (shell_tile_preview_parent_class)->dispose (object);
}

static void
shell_tile_preview_class_init (ShellTilePreviewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_tile_preview_constructed;
  object_class->dispose = shell_tile_preview_dispose;
}

static void
shell_tile_preview_init (ShellTilePreview *self)
{
  self->monitor_index = -1;
}

/**
 * shell_tile_preview_new:
 *
 * Creates the preview of the tiling of a window and adds it to the window
 * group, hidden (`new TilePreview ()`).
 *
 * Returns: (transfer full): a new #ShellTilePreview
 */
ShellTilePreview *
shell_tile_preview_new (void)
{
  return g_object_new (SHELL_TYPE_TILE_PREVIEW, NULL);
}

/**
 * shell_tile_preview_open:
 * @self: a #ShellTilePreview
 * @window: the window which is being tiled
 * @tile_rect: where the window will be
 * @monitor_index: the monitor of the tile
 *
 * Shows the preview of @tile_rect below the actor of @window, animating from
 * the frame of the window if it was not showing (`open ()`).
 */
void
shell_tile_preview_open (ShellTilePreview   *self,
                         MetaWindow         *window,
                         const MtkRectangle *tile_rect,
                         int                 monitor_index)
{
  ShellEaseParams params;
  ShellMonitor *monitor;
  GObject *window_actor;
  gboolean change_monitor;

  g_return_if_fail (SHELL_IS_TILE_PREVIEW (self));
  g_return_if_fail (META_IS_WINDOW (window));
  g_return_if_fail (tile_rect != NULL);

  window_actor = meta_window_get_compositor_private (window);
  if (window_actor == NULL || self->window_group == NULL)
    return;

  clutter_actor_set_child_below_sibling (self->window_group, CLUTTER_ACTOR (self),
                                         CLUTTER_ACTOR (window_actor));

  if (self->has_rect && mtk_rectangle_equal (&self->rect, tile_rect))
    return;

  change_monitor = self->monitor_index == -1 ||
                   self->monitor_index != monitor_index;

  self->monitor_index = monitor_index;
  self->rect = *tile_rect;
  self->has_rect = TRUE;

  monitor = shell_layout_manager_get_monitor (get_layout_manager (), monitor_index);
  if (monitor == NULL)
    return;

  tile_preview_update_style (self, monitor);

  if (!self->showing || change_monitor)
    {
      MtkRectangle monitor_rect = {
        .x = monitor->x,
        .y = monitor->y,
        .width = monitor->width,
        .height = monitor->height,
      };
      MtkRectangle frame, rect = { 0, };

      meta_window_get_frame_rect (window, &frame);
      mtk_rectangle_intersect (&frame, &monitor_rect, &rect);

      clutter_actor_set_size (CLUTTER_ACTOR (self), rect.width, rect.height);
      clutter_actor_set_position (CLUTTER_ACTOR (self), rect.x, rect.y);
      clutter_actor_set_opacity (CLUTTER_ACTOR (self), 0);
    }

  shell_monitor_free (monitor);

  self->showing = TRUE;
  clutter_actor_show (CLUTTER_ACTOR (self));

  init_ease_params (&params, WINDOW_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                    "x", (double) tile_rect->x,
                    "y", (double) tile_rect->y,
                    "width", (double) tile_rect->width,
                    "height", (double) tile_rect->height,
                    "opacity", 255,
                    NULL);
}

static void
tile_preview_close_done_cb (gboolean finished,
                            gpointer user_data)
{
  ShellTilePreview *self = user_data;

  if (finished)
    tile_preview_reset (self);
}

/**
 * shell_tile_preview_close:
 * @self: a #ShellTilePreview
 *
 * Fades the preview out and hides it (`close ()`).
 */
void
shell_tile_preview_close (ShellTilePreview *self)
{
  ShellEaseParams params;

  g_return_if_fail (SHELL_IS_TILE_PREVIEW (self));

  if (!self->showing)
    return;

  self->showing = FALSE;

  init_ease_params (&params, WINDOW_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (CLUTTER_ACTOR (self), &params,
                    tile_preview_close_done_cb, g_object_ref (self), g_object_unref,
                    "opacity", 0,
                    NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellResizePopup                                                         */
/* ------------------------------------------------------------------------ */

struct _ShellResizePopup
{
  StWidget parent;

  StLabel *label;               /* weak */
};

G_DEFINE_FINAL_TYPE (ShellResizePopup, shell_resize_popup, ST_TYPE_WIDGET)

static void
shell_resize_popup_constructed (GObject *object)
{
  ShellResizePopup *self = SHELL_RESIZE_POPUP (object);
  ShellLayoutManager *layout_manager = get_layout_manager ();
  StLabel *label;

  G_OBJECT_CLASS (shell_resize_popup_parent_class)->constructed (object);

  clutter_actor_set_layout_manager (CLUTTER_ACTOR (self), clutter_bin_layout_new ());

  label = g_object_new (ST_TYPE_LABEL,
                        "style-class", "resize-popup",
                        "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        "x-expand", TRUE,
                        "y-expand", TRUE,
                        NULL);
  g_set_weak_pointer (&self->label, label);
  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (label));

  if (layout_manager != NULL)
    clutter_actor_add_child (CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager)),
                             CLUTTER_ACTOR (self));
}

static void
shell_resize_popup_dispose (GObject *object)
{
  ShellResizePopup *self = SHELL_RESIZE_POPUP (object);

  g_clear_weak_pointer (&self->label);

  G_OBJECT_CLASS (shell_resize_popup_parent_class)->dispose (object);
}

static void
shell_resize_popup_class_init (ShellResizePopupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_resize_popup_constructed;
  object_class->dispose = shell_resize_popup_dispose;
}

static void
shell_resize_popup_init (ShellResizePopup *self)
{
}

/**
 * shell_resize_popup_new:
 *
 * Creates the popup with the size of a window being resized and adds it to
 * the ui group (`new ResizePopup ()`).
 *
 * Returns: (transfer full): a new #ShellResizePopup
 */
ShellResizePopup *
shell_resize_popup_new (void)
{
  return g_object_new (SHELL_TYPE_RESIZE_POPUP, NULL);
}

/**
 * shell_resize_popup_set:
 * @self: a #ShellResizePopup
 * @rect: where to put the popup
 * @display_w: the width to show
 * @display_h: the height to show
 *
 * Shows the size of the window in the popup (`set ()`).
 */
void
shell_resize_popup_set (ShellResizePopup   *self,
                        const MtkRectangle *rect,
                        int                 display_w,
                        int                 display_h)
{
  g_autofree char *text = NULL;

  g_return_if_fail (SHELL_IS_RESIZE_POPUP (self));
  g_return_if_fail (rect != NULL);

  /* Translators: This represents the size of a window. The first number is
   * the width of the window and the second is the height. */
  text = g_strdup_printf (_("%d × %d"), display_w, display_h);
  if (self->label != NULL)
    st_label_set_text (self->label, text);

  clutter_actor_set_position (CLUTTER_ACTOR (self), rect->x, rect->y);
  clutter_actor_set_size (CLUTTER_ACTOR (self), rect->width, rect->height);
}

/* ------------------------------------------------------------------------ */
/* ShellDisplayChangeDialog                                                 */
/* ------------------------------------------------------------------------ */

struct _ShellDisplayChangeDialog
{
  ShellModalDialog parent;

  ShellWM *shellwm;             /* weak */
  ShellMessageDialogContent *content; /* weak */
  int count_down;
  guint timeout_id;
};

G_DEFINE_FINAL_TYPE (ShellDisplayChangeDialog, shell_display_change_dialog,
                     SHELL_TYPE_MODAL_DIALOG)

static char *
display_change_format_count_down (ShellDisplayChangeDialog *self)
{
  return g_strdup_printf (ngettext ("Settings changes will revert in %d second",
                                    "Settings changes will revert in %d seconds",
                                    self->count_down),
                          self->count_down);
}

static gboolean
display_change_tick (gpointer user_data)
{
  ShellDisplayChangeDialog *self = user_data;
  g_autofree char *description = NULL;

  self->count_down--;

  if (self->count_down == 0)
    {
      /* mutter already takes care of failing at timeout */
      self->timeout_id = 0;
      shell_modal_dialog_close (SHELL_MODAL_DIALOG (self));
      return G_SOURCE_REMOVE;
    }

  description = display_change_format_count_down (self);
  if (self->content != NULL)
    shell_message_dialog_content_set_description (self->content, description);

  return G_SOURCE_CONTINUE;
}

static void
display_change_complete (ShellDisplayChangeDialog *self,
                         gboolean                  ok)
{
  if (self->shellwm != NULL)
    shell_wm_complete_display_change (self->shellwm, ok);

  shell_modal_dialog_close (SHELL_MODAL_DIALOG (self));
}

static void
display_change_on_failure (gpointer user_data)
{
  display_change_complete (user_data, FALSE);
}

static void
display_change_on_success (gpointer user_data)
{
  display_change_complete (user_data, TRUE);
}

static void
display_change_close (ShellModalDialog *dialog)
{
  ShellDisplayChangeDialog *self = SHELL_DISPLAY_CHANGE_DIALOG (dialog);

  g_clear_handle_id (&self->timeout_id, g_source_remove);

  SHELL_MODAL_DIALOG_CLASS (shell_display_change_dialog_parent_class)->close (dialog);
}

static void
shell_display_change_dialog_constructed (GObject *object)
{
  ShellDisplayChangeDialog *self = SHELL_DISPLAY_CHANGE_DIALOG (object);
  ShellModalDialog *dialog = SHELL_MODAL_DIALOG (self);
  ShellGlobal *global = shell_global_get ();
  MetaMonitorManager *monitor_manager;
  g_autofree char *description = NULL;
  ShellMessageDialogContent *content;

  G_OBJECT_CLASS (shell_display_change_dialog_parent_class)->constructed (object);

  g_set_weak_pointer (&self->shellwm, shell_global_get_window_manager (global));

  monitor_manager = meta_backend_get_monitor_manager (shell_global_get_backend (global));
  self->count_down = meta_monitor_manager_get_display_configuration_timeout (monitor_manager);

  description = display_change_format_count_down (self);

  /* Translators: This string should be shorter than 30 characters */
  content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT,
                          "title", _("Keep these display settings?"),
                          "description", description,
                          NULL);
  g_set_weak_pointer (&self->content, content);
  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (dialog)),
                           CLUTTER_ACTOR (content));

  /* Translators: this and the following message should be limited in length,
     to avoid ellipsizing the labels.
  */
  shell_modal_dialog_add_button (dialog, _("Revert Settings"),
                                 CLUTTER_KEY_Escape, FALSE,
                                 display_change_on_failure, self, NULL);
  shell_modal_dialog_add_button (dialog, _("Keep Changes"),
                                 0, TRUE,
                                 display_change_on_success, self, NULL);

  self->timeout_id = g_timeout_add (ONE_SECOND, display_change_tick, self);
  g_source_set_name_by_id (self->timeout_id, "[gnome-shell] display_change_tick");
}

static void
shell_display_change_dialog_dispose (GObject *object)
{
  ShellDisplayChangeDialog *self = SHELL_DISPLAY_CHANGE_DIALOG (object);

  g_clear_handle_id (&self->timeout_id, g_source_remove);
  g_clear_weak_pointer (&self->shellwm);
  g_clear_weak_pointer (&self->content);

  G_OBJECT_CLASS (shell_display_change_dialog_parent_class)->dispose (object);
}

static void
shell_display_change_dialog_class_init (ShellDisplayChangeDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellModalDialogClass *dialog_class = SHELL_MODAL_DIALOG_CLASS (klass);

  object_class->constructed = shell_display_change_dialog_constructed;
  object_class->dispose = shell_display_change_dialog_dispose;

  dialog_class->close = display_change_close;
}

static void
shell_display_change_dialog_init (ShellDisplayChangeDialog *self)
{
}

/**
 * shell_display_change_dialog_new:
 *
 * Creates the dialog which asks whether to keep the new display settings and
 * reverts them after the timeout of mutter (`new DisplayChangeDialog (wm)`).
 * Open it with shell_modal_dialog_open ().
 *
 * Returns: (transfer full): a new #ShellDisplayChangeDialog
 */
ShellDisplayChangeDialog *
shell_display_change_dialog_new (void)
{
  return g_object_new (SHELL_TYPE_DISPLAY_CHANGE_DIALOG, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellWorkspaceTracker                                                    */
/* ------------------------------------------------------------------------ */

/* What the JS kept in the `_lastRemovedWindow` and `_keepAliveId` expandos of
 * the workspaces */
typedef struct
{
  ShellWorkspaceTracker *tracker; /* not owned */
  MetaWindow *last_removed_window; /* strong */
  guint keep_alive_id;
} WorkspaceData;

struct _ShellWorkspaceTracker
{
  GObject parent;

  GPtrArray *workspaces;        /* of MetaWorkspace, strong */
  GHashTable *workspace_data;   /* MetaWorkspace -> WorkspaceData */

  guint check_workspaces_id;
  gboolean pause_workspace_check;
  gboolean shutting_down;

  GSettings *workspace_settings;

  /* Weak pointers to what is needed without shell_global_get () */
  MetaWorkspaceManager *workspace_manager;
  MetaLaters *laters;
  ShellGlobal *global;
};

G_DEFINE_FINAL_TYPE (ShellWorkspaceTracker, shell_workspace_tracker, G_TYPE_OBJECT)

static void workspace_tracker_queue_check_workspaces (ShellWorkspaceTracker *self);

static void
workspace_data_free (gpointer data)
{
  WorkspaceData *workspace_data = data;

  g_clear_handle_id (&workspace_data->keep_alive_id, g_source_remove);
  g_clear_object (&workspace_data->last_removed_window);
  g_free (workspace_data);
}

static WorkspaceData *
workspace_tracker_get_data (ShellWorkspaceTracker *self,
                            MetaWorkspace         *workspace)
{
  WorkspaceData *data = g_hash_table_lookup (self->workspace_data, workspace);

  if (data == NULL)
    {
      data = g_new0 (WorkspaceData, 1);
      data->tracker = self;
      g_hash_table_insert (self->workspace_data, workspace, data);
    }

  return data;
}

static void
set_empty_workspace (GArray   *empty_workspaces,
                     guint     index,
                     gboolean  value)
{
  /* The JS array grows when written past its end; the index never goes more
   * than one past the end */
  if (index < empty_workspaces->len)
    g_array_index (empty_workspaces, gboolean, index) = value;
  else if (index == empty_workspaces->len)
    g_array_append_val (empty_workspaces, value);
}

static gboolean
workspace_tracker_check_workspaces (gpointer user_data)
{
  ShellWorkspaceTracker *self = user_data;
  MetaWorkspaceManager *workspace_manager = self->workspace_manager;
  g_autoptr (GArray) empty_workspaces = g_array_new (FALSE, FALSE, sizeof (gboolean));
  GSList *sequences;
  GList *window_actors, *l;
  guint32 time;
  int last_index, last_empty_index, active_workspace_index;
  guint i;

  if (workspace_manager == NULL || self->global == NULL)
    {
      self->check_workspaces_id = 0;
      return G_SOURCE_REMOVE;
    }

  if (!meta_prefs_get_dynamic_workspaces ())
    {
      self->check_workspaces_id = 0;
      return G_SOURCE_REMOVE;
    }

  /* Update workspaces only if Dynamic Workspace Management has not been
   * paused by some other function */
  if (self->pause_workspace_check)
    return G_SOURCE_CONTINUE;

  for (i = 0; i < self->workspaces->len; i++)
    {
      MetaWorkspace *workspace = g_ptr_array_index (self->workspaces, i);
      WorkspaceData *data = g_hash_table_lookup (self->workspace_data, workspace);
      gboolean is_empty = TRUE;

      if (data != NULL)
        {
          MetaWindow *last_removed = data->last_removed_window;

          if ((last_removed != NULL &&
               (meta_window_get_window_type (last_removed) == META_WINDOW_SPLASHSCREEN ||
                meta_window_get_window_type (last_removed) == META_WINDOW_DIALOG ||
                meta_window_get_window_type (last_removed) == META_WINDOW_MODAL_DIALOG)) ||
              data->keep_alive_id != 0)
            is_empty = FALSE;
        }

      g_array_append_val (empty_workspaces, is_empty);
    }

  sequences = shell_window_tracker_get_startup_sequences (shell_window_tracker_get_default ());
  for (; sequences != NULL; sequences = sequences->next)
    {
      int index = meta_startup_sequence_get_workspace (sequences->data);

      if (index >= 0 && index <= meta_workspace_manager_get_n_workspaces (workspace_manager))
        set_empty_workspace (empty_workspaces, index, FALSE);
    }

  window_actors = shell_global_get_window_actors (self->global);
  for (l = window_actors; l != NULL; l = l->next)
    {
      MetaWindow *window = meta_window_actor_get_meta_window (l->data);
      MetaWorkspace *workspace;

      if (window == NULL || meta_window_is_on_all_workspaces (window))
        continue;

      workspace = meta_window_get_workspace (window);
      if (workspace == NULL)
        continue;

      set_empty_workspace (empty_workspaces, meta_workspace_index (workspace), FALSE);
    }
  g_list_free (window_actors);

  time = shell_global_get_current_time (self->global);

  /* If we don't have an empty workspace at the end, add one */
  if (empty_workspaces->len == 0 ||
      !g_array_index (empty_workspaces, gboolean, empty_workspaces->len - 1))
    {
      gboolean value = TRUE;

      meta_workspace_manager_append_new_workspace (workspace_manager, FALSE, time);
      g_array_append_val (empty_workspaces, value);
    }

  /* Enforce minimum number of workspaces */
  while (empty_workspaces->len < MIN_NUM_WORKSPACES)
    {
      gboolean value = TRUE;

      meta_workspace_manager_append_new_workspace (workspace_manager, FALSE, time);
      g_array_append_val (empty_workspaces, value);
    }

  last_index = empty_workspaces->len - 1;
  last_empty_index = -1;
  for (i = 0; i < empty_workspaces->len; i++)
    {
      if (!g_array_index (empty_workspaces, gboolean, i))
        last_empty_index = i;
    }
  last_empty_index += 1;

  active_workspace_index = meta_workspace_manager_get_active_workspace_index (workspace_manager);
  set_empty_workspace (empty_workspaces, active_workspace_index, FALSE);

  /* Delete empty workspaces except for the last one; do it from the end
   * to avoid index changes */
  for (i = last_index; (int) i >= 0; i--)
    {
      if (meta_workspace_manager_get_n_workspaces (workspace_manager) == MIN_NUM_WORKSPACES)
        break;

      if (i < empty_workspaces->len &&
          g_array_index (empty_workspaces, gboolean, i) &&
          (int) i != last_empty_index &&
          i < self->workspaces->len)
        {
          MetaWorkspace *workspace = g_object_ref (g_ptr_array_index (self->workspaces, i));

          meta_workspace_manager_remove_workspace (workspace_manager, workspace,
                                                   shell_global_get_current_time (self->global));
          g_object_unref (workspace);
        }
    }

  self->check_workspaces_id = 0;
  return G_SOURCE_REMOVE;
}

static void
workspace_tracker_queue_check_workspaces (ShellWorkspaceTracker *self)
{
  if (self->shutting_down || self->check_workspaces_id != 0 ||
      self->laters == NULL)
    return;

  self->check_workspaces_id = meta_laters_add (self->laters,
                                               META_LATER_BEFORE_REDRAW,
                                               workspace_tracker_check_workspaces,
                                               self, NULL);
}

static void
queue_check_window_added_cb (MetaWorkspace         *workspace,
                             MetaWindow            *window,
                             ShellWorkspaceTracker *self)
{
  workspace_tracker_queue_check_workspaces (self);
}

static void
queue_check_sequence_changed_cb (ShellWindowTracker    *window_tracker,
                                 MetaStartupSequence   *sequence,
                                 ShellWorkspaceTracker *self)
{
  workspace_tracker_queue_check_workspaces (self);
}

static void
queue_check_settings_cb (GSettings             *settings,
                         const char            *key,
                         ShellWorkspaceTracker *self)
{
  workspace_tracker_queue_check_workspaces (self);
}

static void
queue_check_switch_workspace_cb (ShellWM               *wm,
                                 int                    from,
                                 int                    to,
                                 int                    direction,
                                 ShellWorkspaceTracker *self)
{
  workspace_tracker_queue_check_workspaces (self);
}

typedef struct
{
  GWeakRef tracker;
  MetaWorkspace *workspace;     /* strong */
  MetaWindow *window;           /* strong */
} GraceData;

static void
grace_data_free (gpointer data)
{
  GraceData *grace = data;

  g_weak_ref_clear (&grace->tracker);
  g_clear_object (&grace->workspace);
  g_clear_object (&grace->window);
  g_free (grace);
}

static gboolean
workspace_tracker_grace_timeout_cb (gpointer user_data)
{
  GraceData *grace = user_data;
  g_autoptr (ShellWorkspaceTracker) self = g_weak_ref_get (&grace->tracker);
  WorkspaceData *data;

  if (self == NULL || self->shutting_down)
    return G_SOURCE_REMOVE;

  data = g_hash_table_lookup (self->workspace_data, grace->workspace);
  if (data != NULL && data->last_removed_window == grace->window)
    {
      g_clear_object (&data->last_removed_window);
      workspace_tracker_queue_check_workspaces (self);
    }

  return G_SOURCE_REMOVE;
}

static void
workspace_tracker_window_removed_cb (MetaWorkspace         *workspace,
                                     MetaWindow            *window,
                                     ShellWorkspaceTracker *self)
{
  WorkspaceData *data = workspace_tracker_get_data (self, workspace);
  GraceData *grace;
  guint id;

  g_set_object (&data->last_removed_window, window);
  workspace_tracker_queue_check_workspaces (self);

  grace = g_new0 (GraceData, 1);
  g_weak_ref_init (&grace->tracker, self);
  grace->workspace = g_object_ref (workspace);
  grace->window = g_object_ref (window);

  id = g_timeout_add_full (G_PRIORITY_DEFAULT, LAST_WINDOW_GRACE_TIME,
                           workspace_tracker_grace_timeout_cb,
                           grace, grace_data_free);
  g_source_set_name_by_id (id, "[gnome-shell] workspace_tracker_grace_timeout_cb");
}

static void
workspace_tracker_window_left_monitor_cb (MetaDisplay           *display,
                                          int                    monitor_index,
                                          MetaWindow            *window,
                                          ShellWorkspaceTracker *self)
{
  /* If the window left the primary monitor, that
   * might make that workspace empty */
  if (monitor_index == shell_layout_manager_get_primary_index (get_layout_manager ()))
    workspace_tracker_queue_check_workspaces (self);
}

static void
workspace_tracker_window_entered_monitor_cb (MetaDisplay           *display,
                                             int                    monitor_index,
                                             MetaWindow            *window,
                                             ShellWorkspaceTracker *self)
{
  /* If the window entered the primary monitor, that
   * might make that workspace non-empty */
  if (monitor_index == shell_layout_manager_get_primary_index (get_layout_manager ()))
    workspace_tracker_queue_check_workspaces (self);
}

static void
workspace_tracker_n_workspaces_changed (ShellWorkspaceTracker *self)
{
  MetaWorkspaceManager *workspace_manager = self->workspace_manager;
  guint old_num_workspaces = self->workspaces->len;
  guint new_num_workspaces = meta_workspace_manager_get_n_workspaces (workspace_manager);
  guint w;

  if (old_num_workspaces == new_num_workspaces)
    return;

  if (new_num_workspaces > old_num_workspaces)
    {
      /* Assume workspaces are only added at the end */
      for (w = old_num_workspaces; w < new_num_workspaces; w++)
        {
          MetaWorkspace *workspace =
            meta_workspace_manager_get_workspace_by_index (workspace_manager, w);

          g_ptr_array_add (self->workspaces, g_object_ref (workspace));

          g_signal_connect_object (workspace, "window-added",
                                   G_CALLBACK (queue_check_window_added_cb), self, 0);
          g_signal_connect_object (workspace, "window-removed",
                                   G_CALLBACK (workspace_tracker_window_removed_cb), self, 0);
        }
    }
  else
    {
      /* Assume workspaces are only removed sequentially
       * (e.g. 2,3,4 - not 2,4,7) */
      guint removed_index = new_num_workspaces;
      guint removed_num = old_num_workspaces - new_num_workspaces;

      for (w = 0; w < old_num_workspaces; w++)
        {
          MetaWorkspace *workspace =
            meta_workspace_manager_get_workspace_by_index (workspace_manager, w);

          if (g_ptr_array_index (self->workspaces, w) != workspace)
            {
              removed_index = w;
              break;
            }
        }

      for (w = 0; w < removed_num && removed_index < self->workspaces->len; w++)
        {
          MetaWorkspace *workspace = g_ptr_array_index (self->workspaces, removed_index);

          g_signal_handlers_disconnect_by_data (workspace, self);
          g_hash_table_remove (self->workspace_data, workspace);
          g_ptr_array_remove_index (self->workspaces, removed_index);
        }
    }

  workspace_tracker_queue_check_workspaces (self);
}

static void
workspace_tracker_n_workspaces_notify_cb (GObject               *object,
                                          GParamSpec            *pspec,
                                          ShellWorkspaceTracker *self)
{
  if (self->shutting_down)
    return;

  workspace_tracker_n_workspaces_changed (self);
}

static int
compare_workspaces (gconstpointer a,
                    gconstpointer b)
{
  MetaWorkspace *workspace_a = *(MetaWorkspace **) a;
  MetaWorkspace *workspace_b = *(MetaWorkspace **) b;

  return meta_workspace_index (workspace_a) - meta_workspace_index (workspace_b);
}

static void
workspace_tracker_workspaces_reordered_cb (MetaWorkspaceManager *workspace_manager,
                                           ShellWorkspaceTracker *self)
{
  g_ptr_array_sort (self->workspaces, compare_workspaces);
}

static void
workspace_tracker_on_shutdown (ShellGlobal           *global,
                               ShellWorkspaceTracker *self)
{
  self->shutting_down = TRUE;

  if (self->check_workspaces_id != 0 && self->laters != NULL)
    meta_laters_remove (self->laters, self->check_workspaces_id);
  self->check_workspaces_id = 0;
}

static void
shell_workspace_tracker_constructed (GObject *object)
{
  ShellWorkspaceTracker *self = SHELL_WORKSPACE_TRACKER (object);
  ShellGlobal *global = shell_global_get ();
  MetaDisplay *display = shell_global_get_display (global);

  G_OBJECT_CLASS (shell_workspace_tracker_parent_class)->constructed (object);

  g_set_weak_pointer (&self->global, global);
  g_set_weak_pointer (&self->workspace_manager, shell_global_get_workspace_manager (global));
  g_set_weak_pointer (&self->laters,
                      meta_compositor_get_laters (shell_global_get_compositor (global)));

  g_signal_connect_object (global, "shutdown",
                           G_CALLBACK (workspace_tracker_on_shutdown), self, 0);

  g_signal_connect_object (shell_window_tracker_get_default (), "startup-sequence-changed",
                           G_CALLBACK (queue_check_sequence_changed_cb), self, 0);

  g_signal_connect_object (self->workspace_manager, "notify::n-workspaces",
                           G_CALLBACK (workspace_tracker_n_workspaces_notify_cb), self, 0);
  g_signal_connect_object (self->workspace_manager, "workspaces-reordered",
                           G_CALLBACK (workspace_tracker_workspaces_reordered_cb), self, 0);
  g_signal_connect_object (shell_global_get_window_manager (global), "switch-workspace",
                           G_CALLBACK (queue_check_switch_workspace_cb), self, 0);

  g_signal_connect_object (display, "window-entered-monitor",
                           G_CALLBACK (workspace_tracker_window_entered_monitor_cb), self, 0);
  g_signal_connect_object (display, "window-left-monitor",
                           G_CALLBACK (workspace_tracker_window_left_monitor_cb), self, 0);

  self->workspace_settings = g_settings_new ("org.gnome.mutter");
  g_signal_connect_object (self->workspace_settings, "changed::dynamic-workspaces",
                           G_CALLBACK (queue_check_settings_cb), self, 0);

  workspace_tracker_n_workspaces_changed (self);
}

static void
shell_workspace_tracker_dispose (GObject *object)
{
  ShellWorkspaceTracker *self = SHELL_WORKSPACE_TRACKER (object);
  guint i;

  if (self->check_workspaces_id != 0 && self->laters != NULL)
    meta_laters_remove (self->laters, self->check_workspaces_id);
  self->check_workspaces_id = 0;

  if (self->workspaces != NULL)
    {
      for (i = 0; i < self->workspaces->len; i++)
        g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->workspaces, i), self);
    }

  g_clear_pointer (&self->workspaces, g_ptr_array_unref);
  g_clear_pointer (&self->workspace_data, g_hash_table_unref);
  g_clear_object (&self->workspace_settings);

  g_clear_weak_pointer (&self->workspace_manager);
  g_clear_weak_pointer (&self->laters);
  g_clear_weak_pointer (&self->global);

  G_OBJECT_CLASS (shell_workspace_tracker_parent_class)->dispose (object);
}

static void
shell_workspace_tracker_class_init (ShellWorkspaceTrackerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_workspace_tracker_constructed;
  object_class->dispose = shell_workspace_tracker_dispose;
}

static void
shell_workspace_tracker_init (ShellWorkspaceTracker *self)
{
  self->workspaces = g_ptr_array_new_with_free_func (g_object_unref);
  self->workspace_data = g_hash_table_new_full (NULL, NULL, NULL, workspace_data_free);
}

/**
 * shell_workspace_tracker_new:
 *
 * Creates the object which adds and removes workspaces to always have an
 * empty one at the end when the workspaces are dynamic
 * (`new WorkspaceTracker (wm)`).
 *
 * Returns: (transfer full): a new #ShellWorkspaceTracker
 */
ShellWorkspaceTracker *
shell_workspace_tracker_new (void)
{
  return g_object_new (SHELL_TYPE_WORKSPACE_TRACKER, NULL);
}

/**
 * shell_workspace_tracker_block_updates:
 * @self: a #ShellWorkspaceTracker
 *
 * Pauses the removal and addition of workspaces (`blockUpdates ()`).
 */
void
shell_workspace_tracker_block_updates (ShellWorkspaceTracker *self)
{
  g_return_if_fail (SHELL_IS_WORKSPACE_TRACKER (self));

  self->pause_workspace_check = TRUE;
}

/**
 * shell_workspace_tracker_unblock_updates:
 * @self: a #ShellWorkspaceTracker
 *
 * Resumes the removal and addition of workspaces (`unblockUpdates ()`).
 */
void
shell_workspace_tracker_unblock_updates (ShellWorkspaceTracker *self)
{
  g_return_if_fail (SHELL_IS_WORKSPACE_TRACKER (self));

  self->pause_workspace_check = FALSE;
}

static gboolean
keep_alive_done_cb (gpointer user_data)
{
  WorkspaceData *data = user_data;

  data->keep_alive_id = 0;
  workspace_tracker_queue_check_workspaces (data->tracker);

  return G_SOURCE_REMOVE;
}

/**
 * shell_workspace_tracker_keep_workspace_alive:
 * @self: a #ShellWorkspaceTracker
 * @workspace: the workspace which must not be removed
 * @duration: for how long, in milliseconds
 *
 * Keeps @workspace from being removed as an empty workspace for @duration
 * milliseconds (`keepWorkspaceAlive ()`). A later call replaces the earlier.
 */
void
shell_workspace_tracker_keep_workspace_alive (ShellWorkspaceTracker *self,
                                              MetaWorkspace         *workspace,
                                              guint                  duration)
{
  WorkspaceData *data;

  g_return_if_fail (SHELL_IS_WORKSPACE_TRACKER (self));
  g_return_if_fail (META_IS_WORKSPACE (workspace));

  data = workspace_tracker_get_data (self, workspace);

  g_clear_handle_id (&data->keep_alive_id, g_source_remove);

  data->keep_alive_id = g_timeout_add (duration, keep_alive_done_cb, data);
  g_source_set_name_by_id (data->keep_alive_id, "[gnome-shell] keep_alive_done_cb");
}

/* ------------------------------------------------------------------------ */
/* ShellWindowManager                                                       */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_OVERVIEW_VISIBLE,
  PROP_OVERVIEW_CLOSING,
  PROP_CTRL_ALT_TAB_MANAGER,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  OVERVIEW_HIDE_REQUESTED,
  SCREENSHOT_REQUESTED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellWindowManager
{
  GObject parent;

  /* Weak pointers to what the handlers need, so that none of them has to
   * ask shell_global_get (), which the teardown cannot rely on */
  ShellGlobal *global;
  ShellWM *shellwm;
  MetaDisplay *display;
  MetaWorkspaceManager *workspace_manager;
  ClutterStage *stage;

  GHashTable *minimizing;       /* sets of MetaWindowActor, strong */
  GHashTable *unminimizing;
  GHashTable *mapping;
  GHashTable *resizing;
  GHashTable *resize_pending;
  GHashTable *destroying;
  GHashTable *skipped_actors;

  GHashTable *allowed_keybindings; /* name -> ShellActionMode */
  GSettings *keybindings_settings;

  gboolean is_workspace_prepended;
  gboolean can_scroll;          /* limiting scrolling speed */
  guint can_scroll_id;
  guint block_count;

  gboolean switch_in_progress;

  gboolean overview_visible;
  gboolean overview_closing;
  GPtrArray *pending_maps;      /* of PendingMap */

  ShellCtrlAltTabManager *ctrl_alt_tab_manager;

  ShellWorkspaceSwitcherPopup *workspace_switcher_popup; /* weak */
  ShellTilePreview *tile_preview;
  ShellResizePopup *resize_popup;
  ShellWindowMenuManager *window_menu_manager;
  ShellWorkspaceTracker *workspace_tracker;
  ShellWorkspaceAnimationController *workspace_animation;
  ShellEdgeDragGesture *top_drag_gesture;

  /* The master adjustment of the workspaces, and the ones handed out */
  StAdjustment *workspaces_adjustment;
  GPtrArray *derived_adjustments; /* of weak StAdjustment pointers */
};

G_DEFINE_FINAL_TYPE (ShellWindowManager, shell_window_manager, G_TYPE_OBJECT)

typedef struct
{
  ShellWindowManager *wm;
  MetaWindowActor *actor;
} EffectData;

static EffectData *
effect_data_new (ShellWindowManager *self,
                 MetaWindowActor    *actor)
{
  EffectData *data = g_new0 (EffectData, 1);

  data->wm = g_object_ref (self);
  data->actor = g_object_ref (actor);

  return data;
}

static void
effect_data_free (gpointer user_data)
{
  EffectData *data = user_data;

  g_object_unref (data->wm);
  g_object_unref (data->actor);
  g_free (data);
}

static GQuark
window_type_quark (void)
{
  return g_quark_from_static_string ("shell-window-manager-window-type");
}

static GQuark
dimmed_quark (void)
{
  return g_quark_from_static_string ("shell-window-manager-dimmed");
}

static GQuark
animation_info_quark (void)
{
  return g_quark_from_static_string ("shell-window-manager-animation-info");
}

static GQuark
parent_link_quark (void)
{
  return g_quark_from_static_string ("shell-window-manager-parent-link");
}

/* ---- the animation window type, the animation allowance ----------------- */

static MetaWindowType
get_animation_window_type (MetaWindowActor *actor)
{
  MetaWindow *window = meta_window_actor_get_meta_window (actor);
  MetaWindowType window_type = meta_window_get_window_type (window);

  if (window_type != META_WINDOW_NORMAL ||
      meta_window_get_client_type (window) == META_WINDOW_CLIENT_TYPE_X11)
    return window_type;

  /* wayland doesn't use the DIALOG type, but for
   * animations we want transients to behave like ones */
  return meta_window_get_transient_for (window) != NULL
    ? META_WINDOW_DIALOG
    : window_type;
}

static gboolean
should_animate (ShellWindowManager *self)
{
  gboolean overview_open = self->overview_visible && !self->overview_closing;

  return !(overview_open ||
           (self->workspace_animation != NULL &&
            shell_workspace_animation_controller_get_gesture_active (self->workspace_animation)));
}

static gboolean
should_animate_actor (ShellWindowManager *self,
                      MetaWindowActor    *actor,
                      guint               types)
{
  if (g_hash_table_remove (self->skipped_actors, actor))
    return FALSE;

  if (!should_animate (self))
    return FALSE;

  if (meta_window_actor_get_texture (actor) == NULL)
    return FALSE;

  return (types & TYPE_BIT (get_animation_window_type (actor))) != 0;
}

/* ---- minimize ----------------------------------------------------------- */

static void
minimize_window_done (ShellWindowManager *self,
                      MetaWindowActor    *actor)
{
  ClutterActor *clutter_actor = CLUTTER_ACTOR (actor);
  g_autoptr (MetaWindowActor) actor_ref = g_object_ref (actor);

  if (g_hash_table_remove (self->minimizing, actor))
    {
      clutter_actor_remove_all_transitions (clutter_actor);
      clutter_actor_set_scale (clutter_actor, 1.0, 1.0);
      clutter_actor_set_opacity (clutter_actor, 255);
      clutter_actor_set_pivot_point (clutter_actor, 0, 0);

      if (self->shellwm != NULL)
        shell_wm_completed_minimize (self->shellwm, actor);
    }
}

static void
minimize_ease_done_cb (gboolean finished,
                       gpointer user_data)
{
  EffectData *data = user_data;

  minimize_window_done (data->wm, data->actor);
}

static void
on_minimize (ShellWM             *shellwm,
             MetaWindowActor     *actor,
             ShellWindowManager  *self)
{
  ClutterActor *clutter_actor = CLUTTER_ACTOR (actor);
  MetaWindow *window = meta_window_actor_get_meta_window (actor);
  ShellEaseParams params;

  if (!should_animate_actor (self, actor, TYPES_NORMAL_AND_DIALOGS))
    {
      shell_wm_completed_minimize (shellwm, actor);
      return;
    }

  clutter_actor_set_scale (clutter_actor, 1.0, 1.0);

  g_hash_table_add (self->minimizing, g_object_ref (actor));

  init_ease_params (&params, MINIMIZE_WINDOW_ANIMATION_TIME,
                    MINIMIZE_WINDOW_ANIMATION_MODE);

  if (meta_window_is_monitor_sized (window) || !use_motion ())
    {
      shell_actor_ease (clutter_actor, &params,
                        minimize_ease_done_cb, effect_data_new (self, actor), effect_data_free,
                        "opacity", 0,
                        NULL);
    }
  else
    {
      double x_dest, y_dest, x_scale, y_scale;
      MtkRectangle geom;

      if (meta_window_get_icon_geometry (window, &geom))
        {
          x_dest = geom.x;
          y_dest = geom.y;
          x_scale = geom.width / clutter_actor_get_width (clutter_actor);
          y_scale = geom.height / clutter_actor_get_height (clutter_actor);
        }
      else
        {
          ShellMonitor *monitor =
            shell_layout_manager_get_monitor (get_layout_manager (),
                                              meta_window_get_monitor (window));

          if (monitor == NULL)
            {
              minimize_window_done (self, actor);
              return;
            }

          x_dest = monitor->x;
          y_dest = monitor->y;
          if (is_rtl ())
            x_dest += monitor->width;
          x_scale = 0;
          y_scale = 0;

          shell_monitor_free (monitor);
        }

      shell_actor_ease (clutter_actor, &params,
                        minimize_ease_done_cb, effect_data_new (self, actor), effect_data_free,
                        "opacity", 0,
                        "scale-x", x_scale,
                        "scale-y", y_scale,
                        "x", x_dest,
                        "y", y_dest,
                        NULL);
    }
}

/* ---- unminimize --------------------------------------------------------- */

static void
unminimize_window_done (ShellWindowManager *self,
                        MetaWindowActor    *actor)
{
  ClutterActor *clutter_actor = CLUTTER_ACTOR (actor);
  g_autoptr (MetaWindowActor) actor_ref = g_object_ref (actor);

  if (g_hash_table_remove (self->unminimizing, actor))
    {
      clutter_actor_remove_all_transitions (clutter_actor);
      clutter_actor_set_scale (clutter_actor, 1.0, 1.0);
      clutter_actor_set_opacity (clutter_actor, 255);
      clutter_actor_set_pivot_point (clutter_actor, 0, 0);

      if (self->shellwm != NULL)
        shell_wm_completed_unminimize (self->shellwm, actor);
    }
}

static void
unminimize_ease_done_cb (gboolean finished,
                         gpointer user_data)
{
  EffectData *data = user_data;

  unminimize_window_done (data->wm, data->actor);
}

static void
on_unminimize (ShellWM            *shellwm,
               MetaWindowActor    *actor,
               ShellWindowManager *self)
{
  ClutterActor *clutter_actor = CLUTTER_ACTOR (actor);
  MetaWindow *window = meta_window_actor_get_meta_window (actor);
  ShellEaseParams params;

  if (!should_animate_actor (self, actor, TYPES_NORMAL_AND_DIALOGS))
    {
      shell_wm_completed_unminimize (shellwm, actor);
      return;
    }

  g_hash_table_add (self->unminimizing, g_object_ref (actor));

  init_ease_params (&params, MINIMIZE_WINDOW_ANIMATION_TIME,
                    MINIMIZE_WINDOW_ANIMATION_MODE);

  if (meta_window_is_monitor_sized (window) || !use_motion ())
    {
      clutter_actor_set_opacity (clutter_actor, 0);
      clutter_actor_set_scale (clutter_actor, 1.0, 1.0);
      shell_actor_ease (clutter_actor, &params,
                        unminimize_ease_done_cb, effect_data_new (self, actor), effect_data_free,
                        "opacity", 255,
                        NULL);
    }
  else
    {
      MtkRectangle geom, rect;

      if (meta_window_get_icon_geometry (window, &geom))
        {
          clutter_actor_set_position (clutter_actor, geom.x, geom.y);
          clutter_actor_set_scale (clutter_actor,
                                   geom.width / clutter_actor_get_width (clutter_actor),
                                   geom.height / clutter_actor_get_height (clutter_actor));
        }
      else
        {
          ShellMonitor *monitor =
            shell_layout_manager_get_monitor (get_layout_manager (),
                                              meta_window_get_monitor (window));

          if (monitor == NULL)
            {
              clutter_actor_show (clutter_actor);
              unminimize_window_done (self, actor);
              return;
            }

          clutter_actor_set_position (clutter_actor, monitor->x, monitor->y);
          if (is_rtl ())
            clutter_actor_set_x (clutter_actor,
                                 clutter_actor_get_x (clutter_actor) + monitor->width);
          clutter_actor_set_scale (clutter_actor, 0, 0);

          shell_monitor_free (monitor);
        }

      meta_window_get_buffer_rect (window, &rect);

      clutter_actor_show (clutter_actor);
      shell_actor_ease (clutter_actor, &params,
                        unminimize_ease_done_cb, effect_data_new (self, actor), effect_data_free,
                        "scale-x", 1.0,
                        "scale-y", 1.0,
                        "x", (double) rect.x,
                        "y", (double) rect.y,
                        NULL);
    }
}

/* ---- size change -------------------------------------------------------- */

typedef struct
{
  ClutterActor *clone;          /* strong */
  MtkRectangle old_rect;
  gboolean frozen;
} AnimationInfo;

static void
animation_info_free (gpointer data)
{
  AnimationInfo *info = data;

  g_clear_object (&info->clone);
  g_free (info);
}

static AnimationInfo *
get_animation_info (MetaWindowActor *actor)
{
  return g_object_get_qdata (G_OBJECT (actor), animation_info_quark ());
}

/* Returns whether there was animation info */
static gboolean
clear_animation_info (MetaWindowActor *actor)
{
  AnimationInfo *info = get_animation_info (actor);

  if (info == NULL)
    return FALSE;

  if (info->clone != NULL)
    {
      /* the 'destroy' handler of the actor is bound to the clone */
      g_signal_handlers_disconnect_by_data (actor, info->clone);
      clutter_actor_destroy (info->clone);
    }

  if (info->frozen)
    meta_window_actor_thaw (actor);

  g_object_set_qdata (G_OBJECT (actor), animation_info_quark (), NULL);

  return TRUE;
}

static void
animation_actor_destroyed_cb (ClutterActor *actor,
                              gpointer      clone)
{
  clear_animation_info (META_WINDOW_ACTOR (actor));
}

static void
size_change_window_done (ShellWindowManager *self,
                         MetaWindowActor    *actor)
{
  ClutterActor *clutter_actor = CLUTTER_ACTOR (actor);
  g_autoptr (MetaWindowActor) actor_ref = g_object_ref (actor);

  if (g_hash_table_remove (self->resizing, actor))
    {
      clutter_actor_remove_all_transitions (clutter_actor);
      clutter_actor_set_scale (clutter_actor, 1.0, 1.0);
      clutter_actor_set_translation (clutter_actor, 0, 0, 0);
      clear_animation_info (actor);
      if (self->shellwm != NULL)
        shell_wm_completed_size_change (self->shellwm, actor);
    }

  if (g_hash_table_remove (self->resize_pending, actor))
    {
      clear_animation_info (actor);
      if (self->shellwm != NULL)
        shell_wm_completed_size_change (self->shellwm, actor);
    }
}

static void
size_change_ease_done_cb (gboolean finished,
                          gpointer user_data)
{
  EffectData *data = user_data;

  size_change_window_done (data->wm, data->actor);
}

static void
prepare_animation_info (ShellWindowManager *self,
                        ShellWM            *shellwm,
                        MetaWindowActor    *actor,
                        MtkRectangle       *old_frame_rect)
{
  g_autoptr (GError) error = NULL;
  ClutterContent *actor_content;
  ClutterActor *actor_clone;
  AnimationInfo *info;

  /* Position a clone of the window on top of the old position,
   * while actor updates are frozen. */
  actor_content = meta_window_actor_paint_to_content (actor, old_frame_rect, &error);
  if (actor_content == NULL)
    {
      g_warning ("Failed to paint the old window contents: %s", error->message);
      shell_wm_completed_size_change (shellwm, actor);
      return;
    }

  actor_clone = g_object_new (ST_TYPE_WIDGET, "content", actor_content, NULL);
  g_object_ref_sink (actor_clone);
  g_object_unref (actor_content);
  clutter_actor_set_offscreen_redirect (actor_clone, CLUTTER_OFFSCREEN_REDIRECT_ALWAYS);
  clutter_actor_set_position (actor_clone, old_frame_rect->x, old_frame_rect->y);
  clutter_actor_set_size (actor_clone, old_frame_rect->width, old_frame_rect->height);

  meta_window_actor_freeze (actor);

  if (clear_animation_info (actor))
    {
      g_message ("Old animationInfo removed from actor %s",
                 G_OBJECT_TYPE_NAME (actor));
      shell_wm_completed_size_change (self->shellwm, actor);
    }

  g_signal_connect (actor, "destroy", G_CALLBACK (animation_actor_destroyed_cb), actor_clone);

  g_hash_table_add (self->resize_pending, g_object_ref (actor));

  info = g_new0 (AnimationInfo, 1);
  info->clone = actor_clone;
  info->old_rect = *old_frame_rect;
  info->frozen = TRUE;
  g_object_set_qdata_full (G_OBJECT (actor), animation_info_quark (),
                           info, animation_info_free);
}

static void
on_size_change (ShellWM            *shellwm,
                MetaWindowActor    *actor,
                MetaSizeChange      which_change,
                MtkRectangle       *old_frame_rect,
                MtkRectangle       *old_buffer_rect,
                ShellWindowManager *self)
{
  gboolean animate =
    should_animate_actor (self, actor, TYPES_NORMAL) &&
    old_frame_rect->width > 0 &&
    old_frame_rect->height > 0;

  if (animate)
    prepare_animation_info (self, shellwm, actor, old_frame_rect);
  else
    shell_wm_completed_size_change (shellwm, actor);
}

static void
on_size_changed (ShellWM            *shellwm,
                 MetaWindowActor    *actor,
                 ShellWindowManager *self)
{
  ClutterActor *clutter_actor = CLUTTER_ACTOR (actor);
  MetaWindow *window = meta_window_actor_get_meta_window (actor);
  AnimationInfo *info = get_animation_info (actor);
  ClutterActor *actor_clone;
  MtkRectangle source_rect, target_rect;
  ShellEaseParams params;
  double scale_x, scale_y;
  ClutterActor *ui_group;

  if (info == NULL)
    return;
  if (g_hash_table_contains (self->resizing, actor))
    return;

  actor_clone = info->clone;
  source_rect = info->old_rect;
  if (use_motion ())
    meta_window_get_frame_rect (window, &target_rect);
  else
    target_rect = source_rect;

  scale_x = (double) target_rect.width / source_rect.width;
  scale_y = (double) target_rect.height / source_rect.height;

  g_hash_table_remove (self->resize_pending, actor);
  g_hash_table_add (self->resizing, g_object_ref (actor));

  ui_group = CLUTTER_ACTOR (shell_layout_manager_get_ui_group (get_layout_manager ()));
  clutter_actor_add_child (ui_group, actor_clone);

  /* Now scale and fade out the clone */
  init_ease_params (&params, WINDOW_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (actor_clone, &params, NULL, NULL, NULL,
                    "x", (double) target_rect.x,
                    "y", (double) target_rect.y,
                    "scale-x", scale_x,
                    "scale-y", scale_y,
                    "opacity", 0,
                    NULL);

  clutter_actor_set_translation (clutter_actor,
                                 -target_rect.x + source_rect.x,
                                 -target_rect.y + source_rect.y,
                                 0);

  /* Now set scale the actor to size it as the clone. */
  clutter_actor_set_scale (clutter_actor, 1 / scale_x, 1 / scale_y);

  /* Scale it to its actual new size */
  shell_actor_ease (clutter_actor, &params,
                    size_change_ease_done_cb, effect_data_new (self, actor), effect_data_free,
                    "scale-x", 1.0,
                    "scale-y", 1.0,
                    "translation-x", 0.0,
                    "translation-y", 0.0,
                    NULL);

  /* ease didn't animate and cleared the info, we are done */
  info = get_animation_info (actor);
  if (info == NULL)
    return;

  /* Now unfreeze actor updates, to get it to the new size.
   * It's important that we don't wait until the animation is completed to
   * do this, otherwise our scale will be applied to the old texture size. */
  meta_window_actor_thaw (actor);
  info->frozen = FALSE;
}

/* ---- dimming ------------------------------------------------------------ */

static void
dim_window (ShellWindowManager *self,
            MetaWindow         *window,
            gboolean            dimmed)
{
  GObject *actor = meta_window_get_compositor_private (window);
  ShellWindowDimmer *dimmer;

  if (actor == NULL)
    return;

  dimmer = shell_window_dimmer_get_for_actor (CLUTTER_ACTOR (actor));
  if (dimmer == NULL)
    return;

  shell_window_dimmer_set_dimmed (dimmer, dimmed, should_animate (self));
}

static void
check_dimming (ShellWindowManager *self,
               MetaWindow         *window)
{
  gboolean should_dim;
  gboolean is_dimmed;

  if (window == NULL)
    return;

  should_dim = meta_window_has_attached_dialogs (window);
  is_dimmed = GPOINTER_TO_INT (g_object_get_qdata (G_OBJECT (window), dimmed_quark ()));

  if (should_dim && !is_dimmed)
    {
      g_object_set_qdata (G_OBJECT (window), dimmed_quark (), GINT_TO_POINTER (TRUE));
      dim_window (self, window, TRUE);
    }
  else if (!should_dim && is_dimmed)
    {
      g_object_set_qdata (G_OBJECT (window), dimmed_quark (), GINT_TO_POINTER (FALSE));
      dim_window (self, window, FALSE);
    }
}

/* ---- map ---------------------------------------------------------------- */

static void
map_window_done (ShellWindowManager *self,
                 MetaWindowActor    *actor)
{
  ClutterActor *clutter_actor = CLUTTER_ACTOR (actor);
  g_autoptr (MetaWindowActor) actor_ref = g_object_ref (actor);

  if (g_hash_table_remove (self->mapping, actor))
    {
      clutter_actor_remove_all_transitions (clutter_actor);
      clutter_actor_set_opacity (clutter_actor, 255);
      clutter_actor_set_pivot_point (clutter_actor, 0, 0);
      clutter_actor_set_scale (clutter_actor, 1.0, 1.0);
      clutter_actor_set_translation (clutter_actor, 0, 0, 0);
      if (self->shellwm != NULL)
        shell_wm_completed_map (self->shellwm, actor);
    }
}

static void
map_ease_done_cb (gboolean finished,
                  gpointer user_data)
{
  EffectData *data = user_data;

  map_window_done (data->wm, data->actor);
}

typedef struct
{
  ShellWindowManager *wm;       /* strong */
  MetaWindowActor *actor;       /* strong */
  MetaWindowType type;
} PendingMap;

static void
pending_map_free (gpointer data)
{
  PendingMap *pending = data;

  g_object_unref (pending->wm);
  g_object_unref (pending->actor);
  g_free (pending);
}

/* What follows `await this._waitForOverviewToHide ()` */
static void
map_window_animate (ShellWindowManager *self,
                    MetaWindowActor    *actor,
                    MetaWindowType      type)
{
  ClutterActor *clutter_actor = CLUTTER_ACTOR (actor);
  ShellEaseParams params;

  /* The effects were killed while waiting */
  if (!g_hash_table_contains (self->mapping, actor))
    return;

  switch (type)
    {
    case META_WINDOW_NORMAL:
      init_ease_params (&params, SHOW_WINDOW_ANIMATION_TIME, CLUTTER_EASE_OUT_EXPO);
      break;

    default:
      init_ease_params (&params, DIALOG_SHOW_WINDOW_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
      break;
    }

  shell_actor_ease (clutter_actor, &params,
                    map_ease_done_cb, effect_data_new (self, actor), effect_data_free,
                    "opacity", 255,
                    "scale-x", 1.0,
                    "scale-y", 1.0,
                    NULL);
}

static void
wait_for_overview_to_hide (ShellWindowManager *self,
                           MetaWindowActor    *actor,
                           MetaWindowType      type)
{
  PendingMap *pending;

  if (!self->overview_visible)
    {
      map_window_animate (self, actor, type);
      return;
    }

  pending = g_new0 (PendingMap, 1);
  pending->wm = g_object_ref (self);
  pending->actor = g_object_ref (actor);
  pending->type = type;
  g_ptr_array_add (self->pending_maps, pending);
}

static void
flush_pending_maps (ShellWindowManager *self)
{
  g_autoptr (GPtrArray) pending_maps = NULL;
  guint i;

  if (self->pending_maps->len == 0)
    return;

  pending_maps = self->pending_maps;
  self->pending_maps = g_ptr_array_new_with_free_func (pending_map_free);

  /* Free the array only after the last animation started, the entries keep
   * the window manager alive */
  g_ptr_array_set_free_func (pending_maps, pending_map_free);
  for (i = 0; i < pending_maps->len; i++)
    {
      PendingMap *pending = g_ptr_array_index (pending_maps, i);

      map_window_animate (pending->wm, pending->actor, pending->type);
    }
}

static void
on_window_type_changed (MetaWindow         *window,
                        GParamSpec         *pspec,
                        ShellWindowManager *self)
{
  GObject *actor = meta_window_get_compositor_private (window);
  MetaWindowType type = meta_window_get_window_type (window);
  MetaWindowType previous;

  if (actor == NULL)
    return;

  previous = GPOINTER_TO_INT (g_object_get_qdata (actor, window_type_quark ())) - 1;
  if (type == previous)
    return;

  if (type == META_WINDOW_MODAL_DIALOG || previous == META_WINDOW_MODAL_DIALOG)
    check_dimming (self, meta_window_get_transient_for (window));

  g_object_set_qdata (actor, window_type_quark (), GINT_TO_POINTER (type + 1));
}

static void
on_window_unmanaged (MetaWindow         *window,
                     ShellWindowManager *self)
{
  check_dimming (self, meta_window_get_transient_for (window));
}

static void
on_map (ShellWM            *shellwm,
        MetaWindowActor    *actor,
        ShellWindowManager *self)
{
  ClutterActor *clutter_actor = CLUTTER_ACTOR (actor);
  MetaWindow *window = meta_window_actor_get_meta_window (actor);
  MetaWindowType type;

  g_object_set_qdata (G_OBJECT (actor), window_type_quark (),
                      GINT_TO_POINTER (meta_window_get_window_type (window) + 1));

  g_signal_handlers_disconnect_by_func (window, on_window_type_changed, self);
  g_signal_connect (window, "notify::window-type",
                    G_CALLBACK (on_window_type_changed), self);
  g_signal_handlers_disconnect_by_func (window, on_window_unmanaged, self);
  g_signal_connect (window, "unmanaged",
                    G_CALLBACK (on_window_unmanaged), self);

  if (meta_window_is_attached_dialog (window))
    check_dimming (self, meta_window_get_transient_for (window));

  if (!should_animate_actor (self, actor, TYPES_NORMAL_AND_DIALOGS))
    {
      shell_wm_completed_map (shellwm, actor);
      return;
    }

  type = get_animation_window_type (actor);
  switch (type)
    {
    case META_WINDOW_NORMAL:
      clutter_actor_set_pivot_point (clutter_actor, 0.5, 1.0);
      if (use_motion ())
        clutter_actor_set_scale (clutter_actor, 0.01, 0.05);
      clutter_actor_set_opacity (clutter_actor, 0);
      clutter_actor_show (clutter_actor);
      g_hash_table_add (self->mapping, g_object_ref (actor));

      wait_for_overview_to_hide (self, actor, type);
      break;

    case META_WINDOW_MODAL_DIALOG:
    case META_WINDOW_DIALOG:
      clutter_actor_set_pivot_point (clutter_actor, 0.5, 0.5);
      if (use_motion ())
        {
          double scale_x;

          clutter_actor_get_scale (clutter_actor, &scale_x, NULL);
          clutter_actor_set_scale (clutter_actor, scale_x, 0);
          clutter_actor_set_opacity (clutter_actor, 0);
        }
      clutter_actor_show (clutter_actor);
      g_hash_table_add (self->mapping, g_object_ref (actor));

      wait_for_overview_to_hide (self, actor, type);
      break;

    default:
      shell_wm_completed_map (shellwm, actor);
      break;
    }
}

/* ---- destroy ------------------------------------------------------------ */

typedef struct
{
  MetaWindow *parent;           /* strong */
  gulong unmanaged_id;
} ParentLink;

static void
parent_link_free (gpointer data)
{
  ParentLink *link = data;

  g_clear_signal_handler (&link->unmanaged_id, link->parent);
  g_clear_object (&link->parent);
  g_free (link);
}

static void
destroy_window_done (ShellWindowManager *self,
                     MetaWindowActor    *actor)
{
  g_autoptr (MetaWindowActor) actor_ref = g_object_ref (actor);

  if (g_hash_table_remove (self->destroying, actor))
    {
      g_object_set_qdata (G_OBJECT (actor), parent_link_quark (), NULL);

      if (self->shellwm != NULL)
        shell_wm_completed_destroy (self->shellwm, actor);
    }
}

static void
destroy_ease_done_cb (gboolean finished,
                      gpointer user_data)
{
  EffectData *data = user_data;

  destroy_window_done (data->wm, data->actor);
}

static void
parent_unmanaged_cb (MetaWindow *parent,
                     gpointer    user_data)
{
  EffectData *data = user_data;
  g_autoptr (ShellWindowManager) self = g_object_ref (data->wm);
  g_autoptr (MetaWindowActor) actor = g_object_ref (data->actor);

  clutter_actor_remove_all_transitions (CLUTTER_ACTOR (actor));
  destroy_window_done (self, actor);
}

static void
on_destroy (ShellWM            *shellwm,
            MetaWindowActor    *actor,
            ShellWindowManager *self)
{
  ClutterActor *clutter_actor = CLUTTER_ACTOR (actor);
  MetaWindow *window = meta_window_actor_get_meta_window (actor);
  ShellEaseParams params;
  MetaWindowType type;

  g_signal_handlers_disconnect_by_func (window, on_window_type_changed, self);

  if (meta_window_is_attached_dialog (window))
    check_dimming (self, meta_window_get_transient_for (window));

  if (!should_animate_actor (self, actor, TYPES_NORMAL_AND_DIALOGS))
    {
      shell_wm_completed_destroy (shellwm, actor);
      return;
    }

  type = get_animation_window_type (actor);
  switch (type)
    {
    case META_WINDOW_NORMAL:
      clutter_actor_set_pivot_point (clutter_actor, 0.5, 0.5);
      g_hash_table_add (self->destroying, g_object_ref (actor));

      init_ease_params (&params, DESTROY_WINDOW_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
      shell_actor_ease (clutter_actor, &params,
                        destroy_ease_done_cb, effect_data_new (self, actor), effect_data_free,
                        "opacity", 0,
                        "scale-x", use_motion () ? 0.8 : 1.0,
                        "scale-y", use_motion () ? 0.8 : 1.0,
                        NULL);
      break;

    case META_WINDOW_MODAL_DIALOG:
    case META_WINDOW_DIALOG:
      clutter_actor_set_pivot_point (clutter_actor, 0.5, 0.5);
      g_hash_table_add (self->destroying, g_object_ref (actor));

      if (meta_window_is_attached_dialog (window))
        {
          MetaWindow *parent = meta_window_get_transient_for (window);

          if (parent != NULL)
            {
              ParentLink *link = g_new0 (ParentLink, 1);

              link->parent = g_object_ref (parent);
              link->unmanaged_id = g_signal_connect_data (parent, "unmanaged",
                                                          G_CALLBACK (parent_unmanaged_cb),
                                                          effect_data_new (self, actor),
                                                          (GClosureNotify) effect_data_free,
                                                          0);
              g_object_set_qdata_full (G_OBJECT (actor), parent_link_quark (),
                                       link, parent_link_free);
            }
        }

      init_ease_params (&params, DIALOG_DESTROY_WINDOW_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
      if (use_motion ())
        {
          shell_actor_ease (clutter_actor, &params,
                            destroy_ease_done_cb, effect_data_new (self, actor), effect_data_free,
                            "scale-y", 0.0,
                            NULL);
        }
      else
        {
          shell_actor_ease (clutter_actor, &params,
                            destroy_ease_done_cb, effect_data_new (self, actor), effect_data_free,
                            "opacity", 0,
                            NULL);
        }
      break;

    default:
      shell_wm_completed_destroy (shellwm, actor);
      break;
    }
}

static void
on_kill_window_effects (ShellWM            *shellwm,
                        MetaWindowActor    *actor,
                        ShellWindowManager *self)
{
  minimize_window_done (self, actor);
  map_window_done (self, actor);
  destroy_window_done (self, actor);
  size_change_window_done (self, actor);
}

/* ---- key bindings ------------------------------------------------------- */

static gboolean
on_filter_keybinding (ShellWM            *shellwm,
                      MetaKeyBinding     *binding,
                      ShellWindowManager *self)
{
  ShellActionMode action_mode = shell_main_get_action_mode (shell_main_get_default ());
  ShellActionMode allowed;

  if (action_mode == SHELL_ACTION_MODE_NONE)
    return TRUE;

  /* There's little sense in implementing a keybinding in mutter and
   * not having it work in NORMAL mode; handle this case generically
   * so we don't have to explicitly allow all builtin keybindings in
   * NORMAL mode. */
  if (action_mode == SHELL_ACTION_MODE_NORMAL &&
      meta_key_binding_is_builtin (binding))
    return FALSE;

  allowed = GPOINTER_TO_UINT (g_hash_table_lookup (self->allowed_keybindings,
                                                   meta_key_binding_get_name (binding)));

  return !(allowed & action_mode);
}

/**
 * shell_window_manager_allow_keybinding:
 * @self: a #ShellWindowManager
 * @name: the name of the key binding
 * @modes: the #ShellActionMode flags in which it may run
 *
 * Sets in which action modes the key binding @name is not filtered out
 * (`allowKeybinding ()`). Builtin key bindings always run in the normal
 * mode.
 */
void
shell_window_manager_allow_keybinding (ShellWindowManager *self,
                                       const char         *name,
                                       ShellActionMode     modes)
{
  g_return_if_fail (SHELL_IS_WINDOW_MANAGER (self));
  g_return_if_fail (name != NULL);

  g_hash_table_insert (self->allowed_keybindings, g_strdup (name),
                       GUINT_TO_POINTER (modes));
}

/**
 * shell_window_manager_set_custom_keybinding_handler:
 * @self: a #ShellWindowManager
 * @name: the name of the key binding, one of mutter's own
 * @modes: the #ShellActionMode flags in which it may run
 * @handler: (nullable) (scope notified) (closure user_data) (destroy destroy):
 *   the function that runs when the key binding is pressed, or %NULL to give
 *   the key binding back to mutter
 * @user_data: data for @handler
 * @destroy: called on @user_data when the handler is replaced
 *
 * Replaces the action of a key binding of mutter by @handler
 * (`setCustomKeybindingHandler ()`).
 *
 * Returns: whether the key binding was found
 */
gboolean
shell_window_manager_set_custom_keybinding_handler (ShellWindowManager *self,
                                                    const char         *name,
                                                    ShellActionMode     modes,
                                                    MetaKeyHandlerFunc  handler,
                                                    gpointer            user_data,
                                                    GDestroyNotify      destroy)
{
  g_return_val_if_fail (SHELL_IS_WINDOW_MANAGER (self), FALSE);
  g_return_val_if_fail (name != NULL, FALSE);

  if (meta_keybindings_set_custom_handler (name, handler, user_data, destroy))
    {
      shell_window_manager_allow_keybinding (self, name, modes);
      return TRUE;
    }

  return FALSE;
}

/**
 * shell_window_manager_add_keybinding:
 * @self: a #ShellWindowManager
 * @name: the name of the key in @settings that holds the accelerators
 * @settings: the #GSettings with the key
 * @flags: the #MetaKeyBindingFlags
 * @modes: the #ShellActionMode flags in which it may run
 * @handler: (scope notified) (closure user_data) (destroy destroy): the
 *   function that runs when the key binding is pressed
 * @user_data: data for @handler
 * @destroy: called on @user_data when the key binding is removed
 *
 * Adds a key binding (`addKeybinding ()`).
 *
 * Returns: the action id of the key binding (a #MetaKeyBindingAction), 0 if
 *   it could not be added
 */
guint
shell_window_manager_add_keybinding (ShellWindowManager *self,
                                     const char         *name,
                                     GSettings          *settings,
                                     MetaKeyBindingFlags flags,
                                     ShellActionMode     modes,
                                     MetaKeyHandlerFunc  handler,
                                     gpointer            user_data,
                                     GDestroyNotify      destroy)
{
  guint action;

  g_return_val_if_fail (SHELL_IS_WINDOW_MANAGER (self), 0);
  g_return_val_if_fail (name != NULL, 0);
  g_return_val_if_fail (G_IS_SETTINGS (settings), 0);

  if (self->display == NULL)
    return 0;

  action = meta_display_add_keybinding (self->display, name, settings, flags,
                                        handler, user_data, destroy);
  if (action != META_KEYBINDING_ACTION_NONE)
    shell_window_manager_allow_keybinding (self, name, modes);

  return action;
}

/**
 * shell_window_manager_remove_keybinding:
 * @self: a #ShellWindowManager
 * @name: the name of the key binding
 *
 * Removes a key binding added with shell_window_manager_add_keybinding ()
 * (`removeKeybinding ()`).
 */
void
shell_window_manager_remove_keybinding (ShellWindowManager *self,
                                        const char         *name)
{
  g_return_if_fail (SHELL_IS_WINDOW_MANAGER (self));
  g_return_if_fail (name != NULL);

  if (self->display != NULL && meta_display_remove_keybinding (self->display, name))
    shell_window_manager_allow_keybinding (self, name, SHELL_ACTION_MODE_NONE);
}

/* ---- workspaces --------------------------------------------------------- */

static void
on_switch_workspace_done (gpointer user_data)
{
  ShellWindowManager *self = user_data;

  if (self->shellwm != NULL)
    shell_wm_completed_switch_workspace (self->shellwm);
  self->switch_in_progress = FALSE;
}

static void
on_switch_workspace (ShellWM            *shellwm,
                     int                 from,
                     int                 to,
                     int                 direction,
                     ShellWindowManager *self)
{
  if (!session_mode_get_flag ("has-workspaces", FALSE) || !should_animate (self) ||
      self->workspace_animation == NULL)
    {
      shell_wm_completed_switch_workspace (shellwm);
      return;
    }

  self->switch_in_progress = TRUE;

  shell_workspace_animation_controller_animate_switch (self->workspace_animation,
                                                       from, to, direction,
                                                       on_switch_workspace_done,
                                                       g_object_ref (self),
                                                       g_object_unref);
}

static void
switch_workspace_done (ShellWindowManager *self)
{
  if (!self->switch_in_progress)
    return;

  if (self->shellwm != NULL)
    shell_wm_completed_switch_workspace (self->shellwm);
  self->switch_in_progress = FALSE;
}

static void
on_kill_switch_workspace (ShellWM            *shellwm,
                          ShellWindowManager *self)
{
  if (self->workspace_animation != NULL)
    shell_workspace_animation_controller_cancel_switch_animation (self->workspace_animation);
  switch_workspace_done (self);
}

/**
 * shell_window_manager_insert_workspace:
 * @self: a #ShellWindowManager
 * @pos: where the new workspace goes
 *
 * Inserts an empty workspace at @pos by moving the windows of the workspaces
 * from @pos on to the next workspace (`insertWorkspace ()`). Does nothing
 * when the workspaces are not dynamic.
 */
void
shell_window_manager_insert_workspace (ShellWindowManager *self,
                                       int                 pos)
{
  g_autoptr (GList) window_actors = NULL;
  guint32 time;
  int active_index;
  GList *l;

  g_return_if_fail (SHELL_IS_WINDOW_MANAGER (self));

  if (self->global == NULL || self->workspace_manager == NULL)
    return;

  if (!meta_prefs_get_dynamic_workspaces ())
    return;

  time = shell_global_get_current_time (self->global);
  meta_workspace_manager_append_new_workspace (self->workspace_manager, FALSE, time);

  window_actors = shell_global_get_window_actors (self->global);

  /* To create a new workspace, we slide all the windows on workspaces
   * below us to the next workspace, leaving a blank workspace for us
   * to recycle. */
  for (l = window_actors; l != NULL; l = l->next)
    {
      MetaWindow *window = meta_window_actor_get_meta_window (l->data);
      MetaWorkspace *workspace;
      int index;

      if (window == NULL)
        continue;

      /* If the window is attached to an ancestor, we don't need/want
       * to move it */
      if (meta_window_get_transient_for (window) != NULL)
        continue;
      /* Same for OR windows */
      if (meta_window_is_override_redirect (window))
        continue;
      /* Sticky windows don't need moving, in fact moving would
       * unstick them */
      if (meta_window_is_on_all_workspaces (window))
        continue;
      /* Windows on workspaces below pos don't need moving */
      workspace = meta_window_get_workspace (window);
      if (workspace == NULL)
        continue;
      index = meta_workspace_index (workspace);
      if (index < pos)
        continue;
      meta_window_change_workspace_by_index (window, index + 1, TRUE);
    }

  /* If the new workspace was inserted before the active workspace,
   * activate the workspace to which its windows went */
  active_index = meta_workspace_manager_get_active_workspace_index (self->workspace_manager);
  if (active_index >= pos)
    {
      MetaWorkspace *new_ws =
        meta_workspace_manager_get_workspace_by_index (self->workspace_manager,
                                                       active_index + 1);

      if (new_ws != NULL)
        meta_workspace_activate (new_ws, shell_global_get_current_time (self->global));
    }
}

/**
 * shell_window_manager_keep_workspace_alive:
 * @self: a #ShellWindowManager
 * @workspace: the workspace
 * @duration: for how long, in milliseconds
 *
 * Keeps @workspace from being removed as an empty dynamic workspace for
 * @duration milliseconds (`keepWorkspaceAlive ()`).
 */
void
shell_window_manager_keep_workspace_alive (ShellWindowManager *self,
                                           MetaWorkspace      *workspace,
                                           guint               duration)
{
  g_return_if_fail (SHELL_IS_WINDOW_MANAGER (self));
  g_return_if_fail (META_IS_WORKSPACE (workspace));

  if (self->workspace_tracker == NULL)
    return;

  shell_workspace_tracker_keep_workspace_alive (self->workspace_tracker, workspace, duration);
}

/**
 * shell_window_manager_block_workspace_updates:
 * @self: a #ShellWindowManager
 *
 * Pauses the dynamic workspaces until the matching
 * shell_window_manager_unblock_workspace_updates () (`blockWorkspaceUpdates ()`).
 * Calls nest.
 */
void
shell_window_manager_block_workspace_updates (ShellWindowManager *self)
{
  g_return_if_fail (SHELL_IS_WINDOW_MANAGER (self));

  if (++self->block_count > 1)
    return;

  if (self->workspace_tracker == NULL)
    return;

  shell_workspace_tracker_block_updates (self->workspace_tracker);
}

/**
 * shell_window_manager_unblock_workspace_updates:
 * @self: a #ShellWindowManager
 *
 * Reverses shell_window_manager_block_workspace_updates ()
 * (`unblockWorkspaceUpdates ()`).
 */
void
shell_window_manager_unblock_workspace_updates (ShellWindowManager *self)
{
  g_return_if_fail (SHELL_IS_WINDOW_MANAGER (self));

  if (self->block_count == 0)
    {
      g_critical ("Unmatched call to unblockWorkspaceUpdates()");
      return;
    }

  self->block_count -= 1;

  if (self->block_count != 0)
    return;

  if (self->workspace_tracker == NULL)
    return;

  shell_workspace_tracker_unblock_updates (self->workspace_tracker);
}

/**
 * shell_window_manager_skip_next_effect:
 * @self: a #ShellWindowManager
 * @actor: the actor of a window
 *
 * Makes the next effect of @actor (map, destroy, minimize, unminimize or
 * size change) run without animation (`skipNextEffect ()`).
 */
void
shell_window_manager_skip_next_effect (ShellWindowManager *self,
                                       MetaWindowActor    *actor)
{
  g_return_if_fail (SHELL_IS_WINDOW_MANAGER (self));
  g_return_if_fail (META_IS_WINDOW_ACTOR (actor));

  g_hash_table_add (self->skipped_actors, g_object_ref (actor));
}

/**
 * shell_window_manager_action_move_workspace:
 * @self: a #ShellWindowManager
 * @workspace: the workspace to switch to
 *
 * Activates @workspace unless it is the active one or the session has no
 * workspaces (`actionMoveWorkspace ()`).
 */
void
shell_window_manager_action_move_workspace (ShellWindowManager *self,
                                            MetaWorkspace      *workspace)
{
  g_return_if_fail (SHELL_IS_WINDOW_MANAGER (self));
  g_return_if_fail (META_IS_WORKSPACE (workspace));

  if (!session_mode_get_flag ("has-workspaces", FALSE))
    return;

  if (self->workspace_manager != NULL &&
      meta_workspace_manager_get_active_workspace (self->workspace_manager) != workspace)
    meta_workspace_activate (workspace, shell_global_get_current_time (self->global));
}

/**
 * shell_window_manager_action_move_window:
 * @self: a #ShellWindowManager
 * @window: the window to move
 * @workspace: the workspace to move it to
 *
 * Moves @window to @workspace and switches to it, with the window on top of
 * the animation (`actionMoveWindow ()`).
 */
void
shell_window_manager_action_move_window (ShellWindowManager *self,
                                         MetaWindow         *window,
                                         MetaWorkspace      *workspace)
{
  g_return_if_fail (SHELL_IS_WINDOW_MANAGER (self));
  g_return_if_fail (META_IS_WINDOW (window));
  g_return_if_fail (META_IS_WORKSPACE (workspace));

  if (!session_mode_get_flag ("has-workspaces", FALSE))
    return;

  if (self->workspace_manager != NULL &&
      meta_workspace_manager_get_active_workspace (self->workspace_manager) != workspace)
    {
      /* This won't have any effect for "always sticky" windows
       * (like desktop windows or docks) */

      if (self->workspace_animation != NULL)
        shell_workspace_animation_controller_set_moving_window (self->workspace_animation,
                                                                window);
      meta_window_change_workspace (window, workspace);

      meta_display_clear_mouse_mode (self->display);
      meta_workspace_activate_with_focus (workspace, window,
                                          shell_global_get_current_time (self->global));
    }
}

static MetaWorkspace *
get_neighbor (MetaWorkspace *workspace,
              MetaMotionDirection direction)
{
  return meta_workspace_get_neighbor (workspace, direction);
}

static gboolean
can_scroll_timeout_cb (gpointer user_data)
{
  ShellWindowManager *self = user_data;

  self->can_scroll_id = 0;
  self->can_scroll = TRUE;

  return G_SOURCE_REMOVE;
}

/**
 * shell_window_manager_handle_workspace_scroll:
 * @self: a #ShellWindowManager
 * @event: a scroll event
 *
 * Switches to the neighbor workspace in the direction of the scroll @event,
 * at most once every 150 ms (`handleWorkspaceScroll ()`).
 *
 * Returns: %CLUTTER_EVENT_STOP if the event was used, %CLUTTER_EVENT_PROPAGATE
 *   otherwise
 */
gboolean
shell_window_manager_handle_workspace_scroll (ShellWindowManager *self,
                                              const ClutterEvent *event)
{
  ClutterScrollDirection direction;
  MetaWorkspace *active_ws, *ws;
  gboolean vertical;
  gboolean rtl = is_rtl ();

  g_return_val_if_fail (SHELL_IS_WINDOW_MANAGER (self), CLUTTER_EVENT_PROPAGATE);
  g_return_val_if_fail (event != NULL, CLUTTER_EVENT_PROPAGATE);

  if (!self->can_scroll)
    return CLUTTER_EVENT_PROPAGATE;

  if (clutter_event_type (event) != CLUTTER_SCROLL)
    return CLUTTER_EVENT_PROPAGATE;

  direction = clutter_event_get_scroll_direction (event);
  if (direction == CLUTTER_SCROLL_SMOOTH)
    return CLUTTER_EVENT_PROPAGATE;

  if (self->workspace_manager == NULL)
    return CLUTTER_EVENT_PROPAGATE;

  vertical = meta_workspace_manager_get_layout_rows (self->workspace_manager) == -1;
  active_ws = meta_workspace_manager_get_active_workspace (self->workspace_manager);

  switch (direction)
    {
    case CLUTTER_SCROLL_UP:
      if (vertical)
        ws = get_neighbor (active_ws, META_MOTION_UP);
      else if (rtl)
        ws = get_neighbor (active_ws, META_MOTION_RIGHT);
      else
        ws = get_neighbor (active_ws, META_MOTION_LEFT);
      break;

    case CLUTTER_SCROLL_DOWN:
      if (vertical)
        ws = get_neighbor (active_ws, META_MOTION_DOWN);
      else if (rtl)
        ws = get_neighbor (active_ws, META_MOTION_LEFT);
      else
        ws = get_neighbor (active_ws, META_MOTION_RIGHT);
      break;

    case CLUTTER_SCROLL_LEFT:
      ws = get_neighbor (active_ws, META_MOTION_LEFT);
      break;

    case CLUTTER_SCROLL_RIGHT:
      ws = get_neighbor (active_ws, META_MOTION_RIGHT);
      break;

    default:
      return CLUTTER_EVENT_PROPAGATE;
    }

  if (ws != NULL)
    shell_window_manager_action_move_workspace (self, ws);

  self->can_scroll = FALSE;
  g_clear_handle_id (&self->can_scroll_id, g_source_remove);
  self->can_scroll_id = g_timeout_add (SCROLL_TIMEOUT_TIME, can_scroll_timeout_cb, self);
  g_source_set_name_by_id (self->can_scroll_id, "[gnome-shell] can_scroll_timeout_cb");

  return CLUTTER_EVENT_STOP;
}

static gboolean
on_stage_scroll_event (ClutterActor       *stage,
                       const ClutterEvent *event,
                       ShellWindowManager *self)
{
  ClutterModifierType compositor_modifiers;

  if ((SHELL_ACTION_MODE_NORMAL & shell_main_get_action_mode (shell_main_get_default ())) == 0)
    return CLUTTER_EVENT_PROPAGATE;

  if (self->workspace_animation != NULL &&
      shell_workspace_animation_controller_can_handle_scroll_event (self->workspace_animation,
                                                                    event))
    return CLUTTER_EVENT_PROPAGATE;

  compositor_modifiers = meta_display_get_compositor_modifiers (self->display);
  if ((clutter_event_get_state (event) & compositor_modifiers) != compositor_modifiers)
    return CLUTTER_EVENT_PROPAGATE;

  return shell_window_manager_handle_workspace_scroll (self, event);
}

/* ---- workspaces adjustment (Main.createWorkspacesAdjustment ()) ---------- */

static const struct
{
  const char *name;
  GBindingFlags flags;
} adjustment_bindings[] = {
  { "lower", G_BINDING_SYNC_CREATE },
  { "page-increment", G_BINDING_SYNC_CREATE },
  { "page-size", G_BINDING_SYNC_CREATE },
  { "step-increment", G_BINDING_SYNC_CREATE },
  { "upper", G_BINDING_SYNC_CREATE },
  { "value", G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL },
};

static void
derived_adjustment_finalized_cb (gpointer  data,
                                 GObject  *where_the_object_was)
{
  ShellWindowManager *self = data;

  if (self->derived_adjustments != NULL)
    g_ptr_array_remove (self->derived_adjustments, where_the_object_was);
}

static void
workspaces_adjustment_upper_changed_cb (StAdjustment       *adjustment,
                                        GParamSpec         *pspec,
                                        ShellWindowManager *self)
{
  guint i;

  if (self->workspace_manager == NULL)
    return;

  /* A workspace might have been inserted or removed before the active
   * one, causing the adjustment to go out of sync, so update the value */
  for (i = 0; i < self->derived_adjustments->len; i++)
    st_adjustment_remove_transition (g_ptr_array_index (self->derived_adjustments, i),
                                     "value");
  st_adjustment_remove_transition (adjustment, "value");
  st_adjustment_set_value (adjustment,
                           meta_workspace_manager_get_active_workspace_index (self->workspace_manager));
}

/**
 * shell_window_manager_create_workspaces_adjustment:
 * @self: a #ShellWindowManager
 * @actor: the actor the adjustment is animated through
 *
 * Creates an adjustment that has its lower, upper, and value properties set
 * for the number of available workspaces. Consumers of the returned
 * adjustment must only change the 'value' property, and only that
 * (`Main.createWorkspacesAdjustment ()`).
 *
 * Returns: (transfer full): an adjustment representing the current
 *   workspaces layout
 */
StAdjustment *
shell_window_manager_create_workspaces_adjustment (ShellWindowManager *self,
                                                   ClutterActor       *actor)
{
  StAdjustment *adjustment;
  guint i;

  g_return_val_if_fail (SHELL_IS_WINDOW_MANAGER (self), NULL);
  g_return_val_if_fail (actor == NULL || CLUTTER_IS_ACTOR (actor), NULL);

  adjustment = st_adjustment_new (actor, 0, 0, 0, 0, 0, 0);

  for (i = 0; i < G_N_ELEMENTS (adjustment_bindings); i++)
    g_object_bind_property (self->workspaces_adjustment, adjustment_bindings[i].name,
                            adjustment, adjustment_bindings[i].name,
                            adjustment_bindings[i].flags);

  g_ptr_array_add (self->derived_adjustments, adjustment);
  g_object_weak_ref (G_OBJECT (adjustment), derived_adjustment_finalized_cb, self);

  return adjustment;
}

/* ---- the switchers and the other handlers of key bindings --------------- */

static void
switcher_window_activated_cb (ShellSwitcherPopup  *popup,
                              MetaWindow          *window,
                              guint                timestamp,
                              ShellWindowManager  *self)
{
  ShellPanel *panel = shell_main_get_panel (shell_main_get_default ());

  g_signal_emit (self, signals[OVERVIEW_HIDE_REQUESTED], 0);
  if (panel != NULL)
    shell_panel_close_calendar (panel);
}

static void
switcher_skip_next_effect_cb (ShellSwitcherPopup *popup,
                              ClutterActor       *actor,
                              ShellWindowManager *self)
{
  if (META_IS_WINDOW_ACTOR (actor))
    shell_window_manager_skip_next_effect (self, META_WINDOW_ACTOR (actor));
}

static void
switcher_move_window_cb (ShellSwitcherPopup *popup,
                         MetaWindow         *window,
                         MetaWorkspace      *workspace,
                         ShellWindowManager *self)
{
  shell_window_manager_action_move_window (self, window, workspace);
}

static void
start_switcher (MetaDisplay        *display,
                MetaWindow         *window,
                const ClutterEvent *event,
                MetaKeyBinding     *binding,
                gpointer            user_data)
{
  ShellWindowManager *self = user_data;
  const char *name = meta_key_binding_get_name (binding);
  ShellSwitcherPopup *tab_popup = NULL;
  gboolean is_monitor_switcher = FALSE;

  if (g_str_equal (name, "switch-applications") ||
      g_str_equal (name, "switch-applications-backward") ||
      g_str_equal (name, "switch-group") ||
      g_str_equal (name, "switch-group-backward"))
    {
      tab_popup = SHELL_SWITCHER_POPUP (shell_app_switcher_popup_new ());
    }
  else if (g_str_equal (name, "switch-windows") ||
           g_str_equal (name, "switch-windows-backward"))
    {
      tab_popup = SHELL_SWITCHER_POPUP (shell_window_switcher_popup_new ());
    }
  else if (g_str_equal (name, "cycle-windows") ||
           g_str_equal (name, "cycle-windows-backward"))
    {
      tab_popup = SHELL_SWITCHER_POPUP (shell_window_cycler_popup_new ());
    }
  else if (g_str_equal (name, "cycle-group") ||
           g_str_equal (name, "cycle-group-backward"))
    {
      tab_popup = SHELL_SWITCHER_POPUP (shell_group_cycler_popup_new ());
    }
  else if (g_str_equal (name, "switch-monitor"))
    {
      /* constructor is created below, the popup is not created for the
       * other bindings */
      is_monitor_switcher = TRUE;
    }
  else
    {
      return;
    }

  /* prevent a corner case where both tab and workspace switcher popups show up at once */
  if (self->workspace_switcher_popup != NULL)
    clutter_actor_destroy (CLUTTER_ACTOR (self->workspace_switcher_popup));

  if (is_monitor_switcher)
    tab_popup = SHELL_SWITCHER_POPUP (shell_switch_monitor_popup_new ());

  /* The popups activate the window themselves, this is the rest of
   * Main.activateWindow () */
  if (!is_monitor_switcher)
    g_signal_connect_object (tab_popup, "window-activated",
                             G_CALLBACK (switcher_window_activated_cb), self, 0);

  /* The cyclers have no popup to hand the window manager's part of
   * activating a window to */
  if (SHELL_IS_CYCLER_POPUP (tab_popup))
    {
      g_signal_connect_object (tab_popup, "skip-next-effect",
                               G_CALLBACK (switcher_skip_next_effect_cb), self, 0);
      g_signal_connect_object (tab_popup, "move-window",
                               G_CALLBACK (switcher_move_window_cb), self, 0);
    }

  if (!shell_switcher_popup_show_switcher (tab_popup,
                                           meta_key_binding_is_reversed (binding),
                                           name,
                                           meta_key_binding_get_mask (binding)))
    clutter_actor_destroy (CLUTTER_ACTOR (tab_popup));
}

static void
start_a11y_switcher (MetaDisplay        *display,
                     MetaWindow         *window,
                     const ClutterEvent *event,
                     MetaKeyBinding     *binding,
                     gpointer            user_data)
{
  ShellWindowManager *self = user_data;

  if (self->ctrl_alt_tab_manager == NULL)
    return;

  shell_ctrl_alt_tab_manager_popup (self->ctrl_alt_tab_manager,
                                    meta_key_binding_is_reversed (binding),
                                    meta_key_binding_get_name (binding),
                                    meta_key_binding_get_mask (binding),
                                    self->overview_visible);
}

static ShellApp *
get_nth_favorite_app (int n)
{
  g_autoptr (GList) apps = NULL;

  if (!session_mode_get_flag ("has-overview", FALSE))
    return NULL;

  if (n < 0)
    return NULL;

  apps = shell_app_favorites_get_favorites (shell_app_favorites_get_default ());

  return g_list_nth_data (apps, n);
}

/* The number that ends the name of a key binding like
 * "switch-to-application-3" */
static int
get_binding_target (MetaKeyBinding *binding)
{
  const char *name = meta_key_binding_get_name (binding);
  const char *dash = strrchr (name, '-');

  if (dash == NULL)
    return 0;

  return (int) g_ascii_strtoll (dash + 1, NULL, 10);
}

static void
switch_to_application (MetaDisplay        *display,
                       MetaWindow         *window,
                       const ClutterEvent *event,
                       MetaKeyBinding     *binding,
                       gpointer            user_data)
{
  ShellWindowManager *self = user_data;
  ShellApp *app = get_nth_favorite_app (get_binding_target (binding) - 1);

  if (app != NULL)
    {
      g_signal_emit (self, signals[OVERVIEW_HIDE_REQUESTED], 0);
      shell_app_activate (app);
    }
}

static void
open_new_application_window (MetaDisplay        *display,
                             MetaWindow         *window,
                             const ClutterEvent *event,
                             MetaKeyBinding     *binding,
                             gpointer            user_data)
{
  ShellApp *app = get_nth_favorite_app (get_binding_target (binding) - 1);

  if (app != NULL)
    shell_app_open_new_window (app, -1);
}

static void
toggle_calendar (MetaDisplay        *display,
                 MetaWindow         *window,
                 const ClutterEvent *event,
                 MetaKeyBinding     *binding,
                 gpointer            user_data)
{
  ShellPanel *panel = shell_main_get_panel (shell_main_get_default ());

  if (panel != NULL)
    shell_panel_toggle_calendar (panel);
}

static void
toggle_quick_settings (MetaDisplay        *display,
                       MetaWindow         *window,
                       const ClutterEvent *event,
                       MetaKeyBinding     *binding,
                       gpointer            user_data)
{
  ShellPanel *panel = shell_main_get_panel (shell_main_get_default ());

  if (panel != NULL)
    shell_panel_toggle_quick_settings (panel);
}

static void
workspace_switcher_popup_destroyed_cb (ClutterActor       *popup,
                                       ShellWindowManager *self)
{
  shell_window_manager_unblock_workspace_updates (self);
  self->workspace_switcher_popup = NULL;
  self->is_workspace_prepended = FALSE;
}

static void
show_workspace_switcher (MetaDisplay        *display,
                         MetaWindow         *window,
                         const ClutterEvent *event,
                         MetaKeyBinding     *binding,
                         gpointer            user_data)
{
  ShellWindowManager *self = user_data;
  MetaWorkspaceManager *workspace_manager = meta_display_get_workspace_manager (display);
  g_auto (GStrv) parts = NULL;
  const char *action, *target;
  MetaWorkspace *new_ws = NULL;
  MetaMotionDirection direction = 0;
  gboolean have_direction = FALSE;
  gboolean vertical, rtl;
  char *end;
  gint64 number;
  int n_workspaces;

  if (!session_mode_get_flag ("has-workspaces", FALSE))
    return;

  n_workspaces = meta_workspace_manager_get_n_workspaces (workspace_manager);
  if (n_workspaces == 1)
    return;

  parts = g_strsplit (meta_key_binding_get_name (binding), "-", 4);
  if (g_strv_length (parts) < 4)
    return;
  action = parts[0];
  target = parts[3];

  vertical = meta_workspace_manager_get_layout_rows (workspace_manager) == -1;
  rtl = is_rtl ();

  if (g_str_equal (action, "move"))
    {
      /* "Moving" a window to another workspace doesn't make sense when
       * it cannot be unstuck, and is potentially confusing if a new
       * workspaces is added at the start/end */
      if (window == NULL ||
          meta_window_is_always_on_all_workspaces (window) ||
          (meta_prefs_get_workspaces_only_on_primary () &&
           meta_window_get_monitor (window) !=
           shell_layout_manager_get_primary_index (get_layout_manager ())))
        return;
    }

  number = g_ascii_strtoll (target, &end, 10);

  if (g_str_equal (target, "last"))
    {
      if (vertical)
        direction = META_MOTION_DOWN;
      else if (rtl)
        direction = META_MOTION_LEFT;
      else
        direction = META_MOTION_RIGHT;
      have_direction = TRUE;

      new_ws = meta_workspace_manager_get_workspace_by_index (workspace_manager,
                                                              n_workspaces - 1);
    }
  else if (*target == '\0' || *end != '\0')
    {
      /* Prepend a new workspace dynamically */
      const char *prepend_target;

      if (vertical)
        prepend_target = "up";
      else if (rtl)
        prepend_target = "right";
      else
        prepend_target = "left";

      if (meta_workspace_manager_get_active_workspace_index (workspace_manager) == 0 &&
          g_str_equal (action, "move") && g_str_equal (target, prepend_target) &&
          !self->is_workspace_prepended)
        {
          shell_window_manager_insert_workspace (self, 0);
          self->is_workspace_prepended = TRUE;
        }

      if (g_str_equal (target, "left"))
        direction = META_MOTION_LEFT;
      else if (g_str_equal (target, "right"))
        direction = META_MOTION_RIGHT;
      else if (g_str_equal (target, "up"))
        direction = META_MOTION_UP;
      else if (g_str_equal (target, "down"))
        direction = META_MOTION_DOWN;
      else
        return;
      have_direction = TRUE;

      new_ws = meta_workspace_get_neighbor (meta_workspace_manager_get_active_workspace (workspace_manager),
                                            direction);
    }
  else if (number > 0 && number <= n_workspaces)
    {
      number--;
      new_ws = meta_workspace_manager_get_workspace_by_index (workspace_manager, number);

      if (meta_workspace_manager_get_active_workspace_index (workspace_manager) > number)
        {
          if (vertical)
            direction = META_MOTION_UP;
          else if (rtl)
            direction = META_MOTION_RIGHT;
          else
            direction = META_MOTION_LEFT;
        }
      else
        {
          if (vertical)
            direction = META_MOTION_DOWN;
          else if (rtl)
            direction = META_MOTION_LEFT;
          else
            direction = META_MOTION_RIGHT;
        }
      have_direction = TRUE;
    }

  if (meta_workspace_manager_get_layout_rows (workspace_manager) == -1 &&
      (!have_direction ||
       (direction != META_MOTION_UP && direction != META_MOTION_DOWN)))
    return;

  if (meta_workspace_manager_get_layout_columns (workspace_manager) == -1 &&
      (!have_direction ||
       (direction != META_MOTION_LEFT && direction != META_MOTION_RIGHT)))
    return;

  /* The JS threw on an out of range workspace */
  if (new_ws == NULL)
    return;

  if (g_str_equal (action, "switch"))
    shell_window_manager_action_move_workspace (self, new_ws);
  else if (window != NULL)
    shell_window_manager_action_move_window (self, window, new_ws);

  if (!self->overview_visible)
    {
      if (self->workspace_switcher_popup == NULL)
        {
          ClutterActor *ui_group =
            CLUTTER_ACTOR (shell_layout_manager_get_ui_group (get_layout_manager ()));
          ShellWorkspaceSwitcherPopup *popup;

          shell_window_manager_block_workspace_updates (self);
          popup = shell_workspace_switcher_popup_new (ui_group);
          self->workspace_switcher_popup = popup;
          g_signal_connect_object (popup, "destroy",
                                   G_CALLBACK (workspace_switcher_popup_destroyed_cb),
                                   self, 0);
        }

      shell_workspace_switcher_popup_display (self->workspace_switcher_popup,
                                              meta_workspace_index (new_ws));
    }
}

/* ---- the other signals of ShellWM --------------------------------------- */

static void
on_show_tile_preview (ShellWM            *shellwm,
                      MetaWindow         *window,
                      MtkRectangle       *tile_rect,
                      int                 monitor_index,
                      ShellWindowManager *self)
{
  if (self->tile_preview == NULL)
    self->tile_preview = g_object_ref_sink (shell_tile_preview_new ());

  shell_tile_preview_open (self->tile_preview, window, tile_rect, monitor_index);
}

static void
on_hide_tile_preview (ShellWM            *shellwm,
                      ShellWindowManager *self)
{
  if (self->tile_preview == NULL)
    return;

  shell_tile_preview_close (self->tile_preview);
}

static void
on_show_window_menu (ShellWM            *shellwm,
                     MetaWindow         *window,
                     int                 menu,
                     MtkRectangle       *rect,
                     ShellWindowManager *self)
{
  shell_window_menu_manager_show_window_menu_for_window (self->window_menu_manager,
                                                         window, menu, rect);
}

static void
on_confirm_display_change (ShellWM            *shellwm,
                           ShellWindowManager *self)
{
  ShellDisplayChangeDialog *dialog = shell_display_change_dialog_new ();

  shell_modal_dialog_open (SHELL_MODAL_DIALOG (dialog));
}

static MetaCloseDialog *
on_create_close_dialog (ShellWM            *shellwm,
                        MetaWindow         *window,
                        ShellWindowManager *self)
{
  return META_CLOSE_DIALOG (shell_close_dialog_new (window));
}

static MetaInhibitShortcutsDialog *
on_create_inhibit_shortcuts_dialog (ShellWM            *shellwm,
                                    MetaWindow         *window,
                                    ShellWindowManager *self)
{
  return META_INHIBIT_SHORTCUTS_DIALOG (shell_inhibit_shortcuts_dialog_new (window));
}

static void
window_menu_screenshot_requested_cb (ShellWindowMenuManager *manager,
                                     CoglTexture            *texture,
                                     ShellWindowManager     *self)
{
  g_signal_emit (self, signals[SCREENSHOT_REQUESTED], 0, texture);
}

/* ---- the signals of MetaDisplay ----------------------------------------- */

static void
on_show_resize_popup (MetaDisplay        *display,
                      gboolean            show,
                      MtkRectangle       *rect,
                      int                 display_w,
                      int                 display_h,
                      ShellWindowManager *self)
{
  if (show)
    {
      if (self->resize_popup == NULL)
        self->resize_popup = g_object_ref_sink (shell_resize_popup_new ());

      shell_resize_popup_set (self->resize_popup, rect, display_w, display_h);
    }
  else
    {
      if (self->resize_popup == NULL)
        return;

      clutter_actor_destroy (CLUTTER_ACTOR (self->resize_popup));
      g_clear_object (&self->resize_popup);
    }
}

static ClutterActor *
on_show_pad_osd (MetaDisplay        *display,
                 ClutterInputDevice *device,
                 GSettings          *settings,
                 const char         *image_path,
                 gboolean            edition_mode,
                 int                 monitor_index,
                 ShellWindowManager *self)
{
  ShellPadOsd *pad_osd = shell_pad_osd_new (device, settings, image_path,
                                            edition_mode, monitor_index);

  /* The return value of a signal handler is owned by the emission, which
   * hands a new reference to mutter; the OSD itself is owned by the stage */
  return pad_osd != NULL ? g_object_ref (CLUTTER_ACTOR (pad_osd)) : NULL;
}

static void
on_show_osd (MetaDisplay        *display,
             int                 monitor_index,
             const char         *icon_name,
             const char         *label,
             ShellWindowManager *self)
{
  ShellOsdWindowManager *osd_window_manager =
    shell_main_get_osd_window_manager (shell_main_get_default ());
  g_autoptr (GError) error = NULL;
  g_autoptr (GIcon) icon = NULL;

  if (osd_window_manager == NULL)
    return;

  icon = g_icon_new_for_string (icon_name, &error);
  if (icon == NULL)
    {
      g_warning ("%s: %s", G_STRFUNC, error->message);
      return;
    }

  shell_osd_window_manager_show_one (osd_window_manager, monitor_index, icon, label, NAN, 1);
}

static void
start_x11_services_cb (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  g_autoptr (GError) error = NULL;
  gboolean status = TRUE;

  if (!shell_util_start_systemd_unit_finish (res, &error))
    {
      /* Ignore NOT_SUPPORTED error, which indicates we are not systemd
       * managed and gnome-session will have taken care of everything
       * already.
       * Note that we do log cancellation from here. */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
        {
          g_message ("Error starting X11 services: %s", error->message);
          status = FALSE;
        }
    }

  g_task_return_boolean (task, status);
}

static gboolean
on_init_xserver (MetaDisplay        *display,
                 GTask              *task,
                 ShellWindowManager *self)
{
  const char * const xim_args[] = { "--xim", NULL };

  shell_ibus_manager_restart_daemon (shell_ibus_manager_get_default (), xim_args);

  shell_util_start_systemd_unit ("gnome-session-x11-services-ready.target", "fail", NULL,
                                 start_x11_services_cb, g_object_ref (task));

  return TRUE;
}

static void
stop_x11_services_cb (GObject      *source,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  if (!shell_util_stop_systemd_unit_finish (res, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    g_message ("Error stopping X11 services: %s", error->message);
}

static void
on_x11_display_closing (MetaDisplay        *display,
                        ShellWindowManager *self)
{
  shell_util_stop_systemd_unit ("gnome-session-x11-services.target", "fail", NULL,
                                stop_x11_services_cb, NULL);

  shell_ibus_manager_restart_daemon (shell_ibus_manager_get_default (), NULL);
}

/* ---- the gesture that ends the fullscreen mode -------------------------- */

static gboolean
top_drag_may_recognize_cb (ClutterGesture *gesture,
                           gpointer        user_data)
{
  return (SHELL_ACTION_MODE_NORMAL & shell_main_get_action_mode (shell_main_get_default ())) != 0;
}

static void
top_drag_end_cb (ClutterGesture      *gesture,
                 ShellWindowManager  *self)
{
  MetaWindow *current_window = meta_display_get_focus_window (self->display);

  if (current_window != NULL)
    meta_window_unmake_fullscreen (current_window);
}

static void
update_unfullscreen_gesture (ShellWindowManager *self)
{
  MetaWindow *current_window = meta_display_get_focus_window (self->display);

  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->top_drag_gesture),
                                  current_window != NULL &&
                                  meta_window_is_fullscreen (current_window));
}

static void
focus_window_changed_cb (MetaDisplay        *display,
                         GParamSpec         *pspec,
                         ShellWindowManager *self)
{
  update_unfullscreen_gesture (self);
}

static void
in_fullscreen_changed_cb (MetaDisplay        *display,
                          ShellWindowManager *self)
{
  update_unfullscreen_gesture (self);
}

/* ---- properties --------------------------------------------------------- */

/**
 * shell_window_manager_get_overview_visible:
 * @self: a #ShellWindowManager
 *
 * Returns: whether the overview is visible, including while it is
 *   animating in or out (`Main.overview.visible`)
 */
gboolean
shell_window_manager_get_overview_visible (ShellWindowManager *self)
{
  g_return_val_if_fail (SHELL_IS_WINDOW_MANAGER (self), FALSE);

  return self->overview_visible;
}

/**
 * shell_window_manager_set_overview_visible:
 * @self: a #ShellWindowManager
 * @visible: whether the overview is visible
 *
 * Sets #ShellWindowManager:overview-visible. main.js sets it to %TRUE when
 * the overview starts showing and to %FALSE when it is hidden. The windows
 * which were waiting for the overview to hide to animate their mapping
 * start to animate when it becomes %FALSE.
 */
void
shell_window_manager_set_overview_visible (ShellWindowManager *self,
                                           gboolean            visible)
{
  g_return_if_fail (SHELL_IS_WINDOW_MANAGER (self));

  visible = !!visible;
  if (self->overview_visible == visible)
    return;

  self->overview_visible = visible;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OVERVIEW_VISIBLE]);

  if (!visible)
    flush_pending_maps (self);
}

/**
 * shell_window_manager_get_overview_closing:
 * @self: a #ShellWindowManager
 *
 * Returns: whether the overview is animating out (`Main.overview.closing`)
 */
gboolean
shell_window_manager_get_overview_closing (ShellWindowManager *self)
{
  g_return_val_if_fail (SHELL_IS_WINDOW_MANAGER (self), FALSE);

  return self->overview_closing;
}

/**
 * shell_window_manager_set_overview_closing:
 * @self: a #ShellWindowManager
 * @closing: whether the overview is animating out
 *
 * Sets #ShellWindowManager:overview-closing. main.js sets it to %TRUE when
 * the overview starts hiding and to %FALSE when it is hidden or shown.
 */
void
shell_window_manager_set_overview_closing (ShellWindowManager *self,
                                           gboolean            closing)
{
  g_return_if_fail (SHELL_IS_WINDOW_MANAGER (self));

  closing = !!closing;
  if (self->overview_closing == closing)
    return;

  self->overview_closing = closing;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OVERVIEW_CLOSING]);
}

/**
 * shell_window_manager_get_ctrl_alt_tab_manager:
 * @self: a #ShellWindowManager
 *
 * Returns: (transfer none) (nullable): the manager of the Ctrl+Alt+Tab
 *   popup (`Main.ctrlAltTabManager`)
 */
ShellCtrlAltTabManager *
shell_window_manager_get_ctrl_alt_tab_manager (ShellWindowManager *self)
{
  g_return_val_if_fail (SHELL_IS_WINDOW_MANAGER (self), NULL);

  return self->ctrl_alt_tab_manager;
}

/**
 * shell_window_manager_set_ctrl_alt_tab_manager:
 * @self: a #ShellWindowManager
 * @manager: (nullable): the manager of the Ctrl+Alt+Tab popup
 *
 * Sets #ShellWindowManager:ctrl-alt-tab-manager, which the window manager
 * pops up for the `switch-panels` key bindings.
 */
void
shell_window_manager_set_ctrl_alt_tab_manager (ShellWindowManager     *self,
                                               ShellCtrlAltTabManager *manager)
{
  g_return_if_fail (SHELL_IS_WINDOW_MANAGER (self));
  g_return_if_fail (manager == NULL || SHELL_IS_CTRL_ALT_TAB_MANAGER (manager));

  if (g_set_object (&self->ctrl_alt_tab_manager, manager))
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CTRL_ALT_TAB_MANAGER]);
}

static void
shell_window_manager_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  ShellWindowManager *self = SHELL_WINDOW_MANAGER (object);

  switch (prop_id)
    {
    case PROP_OVERVIEW_VISIBLE:
      g_value_set_boolean (value, self->overview_visible);
      break;

    case PROP_OVERVIEW_CLOSING:
      g_value_set_boolean (value, self->overview_closing);
      break;

    case PROP_CTRL_ALT_TAB_MANAGER:
      g_value_set_object (value, self->ctrl_alt_tab_manager);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_window_manager_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  ShellWindowManager *self = SHELL_WINDOW_MANAGER (object);

  switch (prop_id)
    {
    case PROP_OVERVIEW_VISIBLE:
      shell_window_manager_set_overview_visible (self, g_value_get_boolean (value));
      break;

    case PROP_OVERVIEW_CLOSING:
      shell_window_manager_set_overview_closing (self, g_value_get_boolean (value));
      break;

    case PROP_CTRL_ALT_TAB_MANAGER:
      shell_window_manager_set_ctrl_alt_tab_manager (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

/* ---- construction ------------------------------------------------------- */

static void
setup_keybindings (ShellWindowManager *self)
{
  static const char * const workspace_direction_names[] = {
    "left", "right", "up", "down", NULL
  };
  static const char * const switcher_names[] = {
    "switch-applications", "switch-group",
    "switch-applications-backward", "switch-group-backward",
    "switch-windows", "switch-windows-backward",
    "cycle-windows", "cycle-windows-backward",
    "cycle-group", "cycle-group-backward",
    NULL
  };
  static const ShellActionMode a11y_modes =
    SHELL_ACTION_MODE_NORMAL | SHELL_ACTION_MODE_OVERVIEW |
    SHELL_ACTION_MODE_LOCK_SCREEN | SHELL_ACTION_MODE_UNLOCK_SCREEN |
    SHELL_ACTION_MODE_LOGIN_SCREEN;
  GSettings *settings = self->keybindings_settings;
  int i, d;

  for (i = 1; i <= 12; i++)
    {
      g_autofree char *session_name = g_strdup_printf ("switch-to-session-%d", i);

      shell_window_manager_allow_keybinding (self, session_name, SHELL_ACTION_MODE_ALL);
    }

  /* switch-to-workspace-{left,right,up,down,last,1..12} */
  for (d = 0; workspace_direction_names[d] != NULL; d++)
    {
      g_autofree char *switch_name =
        g_strdup_printf ("switch-to-workspace-%s", workspace_direction_names[d]);

      shell_window_manager_set_custom_keybinding_handler (self, switch_name,
                                                          MODES_NORMAL_OVERVIEW,
                                                          show_workspace_switcher, self, NULL);
    }
  for (d = 0; workspace_direction_names[d] != NULL; d++)
    {
      g_autofree char *move_name =
        g_strdup_printf ("move-to-workspace-%s", workspace_direction_names[d]);

      shell_window_manager_set_custom_keybinding_handler (self, move_name,
                                                          MODES_NORMAL_OVERVIEW,
                                                          show_workspace_switcher, self, NULL);
    }
  shell_window_manager_set_custom_keybinding_handler (self, "switch-to-workspace-last",
                                                      MODES_NORMAL_OVERVIEW,
                                                      show_workspace_switcher, self, NULL);
  for (i = 1; i <= 12; i++)
    {
      g_autofree char *switch_name = g_strdup_printf ("switch-to-workspace-%d", i);

      shell_window_manager_set_custom_keybinding_handler (self, switch_name,
                                                          MODES_NORMAL_OVERVIEW,
                                                          show_workspace_switcher, self, NULL);
    }
  for (i = 1; i <= 12; i++)
    {
      g_autofree char *move_name = g_strdup_printf ("move-to-workspace-%d", i);

      shell_window_manager_set_custom_keybinding_handler (self, move_name,
                                                          SHELL_ACTION_MODE_NORMAL,
                                                          show_workspace_switcher, self, NULL);
    }
  shell_window_manager_set_custom_keybinding_handler (self, "move-to-workspace-last",
                                                      SHELL_ACTION_MODE_NORMAL,
                                                      show_workspace_switcher, self, NULL);

  for (i = 0; switcher_names[i] != NULL; i++)
    shell_window_manager_set_custom_keybinding_handler (self, switcher_names[i],
                                                        SHELL_ACTION_MODE_NORMAL,
                                                        start_switcher, self, NULL);

  shell_window_manager_set_custom_keybinding_handler (self, "switch-panels",
                                                      a11y_modes,
                                                      start_a11y_switcher, self, NULL);
  shell_window_manager_set_custom_keybinding_handler (self, "switch-panels-backward",
                                                      a11y_modes,
                                                      start_a11y_switcher, self, NULL);

  shell_window_manager_set_custom_keybinding_handler (self, "switch-monitor",
                                                      MODES_NORMAL_OVERVIEW,
                                                      start_switcher, self, NULL);

  shell_window_manager_add_keybinding (self, "toggle-message-tray", settings,
                                       META_KEY_BINDING_IGNORE_AUTOREPEAT,
                                       SHELL_ACTION_MODE_NORMAL | SHELL_ACTION_MODE_OVERVIEW |
                                       SHELL_ACTION_MODE_POPUP,
                                       toggle_calendar, self, NULL);
  shell_window_manager_add_keybinding (self, "toggle-quick-settings", settings,
                                       META_KEY_BINDING_IGNORE_AUTOREPEAT,
                                       SHELL_ACTION_MODE_NORMAL | SHELL_ACTION_MODE_OVERVIEW |
                                       SHELL_ACTION_MODE_POPUP,
                                       toggle_quick_settings, self, NULL);

  for (i = 1; i <= 9; i++)
    {
      g_autofree char *switch_name = g_strdup_printf ("switch-to-application-%d", i);

      shell_window_manager_add_keybinding (self, switch_name, settings,
                                           META_KEY_BINDING_IGNORE_AUTOREPEAT,
                                           MODES_NORMAL_OVERVIEW,
                                           switch_to_application, self, NULL);
    }
  for (i = 1; i <= 9; i++)
    {
      g_autofree char *open_name = g_strdup_printf ("open-new-window-application-%d", i);

      shell_window_manager_add_keybinding (self, open_name, settings,
                                           META_KEY_BINDING_IGNORE_AUTOREPEAT,
                                           MODES_NORMAL_OVERVIEW,
                                           open_new_application_window, self, NULL);
    }

}

static void
window_manager_on_shutdown (ShellGlobal        *global,
                            ShellWindowManager *self)
{
  g_clear_handle_id (&self->can_scroll_id, g_source_remove);
}

static void
shell_window_manager_constructed (GObject *object)
{
  ShellWindowManager *self = SHELL_WINDOW_MANAGER (object);
  ShellGlobal *global = shell_global_get ();
  MetaDisplay *display = shell_global_get_display (global);
  ClutterStage *stage = shell_global_get_stage (global);
  ShellWM *shellwm = shell_global_get_window_manager (global);
  MetaWorkspaceManager *workspace_manager = shell_global_get_workspace_manager (global);

  G_OBJECT_CLASS (shell_window_manager_parent_class)->constructed (object);

  g_set_weak_pointer (&self->global, global);
  g_set_weak_pointer (&self->shellwm, shellwm);
  g_set_weak_pointer (&self->display, display);
  g_set_weak_pointer (&self->workspace_manager, workspace_manager);
  g_set_weak_pointer (&self->stage, stage);

  g_signal_connect_object (global, "shutdown",
                           G_CALLBACK (window_manager_on_shutdown), self, 0);

  /* Main.createWorkspacesAdjustment () */
  self->workspaces_adjustment =
    st_adjustment_new (NULL,
                       meta_workspace_manager_get_active_workspace_index (workspace_manager),
                       0,
                       meta_workspace_manager_get_n_workspaces (workspace_manager),
                       0, 1, 1);
  g_object_bind_property (workspace_manager, "n-workspaces",
                          self->workspaces_adjustment, "upper",
                          G_BINDING_SYNC_CREATE);
  g_signal_connect_object (self->workspaces_adjustment, "notify::upper",
                           G_CALLBACK (workspaces_adjustment_upper_changed_cb), self, 0);

  g_signal_connect_object (shellwm, "kill-window-effects",
                           G_CALLBACK (on_kill_window_effects), self, 0);

  g_signal_connect_object (shellwm, "switch-workspace",
                           G_CALLBACK (on_switch_workspace), self, 0);
  g_signal_connect_object (shellwm, "show-tile-preview",
                           G_CALLBACK (on_show_tile_preview), self, 0);
  g_signal_connect_object (shellwm, "hide-tile-preview",
                           G_CALLBACK (on_hide_tile_preview), self, 0);
  g_signal_connect_object (shellwm, "show-window-menu",
                           G_CALLBACK (on_show_window_menu), self, 0);
  g_signal_connect_object (shellwm, "minimize",
                           G_CALLBACK (on_minimize), self, 0);
  g_signal_connect_object (shellwm, "unminimize",
                           G_CALLBACK (on_unminimize), self, 0);
  g_signal_connect_object (shellwm, "size-change",
                           G_CALLBACK (on_size_change), self, 0);
  g_signal_connect_object (shellwm, "size-changed",
                           G_CALLBACK (on_size_changed), self, 0);
  g_signal_connect_object (shellwm, "map",
                           G_CALLBACK (on_map), self, 0);
  g_signal_connect_object (shellwm, "destroy",
                           G_CALLBACK (on_destroy), self, 0);
  g_signal_connect_object (shellwm, "filter-keybinding",
                           G_CALLBACK (on_filter_keybinding), self, 0);
  g_signal_connect_object (shellwm, "confirm-display-change",
                           G_CALLBACK (on_confirm_display_change), self, 0);
  g_signal_connect_object (shellwm, "create-close-dialog",
                           G_CALLBACK (on_create_close_dialog), self, 0);
  g_signal_connect_object (shellwm, "create-inhibit-shortcuts-dialog",
                           G_CALLBACK (on_create_inhibit_shortcuts_dialog), self, 0);

  self->keybindings_settings = g_settings_new (SHELL_KEYBINDINGS_SCHEMA);

  /* Created before the key bindings: the handlers use it */
  self->workspace_animation = shell_workspace_animation_controller_new (self);

  setup_keybindings (self);

  g_signal_connect_object (stage, "scroll-event",
                           G_CALLBACK (on_stage_scroll_event), self, 0);

  g_signal_connect_object (display, "show-resize-popup",
                           G_CALLBACK (on_show_resize_popup), self, 0);
  g_signal_connect_object (display, "show-pad-osd",
                           G_CALLBACK (on_show_pad_osd), self, 0);
  g_signal_connect_object (display, "show-osd",
                           G_CALLBACK (on_show_osd), self, 0);
  g_signal_connect_object (display, "init-xserver",
                           G_CALLBACK (on_init_xserver), self, 0);
  g_signal_connect_object (display, "x11-display-closing",
                           G_CALLBACK (on_x11_display_closing), self, 0);

  self->window_menu_manager = shell_window_menu_manager_new ();
  g_signal_connect_object (self->window_menu_manager, "screenshot-requested",
                           G_CALLBACK (window_menu_screenshot_requested_cb), self, 0);

  if (session_mode_get_flag ("has-workspaces", FALSE))
    self->workspace_tracker = shell_workspace_tracker_new ();

  self->top_drag_gesture = g_object_ref_sink (g_object_new (SHELL_TYPE_EDGE_DRAG_GESTURE,
                                                            "name", "Window unfullscreen top drag",
                                                            "side", ST_SIDE_TOP,
                                                            NULL));
  g_signal_connect (self->top_drag_gesture, "may-recognize",
                    G_CALLBACK (top_drag_may_recognize_cb), self);
  g_signal_connect (self->top_drag_gesture, "end",
                    G_CALLBACK (top_drag_end_cb), self);

  g_signal_connect_object (display, "notify::focus-window",
                           G_CALLBACK (focus_window_changed_cb), self, 0);
  g_signal_connect_object (display, "in-fullscreen-changed",
                           G_CALLBACK (in_fullscreen_changed_cb), self, 0);
  update_unfullscreen_gesture (self);

  clutter_actor_add_action (CLUTTER_ACTOR (stage), CLUTTER_ACTION (self->top_drag_gesture));

  g_signal_connect_object (shellwm, "kill-switch-workspace",
                           G_CALLBACK (on_kill_switch_workspace), self, 0);
}

static void
shell_window_manager_dispose (GObject *object)
{
  ShellWindowManager *self = SHELL_WINDOW_MANAGER (object);
  guint i;

  g_clear_handle_id (&self->can_scroll_id, g_source_remove);

  if (self->derived_adjustments != NULL)
    {
      for (i = 0; i < self->derived_adjustments->len; i++)
        g_object_weak_unref (g_ptr_array_index (self->derived_adjustments, i),
                             derived_adjustment_finalized_cb, self);
      g_clear_pointer (&self->derived_adjustments, g_ptr_array_unref);
    }
  g_clear_object (&self->workspaces_adjustment);

  g_clear_pointer (&self->pending_maps, g_ptr_array_unref);

  g_clear_object (&self->workspace_tracker);
  g_clear_object (&self->workspace_animation);
  g_clear_object (&self->window_menu_manager);
  g_clear_object (&self->tile_preview);
  g_clear_object (&self->resize_popup);
  g_clear_object (&self->top_drag_gesture);
  g_clear_object (&self->ctrl_alt_tab_manager);
  g_clear_object (&self->keybindings_settings);

  g_clear_pointer (&self->minimizing, g_hash_table_unref);
  g_clear_pointer (&self->unminimizing, g_hash_table_unref);
  g_clear_pointer (&self->mapping, g_hash_table_unref);
  g_clear_pointer (&self->resizing, g_hash_table_unref);
  g_clear_pointer (&self->resize_pending, g_hash_table_unref);
  g_clear_pointer (&self->destroying, g_hash_table_unref);
  g_clear_pointer (&self->skipped_actors, g_hash_table_unref);

  g_clear_weak_pointer (&self->global);
  g_clear_weak_pointer (&self->shellwm);
  g_clear_weak_pointer (&self->display);
  g_clear_weak_pointer (&self->workspace_manager);
  g_clear_weak_pointer (&self->stage);

  G_OBJECT_CLASS (shell_window_manager_parent_class)->dispose (object);
}

static void
shell_window_manager_finalize (GObject *object)
{
  ShellWindowManager *self = SHELL_WINDOW_MANAGER (object);

  g_clear_pointer (&self->allowed_keybindings, g_hash_table_unref);

  G_OBJECT_CLASS (shell_window_manager_parent_class)->finalize (object);
}

static void
shell_window_manager_class_init (ShellWindowManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_window_manager_constructed;
  object_class->dispose = shell_window_manager_dispose;
  object_class->finalize = shell_window_manager_finalize;
  object_class->get_property = shell_window_manager_get_property;
  object_class->set_property = shell_window_manager_set_property;

  /**
   * ShellWindowManager:overview-visible:
   *
   * Whether the overview is visible, including while it is animating in or
   * out (`Main.overview.visible`). main.js keeps it in sync: it sets it to
   * %TRUE when the overview emits `showing` and to %FALSE when it emits
   * `hidden`.
   */
  props[PROP_OVERVIEW_VISIBLE] =
    g_param_spec_boolean ("overview-visible", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellWindowManager:overview-closing:
   *
   * Whether the overview is animating out (`Main.overview.closing`). main.js
   * sets it to %TRUE when the overview emits `hiding` and back to %FALSE
   * when it emits `showing`, `hidden` or `shown`. The window animations
   * only wait for an overview which is visible and not closing.
   */
  props[PROP_OVERVIEW_CLOSING] =
    g_param_spec_boolean ("overview-closing", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellWindowManager:ctrl-alt-tab-manager:
   *
   * The #ShellCtrlAltTabManager (`Main.ctrlAltTabManager`) which the
   * `switch-panels` key bindings pop up. main.js sets it.
   */
  props[PROP_CTRL_ALT_TAB_MANAGER] =
    g_param_spec_object ("ctrl-alt-tab-manager", NULL, NULL,
                         SHELL_TYPE_CTRL_ALT_TAB_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellWindowManager::overview-hide-requested:
   * @self: the #ShellWindowManager
   *
   * Emitted when the window manager activates a window or an application
   * from a key binding, which hides the overview (`Main.overview.hide ()`).
   * main.js connects it. The handler must not return a value.
   */
  signals[OVERVIEW_HIDE_REQUESTED] =
    g_signal_new ("overview-hide-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellWindowManager::screenshot-requested:
   * @self: the #ShellWindowManager
   * @texture: the texture with the contents of a window
   *
   * Emitted when "Take Screenshot" is activated in a window menu. main.js
   * connects it to `Screenshot.captureScreenshot (texture, null, 1, null)`,
   * which is still JS.
   */
  signals[SCREENSHOT_REQUESTED] =
    g_signal_new ("screenshot-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  COGL_TYPE_TEXTURE);
}

static void
shell_window_manager_init (ShellWindowManager *self)
{
  self->minimizing = g_hash_table_new_full (NULL, NULL, g_object_unref, NULL);
  self->unminimizing = g_hash_table_new_full (NULL, NULL, g_object_unref, NULL);
  self->mapping = g_hash_table_new_full (NULL, NULL, g_object_unref, NULL);
  self->resizing = g_hash_table_new_full (NULL, NULL, g_object_unref, NULL);
  self->resize_pending = g_hash_table_new_full (NULL, NULL, g_object_unref, NULL);
  self->destroying = g_hash_table_new_full (NULL, NULL, g_object_unref, NULL);
  self->skipped_actors = g_hash_table_new_full (NULL, NULL, g_object_unref, NULL);

  self->allowed_keybindings = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  self->pending_maps = g_ptr_array_new_with_free_func (pending_map_free);
  self->derived_adjustments = g_ptr_array_new ();

  self->can_scroll = TRUE;
}

/**
 * shell_window_manager_new:
 *
 * Creates the window manager of the shell (`new WindowManager ()`), which
 * hooks into the #ShellWM of #ShellGlobal. main.js creates it once, after
 * the layout manager, the overview and the Ctrl+Alt+Tab manager; set
 * #ShellWindowManager:ctrl-alt-tab-manager, connect its signals and keep
 * its overview properties in sync.
 *
 * Returns: (transfer full): a new #ShellWindowManager
 */
ShellWindowManager *
shell_window_manager_new (void)
{
  return g_object_new (SHELL_TYPE_WINDOW_MANAGER, NULL);
}
