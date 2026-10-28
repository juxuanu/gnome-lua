/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * inhibit-shortcuts-dialog.c: The dialog which asks whether an application
 * may inhibit the keyboard shortcuts of the shell. Port of
 * js/ui/inhibitShortcutsDialog.js.
 *
 * ShellInhibitShortcutsDialog implements #MetaInhibitShortcutsDialog: mutter
 * calls show () and hide () (the former `vfunc_show ()` and `vfunc_hide ()`)
 * and the dialog answers with meta_inhibit_shortcuts_dialog_response (),
 * which is what `this.emit ('response', response)` did.
 *
 * Differences from the JS class, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The window is a weak reference, the window owns its dialog. The JS
 *    class kept the window alive.
 *  - The modal dialog is not destroyed when it is closed (the JS one was,
 *    and showing the dialog again used the destroyed dialog); it is destroyed
 *    with this object.
 *  - A window which has no application and no title gets the generic
 *    description, like a window with an unnamed application.
 *  - Answers of the permission store arriving after the dialog is gone are
 *    ignored.
 */

#include "config.h"

#include <clutter/clutter-pango.h>
#include <glib/gi18n-lib.h>
#include <meta/prefs.h>

#include "misc/permission-store.h"
#include "shell-app.h"
#include "shell-window-tracker.h"
#include "st.h"
#include "ui/dialog.h"
#include "ui/inhibit-shortcuts-dialog.h"
#include "ui/modal-dialog.h"

static const char * const APP_ALLOWLIST[] = { "org.gnome.Settings.desktop", NULL };

#define APP_PERMISSIONS_TABLE "gnome"
#define APP_PERMISSIONS_ID "shortcuts-inhibitor"
#define GRANTED "GRANTED"
#define DENIED "DENIED"

enum
{
  PROP_0,

  PROP_WINDOW,

  N_PROPS
};

struct _ShellInhibitShortcutsDialog
{
  GObject parent;

  MetaWindow *window; /* weak */

  ShellModalDialog *dialog; /* strong; destroyed in dispose () */
  ShellPortalPermissionStore *perm_store; /* NULL until the lookup has begun */
  GCancellable *cancellable;
};

static void shell_inhibit_shortcuts_dialog_iface_init (MetaInhibitShortcutsDialogInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellInhibitShortcutsDialog, shell_inhibit_shortcuts_dialog,
                               G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (META_TYPE_INHIBIT_SHORTCUTS_DIALOG,
                                                      shell_inhibit_shortcuts_dialog_iface_init))

/* The application of the window (`_app`), if it has one */
static ShellApp *
get_app (ShellInhibitShortcutsDialog *self)
{
  if (self->window == NULL)
    return NULL;

  return shell_window_tracker_get_window_app (shell_window_tracker_get_default (),
                                              self->window);
}

static gboolean
should_use_perm_store (ShellApp *app)
{
  return app != NULL && !shell_app_is_window_backed (app);
}

static void
set_permission_ready_cb (GObject      *source,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  if (!shell_portal_permission_store_call_set_permission_finish (SHELL_PORTAL_PERMISSION_STORE (source),
                                                                 res, &error))
    g_message ("%s: %s", G_STRFUNC, error->message);
}

static void
save_to_permission_store (ShellInhibitShortcutsDialog *self,
                          const char                  *grant)
{
  g_autoptr (ShellApp) app = get_app (self);
  const char * const permissions[] = { grant, NULL };

  if (!should_use_perm_store (app) || self->perm_store == NULL)
    return;

  /* The call keeps the proxy alive until the answer, even if this dialog is
   * gone by then */
  shell_portal_permission_store_call_set_permission (self->perm_store,
                                                     APP_PERMISSIONS_TABLE,
                                                     TRUE,
                                                     APP_PERMISSIONS_ID,
                                                     shell_app_get_id (app),
                                                     permissions,
                                                     NULL,
                                                     set_permission_ready_cb,
                                                     NULL);
}

static void
emit_response (ShellInhibitShortcutsDialog       *self,
               MetaInhibitShortcutsDialogResponse response)
{
  g_autoptr (ShellInhibitShortcutsDialog) keep_alive = g_object_ref (self);

  meta_inhibit_shortcuts_dialog_response (META_INHIBIT_SHORTCUTS_DIALOG (self),
                                          response);

  if (self->dialog != NULL)
    shell_modal_dialog_close (self->dialog);
}

/* A weak reference to the dialog for the callbacks of its buttons, which
 * mutter may have dropped the dialog under */
typedef struct
{
  GWeakRef dialog;
  MetaInhibitShortcutsDialogResponse response;
  const char *grant;
} ButtonData;

static ButtonData *
button_data_new (ShellInhibitShortcutsDialog       *self,
                 MetaInhibitShortcutsDialogResponse response,
                 const char                        *grant)
{
  ButtonData *data = g_new0 (ButtonData, 1);

  g_weak_ref_init (&data->dialog, self);
  data->response = response;
  data->grant = grant;

  return data;
}

static void
button_data_free (gpointer user_data)
{
  ButtonData *data = user_data;

  g_weak_ref_clear (&data->dialog);
  g_free (data);
}

static void
on_button_clicked (gpointer user_data)
{
  ButtonData *data = user_data;
  g_autoptr (ShellInhibitShortcutsDialog) self = g_weak_ref_get (&data->dialog);

  if (self == NULL)
    return;

  save_to_permission_store (self, data->grant);
  emit_response (self, data->response);
}

static void
build_layout (ShellInhibitShortcutsDialog *self)
{
  g_autoptr (ShellApp) app = get_app (self);
  g_autofree char *restore_accel = NULL;
  g_autofree char *description = NULL;
  const char *name = NULL;
  ClutterActor *content;
  ClutterActor *content_layout;

  if (app != NULL)
    name = shell_app_get_name (app);
  else if (self->window != NULL)
    name = meta_window_get_title (self->window);

  if (name != NULL && name[0] != '\0')
    {
      /* Translators: %s is an application name like "Settings" */
      description = g_strdup_printf (_("The app %s wants to inhibit shortcuts"), name);
    }
  else
    {
      description = g_strdup (_("An app wants to inhibit shortcuts"));
    }

  content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT,
                          "title", _("Allow inhibiting shortcuts"),
                          "description", description,
                          NULL);

  restore_accel = meta_prefs_get_keybinding_label ("restore-shortcuts");
  if (restore_accel != NULL && restore_accel[0] != '\0')
    {
      g_autofree char *restore_text = NULL;
      ClutterActor *restore_label;

      /* Translators: %s is a keyboard shortcut like "Super+x" */
      restore_text = g_strdup_printf (_("You can restore shortcuts by pressing %s"), restore_accel);
      restore_label = g_object_new (ST_TYPE_LABEL,
                                    "text", restore_text,
                                    "style-class", "message-dialog-description",
                                    NULL);
      clutter_text_set_ellipsize (st_label_get_clutter_text (ST_LABEL (restore_label)),
                                  PANGO_ELLIPSIZE_NONE);
      clutter_text_set_line_wrap (st_label_get_clutter_text (ST_LABEL (restore_label)),
                                  TRUE);
      clutter_actor_add_child (content, restore_label);
    }

  content_layout = CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (self->dialog));
  clutter_actor_add_child (content_layout, content);

  shell_modal_dialog_add_button (self->dialog, _("Deny"), CLUTTER_KEY_Escape, FALSE,
                                 on_button_clicked,
                                 button_data_new (self,
                                                  META_INHIBIT_SHORTCUTS_DIALOG_RESPONSE_DENY,
                                                  DENIED),
                                 button_data_free);

  shell_modal_dialog_add_button (self->dialog, _("Allow"), 0, TRUE,
                                 on_button_clicked,
                                 button_data_new (self,
                                                  META_INHIBIT_SHORTCUTS_DIALOG_RESPONSE_ALLOW,
                                                  GRANTED),
                                 button_data_free);
}

/* What the permission store lookup needs to know when it answers */
typedef struct
{
  GWeakRef dialog;
  char *app_id;
} CheckData;

static void
check_data_free (CheckData *data)
{
  g_weak_ref_clear (&data->dialog);
  g_free (data->app_id);
  g_free (data);
}

static void
lookup_ready_cb (GObject      *source,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  CheckData *data = user_data;
  g_autoptr (ShellInhibitShortcutsDialog) self = NULL;
  g_autoptr (GVariant) permissions = NULL;
  g_autoptr (GVariant) lookup_data = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) app_permissions = NULL;
  const char *first = NULL;

  if (!shell_portal_permission_store_call_lookup_finish (SHELL_PORTAL_PERMISSION_STORE (source),
                                                         &permissions, &lookup_data,
                                                         res, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          self = g_weak_ref_get (&data->dialog);
          if (self != NULL && self->dialog != NULL)
            shell_modal_dialog_open (self->dialog);
          g_message ("%s: %s", G_STRFUNC, error->message);
        }

      check_data_free (data);
      return;
    }

  self = g_weak_ref_get (&data->dialog);
  if (self != NULL)
    {
      app_permissions = g_variant_lookup_value (permissions, data->app_id,
                                                G_VARIANT_TYPE ("as"));
      if (app_permissions == NULL) /* Not found */
        {
          if (self->dialog != NULL)
            shell_modal_dialog_open (self->dialog);
        }
      else
        {
          if (g_variant_n_children (app_permissions) > 0)
            g_variant_get_child (app_permissions, 0, "&s", &first);

          if (g_strcmp0 (first, GRANTED) == 0)
            emit_response (self, META_INHIBIT_SHORTCUTS_DIALOG_RESPONSE_ALLOW);
          else
            emit_response (self, META_INHIBIT_SHORTCUTS_DIALOG_RESPONSE_DENY);
        }
    }

  check_data_free (data);
}

static void
perm_store_ready_cb (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  CheckData *data = user_data;
  g_autoptr (ShellInhibitShortcutsDialog) self = NULL;
  g_autoptr (ShellPortalPermissionStore) store = NULL;
  g_autoptr (GError) error = NULL;

  store = shell_permission_store_new_finish (res, &error);
  if (store == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          self = g_weak_ref_get (&data->dialog);
          if (self != NULL && self->dialog != NULL)
            shell_modal_dialog_open (self->dialog);
          g_message ("%s: %s", G_STRFUNC, error->message);
        }

      check_data_free (data);
      return;
    }

  self = g_weak_ref_get (&data->dialog);
  if (self == NULL)
    {
      check_data_free (data);
      return;
    }

  g_set_object (&self->perm_store, store);

  shell_portal_permission_store_call_lookup (self->perm_store,
                                             APP_PERMISSIONS_TABLE,
                                             APP_PERMISSIONS_ID,
                                             self->cancellable,
                                             lookup_ready_cb,
                                             data);
}

/* Check with the permission store */
static void
check_permission_store (ShellInhibitShortcutsDialog *self,
                        const char                  *app_id)
{
  CheckData *data = g_new0 (CheckData, 1);

  g_weak_ref_init (&data->dialog, self);
  data->app_id = g_strdup (app_id);

  shell_permission_store_new (self->cancellable, perm_store_ready_cb, data);
}

static void
shell_inhibit_shortcuts_dialog_show (MetaInhibitShortcutsDialog *dialog)
{
  ShellInhibitShortcutsDialog *self = SHELL_INHIBIT_SHORTCUTS_DIALOG (dialog);
  g_autoptr (ShellApp) app = get_app (self);

  if (app != NULL && g_strv_contains (APP_ALLOWLIST, shell_app_get_id (app)))
    {
      emit_response (self, META_INHIBIT_SHORTCUTS_DIALOG_RESPONSE_ALLOW);
      return;
    }

  if (!should_use_perm_store (app))
    {
      shell_modal_dialog_open (self->dialog);
      return;
    }

  check_permission_store (self, shell_app_get_id (app));
}

static void
shell_inhibit_shortcuts_dialog_hide (MetaInhibitShortcutsDialog *dialog)
{
  ShellInhibitShortcutsDialog *self = SHELL_INHIBIT_SHORTCUTS_DIALOG (dialog);

  if (self->dialog != NULL)
    shell_modal_dialog_close (self->dialog);
}

static void
shell_inhibit_shortcuts_dialog_iface_init (MetaInhibitShortcutsDialogInterface *iface)
{
  iface->show = shell_inhibit_shortcuts_dialog_show;
  iface->hide = shell_inhibit_shortcuts_dialog_hide;
}

static void
shell_inhibit_shortcuts_dialog_constructed (GObject *object)
{
  ShellInhibitShortcutsDialog *self = SHELL_INHIBIT_SHORTCUTS_DIALOG (object);

  G_OBJECT_CLASS (shell_inhibit_shortcuts_dialog_parent_class)->constructed (object);

  self->cancellable = g_cancellable_new ();

  self->dialog = g_object_ref_sink (g_object_new (SHELL_TYPE_MODAL_DIALOG,
                                                  "destroy-on-close", FALSE,
                                                  NULL));
  build_layout (self);
}

static void
shell_inhibit_shortcuts_dialog_get_property (GObject    *object,
                                             guint       prop_id,
                                             GValue     *value,
                                             GParamSpec *pspec)
{
  ShellInhibitShortcutsDialog *self = SHELL_INHIBIT_SHORTCUTS_DIALOG (object);

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
shell_inhibit_shortcuts_dialog_set_property (GObject      *object,
                                             guint         prop_id,
                                             const GValue *value,
                                             GParamSpec   *pspec)
{
  ShellInhibitShortcutsDialog *self = SHELL_INHIBIT_SHORTCUTS_DIALOG (object);

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
shell_inhibit_shortcuts_dialog_dispose (GObject *object)
{
  ShellInhibitShortcutsDialog *self = SHELL_INHIBIT_SHORTCUTS_DIALOG (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->perm_store);

  if (self->dialog != NULL)
    {
      clutter_actor_destroy (CLUTTER_ACTOR (self->dialog));
      g_clear_object (&self->dialog);
    }

  g_clear_weak_pointer (&self->window);

  G_OBJECT_CLASS (shell_inhibit_shortcuts_dialog_parent_class)->dispose (object);
}

static void
shell_inhibit_shortcuts_dialog_class_init (ShellInhibitShortcutsDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_inhibit_shortcuts_dialog_constructed;
  object_class->get_property = shell_inhibit_shortcuts_dialog_get_property;
  object_class->set_property = shell_inhibit_shortcuts_dialog_set_property;
  object_class->dispose = shell_inhibit_shortcuts_dialog_dispose;

  /**
   * ShellInhibitShortcutsDialog:window:
   *
   * The window which asks to inhibit the shortcuts
   * (#MetaInhibitShortcutsDialog:window). Only a weak reference is kept.
   */
  g_object_class_override_property (object_class, PROP_WINDOW, "window");
}

static void
shell_inhibit_shortcuts_dialog_init (ShellInhibitShortcutsDialog *self)
{
}

/**
 * shell_inhibit_shortcuts_dialog_new:
 * @window: the window which asks to inhibit the shortcuts
 *
 * Creates the dialog of @window (`new InhibitShortcutsDialog (window)`), to
 * be returned from the ShellWM::create-inhibit-shortcuts-dialog handler.
 *
 * Returns: (transfer full): a new #ShellInhibitShortcutsDialog
 */
ShellInhibitShortcutsDialog *
shell_inhibit_shortcuts_dialog_new (MetaWindow *window)
{
  g_return_val_if_fail (META_IS_WINDOW (window), NULL);

  return g_object_new (SHELL_TYPE_INHIBIT_SHORTCUTS_DIALOG,
                       "window", window,
                       NULL);
}
