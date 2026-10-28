/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * extension-downloader.c: Installs extensions from extensions.gnome.org and
 * looks for their updates. Port of js/ui/extensionDownloader.js.
 *
 * The flow is the one of the JS module: InstallRemoteExtension () asks the
 * index for the description of the extension, the user confirms in a dialog,
 * the archive is downloaded and extracted into the per-user extensions
 * directory (its schemas are compiled) and the extension is loaded and
 * enabled. The check for updates sends the versions of the per-user
 * extensions to the index and downloads the newer ones into
 * `extension-updates`, which the manager installs at the next start.
 *
 * Where the extensions come from is in one place, the block of constants
 * below: the URLs of the index, the names of the parameters of its requests
 * and of the fields of its answers and of metadata.json that it uses. They are
 * the ones of extensions.gnome.org, which serves the zip of an extension for
 * the version of the shell it is asked for; an index of Lua extensions, which
 * is selected by `shell-api`, only needs these to change. The archives hold
 * a Lua extension (`metadata.json` with `shell-api`, `extension.lua`).
 *
 * Differences from the JS module:
 *
 *  - The UUID is checked before it is used in a URL and a path: it must be a
 *    plain file name.
 *  - A failure to download the description no longer puts an installed
 *    extension with the same UUID in the error state.
 *  - An archive that does not hold a loadable Lua extension is removed again
 *    and reported as ENABLE_FAILED (a JS extension from the index cannot run).
 *  - The temporary archive is deleted after the extraction.
 *  - A link to a local copy of an extension is never deleted through: an
 *    extension that is a link is neither replaced by an installation nor by
 *    an update.
 *  - The dialog answers `cancelled` if it is destroyed unanswered.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n-lib.h>
#include <gnome-autoar/gnome-autoar.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include "shell-global.h"
#include "ui/extension-downloader.h"
#include "ui/extension-system-private.h"
#include "ui/modal-dialog.h"

/* ------------------------------------------------------------------------ */
/* The index of extensions                                                   */
/* ------------------------------------------------------------------------ */

/* URLs. The download one has the UUID of the extension in it. */
#define INDEX_URL_DOWNLOAD "https://extensions.gnome.org/download-extension/%s.shell-extension.zip"
#define INDEX_URL_INFO     "https://extensions.gnome.org/extension-info/"
#define INDEX_URL_UPDATE   "https://extensions.gnome.org/update-info/"

/* The parameters of the requests */
#define INDEX_PARAM_UUID                       "uuid"
#define INDEX_PARAM_SHELL_VERSION              "shell_version"
#define INDEX_PARAM_DISABLE_VERSION_VALIDATION "disable_version_validation"

/* What the index is told about the shell. It selects the version of the
 * extension by it: today the version of the shell. */
#define INDEX_SHELL_VERSION PACKAGE_VERSION

/* The field of metadata.json that holds the version of an extension, which
 * the update request sends, and the field of the description that holds its
 * name */
#define INDEX_METADATA_VERSION_FIELD "version"
#define INDEX_INFO_NAME_FIELD        "name"

/* What the index answers for an extension that has a newer (or other) version */
#define INDEX_OPERATION_UPGRADE   "upgrade"
#define INDEX_OPERATION_DOWNGRADE "downgrade"

#define DISABLE_VERSION_VALIDATION_KEY "disable-extension-version-validation"
#define ALLOW_EXTENSION_INSTALLATION_KEY "allow-extension-installation"

struct _ShellExtensionDownloader
{
  GObject parent;

  ShellExtensionManager *manager;   /* weak */
  SoupSession *session;
};

G_DEFINE_FINAL_TYPE (ShellExtensionDownloader, shell_extension_downloader, G_TYPE_OBJECT)

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

/* `checkResponse ()` */
static gboolean
check_response (SoupMessage  *message,
                GError      **error)
{
  SoupStatus status = soup_message_get_status (message);

  if (status == SOUP_STATUS_OK)
    return TRUE;

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Unexpected response: %s",
               soup_status_get_phrase (status));
  return FALSE;
}

/* The UUID ends up in a URL and in file names */
static gboolean
uuid_is_valid (const char *uuid)
{
  return uuid != NULL && *uuid != '\0' &&
         strcmp (uuid, ".") != 0 && strcmp (uuid, "..") != 0 &&
         strchr (uuid, '/') == NULL && strlen (uuid) < 255 &&
         g_utf8_validate (uuid, -1, NULL);
}

static GFile *
get_extension_dir (ShellExtensionManager *manager,
                   const char            *subdir,
                   const char            *uuid)
{
  g_autofree char *userdatadir = shell_extension_manager_dup_userdatadir (manager);
  g_autofree char *path = g_build_filename (userdatadir, subdir, uuid, NULL);

  return g_file_new_for_path (path);
}

static SoupMessage *
new_download_message (const char *uuid)
{
  g_autofree char *url = g_strdup_printf (INDEX_URL_DOWNLOAD, uuid);

  return soup_message_new_from_encoded_form (SOUP_METHOD_GET, url,
                                             soup_form_encode (INDEX_PARAM_SHELL_VERSION,
                                                               INDEX_SHELL_VERSION,
                                                               NULL));
}

/* ------------------------------------------------------------------------ */
/* Extracting an archive                                                     */
/* ------------------------------------------------------------------------ */

typedef struct
{
  GBytes *bytes;
  GFile *dir;                    /* where the extension goes */
  GFile *archive;                /* the temporary file with the archive */
  GFile *tmp_dir;                /* where it is extracted to, next to @dir */
  AutoarExtractor *extractor;
} ExtractData;

static void
extract_data_free (ExtractData *data)
{
  if (data->extractor != NULL)
    g_signal_handlers_disconnect_by_data (data->extractor, data);
  g_clear_object (&data->extractor);
  g_clear_pointer (&data->bytes, g_bytes_unref);
  g_clear_object (&data->dir);
  g_clear_object (&data->archive);
  g_clear_object (&data->tmp_dir);
  g_free (data);
}

/* Removes what the extraction left and returns @error from the task */
static void
extract_fail (GTask  *task,
              GError *error)
{
  ExtractData *data = g_task_get_task_data (task);

  if (data->tmp_dir != NULL)
    shell_extension_recursively_delete_dir (data->tmp_dir, TRUE, NULL);
  if (data->archive != NULL)
    g_file_delete (data->archive, NULL, NULL);

  g_task_return_error (task, error);
  g_object_unref (task);
}

static void
extract_compiled_cb (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  GTask *task = user_data;
  ExtractData *data = g_task_get_task_data (task);
  g_autoptr (GError) error = NULL;

  if (!g_subprocess_wait_check_finish (G_SUBPROCESS (source), result, &error))
    {
      g_autofree char *name = g_file_get_basename (data->dir);

      g_message ("Error while compiling schema for extension %s: (%s)", name, error->message);
    }

  g_file_delete (data->archive, NULL, NULL);
  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

static void
extract_compile_schemas (GTask *task)
{
  ExtractData *data = g_task_get_task_data (task);
  g_autoptr (GFile) schemas = g_file_get_child (data->dir, "schemas");
  g_autoptr (GFileInfo) info = NULL;
  g_autoptr (GSubprocess) compile = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = NULL;

  info = g_file_query_info (schemas, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                            G_FILE_QUERY_INFO_NONE, NULL, &error);
  if (info == NULL || g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
    {
      if (info != NULL)
        {
          g_autofree char *name = g_file_get_basename (data->dir);

          g_message ("Error while looking for schema for extension %s: schemas is not a directory", name);
        }
      else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_autofree char *name = g_file_get_basename (data->dir);

          g_message ("Error while looking for schema for extension %s: %s", name, error->message);
        }

      g_file_delete (data->archive, NULL, NULL);
      g_task_return_boolean (task, TRUE);
      g_object_unref (task);
      return;
    }

  path = g_file_get_path (schemas);
  compile = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, &error,
                              "glib-compile-schemas", "--strict", path, NULL);
  if (compile == NULL)
    {
      g_message ("Error while compiling schema for extension %s: (%s)", path, error->message);
      g_file_delete (data->archive, NULL, NULL);
      g_task_return_boolean (task, TRUE);
      g_object_unref (task);
      return;
    }

  g_subprocess_wait_check_async (compile, g_task_get_cancellable (task),
                                 extract_compiled_cb, task);
}

static void
extract_moved_cb (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  GTask *task = user_data;
  ExtractData *data = g_task_get_task_data (task);
  g_autoptr (GError) error = NULL;

  if (!g_file_move_finish (G_FILE (source), result, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
        {
          extract_fail (task, g_steal_pointer (&error));
          return;
        }

      /* No rename across these file systems: copy it over by hand */
      g_clear_error (&error);
      if (!shell_extension_recursively_move_dir (data->tmp_dir, data->dir, &error))
        {
          extract_fail (task, g_steal_pointer (&error));
          return;
        }
      shell_extension_recursively_delete_dir (data->tmp_dir, TRUE, NULL);
    }

  extract_compile_schemas (task);
}

static void
on_extractor_completed (AutoarExtractor *extractor,
                        ExtractData     *data)
{
  GTask *task = g_object_get_data (G_OBJECT (extractor), "task");
  g_autoptr (GError) error = NULL;

  g_signal_handlers_disconnect_by_data (extractor, data);

  /* Replace the extension that is there */
  if (g_file_query_exists (data->dir, NULL) &&
      !shell_extension_recursively_delete_dir (data->dir, TRUE, &error))
    {
      extract_fail (task, g_steal_pointer (&error));
      return;
    }

  g_file_move_async (data->tmp_dir, data->dir,
                     G_FILE_COPY_NO_FALLBACK_FOR_MOVE, G_PRIORITY_DEFAULT,
                     g_task_get_cancellable (task), NULL, NULL,
                     extract_moved_cb, task);
}

static void
on_extractor_error (AutoarExtractor *extractor,
                    GError          *error,
                    ExtractData     *data)
{
  GTask *task = g_object_get_data (G_OBJECT (extractor), "task");

  g_signal_handlers_disconnect_by_data (extractor, data);
  extract_fail (task, g_error_copy (error));
}

static void
on_extractor_cancelled (AutoarExtractor *extractor,
                        ExtractData     *data)
{
  GTask *task = g_object_get_data (G_OBJECT (extractor), "task");

  g_signal_handlers_disconnect_by_data (extractor, data);
  extract_fail (task, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                           "Operation was cancelled"));
}

static void
extract_written_cb (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  GTask *task = user_data;
  ExtractData *data = g_task_get_task_data (task);
  g_autoptr (GFile) parent = NULL;
  g_autofree char *archive_name = NULL;
  g_autofree char *tmp_name = NULL;
  GError *error = NULL;

  if (!g_file_replace_contents_finish (G_FILE (source), result, NULL, &error))
    {
      extract_fail (task, error);
      return;
    }

  /* The archive is "XXXXXX.shell-extension.zip": the directory it is extracted
   * to is "XXXXXX.shell-extension", next to the final directory */
  archive_name = g_file_get_basename (data->archive);
  tmp_name = g_strndup (archive_name, strlen (archive_name) - strlen (".zip"));
  parent = g_file_get_parent (data->dir);
  data->tmp_dir = g_file_get_child (parent, tmp_name);
  if (!g_file_make_directory_with_parents (data->tmp_dir, NULL, &error))
    {
      g_clear_object (&data->tmp_dir);
      extract_fail (task, error);
      return;
    }

  data->extractor = autoar_extractor_new (data->archive, data->tmp_dir);
  autoar_extractor_set_output_is_dest (data->extractor, TRUE);
  g_object_set_data (G_OBJECT (data->extractor), "task", task);
  g_signal_connect (data->extractor, "error", G_CALLBACK (on_extractor_error), data);
  g_signal_connect (data->extractor, "completed", G_CALLBACK (on_extractor_completed), data);
  g_signal_connect (data->extractor, "cancelled", G_CALLBACK (on_extractor_cancelled), data);
  autoar_extractor_start_async (data->extractor, g_task_get_cancellable (task));
}

/* Extracts the zip in @bytes to @dir, replacing what is there */
static void
extract_archive_async (ShellExtensionDownloader *self,
                       GBytes                   *bytes,
                       GFile                    *dir,
                       GCancellable             *cancellable,
                       GAsyncReadyCallback       callback,
                       gpointer                  user_data)
{
  GTask *task = g_task_new (self, cancellable, callback, user_data);
  ExtractData *data = g_new0 (ExtractData, 1);
  g_autoptr (GFileIOStream) stream = NULL;
  GError *error = NULL;

  g_task_set_source_tag (task, extract_archive_async);
  data->bytes = g_bytes_ref (bytes);
  data->dir = g_object_ref (dir);
  g_task_set_task_data (task, data, (GDestroyNotify) extract_data_free);

  if (g_file_query_file_type (dir, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_EXISTS,
                               "%s is a link to a local copy of the extension",
                               g_file_peek_path (dir));
      g_object_unref (task);
      return;
    }

  data->archive = g_file_new_tmp ("XXXXXX.shell-extension.zip", &stream, &error);
  if (data->archive == NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }
  g_io_stream_close (G_IO_STREAM (stream), NULL, NULL);

  g_file_replace_contents_bytes_async (data->archive, data->bytes, NULL, FALSE,
                                       G_FILE_CREATE_REPLACE_DESTINATION, cancellable,
                                       extract_written_cb, task);
}

static gboolean
extract_archive_finish (ShellExtensionDownloader  *self,
                        GAsyncResult              *result,
                        GError                   **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ------------------------------------------------------------------------ */
/* Installing                                                                */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellExtensionDownloader *downloader;  /* strong */
  char *uuid;
  char *name;
  SoupMessage *message;
  ShellModalDialog *dialog;              /* weak */
  gboolean was_installed;
  gboolean confirmed;                    /* Install was pressed */
  gboolean answered;                     /* the task has its result */
} InstallData;

static void
install_data_free (InstallData *data)
{
  g_clear_object (&data->downloader);
  g_clear_weak_pointer (&data->dialog);
  g_clear_object (&data->message);
  g_free (data->uuid);
  g_free (data->name);
  g_free (data);
}

static void
install_return_error (GTask         *task,
                      ShellExtensionError code,
                      const char    *message)
{
  InstallData *data = g_task_get_task_data (task);

  data->answered = TRUE;
  g_message ("Error while installing %s: %s", data->uuid, message);
  g_task_return_new_error (task, SHELL_EXTENSION_ERROR, code, "%s", message);
}

static void
install_return_result (GTask      *task,
                       const char *result)
{
  InstallData *data = g_task_get_task_data (task);

  data->answered = TRUE;
  g_task_return_pointer (task, g_strdup (result), g_free);
}

static void
install_extracted_cb (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  GTask *task = user_data;
  InstallData *data = g_task_get_task_data (task);
  ShellExtensionDownloader *self = data->downloader;
  g_autoptr (GError) error = NULL;

  if (!extract_archive_finish (self, result, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          data->answered = TRUE;
          g_task_return_error (task, g_steal_pointer (&error));
        }
      else
        {
          install_return_error (task, SHELL_EXTENSION_ERROR_EXTRACT_FAILED, error->message);
        }
      g_object_unref (task);
      return;
    }

  if (self->manager == NULL ||
      !shell_extension_manager_add_installed (self->manager, data->uuid, &error))
    {
      /* An archive that is no Lua extension is not left behind (but the
       * files of an extension that was there are replaced already) */
      if (self->manager != NULL && !data->was_installed &&
          shell_extension_manager_lookup (self->manager, data->uuid) == NULL)
        {
          g_autoptr (GFile) dir = get_extension_dir (self->manager, "extensions", data->uuid);

          shell_extension_recursively_delete_dir (dir, TRUE, NULL);
        }

      install_return_error (task, SHELL_EXTENSION_ERROR_ENABLE_FAILED,
                            error != NULL ? error->message : "The extension manager is gone");
      g_object_unref (task);
      return;
    }

  install_return_result (task, "successful");
  g_object_unref (task);
}

static void
install_downloaded_cb (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  GTask *task = user_data;
  InstallData *data = g_task_get_task_data (task);
  ShellExtensionDownloader *self = data->downloader;
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) dir = NULL;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  if (bytes == NULL || !check_response (data->message, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          data->answered = TRUE;
          g_task_return_error (task, g_steal_pointer (&error));
        }
      else
        {
          install_return_error (task, SHELL_EXTENSION_ERROR_DOWNLOAD_FAILED, error->message);
        }
      g_object_unref (task);
      return;
    }

  if (self->manager == NULL)
    {
      install_return_error (task, SHELL_EXTENSION_ERROR_DOWNLOAD_FAILED,
                            "The extension manager is gone");
      g_object_unref (task);
      return;
    }

  data->was_installed = shell_extension_manager_lookup (self->manager, data->uuid) != NULL;
  dir = get_extension_dir (self->manager, "extensions", data->uuid);
  extract_archive_async (self, bytes, dir, g_task_get_cancellable (task),
                         install_extracted_cb, task);
}

/* The Install button */
static void
install_confirmed (gpointer user_data)
{
  GTask *task = user_data;
  InstallData *data = g_task_get_task_data (task);
  ShellExtensionDownloader *self = data->downloader;

  if (data->answered || data->confirmed)
    return;

  data->confirmed = TRUE;
  if (data->dialog != NULL)
    shell_modal_dialog_close (data->dialog);

  g_clear_object (&data->message);
  data->message = new_download_message (data->uuid);
  soup_session_send_and_read_async (self->session, data->message, G_PRIORITY_DEFAULT,
                                    g_task_get_cancellable (task),
                                    install_downloaded_cb, g_object_ref (task));
}

/* The Cancel button */
static void
install_cancelled (gpointer user_data)
{
  GTask *task = user_data;
  InstallData *data = g_task_get_task_data (task);

  if (data->answered || data->confirmed)
    return;

  install_return_result (task, "cancelled");
  if (data->dialog != NULL)
    shell_modal_dialog_close (data->dialog);
}

static void
on_install_dialog_destroy (ClutterActor *dialog,
                           GTask        *task)
{
  InstallData *data = g_task_get_task_data (task);

  /* Gone without an answer, as at shutdown */
  if (!data->answered && !data->confirmed)
    install_return_result (task, "cancelled");
}

static void
show_install_dialog (GTask *task)
{
  InstallData *data = g_task_get_task_data (task);
  g_autofree char *description = NULL;
  ShellMessageDialogContent *content;
  ShellModalDialog *dialog;

  dialog = g_object_new (SHELL_TYPE_MODAL_DIALOG,
                         "dialog-style-class", "extension-dialog",
                         NULL);
  g_set_weak_pointer (&data->dialog, dialog);

  shell_modal_dialog_add_button (dialog, _("Cancel"), CLUTTER_KEY_Escape, FALSE,
                                 install_cancelled, g_object_ref (task), g_object_unref);
  shell_modal_dialog_add_button (dialog, _("Install"), 0, TRUE,
                                 install_confirmed, g_object_ref (task), g_object_unref);

  description = g_strdup_printf (_("Download and install “%s” from extensions.gnome.org?"),
                                 data->name);
  content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT,
                          "title", _("Install Extension"),
                          "description", description,
                          NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (dialog)),
                           CLUTTER_ACTOR (content));

  g_signal_connect_object (dialog, "destroy", G_CALLBACK (on_install_dialog_destroy),
                           task, 0);

  if (!shell_modal_dialog_open (dialog))
    {
      /* The input could not be grabbed: nobody can answer */
      install_return_result (task, "cancelled");
      clutter_actor_destroy (CLUTTER_ACTOR (dialog));
    }
}

static void
install_info_cb (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  GTask *task = user_data;
  InstallData *data = g_task_get_task_data (task);
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  JsonNode *root;
  const char *name = NULL;
  gsize size;
  gconstpointer contents;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  if (bytes == NULL || !check_response (data->message, &error))
    goto failed;

  contents = g_bytes_get_data (bytes, &size);
  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, contents, size, &error))
    goto failed;

  root = json_parser_get_root (parser);
  if (root != NULL && JSON_NODE_HOLDS_OBJECT (root) &&
      json_object_has_member (json_node_get_object (root), INDEX_INFO_NAME_FIELD))
    name = json_object_get_string_member_with_default (json_node_get_object (root),
                                                       INDEX_INFO_NAME_FIELD, NULL);

  data->name = g_strdup (name != NULL ? name : data->uuid);
  show_install_dialog (task);
  g_object_unref (task);
  return;

failed:
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      data->answered = TRUE;
      g_task_return_error (task, g_steal_pointer (&error));
    }
  else
    {
      install_return_error (task, SHELL_EXTENSION_ERROR_INFO_DOWNLOAD_FAILED, error->message);
    }
  g_object_unref (task);
}

void
shell_extension_downloader_install_async (ShellExtensionDownloader *self,
                                          const char               *uuid,
                                          GCancellable             *cancellable,
                                          GAsyncReadyCallback       callback,
                                          gpointer                  user_data)
{
  GTask *task;
  InstallData *data;
  GSettings *settings;

  g_return_if_fail (SHELL_IS_EXTENSION_DOWNLOADER (self));
  g_return_if_fail (uuid != NULL);

  task = g_task_new (self->manager, cancellable, callback, user_data);
  g_task_set_source_tag (task, shell_extension_downloader_install_async);

  if (self->manager == NULL)
    {
      g_task_return_new_error (task, SHELL_EXTENSION_ERROR,
                               SHELL_EXTENSION_ERROR_NOT_ALLOWED,
                               "The extension manager is gone");
      g_object_unref (task);
      return;
    }

  settings = shell_extension_manager_get_settings (self->manager);
  if (!g_settings_get_boolean (settings, ALLOW_EXTENSION_INSTALLATION_KEY))
    {
      g_task_return_new_error (task, SHELL_EXTENSION_ERROR,
                               SHELL_EXTENSION_ERROR_NOT_ALLOWED,
                               "Extension installation is not allowed");
      g_object_unref (task);
      return;
    }

  if (!uuid_is_valid (uuid))
    {
      g_task_return_new_error (task, SHELL_EXTENSION_ERROR,
                               SHELL_EXTENSION_ERROR_INFO_DOWNLOAD_FAILED,
                               "Invalid extension UUID");
      g_object_unref (task);
      return;
    }

  data = g_new0 (InstallData, 1);
  data->downloader = g_object_ref (self);
  data->uuid = g_strdup (uuid);
  g_task_set_task_data (task, data, (GDestroyNotify) install_data_free);

  data->message = soup_message_new_from_encoded_form (SOUP_METHOD_GET, INDEX_URL_INFO,
                                                      soup_form_encode (INDEX_PARAM_UUID, uuid,
                                                                        INDEX_PARAM_SHELL_VERSION,
                                                                        INDEX_SHELL_VERSION,
                                                                        NULL));
  soup_session_send_and_read_async (self->session, data->message, G_PRIORITY_DEFAULT,
                                    cancellable, install_info_cb, task);
}

char *
shell_extension_downloader_install_finish (ShellExtensionDownloader  *self,
                                           GAsyncResult              *result,
                                           GError                   **error)
{
  g_return_val_if_fail (SHELL_IS_EXTENSION_DOWNLOADER (self), NULL);
  g_return_val_if_fail (g_task_is_valid (result, self->manager), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ------------------------------------------------------------------------ */
/* Updates                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellExtensionDownloader *downloader;   /* strong */
  guint pending;                          /* updates still being downloaded */
  SoupMessage *message;
} UpdateCheckData;

static void
update_check_data_free (UpdateCheckData *data)
{
  g_clear_object (&data->downloader);
  g_clear_object (&data->message);
  g_free (data);
}

typedef struct
{
  GTask *check;                  /* the task of the check, strong */
  char *uuid;
  SoupMessage *message;
} UpdateDownload;

static void
update_download_free (UpdateDownload *download)
{
  g_clear_object (&download->message);
  g_free (download->uuid);
  g_free (download);
}

static void
update_download_finish_one (UpdateDownload *download)
{
  GTask *check = download->check;
  UpdateCheckData *data = g_task_get_task_data (check);

  if (--data->pending == 0)
    {
      g_task_return_boolean (check, TRUE);
    }

  g_object_unref (check);
  update_download_free (download);
}

static void
update_extracted_cb (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  UpdateDownload *download = user_data;
  UpdateCheckData *data = g_task_get_task_data (download->check);
  ShellExtensionDownloader *self = data->downloader;
  g_autoptr (GError) error = NULL;

  if (!extract_archive_finish (self, result, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Error while downloading update for extension %s: (%s)",
                   download->uuid, error->message);
    }
  else if (self->manager != NULL)
    {
      shell_extension_manager_notify_extension_update (self->manager, download->uuid);
    }

  update_download_finish_one (download);
}

static void
update_downloaded_cb (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  UpdateDownload *download = user_data;
  UpdateCheckData *data = g_task_get_task_data (download->check);
  ShellExtensionDownloader *self = data->downloader;
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) dir = NULL;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  if (bytes == NULL || !check_response (download->message, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Error while downloading update for extension %s: (%s)",
                   download->uuid, error->message);
      update_download_finish_one (download);
      return;
    }

  if (self->manager == NULL)
    {
      update_download_finish_one (download);
      return;
    }

  dir = get_extension_dir (self->manager, "extension-updates", download->uuid);
  extract_archive_async (self, bytes, dir, g_task_get_cancellable (download->check),
                         update_extracted_cb, download);
}

static void
download_extension_update (GTask      *check,
                           const char *uuid)
{
  UpdateCheckData *data = g_task_get_task_data (check);
  UpdateDownload *download = g_new0 (UpdateDownload, 1);

  download->check = g_object_ref (check);
  download->uuid = g_strdup (uuid);
  download->message = new_download_message (uuid);
  data->pending++;

  soup_session_send_and_read_async (data->downloader->session, download->message,
                                    G_PRIORITY_DEFAULT, g_task_get_cancellable (check),
                                    update_downloaded_cb, download);
}

static void
update_info_cb (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  GTask *task = user_data;
  UpdateCheckData *data = g_task_get_task_data (task);
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GPtrArray) updates = g_ptr_array_new_with_free_func (g_free);
  JsonNode *root;
  gsize size;
  gconstpointer contents;
  guint i;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  if (bytes == NULL || !check_response (data->message, &error))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "Update check failed: %s",
                               error->message);
      g_object_unref (task);
      return;
    }

  contents = g_bytes_get_data (bytes, &size);
  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, contents, size, &error))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "Update check failed: %s",
                               error->message);
      g_object_unref (task);
      return;
    }

  root = json_parser_get_root (parser);
  if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
    {
      JsonObject *operations = json_node_get_object (root);
      g_autoptr (GList) members = json_object_get_members (operations);
      GList *l;

      for (l = members; l != NULL; l = l->next)
        {
          JsonNode *operation = json_object_get_member (operations, l->data);
          const char *value;

          if (!JSON_NODE_HOLDS_VALUE (operation) ||
              json_node_get_value_type (operation) != G_TYPE_STRING)
            continue;

          value = json_node_get_string (operation);
          if ((strcmp (value, INDEX_OPERATION_UPGRADE) == 0 ||
               strcmp (value, INDEX_OPERATION_DOWNGRADE) == 0) &&
              uuid_is_valid (l->data))
            g_ptr_array_add (updates, g_strdup (l->data));
        }
    }

  if (updates->len == 0)
    {
      g_task_return_boolean (task, TRUE);
      g_object_unref (task);
      return;
    }

  /* Held while the downloads are started, so that one that fails at once
   * does not complete the check before the others began */
  data->pending++;
  for (i = 0; i < updates->len; i++)
    download_extension_update (task, g_ptr_array_index (updates, i));
  if (--data->pending == 0)
    g_task_return_boolean (task, TRUE);

  g_object_unref (task);
}

void
shell_extension_downloader_check_for_updates_async (ShellExtensionDownloader *self,
                                                    GCancellable             *cancellable,
                                                    GAsyncReadyCallback       callback,
                                                    gpointer                  user_data)
{
  g_autoptr (GTask) task = NULL;
  g_autoptr (JsonBuilder) builder = NULL;
  g_autoptr (JsonGenerator) generator = NULL;
  g_autoptr (JsonNode) body_node = NULL;
  g_autoptr (GBytes) request_body = NULL;
  g_auto (GStrv) uuids = NULL;
  g_autofree char *body = NULL;
  g_autofree char *encoded = NULL;
  g_autofree char *url = NULL;
  UpdateCheckData *data;
  gboolean disable_validation;
  guint i, n_extensions = 0;

  g_return_if_fail (SHELL_IS_EXTENSION_DOWNLOADER (self));

  task = g_task_new (self->manager, cancellable, callback, user_data);
  g_task_set_source_tag (task, shell_extension_downloader_check_for_updates_async);

  if (self->manager == NULL || !shell_extension_manager_get_updates_supported (self->manager))
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  builder = json_builder_new ();
  json_builder_begin_object (builder);

  uuids = shell_extension_manager_get_uuids (self->manager);
  for (i = 0; uuids[i] != NULL; i++)
    {
      ShellLuaExtension *extension = shell_extension_manager_lookup (self->manager, uuids[i]);
      JsonObject *metadata;
      JsonNode *version;

      if (extension == NULL ||
          shell_lua_extension_get_extension_type (extension) != SHELL_LUA_EXTENSION_TYPE_PER_USER)
        continue;
      if (shell_extension_manager_has_update (self->manager, uuids[i]))
        continue;

      metadata = shell_lua_extension_get_metadata (extension);
      version = json_object_get_member (metadata, INDEX_METADATA_VERSION_FIELD);

      json_builder_set_member_name (builder, uuids[i]);
      json_builder_begin_object (builder);
      if (version != NULL)
        {
          json_builder_set_member_name (builder, INDEX_METADATA_VERSION_FIELD);
          json_builder_add_value (builder, json_node_copy (version));
        }
      json_builder_end_object (builder);
      n_extensions++;
    }

  json_builder_end_object (builder);

  if (n_extensions == 0)
    {
      g_task_return_boolean (task, TRUE);   /* nothing to update */
      return;
    }

  body_node = json_builder_get_root (builder);
  generator = json_generator_new ();
  json_generator_set_root (generator, body_node);
  body = json_generator_to_data (generator, NULL);
  request_body = g_bytes_new (body, strlen (body));

  disable_validation = g_settings_get_boolean (shell_extension_manager_get_settings (self->manager),
                                               DISABLE_VERSION_VALIDATION_KEY);
  encoded = soup_form_encode (INDEX_PARAM_SHELL_VERSION, INDEX_SHELL_VERSION,
                              INDEX_PARAM_DISABLE_VERSION_VALIDATION,
                              disable_validation ? "true" : "false", NULL);
  url = g_strdup_printf ("%s?%s", INDEX_URL_UPDATE, encoded);

  data = g_new0 (UpdateCheckData, 1);
  data->downloader = g_object_ref (self);
  g_task_set_task_data (task, data, (GDestroyNotify) update_check_data_free);

  data->message = soup_message_new (SOUP_METHOD_POST, url);
  soup_message_set_request_body_from_bytes (data->message, "application/json", request_body);

  soup_session_send_and_read_async (self->session, data->message, G_PRIORITY_DEFAULT,
                                    cancellable, update_info_cb, g_steal_pointer (&task));
}

gboolean
shell_extension_downloader_check_for_updates_finish (ShellExtensionDownloader  *self,
                                                     GAsyncResult              *result,
                                                     GError                   **error)
{
  g_return_val_if_fail (SHELL_IS_EXTENSION_DOWNLOADER (self), FALSE);
  g_return_val_if_fail (g_task_is_valid (result, self->manager), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ------------------------------------------------------------------------ */
/* Object                                                                    */
/* ------------------------------------------------------------------------ */

static void
shell_extension_downloader_dispose (GObject *object)
{
  ShellExtensionDownloader *self = SHELL_EXTENSION_DOWNLOADER (object);

  if (self->session != NULL)
    soup_session_abort (self->session);
  g_clear_object (&self->session);
  g_clear_weak_pointer (&self->manager);

  G_OBJECT_CLASS (shell_extension_downloader_parent_class)->dispose (object);
}

static void
shell_extension_downloader_class_init (ShellExtensionDownloaderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_extension_downloader_dispose;
}

static void
shell_extension_downloader_init (ShellExtensionDownloader *self)
{
  self->session = soup_session_new ();
}

ShellExtensionDownloader *
shell_extension_downloader_new (ShellExtensionManager *manager)
{
  ShellExtensionDownloader *self;

  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (manager), NULL);

  self = g_object_new (SHELL_TYPE_EXTENSION_DOWNLOADER, NULL);
  g_set_weak_pointer (&self->manager, manager);

  return self;
}
