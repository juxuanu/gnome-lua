/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * close-dialog.c: The dialog which offers to force quit an application which
 * does not respond to the ping of the window manager. Port of
 * js/ui/closeDialog.js.
 *
 * ShellCloseDialog implements #MetaCloseDialog: mutter calls show (), hide ()
 * and focus () (the former `vfunc_show ()`, `vfunc_hide ()` and
 * `vfunc_focus ()`), and the buttons of the dialog answer with
 * meta_close_dialog_response ().
 *
 * Differences from the JS class, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The window is a weak reference, the window owns its close dialog. The JS
 *    class kept the window alive.
 *  - The `notify::scale-factor` handler of the theme context is disconnected
 *    when the dialog is hidden; the JS one stayed connected and touched a
 *    null dialog after the first hide.
 *  - The window of the dialog is always a #MetaWindow of a mapped actor: a
 *    window without compositor actor logs a warning and no dialog is shown,
 *    where the JS threw.
 *  - A window which belongs to no application is named by its title.
 */

#include "config.h"

#include <glib/gi18n-lib.h>
#include <meta/display.h>
#include <meta/meta-window-actor.h>

#include "shell-app.h"
#include "shell-global.h"
#include "shell-window-tracker.h"
#include "st.h"
#include "ui/close-dialog.h"
#include "ui/dialog.h"
#include "ui/environment.h"

#define FROZEN_WINDOW_BRIGHTNESS -0.3
#define DIALOG_TRANSITION_TIME 150
#define ALIVE_TIMEOUT 5000

#define FROZEN_WINDOW_EFFECT_NAME "gnome-shell-frozen-window"

enum
{
  PROP_0,

  PROP_WINDOW,

  N_PROPS
};

struct _ShellCloseDialog
{
  GObject parent;

  MetaWindow *window;           /* weak */

  ShellDialog *dialog;          /* strong; not NULL while the dialog is shown */
  guint timeout_id;

  /* Weak pointers to what is needed to undo show () without asking
   * shell_global_get (), which hide () and dispose () cannot rely on */
  MetaCompositor *compositor;
  ShellGlobal *global;
  gulong shutdown_id;
  StThemeContext *theme_context;
  gulong scale_factor_id;
};

static void shell_close_dialog_iface_init (MetaCloseDialogInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellCloseDialog, shell_close_dialog, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (META_TYPE_CLOSE_DIALOG,
                                                      shell_close_dialog_iface_init))

/* A weak reference to the dialog for the callbacks of its buttons, which may
 * run while the fade out animation is still going on */
typedef struct
{
  GWeakRef dialog;
} ResponseData;

static ResponseData *
response_data_new (ShellCloseDialog *self)
{
  ResponseData *data = g_new0 (ResponseData, 1);

  g_weak_ref_init (&data->dialog, self);

  return data;
}

static void
response_data_free (gpointer user_data)
{
  ResponseData *data = user_data;

  g_weak_ref_clear (&data->dialog);
  g_free (data);
}

static void
respond (ResponseData           *data,
         MetaCloseDialogResponse response)
{
  g_autoptr (ShellCloseDialog) self = g_weak_ref_get (&data->dialog);

  if (self != NULL)
    meta_close_dialog_response (META_CLOSE_DIALOG (self), response);
}

static void
on_wait (gpointer user_data)
{
  respond (user_data, META_CLOSE_DIALOG_RESPONSE_WAIT);
}

static void
on_close (gpointer user_data)
{
  respond (user_data, META_CLOSE_DIALOG_RESPONSE_FORCE_CLOSE);
}

static void
on_shutdown (ShellGlobal      *global,
             ShellCloseDialog *self)
{
  meta_close_dialog_response (META_CLOSE_DIALOG (self),
                              META_CLOSE_DIALOG_RESPONSE_WAIT);
}

static ClutterActor *
create_dialog_content (ShellCloseDialog *self)
{
  ShellWindowTracker *tracker = shell_window_tracker_get_default ();
  ShellApp *window_app = shell_window_tracker_get_window_app (tracker, self->window);
  const char *name = window_app != NULL ? shell_app_get_name (window_app)
                                        : meta_window_get_title (self->window);
  g_autofree char *title = NULL;

  /* Translators: %s is an application name */
  title = g_strdup_printf (_("“%s” Is Not Responding"), name != NULL ? name : "");

  return g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT,
                       "title", title,
                       "description", _("You may choose to wait a short while for it to "
                                        "continue or force the app to quit entirely"),
                       NULL);
}

static void
update_scale (ShellCloseDialog *self)
{
  int scale_factor;

  if (self->dialog == NULL || self->theme_context == NULL || self->window == NULL)
    return;

  /* Since this is a child of MetaWindowActor (which, for Wayland clients,
   * applies the geometry scale factor to its children itself, see
   * meta_window_actor_set_geometry_scale()), make sure we don't apply
   * the factor twice in the end. */
  if (meta_window_get_client_type (self->window) != META_WINDOW_CLIENT_TYPE_WAYLAND)
    return;

  scale_factor = st_theme_context_get_scale_factor (self->theme_context);
  clutter_actor_set_scale (CLUTTER_ACTOR (self->dialog),
                           1.0 / scale_factor, 1.0 / scale_factor);
}

static void
scale_factor_changed_cb (StThemeContext   *theme_context,
                         GParamSpec       *pspec,
                         ShellCloseDialog *self)
{
  update_scale (self);
}

static void
init_dialog (ShellCloseDialog *self,
             ClutterActor     *window_actor)
{
  ShellGlobal *global = shell_global_get ();
  ClutterActor *dialog;
  ResponseData *close_data, *wait_data;

  if (self->dialog != NULL)
    return;

  self->dialog = g_object_ref_sink (shell_dialog_new (window_actor, "close-dialog"));
  dialog = CLUTTER_ACTOR (self->dialog);

  clutter_actor_set_width (dialog, clutter_actor_get_width (window_actor));
  clutter_actor_set_height (dialog, clutter_actor_get_height (window_actor));

  clutter_actor_add_child (CLUTTER_ACTOR (shell_dialog_get_content_layout (self->dialog)),
                           create_dialog_content (self));

  close_data = response_data_new (self);
  shell_dialog_add_button (self->dialog, _("Force Quit"), 0, TRUE,
                           on_close, close_data, response_data_free);
  wait_data = response_data_new (self);
  shell_dialog_add_button (self->dialog, _("Wait"), CLUTTER_KEY_Escape, FALSE,
                           on_wait, wait_data, response_data_free);

  st_focus_manager_add_group (shell_global_get_focus_manager (global),
                              ST_WIDGET (self->dialog));

  g_set_weak_pointer (&self->theme_context,
                      st_theme_context_get_for_stage (shell_global_get_stage (global)));
  self->scale_factor_id =
    g_signal_connect (self->theme_context, "notify::scale-factor",
                      G_CALLBACK (scale_factor_changed_cb), self);

  update_scale (self);
}

static void
add_window_effect (ClutterActor *window_actor)
{
  ClutterActor *surface_actor = clutter_actor_get_first_child (window_actor);
  ClutterEffect *effect;

  /* We set the effect on the surface actor, so the dialog itself
   * (which is a child of the MetaWindowActor) does not get the
   * effect applied itself. */
  if (surface_actor == NULL)
    return;

  effect = clutter_brightness_contrast_effect_new ();
  clutter_brightness_contrast_effect_set_brightness (CLUTTER_BRIGHTNESS_CONTRAST_EFFECT (effect),
                                                     FROZEN_WINDOW_BRIGHTNESS);
  clutter_actor_add_effect_with_name (surface_actor,
                                      FROZEN_WINDOW_EFFECT_NAME, effect);
}

static void
remove_window_effect (ShellCloseDialog *self)
{
  GObject *window_actor;
  ClutterActor *surface_actor;

  if (self->window == NULL)
    return;

  window_actor = meta_window_get_compositor_private (self->window);
  if (!CLUTTER_IS_ACTOR (window_actor))
    return;

  surface_actor = clutter_actor_get_first_child (CLUTTER_ACTOR (window_actor));
  if (surface_actor != NULL)
    clutter_actor_remove_effect_by_name (surface_actor, FROZEN_WINDOW_EFFECT_NAME);
}

static gboolean
alive_timeout_cb (gpointer user_data)
{
  ShellCloseDialog *self = user_data;

  if (self->window != NULL)
    {
      MetaDisplay *display = meta_window_get_display (self->window);

      meta_window_check_alive (self->window,
                               meta_display_get_current_time_roundtrip (display));
    }

  return G_SOURCE_CONTINUE;
}

/* The part of hide () which does not need the animation, shared with dispose () */
static void
release_show_state (ShellCloseDialog *self)
{
  if (self->compositor != NULL)
    {
      meta_compositor_enable_unredirect (self->compositor);
      g_clear_weak_pointer (&self->compositor);
    }

  g_clear_handle_id (&self->timeout_id, g_source_remove);

  if (self->global != NULL)
    {
      g_clear_signal_handler (&self->shutdown_id, self->global);
      g_clear_weak_pointer (&self->global);
    }

  if (self->theme_context != NULL)
    {
      g_clear_signal_handler (&self->scale_factor_id, self->theme_context);
      g_clear_weak_pointer (&self->theme_context);
    }
}

static void
shell_close_dialog_show (MetaCloseDialog *close_dialog)
{
  ShellCloseDialog *self = SHELL_CLOSE_DIALOG (close_dialog);
  ShellGlobal *global;
  MetaCompositor *compositor;
  GObject *window_actor;
  ClutterActor *frame;
  ShellEaseParams params = {
    .duration = DIALOG_TRANSITION_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_BACK,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  if (self->dialog != NULL)
    return;

  g_return_if_fail (META_IS_WINDOW (self->window));

  window_actor = meta_window_get_compositor_private (self->window);
  if (!META_IS_WINDOW_ACTOR (window_actor))
    {
      g_warning ("%s: the window has no actor", G_STRFUNC);
      return;
    }

  global = shell_global_get ();
  compositor = meta_display_get_compositor (meta_window_get_display (self->window));
  meta_compositor_disable_unredirect (compositor);
  g_set_weak_pointer (&self->compositor, compositor);

  self->timeout_id = g_timeout_add (ALIVE_TIMEOUT, alive_timeout_cb, self);

  add_window_effect (CLUTTER_ACTOR (window_actor));
  init_dialog (self, CLUTTER_ACTOR (window_actor));

  g_set_weak_pointer (&self->global, global);
  self->shutdown_id = g_signal_connect (global, "shutdown",
                                        G_CALLBACK (on_shutdown), self);

  frame = CLUTTER_ACTOR (shell_dialog_get_frame (self->dialog));
  clutter_actor_set_pivot_point (frame, 0.5, 0.5);
  clutter_actor_set_scale (frame, 0.8, 0.8);

  shell_actor_ease (frame, &params, NULL, NULL, NULL,
                    "scale-x", 1.0,
                    "scale-y", 1.0,
                    NULL);
}

static void
dialog_faded_out_cb (gboolean finished,
                     gpointer user_data)
{
  clutter_actor_destroy (CLUTTER_ACTOR (user_data));
}

static void
shell_close_dialog_hide (MetaCloseDialog *close_dialog)
{
  ShellCloseDialog *self = SHELL_CLOSE_DIALOG (close_dialog);
  g_autoptr (ShellDialog) dialog = NULL;
  ClutterActor *frame;
  ShellEaseParams params = {
    .duration = DIALOG_TRANSITION_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  if (self->dialog == NULL)
    return;

  release_show_state (self);

  dialog = g_steal_pointer (&self->dialog);
  remove_window_effect (self);

  frame = CLUTTER_ACTOR (shell_dialog_get_frame (dialog));
  if (frame == NULL)
    {
      /* Destroyed along with the window already */
      clutter_actor_destroy (CLUTTER_ACTOR (dialog));
      return;
    }

  clutter_actor_remove_all_transitions (frame);

  shell_dialog_make_inactive (dialog);
  shell_actor_ease (frame, &params,
                    dialog_faded_out_cb, g_object_ref (dialog), g_object_unref,
                    "opacity", 0u,
                    "scale-x", 0.8,
                    "scale-y", 0.8,
                    NULL);
}

static void
shell_close_dialog_focus (MetaCloseDialog *close_dialog)
{
  ShellCloseDialog *self = SHELL_CLOSE_DIALOG (close_dialog);
  ClutterActor *stage, *key_focus, *initial_key_focus;

  if (self->dialog == NULL)
    return;

  stage = clutter_actor_get_stage (CLUTTER_ACTOR (self->dialog));
  if (stage == NULL)
    return;

  key_focus = clutter_stage_get_key_focus (CLUTTER_STAGE (stage));
  initial_key_focus = shell_dialog_get_initial_key_focus (self->dialog);

  if (initial_key_focus != NULL &&
      (key_focus == NULL ||
       !clutter_actor_contains (CLUTTER_ACTOR (self->dialog), key_focus)))
    clutter_actor_grab_key_focus (initial_key_focus);
}

static void
shell_close_dialog_iface_init (MetaCloseDialogInterface *iface)
{
  iface->show = shell_close_dialog_show;
  iface->hide = shell_close_dialog_hide;
  iface->focus = shell_close_dialog_focus;
}

static void
shell_close_dialog_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellCloseDialog *self = SHELL_CLOSE_DIALOG (object);

  switch (prop_id)
    {
    case PROP_WINDOW:
      g_value_set_object (value, self->window);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_close_dialog_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellCloseDialog *self = SHELL_CLOSE_DIALOG (object);

  switch (prop_id)
    {
    case PROP_WINDOW:
      g_set_weak_pointer (&self->window, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_close_dialog_dispose (GObject *object)
{
  ShellCloseDialog *self = SHELL_CLOSE_DIALOG (object);

  release_show_state (self);

  if (self->dialog != NULL)
    {
      remove_window_effect (self);
      clutter_actor_destroy (CLUTTER_ACTOR (self->dialog));
      g_clear_object (&self->dialog);
    }

  g_clear_weak_pointer (&self->window);

  G_OBJECT_CLASS (shell_close_dialog_parent_class)->dispose (object);
}

static void
shell_close_dialog_class_init (ShellCloseDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_close_dialog_dispose;
  object_class->get_property = shell_close_dialog_get_property;
  object_class->set_property = shell_close_dialog_set_property;

  /**
   * ShellCloseDialog:window:
   *
   * The window which does not respond (#MetaCloseDialog:window). Only a weak
   * reference is kept.
   */
  g_object_class_override_property (object_class, PROP_WINDOW, "window");
}

static void
shell_close_dialog_init (ShellCloseDialog *self)
{
}

/**
 * shell_close_dialog_new:
 * @window: the window which does not respond
 *
 * Creates the close dialog of @window (`new CloseDialog (window)`), to be
 * returned from the ShellWM::create-close-dialog handler.
 *
 * Returns: (transfer full): a new #ShellCloseDialog
 */
ShellCloseDialog *
shell_close_dialog_new (MetaWindow *window)
{
  g_return_val_if_fail (META_IS_WINDOW (window), NULL);

  return g_object_new (SHELL_TYPE_CLOSE_DIALOG,
                       "window", window,
                       NULL);
}
