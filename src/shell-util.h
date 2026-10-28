/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#pragma once

#include <gio/gio.h>
#include <clutter/clutter.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <meta/meta-cursor-tracker.h>
#include <meta/meta-window-actor.h>

G_BEGIN_DECLS

void     shell_util_set_hidden_from_pick       (ClutterActor     *actor,
                                                gboolean          hidden);

int      shell_util_get_week_start             (void);

const char *shell_util_translate_time_string   (const char *str);

char    *shell_util_regex_escape               (const char *str);

gboolean shell_write_string_to_stream          (GOutputStream    *stream,
                                                const char       *str,
                                                GError          **error);

char    *shell_get_file_contents_utf8_sync     (const char       *path,
                                                GError          **error);

void     shell_util_touch_file_async           (GFile               *file,
                                                GAsyncReadyCallback  callback,
                                                gpointer             user_data);
gboolean shell_util_touch_file_finish          (GFile               *file,
                                                GAsyncResult        *res,
                                                GError             **error);

gboolean shell_util_wifexited                  (int               status,
                                                int              *exit);

GdkPixbuf *shell_util_create_pixbuf_from_data (const guchar      *data,
                                               gsize              len,
                                               GdkColorspace      colorspace,
                                               gboolean           has_alpha,
                                               int                bits_per_sample,
                                               int                width,
                                               int                height,
                                               int                rowstride);

void shell_util_check_cloexec_fds (void);

void   shell_util_start_systemd_unit          (const char           *unit,
                                               const char           *mode,
                                               GCancellable         *cancellable,
                                               GAsyncReadyCallback   callback,
                                               gpointer              user_data);
gboolean shell_util_start_systemd_unit_finish (GAsyncResult         *res,
                                               GError              **error);

void  shell_util_stop_systemd_unit           (const char           *unit,
                                              const char           *mode,
                                              GCancellable         *cancellable,
                                              GAsyncReadyCallback   callback,
                                              gpointer              user_data);
gboolean shell_util_stop_systemd_unit_finish (GAsyncResult         *res,
                                              GError              **error);

void shell_util_systemd_unit_exists (const gchar         *unit,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data);
gboolean shell_util_systemd_unit_exists_finish (GAsyncResult  *res,
                                                GError       **error);

void shell_util_sd_notify (void);

gboolean shell_util_has_x11_display_extension (MetaDisplay *display,
                                               const char  *extension);

char *shell_util_get_translated_folder_name (const char *name);

gint shell_util_get_uid (void);

GPid shell_util_spawn_async_with_pipes_and_fds (const char          *working_directory,
                                                const char * const  *argv,
                                                const char * const  *envp,
                                                GSpawnFlags          flags,
                                                int                  stdin_fd,
                                                int                  stdout_fd,
                                                int                  stderr_fd,
                                                const int           *source_fds,
                                                const int           *target_fds,
                                                size_t               n_fds,
                                                int                 *stdin_pipe_out,
                                                int                 *stdout_pipe_out,
                                                int                 *stderr_pipe_out,
                                                GError             **error);

GPid shell_util_spawn_async_with_pipes (const char          *working_directory,
                                        const char * const  *argv,
                                        const char * const  *envp,
                                        GSpawnFlags          flags,
                                        int                 *standard_input,
                                        int                 *standard_output,
                                        int                 *standard_error,
                                        GError             **error);

GPid shell_util_spawn_async_with_fds (const char          *working_directory,
                                      const char * const  *argv,
                                      const char * const  *envp,
                                      GSpawnFlags          flags,
                                      int                  stdin_fd,
                                      int                  stdout_fd,
                                      int                  stderr_fd,
                                      GError             **error);

GPid shell_util_spawn_async (const char          *working_directory,
                             const char * const  *argv,
                             const char * const  *envp,
                             GSpawnFlags          flags,
                             GError             **error);

/* Ports of the helpers of js/misc/util.js and js/misc/fileUtils.js */
gboolean shell_util_try_spawn                  (const char * const *argv,
                                                GError            **error);
gboolean shell_util_try_spawn_command_line     (const char         *command_line,
                                                GError            **error);
void     shell_util_spawn                      (const char * const *argv);
void     shell_util_spawn_command_line         (const char         *command_line);
void     shell_util_spawn_app                  (const char * const *argv);

int      shell_util_gnome_version_compare      (const char         *version1,
                                                const char         *version2);

guint    shell_util_insert_sorted              (GPtrArray          *array,
                                                gpointer            val,
                                                GCompareFunc        cmp);

#define SHELL_TYPE_DATADIR_CHILD (shell_datadir_child_get_type ())

typedef struct _ShellDatadirChild ShellDatadirChild;

/**
 * ShellDatadirChild:
 * @file: the file of the child
 * @info: the file info of the child, with the `standard::name` and
 *   `standard::type` attributes
 *
 * A child of a `gnome-shell/<subdir>` directory of a data directory, as
 * returned by shell_util_collect_from_datadirs ().
 */
struct _ShellDatadirChild
{
  GFile     *file;
  GFileInfo *info;
};

GType              shell_datadir_child_get_type        (void);
ShellDatadirChild *shell_datadir_child_copy            (const ShellDatadirChild *child);
void               shell_datadir_child_free            (ShellDatadirChild       *child);

GPtrArray *shell_util_collect_from_datadirs    (const char         *subdir,
                                                gboolean            include_user_dir);

G_END_DECLS
